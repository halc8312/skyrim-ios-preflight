/* SPDX-License-Identifier: GPL-3.0-only */
#include "pipeline_cache.h"
#include "pipeline_cache_control.h"
#include "performance_metrics.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr size_t maxPayload = 64 * 1024 * 1024;
constexpr size_t headerSize = 32;
/* Process-lifetime registry: never destroy its mutex during static teardown
 * while an optional UIKit checkpoint worker may still be completing. */
struct RegistryMutex {
    pthread_mutex_t value = PTHREAD_MUTEX_INITIALIZER;
    void lock() { pthread_mutex_lock(&value); }
    bool try_lock() { return pthread_mutex_trylock(&value) == 0; }
    void unlock() { pthread_mutex_unlock(&value); }
} registryMutex;
skyrim::PipelineCache *registryHead;
std::atomic<uint64_t> issuedTicket{0}, cancelledThrough{0};
std::atomic<uint64_t> activeSaveTicket{0}, foregroundTicket{0}, foregroundEpoch{0};
std::atomic<bool> foregroundPermission{false};
uint64_t lastForegroundAttemptNs; // registry lock
constexpr uint64_t foregroundQuietNs = UINT64_C(10000000000);
constexpr uint64_t foregroundCooldownNs = UINT64_C(60000000000);
bool cancelled(uint64_t ticket) {
    return !ticket || ticket <= cancelledThrough.load(std::memory_order_relaxed);
}
void cancelForeground() {
    foregroundEpoch.fetch_add(1);
    const uint64_t ticket = foregroundTicket.load();
    if (ticket) skyrim_pipeline_checkpoint_cancel(ticket);
}
struct SaveLease {
    uint64_t ticket;
    ~SaveLease() {
        uint64_t expected = ticket;
        foregroundTicket.compare_exchange_strong(expected, 0);
        activeSaveTicket.store(0, std::memory_order_release);
    }
};
uint64_t readLE(const uint8_t *p, unsigned size) {
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}
void writeLE(uint8_t *p, uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) p[i] = uint8_t(value >> (8 * i));
}
/* Accidental corruption checksum, not a signature or trust assertion. The full
 * identity and Vulkan header are compared as well; a filename hash is not a key. */
uint64_t checksum(const void *data, size_t size) {
    uint64_t h = UINT64_C(14695981039346656037);
    const auto *p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
struct File {
    int fd;
    explicit File(int value) : fd(value) {}
    ~File() { if (fd >= 0) close(fd); }
};
bool readExact(int fd, void *data, size_t bytes) {
    auto *p = static_cast<uint8_t*>(data);
    while (bytes) {
        ssize_t n = read(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; bytes -= size_t(n);
    }
    return true;
}
bool writeExact(int fd, const void *data, size_t bytes, uint64_t ticket, int& error) {
    auto *p = static_cast<const uint8_t*>(data);
    while (bytes && !cancelled(ticket)) {
        ssize_t n = write(fd, p, std::min(bytes, size_t(1024 * 1024)));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = n < 0 ? errno : EIO; return false; }
        p += n; bytes -= size_t(n);
    }
    return !bytes && !cancelled(ticket);
}
}

