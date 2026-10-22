#include "NativeTraceSDKProbe.h"
// This source has no code in the unsigned Release app. The Debug SDK test uses
// the app's ordinary -O2 -fexceptions -funwind-tables flags; frame-pointer
// preservation is deliberately not added to improve the result.
#if defined(OWNED_HOST_SDK_TRACE_TEST)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/machine.h>
#include <mach/vm_prot.h>
#include <limits.h>

// Apple cctools include/mach-o/ldsyms.h declares this linker-defined symbol
// with this 64-bit type. An explicit declaration avoids requiring that extra
// header in the public cross-compile headers; no implementation is supplied.
extern "C" const struct mach_header_64 _mh_execute_header;

static_assert(sizeof(OHNativeTraceSDKResult) == 32);
namespace {
constexpr uint32_t kMaximumFrames = 32;
constexpr uint32_t kMaximumImages = 4096;
struct SelectedImage { uint32_t index = 0; int64_t slide = 0; };

// The same selector is used by dyld lookup and these compile-time inventory
// regressions. Image index 0 is not assumed to identify the executable.
template<class HeaderAt, class SlideAt>
constexpr bool select_unique_image(uint32_t count, const void *owner,
                                   HeaderAt header_at, SlideAt slide_at,
                                   SelectedImage *out) {
    if (!owner || !out || count == 0 || count > kMaximumImages) return false;
    uint32_t matches = 0;
    SelectedImage selected{};
    for (uint32_t index = 0; index < count; ++index) {
        if (header_at(index) != owner) continue;
        if (++matches != 1) return false;
        selected = {index, slide_at(index)};
    }
    if (matches != 1) return false;
    *out = selected;
    return true;
}
constexpr bool selection_regressions() {
    const int images[3] = {1, 2, 3};
    const int absent = 4;
    SelectedImage selected{};
    const auto headers = [&](uint32_t index) -> const void * { return &images[index]; };
    const auto slides = [](uint32_t index) { return int64_t(index) - 7; };
    if (!select_unique_image(3, &images[2], headers, slides, &selected)
        || selected.index != 2 || selected.slide != -5) return false;
    if (!select_unique_image(3, &images[0], headers, slides, &selected)
        || selected.index != 0 || selected.slide != -7) return false;
    if (select_unique_image(3, &absent, headers, slides, &selected)) return false;
    if (select_unique_image(2, &images[2], [&](uint32_t) -> const void * { return &images[2]; },
                            slides, &selected)) return false;
    if (select_unique_image(0, &images[2], headers, slides, &selected)) return false;
    if (select_unique_image(kMaximumImages + 1, &images[2], headers, slides, &selected)) return false;
    if (select_unique_image(3, nullptr, headers, slides, &selected)) return false;
    return true;
}
static_assert(selection_regressions());
struct Capture {
    void *frames[kMaximumFrames]{};
    uintptr_t parents[2]{};
    volatile uint32_t after_call = 0;
    int count = 0;
};
// These three frames remain live during capture. The stored parent PCs refer
// to instructions after the calls inside the two other hand-authored frames.
__attribute__((noinline)) int inner(Capture *state) {
    state->parents[0] = reinterpret_cast<uintptr_t>(
        __builtin_extract_return_addr(__builtin_return_address(0)));
    state->count = backtrace(state->frames, kMaximumFrames);
    return state->count;
}
__attribute__((noinline)) int middle(Capture *state) {
    state->parents[1] = reinterpret_cast<uintptr_t>(
        __builtin_extract_return_addr(__builtin_return_address(0)));
    const int count = inner(state);
    state->after_call = state->after_call + 3;
    return count;
}
__attribute__((noinline)) int outer(Capture *state) {
    const int count = middle(state);
    state->after_call = state->after_call + 7;
    return count;
}

bool slide_address(uint64_t address, int64_t slide, uint64_t *out) {
    if (slide >= 0) {
        const uint64_t positive = static_cast<uint64_t>(slide);
        if (address > UINT64_MAX - positive) return false;
        *out = address + positive;
    } else {
        const uint64_t magnitude = static_cast<uint64_t>(-(slide + 1)) + 1;
        if (address < magnitude) return false;
        *out = address - magnitude;
    }
    return true;
}

// Only the main executable's file-backed, initially executable/nonwritable
// segment bytes count. A test-bundle/system-image return PC cannot satisfy it.
bool main_app_rx(const mach_header_64 *header, int64_t slide, uintptr_t pc) {
    if (!header || header->magic != MH_MAGIC_64 || header->filetype != MH_EXECUTE
        || header->cputype != CPU_TYPE_ARM64 || header->cpusubtype != 0
        || header->ncmds > 1024 || header->sizeofcmds > 65536 || (pc & 3)) return false;
    const auto *commands = reinterpret_cast<const unsigned char *>(header) + sizeof(*header);
    size_t cursor = 0;
    bool found = false;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (cursor > header->sizeofcmds || header->sizeofcmds - cursor < sizeof(load_command)) return false;
        const auto *command = reinterpret_cast<const load_command *>(commands + cursor);
        if (command->cmdsize < sizeof(load_command) || (command->cmdsize & 7)
            || command->cmdsize > header->sizeofcmds - cursor) return false;
        if (command->cmd == LC_SEGMENT_64) {
            if (command->cmdsize < sizeof(segment_command_64)) return false;
            const auto *segment = reinterpret_cast<const segment_command_64 *>(command);
            if (segment->nsects > (command->cmdsize - sizeof(*segment)) / sizeof(section_64)
                || command->cmdsize != sizeof(*segment) + segment->nsects * sizeof(section_64)) return false;
            const uint64_t file_bytes = segment->filesize < segment->vmsize ? segment->filesize : segment->vmsize;
            uint64_t begin = 0;
            if (!slide_address(segment->vmaddr, slide, &begin) || file_bytes > UINT64_MAX - begin) return false;
            if ((segment->initprot & (VM_PROT_READ | VM_PROT_EXECUTE)) == (VM_PROT_READ | VM_PROT_EXECUTE)
                && !(segment->initprot & VM_PROT_WRITE)
                && file_bytes != 0 && static_cast<uint64_t>(pc) >= begin
                && static_cast<uint64_t>(pc) < begin + file_bytes) found = true;
        }
        cursor += command->cmdsize;
    }
    return cursor == header->sizeofcmds && found;
}

