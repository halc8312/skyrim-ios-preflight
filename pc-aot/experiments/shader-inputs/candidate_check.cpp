// Private, one-candidate DXSO analyzer. Never compiles SPIR-V or creates a device.
#include "dxso/dxso_module.h"
#include "dxvk/dxvk_shader_key.h"
#include "util/sha1/sha1_util.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dxvk { Logger Logger::s_instance("candidate-dxso-probe.log"); }
namespace dxvk::env { std::string getExePath() { return "candidate-dxso-probe"; } }

static constexpr size_t MaxInput = 1u << 20;
static constexpr rlim_t AddressLimit = 512u << 20;
static constexpr rlim_t OutputLimit = 1u << 20;

static void limit(int resource, rlim_t soft, rlim_t hard) {
  const rlimit requested{soft, hard};
  rlimit observed{};
  if (setrlimit(resource, &requested) || getrlimit(resource, &observed)
      || observed.rlim_cur != soft || observed.rlim_max != hard)
    throw std::runtime_error("could not establish required resource limit");
}

static void limits() {
  limit(RLIMIT_AS, AddressLimit, AddressLimit);
  limit(RLIMIT_CPU, 2, 3);
  limit(RLIMIT_FSIZE, OutputLimit, OutputLimit);
  limit(RLIMIT_CORE, 0, 0);
  limit(RLIMIT_NOFILE, 32, 32);
  // Also disable dumpability: a system core collector can bypass RLIMIT_CORE.
  if (prctl(PR_SET_DUMPABLE, 0) || prctl(PR_GET_DUMPABLE) != 0)
    throw std::runtime_error("could not disable core dumpability");
  std::puts("{\"kind\":\"isolation\",\"address_space_bytes\":536870912,"
            "\"cpu_soft_seconds\":2,\"cpu_hard_seconds\":3,"
            "\"file_size_bytes\":1048576,\"core_bytes\":0,\"open_files\":32,\"dumpable\":false}");
  std::fflush(stdout);
}

struct Input {
  void* mapping = MAP_FAILED;
  size_t mappedBytes = 0;
  char* data = nullptr;
  size_t bytes = 0;

  ~Input() { if (mapping != MAP_FAILED) munmap(mapping, mappedBytes); }

  void load(const char* path) {
    const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("input open failed");
    struct Close { int fd; ~Close() { close(fd); } } closeInput{fd};
    struct stat before{}, after{};
    if (fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 8
        || static_cast<uint64_t>(before.st_size) > MaxInput || before.st_size % 4)
      throw std::runtime_error("input size or type rejected");
    bytes = static_cast<size_t>(before.st_size);
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || page % 4) throw std::runtime_error("page size unavailable");
    const size_t rounded = ((bytes + size_t(page) - 1) / size_t(page)) * size_t(page);
    mappedBytes = rounded + 2 * size_t(page);
    mapping = mmap(nullptr, mappedBytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) throw std::runtime_error("guard allocation failed");
    char* writable = static_cast<char*>(mapping) + page;
    if (mprotect(writable, rounded, PROT_READ | PROT_WRITE))
      throw std::runtime_error("guard setup failed");
    // Exact input end abuts PROT_NONE; no padding END token can hide a past-end read.
    data = writable + rounded - bytes;
    size_t done = 0;
    while (done < bytes) {
      const ssize_t count = read(fd, data + done, bytes - done);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) throw std::runtime_error("input read failed");
      done += static_cast<size_t>(count);
    }
    char extra;
    if (read(fd, &extra, 1) != 0 || fstat(fd, &after)
        || before.st_dev != after.st_dev || before.st_ino != after.st_ino
        || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
      throw std::runtime_error("input changed during read");
    if (mprotect(writable, rounded, PROT_READ))
      throw std::runtime_error("input read-only protection failed");
  }
};

int main(int argc, char** argv) {
  try { limits(); }
  catch (const std::exception& error) {
    std::fprintf(stderr, "isolation_failed: %.512s\n", error.what());
    return 77;
  }
  try {
    if (argc != 2) throw std::runtime_error("one candidate path or authored control required");
    const std::string argument(argv[1]);
    // These controls contain no game input and exercise the actual child limits.
    if (argument == "--authored-wall") {
      for (;;) pause();
    }
    if (argument == "--authored-cpu") {
      volatile uint64_t value = 0;
      for (;;) value += 1;
    }
    if (argument == "--authored-memory") {
      void* value = mmap(nullptr, AddressLimit + 4096, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (value != MAP_FAILED) {
        munmap(value, AddressLimit + 4096);
        throw std::runtime_error("address space bound was not enforced");
      }
      std::puts("{\"kind\":\"control\",\"memory_limit_observed\":true}");
      return 0;
    }
    if (argument == "--authored-output") {
      const char block[4096] = {};
      for (;;) {
        if (write(STDERR_FILENO, block, sizeof(block)) < 0)
          throw std::runtime_error("output limit write rejected");
      }
    }
    Input input;
    input.load(argv[1]);
    dxvk::DxsoReader reader(input.data);
    dxvk::DxsoModule module(reader);
    if (module.info().majorVersion() != 3 || module.info().minorVersion() != 0)
      throw std::runtime_error("only SM3 candidates are admitted");
    const auto analysis = module.analyze();
    if (analysis.bytecodeByteLength != input.bytes)
      throw std::runtime_error("native consumption differs from exact candidate length");
    const auto stage = module.info().shaderStage();
    const dxvk::DxvkShaderKey key(stage,
      dxvk::Sha1Hash::compute(input.data, analysis.bytecodeByteLength));
    std::printf("{\"kind\":\"analysis\",\"stage_vk\":%u,\"analyzed_bytes\":%zu,"
                "\"dxvk_key\":\"%s\",\"read_only_input\":true,\"end_guard\":true}\n",
                unsigned(stage), size_t(analysis.bytecodeByteLength), key.toString().c_str());
    return 0;
  } catch (const dxvk::DxvkError& error) {
    std::fprintf(stderr, "candidate_rejected: %.512s\n", error.message().c_str());
    return 3;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "candidate_rejected: %.512s\n", error.what());
    return 3;
  } catch (...) {
    std::fputs("candidate_rejected: unknown native exception\n", stderr);
    return 4;
  }
}
