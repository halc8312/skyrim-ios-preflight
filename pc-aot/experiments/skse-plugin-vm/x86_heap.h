/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_HEAP_H
#define SKYRIM_EXPERIMENT_SKSE_X86_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SKSE_X86_HEAP_BYTES = 1024 * 1024, SKSE_X86_HEAP_OBJECTS = 128 };

typedef struct {
    uint32_t offset, size;
} SkseX86HeapBlock;

typedef struct {
    uint32_t base;
    uint8_t *bytes;
    size_t count;
    SkseX86HeapBlock blocks[SKSE_X86_HEAP_OBJECTS];
} SkseX86Heap;

/* The caller reserves a disjoint virtual range and owns the heap for the
 * lifetime of the plugin group. The arena is allocated lazily. */
uint8_t *skse_x86_heap_memory(SkseX86Heap *heap, uint32_t address, size_t length);
uint32_t skse_x86_heap_allocate(SkseX86Heap *heap, uint32_t size, bool clear);
bool skse_x86_heap_release(SkseX86Heap *heap, uint32_t address);
uint32_t skse_x86_heap_resize(SkseX86Heap *heap, uint32_t address, uint32_t size, bool clear, bool in_place);
void skse_x86_heap_close(SkseX86Heap *heap);
#endif