// Use a private, internal-linkage body as the dladdr anchor. An external API
// address can resolve through an interposed/stub symbol. The executable's
// linker-defined header independently supplies the required main-image identity.
// Return only numeric stage codes; no path, symbol or process PC is emitted.
int32_t locate_main_probe_image(const mach_header_64 **out_header, int64_t *out_slide) {
    Dl_info owner{};
    if (!dladdr(reinterpret_cast<const void *>(&outer), &owner) || !owner.dli_fbase) return -3;
    const auto *header = reinterpret_cast<const mach_header_64 *>(&_mh_execute_header);
    if (owner.dli_fbase != header) return -4;
    if (header->magic != MH_MAGIC_64 || header->filetype != MH_EXECUTE
        || header->cputype != CPU_TYPE_ARM64 || header->cpusubtype != 0) return -5;
    const uint32_t count = _dyld_image_count();
    if (count == 0 || count > kMaximumImages) return -6;
    SelectedImage selected{};
    if (!select_unique_image(count, header,
            [](uint32_t index) -> const void * { return _dyld_get_image_header(index); },
            [](uint32_t index) { return static_cast<int64_t>(_dyld_get_image_vmaddr_slide(index)); },
            &selected)) return -7;
    // Enumeration is not an atomic dyld snapshot. Reject a changing inventory
    // or a mismatched header/slide pair rather than accepting a guessed owner.
    if (_dyld_image_count() != count
        || static_cast<const void *>(_dyld_get_image_header(selected.index)) != static_cast<const void *>(header)
        || static_cast<int64_t>(_dyld_get_image_vmaddr_slide(selected.index)) != selected.slide)
        return -8;
    if (!main_app_rx(header, selected.slide, reinterpret_cast<uintptr_t>(&outer))) return -9;
    *out_header = header;
    *out_slide = selected.slide;
    return 0;
}

bool parent_in_main_image(const mach_header_64 *header, int64_t slide, uintptr_t pc) {
    Dl_info owner{};
    return dladdr(reinterpret_cast<const void *>(pc), &owner)
        && owner.dli_fbase == header && main_app_rx(header, slide, pc);
}
}

extern "C" int32_t OHTestNativeTraceSDKV1(OHNativeTraceSDKResult *out, size_t capacity) {
    if (!out || capacity < sizeof(*out)
        || reinterpret_cast<uintptr_t>(out) % alignof(OHNativeTraceSDKResult)) return -1;
    OHNativeTraceSDKResult result{1, sizeof(result), 0, 2, 0, 0, 0, 0};
#if defined(__APPLE__) && defined(__aarch64__) && !defined(__arm64e__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST
    result.flags |= 1;
    const mach_header_64 *header = nullptr;
    int64_t slide = 0;
    const int32_t owner_status = locate_main_probe_image(&header, &slide);
    if (owner_status == 0) result.flags |= 2;
    Capture state;
    const int count = outer(&state);
    if (count > 0 && count <= static_cast<int>(kMaximumFrames)) {
        result.captured_count = static_cast<uint32_t>(count);
        if (state.parents[0] && state.parents[1] && state.parents[0] != state.parents[1]
            && state.after_call == 10) {
            for (uintptr_t expected : state.parents) {
                if ((result.flags & 2) && parent_in_main_image(header, slide, expected)) ++result.rx_parent_count;
                for (int i = 0; i < count; ++i) {
                    const uintptr_t actual = reinterpret_cast<uintptr_t>(
                        __builtin_extract_return_addr(state.frames[i]));
                    if (actual == expected) { ++result.matched_parent_count; break; }
                }
            }
        }
    }
    if (result.rx_parent_count == 2) result.flags |= 4;
    if (result.matched_parent_count == 2) result.flags |= 8;
    if (owner_status != 0) { *out = result; return owner_status; }
#endif
    *out = result;
    return result.flags == 15 && result.captured_count >= 3 ? 0 : -2;
}
#endif
