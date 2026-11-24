/* SPDX-License-Identifier: GPL-3.0-only */
#include "cpu_sample_core.h"
#include <stdio.h>
#include <string.h>

/* Everything below runs on the sampler thread only. */
static CpuSampleImage images[CPU_SAMPLE_MAX_IMAGES];
static unsigned image_count;

typedef struct Stack {
    uint64_t hash;
    uint32_t count;
    uint32_t offset[CPU_SAMPLE_MAX_DEPTH];
    uint16_t thread;
    uint8_t main_thread, depth, used;
    uint8_t image[CPU_SAMPLE_MAX_DEPTH];
} Stack;
static Stack stacks[CPU_SAMPLE_STACK_CAPACITY];
static unsigned stacks_used;
static struct { unsigned ticks, samples, unresolved_leaf, suspend_errors, overruns, dropped; } window;

unsigned cpu_sample_set_images(const CpuSampleImage *list, unsigned count) {
    image_count = 0;
    for (unsigned i = 0; list && i < count && i < CPU_SAMPLE_MAX_IMAGES; ++i) {
        /* A text size above 32 bits cannot be stored in an offset; drop it
         * rather than report wrapped offsets. */
        if (!list[i].base || !list[i].text_size || list[i].text_size > UINT32_MAX) continue;
        images[image_count++] = list[i];
    }
    return image_count;
}
unsigned cpu_sample_image_count(void) { return image_count; }
const CpuSampleImage *cpu_sample_image(unsigned index) { return index < image_count ? &images[index] : NULL; }

int cpu_sample_resolve(uint64_t pc, unsigned *index, uint64_t *offset) {
    for (unsigned i = 0; i < image_count; ++i) {
        if (pc >= images[i].base && pc - images[i].base < images[i].text_size) {
            *index = i;
            *offset = pc - images[i].base;
            return 1;
        }
    }
    return 0;
}

/* Arm64 user addresses on iOS fit well inside 40 bits; pointer-authentication
 * signatures live above them. Anything that still fails to resolve is counted
 * as unresolved, never guessed. */
static uint64_t strip(uint64_t value) { return value & UINT64_C(0x000000FFFFFFFFFF); }

unsigned cpu_sample_unwind(uint64_t pc, uint64_t lr, uint64_t fp,
                           uint64_t stack_lo, uint64_t stack_hi,
                           uint64_t out[CPU_SAMPLE_MAX_DEPTH]) {
    (void)lr; /* After a call returns lr names this function itself, not its caller. */
    unsigned depth = 0;
    pc = strip(pc);
    if (!pc) return 0;
    out[depth++] = pc;
    uint64_t previous = 0;
    while (depth < CPU_SAMPLE_MAX_DEPTH) {
        /* One 16-byte frame record: [fp] = caller's fp, [fp + 8] = return address. */
        if (fp & 15u || fp < stack_lo || stack_hi < 16 || fp > stack_hi - 16 || fp <= previous) break;
        const uint64_t *record = (const uint64_t *)(uintptr_t)fp;
        const uint64_t next_fp = record[0];
        const uint64_t ret = strip(record[1]);
        if (!ret) break;
        out[depth++] = ret;
        previous = fp;
        if (next_fp <= fp) break;     /* the chain must move up the stack */
        fp = next_fp;
    }
    return depth;
}

