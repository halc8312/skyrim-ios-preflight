/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* Opt-in CPU sampling core: portable C, no Apple headers, no allocation.
 *
 * The Apple layer (cpu_sampler.m) suspends one running thread at a time, reads
 * its registers, walks its frame-pointer chain and resumes it. Everything else
 * lives here so it can be tested without a device: bounds-checked unwinding,
 * mapping a code address to (image index, offset), aggregating stacks in a
 * fixed table and formatting one bounded line per report window.
 *
 * Output is numeric only: an anonymous thread ordinal, an image index, and an
 * offset from that image's mach header. No names, paths or addresses of data.
 * The PC-side analyzer turns offsets into names with the symbol table of the
 * same private build (kept outside the repository). */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPU_SAMPLE_MAX_DEPTH 8
#define CPU_SAMPLE_MAX_IMAGES 32
#define CPU_SAMPLE_STACK_CAPACITY 1024      /* power of two */
#define CPU_SAMPLE_UNRESOLVED 255u          /* frame outside every known image */

typedef struct CpuSampleImage {
    uint64_t base;          /* address of the image's mach header */
    uint64_t text_size;     /* size of its __TEXT segment */
    uint8_t uuid[16];       /* LC_UUID, to check the symbol table matches */
} CpuSampleImage;

/* Replace the image table. Not thread safe against a concurrent resolve: only
 * the sampler thread calls both. Returns the number of images kept. */
unsigned cpu_sample_set_images(const CpuSampleImage *images, unsigned count);
unsigned cpu_sample_image_count(void);
const CpuSampleImage *cpu_sample_image(unsigned index);
/* Returns 1, and sets the index and offset outputs, when pc lies inside a known image text segment. */
int cpu_sample_resolve(uint64_t pc, unsigned *index, uint64_t *offset);

/* Walk a standard arm64 frame-pointer chain. out[0] is pc; the rest are return
 * addresses read from the stack. Every frame record read is checked to lie fully
 * inside [stack_lo, stack_hi), to be 16-byte aligned, and to move strictly up the
 * stack, so a corrupt chain ends the walk instead of faulting. Pointer
 * authentication bits are removed. Reads only the suspended thread's stack and
 * allocates nothing. Returns the depth (>= 1 when pc != 0). */
unsigned cpu_sample_unwind(uint64_t pc, uint64_t lr, uint64_t fp,
                           uint64_t stack_lo, uint64_t stack_hi,
                           uint64_t out[CPU_SAMPLE_MAX_DEPTH]);

/* Record one sample. Safe to call only from the sampler thread. */
void cpu_sample_add(unsigned thread_ordinal, int is_main,
                    const uint64_t *frames, unsigned depth);
void cpu_sample_note_tick(void);
void cpu_sample_note_suspend_error(void);
void cpu_sample_note_overrun(void);

/* Format and clear the current window into one line (no trailing newline written
 * beyond the one included). The line lists the most frequent stacks that fit and
 * states how many it left out, so totals always reconcile. Returns the length,
 * or 0 when the window held no ticks. */
size_t cpu_sample_format_window(char *out, size_t capacity, uint64_t end_ns,
                                uint64_t window_ns, unsigned target_hz);
/* One line per image, for the analyzer to verify its symbol table. */
size_t cpu_sample_format_image(char *out, size_t capacity, unsigned index);

#ifdef __cplusplus
}
#endif