namespace skyrim {
namespace {
enum Outcome : unsigned { Stored, Clean, NoPath, Cancelled, DriverError,
    SizeRange, RetryExhausted, WriteFailed, Exception, InvalidPayload };
enum Stage : unsigned { Start, SizeQuery, DataQuery, RemovePending, Open,
    Write, Sync, Close, Rename, Done, ImportPublish };
constexpr uint64_t snapshotBudgetNs = UINT64_C(1000000000);
}
struct CacheCheckpointReport {
    uint64_t ticket, started = skyrim_perf_now_ns(), generation, savedGeneration;
    unsigned outcome = Exception, stage = Start, attempts = 0, budgetExpired = 0;
    VkResult vk = VK_SUCCESS;
    size_t sizeQuery = 0, bytes = 0;
    int error = 0;
    int cleanupError = 0;
    unsigned importPublish = 0;
    CacheCheckpointReport(uint64_t t, uint64_t g, uint64_t saved)
        : ticket(t), generation(g), savedGeneration(saved) {}
    void end(unsigned value) { outcome = value; }
    // This is admission control between calls, not a deadline imposed on a
    // running driver call or fsync. Cancellation never destroys live driver work.
    bool stopped() {
        if (cancelled(ticket)) { end(Cancelled); return true; }
        uint64_t now = skyrim_perf_now_ns();
        if (!started || !now || now < started || now - started >= snapshotBudgetNs) {
            budgetExpired = 1; end(RetryExhausted); return true;
        }
        return false;
    }
    ~CacheCheckpointReport() {
        const int savedErrno = errno;
        const uint64_t now = skyrim_perf_now_ns();
        fprintf(stderr, "IOS_PIPE_CACHE event=checkpoint ticket=%llu outcome=%u stage=%u attempts=%u vk=%d size_query=%zu bytes=%zu gen=%llu saved_gen=%llu errno=%d budget_expired=%u stored=%u elapsed_ns=%llu import_publish=%u cleanup_errno=%d\n",
            (unsigned long long)ticket, outcome, stage, attempts, int(vk), sizeQuery, bytes,
            (unsigned long long)generation, (unsigned long long)savedGeneration, error,
            budgetExpired, unsigned(outcome == Stored),
            (unsigned long long)(started && now >= started ? now - started : 0), importPublish, cleanupError);
        errno = savedErrno;
    }
};
PipelineCache::PipelineCache(VkDevice device, PFN_vkCreatePipelineCache create,
    PFN_vkGetPipelineCacheData getData, PFN_vkDestroyPipelineCache destroy,
    const VkPhysicalDeviceProperties& properties, const char *directory,
    const char *sourceNamespace, const char *legacyDirectory)
    : device_(device), getData_(getData), destroy_(destroy), properties_(properties) {
    std::vector<uint8_t> initial;
    bool imported = false;
    try {
        struct utsname os{};
        struct stat root{};
        if (directory && directory[0] == '/' && sourceNamespace && *sourceNamespace &&
            !lstat(directory, &root) && S_ISDIR(root.st_mode) && !uname(&os)) {
            char gpu[128];
            int n = snprintf(gpu, sizeof(gpu), "%08x:%08x:%08x:%08x:",
                properties.vendorID, properties.deviceID, properties.driverVersion, properties.apiVersion);
            for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
                n += snprintf(gpu + n, sizeof(gpu) - size_t(n), "%02x", properties.pipelineCacheUUID[i]);
            identity_ = std::string(sourceNamespace) + "|" + gpu + "|" + os.sysname + "|" +
                os.release + "|" + os.version + "|" + os.machine;
            if (identity_.size() <= 2048) {
                // One published file plus one bounded pending file per app.
                // A different GPU/build is a cold miss, not another disk copy.
                path_ = std::string(directory) + "/pipeline-v1.cache";
                initial = load(path_);
                struct stat target{}, legacy{};
                // Import only when the new name is absent. An invalid, newer,
                // unreadable or symlink target must not revive stale legacy data.
                if (legacyDirectory && legacyDirectory[0] == '/' &&
                    lstat(path_.c_str(), &target) && errno == ENOENT &&
                    !lstat(legacyDirectory, &legacy) && S_ISDIR(legacy.st_mode)) {
                    initial = load(std::string(legacyDirectory) + "/pipeline-v1.cache");
                    imported = !initial.empty();
                }
            }
        }
    } catch (...) { path_.clear(); initial.clear(); imported = false; }
    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    // flags=0 retains the driver's internal synchronization.
    info.initialDataSize = initial.size();
    info.pInitialData = initial.empty() ? nullptr : initial.data();
    result_ = create(device_, &info, nullptr, &handle_);
    const size_t offered = initial.size();
    bool retry = result_ != VK_SUCCESS && !initial.empty();
    if (retry) {
        handle_ = VK_NULL_HANDLE;
        info.initialDataSize = 0; info.pInitialData = nullptr;
        result_ = create(device_, &info, nullptr, &handle_);
    }
    fprintf(stderr, "IOS_PIPE_CACHE event=create offered_bytes=%zu empty_retry=%u result=%d hit_count_unknown=1 imported=%u\n",
        offered, unsigned(retry), int(result_), unsigned(imported));
    if (result_ == VK_SUCCESS) {
        importPending_ = imported && !retry;
        if (importPending_) generation_.store(1, std::memory_order_relaxed);
        lastCompileNs_.store(skyrim_perf_now_ns(), std::memory_order_relaxed);
        std::lock_guard<RegistryMutex> lock(registryMutex);
        next_ = registryHead; registryHead = this; registered_ = true;
    }
}

PipelineCache::~PipelineCache() {
    {
        // A checkpoint cannot outlive its Vulkan cache/device. Pipeline compile
        // itself never takes this registry lock and is not serialized here.
        std::lock_guard<RegistryMutex> lock(registryMutex);
        if (registered_) {
            auto **p = &registryHead;
            while (*p && *p != this) p = &(*p)->next_;
            if (*p) *p = next_;
        }
    }
    if (result_ == VK_SUCCESS) destroy_(device_, handle_, nullptr);
}

void PipelineCache::compileStarted() noexcept {
    activeCompiles_.fetch_add(1, std::memory_order_acq_rel);
    cancelForeground();
}

void PipelineCache::compileFinished() noexcept {
    uint64_t now = skyrim_perf_now_ns();
    // Finishers may publish in a different order from their clock reads.
    // Never replace a later completion by an older sample. A clock failure
    // disables optional foreground admission for this cache until restart.
    if (!now) now = UINT64_MAX;
    uint64_t last = lastCompileNs_.load(std::memory_order_relaxed);
    while (last < now && !lastCompileNs_.compare_exchange_weak(last, now, std::memory_order_relaxed)) {}
    activeCompiles_.fetch_sub(1, std::memory_order_release);
}

bool PipelineCache::foregroundQuiet(uint64_t now) const {
    if (activeCompiles_.load(std::memory_order_acquire)) return false;
    const uint64_t last = lastCompileNs_.load(std::memory_order_relaxed);
    return last && now >= last && now - last >= foregroundQuietNs;
}

uint64_t PipelineCache::foregroundBegin() {
    if (!foregroundPermission.load() || foregroundTicket.load() ||
        activeSaveTicket.load(std::memory_order_acquire)) return 0;
    std::unique_lock<RegistryMutex> lock(registryMutex, std::try_to_lock);
    if (!lock.owns_lock()) return 0;
    const uint64_t epoch = foregroundEpoch.load();
    const uint64_t now = skyrim_perf_now_ns();
    if (!now || (lastForegroundAttemptNs &&
        (now < lastForegroundAttemptNs || now - lastForegroundAttemptNs < foregroundCooldownNs))) return 0;
    bool dirty = false;
    for (auto *p = registryHead; p; p = p->next_) {
        if (!p->foregroundQuiet(now)) return 0;
        dirty |= !p->path_.empty() && p->generation_.load(std::memory_order_relaxed) != p->savedGeneration_;
    }
    if (!dirty) return 0;
    const uint64_t ticket = issuedTicket.fetch_add(1, std::memory_order_relaxed) + 1;
    uint64_t expected = 0;
    if (!foregroundTicket.compare_exchange_strong(expected, ticket)) return 0;
    // If compile start raced with admission, either it sees our reservation and
    // cancels it, or this epoch check rejects it. An already-started driver call
    // remains uncancellable; subsequent snapshot/I/O steps check the ticket.
    if (!foregroundPermission.load() || foregroundEpoch.load() != epoch || cancelled(ticket)) {
        skyrim_pipeline_checkpoint_cancel(ticket);
        return 0;
    }
    lastForegroundAttemptNs = now;
    fprintf(stderr, "IOS_PIPE_CACHE event=foreground_admit ticket=%llu quiet_ns=%llu cooldown_ns=%llu\n",
        (unsigned long long)ticket, (unsigned long long)foregroundQuietNs,
        (unsigned long long)foregroundCooldownNs);
    return ticket;
}

bool PipelineCache::validPayload(const std::vector<uint8_t>& bytes) const {
    // The pinned MoltenVK emits a 32-byte V1 header and at least the EOF word.
    return bytes.size() >= 36 && bytes.size() <= maxPayload &&
        readLE(bytes.data(), 4) == 32 &&
        readLE(bytes.data() + 4, 4) == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
        readLE(bytes.data() + 8, 4) == properties_.vendorID &&
        readLE(bytes.data() + 12, 4) == properties_.deviceID &&
        !memcmp(bytes.data() + 16, properties_.pipelineCacheUUID, VK_UUID_SIZE);
}

std::vector<uint8_t> PipelineCache::load(const std::string& path) const {
    if (path.empty()) return {};
    File file(open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat st{};
    uint8_t h[headerSize];
    if (file.fd < 0 || fstat(file.fd, &st) || !S_ISREG(st.st_mode) ||
        st.st_size < static_cast<off_t>(headerSize) ||
        st.st_size > static_cast<off_t>(headerSize + 2048 + maxPayload) ||
        !readExact(file.fd, h, sizeof(h)) || memcmp(h, "SKPC0001", 8)) return {};
    uint64_t keyBytes = readLE(h + 8, 8), payloadBytes = readLE(h + 16, 8);
    if (keyBytes != identity_.size() || payloadBytes < 36 || payloadBytes > maxPayload ||
        uint64_t(st.st_size) != headerSize + keyBytes + payloadBytes) return {};
    std::string key(size_t(keyBytes), '\0');
    if (!readExact(file.fd, key.data(), key.size()) || key != identity_) return {};
    std::vector<uint8_t> bytes(static_cast<size_t>(payloadBytes));
    if (!readExact(file.fd, bytes.data(), bytes.size()) || !validPayload(bytes) ||
        readLE(h + 24, 8) != checksum(bytes.data(), bytes.size())) return {};
    return bytes;
}

bool PipelineCache::save(const std::vector<uint8_t>& bytes, uint64_t ticket,
                         CacheCheckpointReport& r) const {
    if (path_.empty() || !validPayload(bytes) || cancelled(ticket)) return false;
    std::string temporary = path_ + ".pending";
    // Remove only our unpublished name, never the last valid cache.
    r.stage = RemovePending;
    if (unlink(temporary.c_str()) && errno != ENOENT) { r.error = errno; return false; }
    r.stage = Open;
    File file(open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.fd < 0) { r.error = errno; return false; }
    uint8_t header[headerSize]{};
    memcpy(header, "SKPC0001", 8);
    writeLE(header + 8, identity_.size(), 8);
    writeLE(header + 16, bytes.size(), 8);
    writeLE(header + 24, checksum(bytes.data(), bytes.size()), 8);
    r.stage = Write;
    bool ok = writeExact(file.fd, header, sizeof(header), ticket, r.error) &&
        writeExact(file.fd, identity_.data(), identity_.size(), ticket, r.error) &&
        writeExact(file.fd, bytes.data(), bytes.size(), ticket, r.error);
    if (ok) {
        r.stage = Sync;
        if (fsync(file.fd)) { r.error = errno; ok = false; }
        ok = ok && !cancelled(ticket);
    }
    if (ok) r.stage = Close;
    int closeResult = close(file.fd); file.fd = -1;
    if (ok && closeResult) r.error = errno;
    ok = ok && !closeResult && !cancelled(ticket);
    if (ok) {
        if (importPending_) {
            r.stage = ImportPublish;
            r.importPublish = 1;
            // Same-directory link publishes without replacing an intervening
            // target. Unsupported filesystems and EEXIST remain explicit
            // optional-write failures; never fall back to overwriting rename.
            if (link(temporary.c_str(), path_.c_str())) { r.error = errno; ok = false; }
            else if (unlink(temporary.c_str())) r.cleanupError = errno;
        } else {
            r.stage = Rename;
            if (rename(temporary.c_str(), path_.c_str())) { r.error = errno; ok = false; }
        }
    }
    if (!ok) unlink(temporary.c_str());
    else r.stage = Done;
    return ok;
}

void PipelineCache::checkpoint(uint64_t ticket) {
    CacheCheckpointReport r(ticket, generation_.load(std::memory_order_relaxed), savedGeneration_);
    if (path_.empty()) return r.end(NoPath);
    if (cancelled(ticket)) return r.end(Cancelled);
    if (r.generation == savedGeneration_) return r.end(Clean);
    std::vector<uint8_t> bytes;
    for (;;) {
        if (r.stopped()) return;
        ++r.attempts;
        r.sizeQuery = r.bytes = 0;
        r.generation = generation_.load(std::memory_order_relaxed);
        size_t size = 0;
        r.stage = SizeQuery;
        r.vk = getData_(device_, handle_, &size, nullptr);
        r.sizeQuery = size;
        if (r.stopped()) return;
        if (r.vk != VK_SUCCESS) return r.end(DriverError);
        if (size < 36 || size > maxPayload) return r.end(SizeRange);
        // Reuse the allocation when possible. On growth, release it first so
        // vector growth cannot double a previously large allocation.
        if (size > bytes.capacity()) std::vector<uint8_t>().swap(bytes);
        bytes.resize(size);
        if (r.stopped()) return;
        r.stage = DataQuery;
        r.vk = getData_(device_, handle_, &size, bytes.data());
        r.bytes = size;
        if (r.stopped()) return;
        if (size > bytes.size()) return r.end(DriverError);
        if (r.vk == VK_INCOMPLETE || (r.vk == VK_SUCCESS && size < 36)) {
            if (r.attempts == 3) return r.end(RetryExhausted);
            continue; // Only this race-shaped result gets a fresh size query.
        }
        if (r.vk != VK_SUCCESS) return r.end(DriverError);
        bytes.resize(size);
        if (!validPayload(bytes)) return r.end(InvalidPayload);
        if (r.stopped()) return;
        bool stored = save(bytes, ticket, r);
        if (stored) {
            importPending_ = false;
            savedGeneration_ = r.generation; // later compiles remain dirty
            r.savedGeneration = savedGeneration_;
            return r.end(Stored);
        }
        return r.end(cancelled(ticket) ? Cancelled : WriteFailed);
    }
}

void PipelineCache::checkpointAll(uint64_t ticket) {
    if (cancelled(ticket)) {
        fprintf(stderr, "IOS_PIPE_CACHE event=checkpoint_all ticket=%llu cancelled=1 caches=0\n",
            (unsigned long long)ticket);
        return;
    }
    uint64_t expected = 0;
    if (!activeSaveTicket.compare_exchange_strong(expected, ticket, std::memory_order_acq_rel)) {
        // Do not queue behind another snapshot or duplicate its ticket. UIKit
        // normally uses one serial queue; this also protects non-UIKit callers.
        if (expected != ticket) {
            uint64_t pending = ticket;
            foregroundTicket.compare_exchange_strong(pending, 0);
        }
        fprintf(stderr, "IOS_PIPE_CACHE event=checkpoint_all ticket=%llu busy=1 caches=0\n",
            (unsigned long long)ticket);
        return;
    }
    SaveLease lease{ticket};
    std::lock_guard<RegistryMutex> lock(registryMutex);
    unsigned visited = 0;
    for (auto *p = registryHead; p && !cancelled(ticket); p = p->next_) {
        ++visited;
        try { p->checkpoint(ticket); }
        catch (...) { /* The report already emitted the one exception outcome. */ }
    }
    fprintf(stderr, "IOS_PIPE_CACHE event=checkpoint_all ticket=%llu cancelled=%u caches=%u\n",
        (unsigned long long)ticket, unsigned(cancelled(ticket)), visited);
}
}

extern "C" uint64_t skyrim_pipeline_checkpoint_begin(void) {
    uint64_t ticket = issuedTicket.fetch_add(1, std::memory_order_relaxed) + 1;
    skyrim_pipeline_checkpoint_cancel(ticket - 1);
    return ticket;
}
extern "C" void skyrim_pipeline_checkpoint_cancel(uint64_t ticket) {
    uint64_t old = cancelledThrough.load(std::memory_order_relaxed);
    while (old < ticket && !cancelledThrough.compare_exchange_weak(old, ticket, std::memory_order_relaxed)) {}
    uint64_t pending = foregroundTicket.load();
    while (pending && pending <= ticket && !foregroundTicket.compare_exchange_weak(pending, 0)) {}
}
extern "C" void skyrim_pipeline_checkpoint_save(uint64_t ticket) {
    skyrim::PipelineCache::checkpointAll(ticket);
}
extern "C" void skyrim_pipeline_checkpoint_foreground_permission(int allowed) {
    foregroundPermission.store(allowed != 0);
    if (!allowed) cancelForeground();
}
extern "C" uint64_t skyrim_pipeline_checkpoint_foreground_begin(void) {
    return skyrim::PipelineCache::foregroundBegin();
}