static uint64_t fnv(uint64_t h, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) {
        h ^= (value >> (i * 8)) & 0xffu;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

void cpu_sample_note_tick(void) { ++window.ticks; }
void cpu_sample_note_suspend_error(void) { ++window.suspend_errors; }
void cpu_sample_note_overrun(void) { ++window.overruns; }

void cpu_sample_add(unsigned thread_ordinal, int is_main, const uint64_t *frames, unsigned depth) {
    if (!frames || !depth) return;
    if (depth > CPU_SAMPLE_MAX_DEPTH) depth = CPU_SAMPLE_MAX_DEPTH;
    Stack key;
    memset(&key, 0, sizeof key);
    key.thread = thread_ordinal > UINT16_MAX ? UINT16_MAX : (uint16_t)thread_ordinal;
    key.main_thread = is_main ? 1 : 0;
    key.depth = (uint8_t)depth;
    uint64_t hash = fnv(fnv(UINT64_C(14695981039346656037), key.thread), key.main_thread * 256u + depth);
    for (unsigned i = 0; i < depth; ++i) {
        unsigned image;
        uint64_t offset;
        if (cpu_sample_resolve(frames[i], &image, &offset)) {
            key.image[i] = (uint8_t)image;
            key.offset[i] = (uint32_t)offset;
        } else {
            key.image[i] = CPU_SAMPLE_UNRESOLVED;
        }
        hash = fnv(hash, ((uint64_t)key.image[i] << 32) | key.offset[i]);
    }
    ++window.samples;
    if (key.image[0] == CPU_SAMPLE_UNRESOLVED) ++window.unresolved_leaf;
    key.hash = hash;
    for (unsigned probe = 0; probe < CPU_SAMPLE_STACK_CAPACITY; ++probe) {
        Stack *slot = &stacks[(hash + probe) & (CPU_SAMPLE_STACK_CAPACITY - 1)];
        if (!slot->used) {
            *slot = key;
            slot->used = 1;
            slot->count = 1;
            ++stacks_used;
            return;
        }
        if (slot->hash == hash && slot->thread == key.thread && slot->main_thread == key.main_thread &&
            slot->depth == key.depth && !memcmp(slot->image, key.image, sizeof key.image) &&
            !memcmp(slot->offset, key.offset, sizeof key.offset)) {
            ++slot->count;
            return;
        }
    }
    ++window.dropped;   /* table full: counted, never silently lost */
}

size_t cpu_sample_format_window(char *out, size_t capacity, uint64_t end_ns,
                                uint64_t window_ns, unsigned target_hz) {
    if (!out || capacity < 512) return 0;
    if (!window.ticks) return 0;
    /* Reserve the header; stacks are appended most frequent first until full. */
    const unsigned total_stacks = stacks_used;
    unsigned listed = 0, omitted_stacks = 0;
    uint64_t omitted_samples = 0;
    char body[7000];
    size_t used = 0;
    body[0] = 0;
    for (;;) {
        Stack *best = NULL;
        for (unsigned i = 0; i < CPU_SAMPLE_STACK_CAPACITY; ++i)
            if (stacks[i].used && (!best || stacks[i].count > best->count)) best = &stacks[i];
        if (!best) break;
        char piece[256];
        int n = snprintf(piece, sizeof piece, "%s%u:%u:%u:", listed ? ";" : "",
                         best->count, best->thread, best->main_thread);
        for (unsigned f = 0; f < best->depth && n > 0 && (size_t)n < sizeof piece; ++f) {
            int m = snprintf(piece + n, sizeof piece - (size_t)n, "%s%u@%x", f ? "," : "",
                             best->image[f], best->offset[f]);
            if (m < 0 || (size_t)m >= sizeof piece - (size_t)n) { n = -1; break; }
            n += m;
        }
        if (n > 0 && used + (size_t)n < sizeof body) {
            memcpy(body + used, piece, (size_t)n);
            used += (size_t)n;
            body[used] = 0;
            ++listed;
        } else {
            /* No room: this and every remaining stack is accounted for, not listed. */
            for (unsigned i = 0; i < CPU_SAMPLE_STACK_CAPACITY; ++i) {
                if (!stacks[i].used) continue;
                ++omitted_stacks;
                omitted_samples += stacks[i].count;
            }
            break;
        }
        best->used = 0;     /* consumed; the rest of the table is cleared below */
        best->count = 0;
    }
    int length = snprintf(out, capacity,
        "IOS_CPU_SAMPLE schema=1 end_ns=%llu window_ns=%llu target_hz=%u ticks=%u samples=%u "
        "unresolved_leaf=%u suspend_errors=%u overruns=%u dropped=%u stacks_total=%u "
        "omitted_stacks=%u omitted_samples=%llu stacks=%s\n",
        (unsigned long long)end_ns, (unsigned long long)window_ns, target_hz, window.ticks,
        window.samples, window.unresolved_leaf, window.suspend_errors, window.overruns,
        window.dropped, total_stacks, omitted_stacks, (unsigned long long)omitted_samples, body);
    memset(stacks, 0, sizeof stacks);
    stacks_used = 0;
    memset(&window, 0, sizeof window);
    if (length < 0 || (size_t)length >= capacity) return 0;
    return (size_t)length;
}

size_t cpu_sample_format_image(char *out, size_t capacity, unsigned index) {
    if (!out || index >= image_count) return 0;
    char uuid[33];
    for (unsigned i = 0; i < 16; ++i) snprintf(uuid + i * 2, 3, "%02x", images[index].uuid[i]);
    int n = snprintf(out, capacity, "IOS_CPU_IMAGE schema=1 index=%u uuid=%s text_size=%llu\n",
                     index, uuid, (unsigned long long)images[index].text_size);
    return n > 0 && (size_t)n < capacity ? (size_t)n : 0;
}
