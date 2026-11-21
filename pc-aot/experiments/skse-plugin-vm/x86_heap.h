/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SKYRIM_EXPERIMENT_SKSE_X86_HEAP_H
#define SKYRIM_EXPERIMENT_SKSE_X86_HEAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SKSE_X86_HEAP_BYTES = 1024 * 1024, SKSE_X86_HEAP_OBJECTS = 1024 };

typedef enum { SKSE_X86_HEAP_GENERAL, SKSE_X86_HEAP_ENVIRONMENT } SkseX86HeapOwner;

typedef struct {
    uint32_t offset, size;
    SkseX86HeapOwner owner;
} SkseX86HeapBlock;

typedef struct {
    uint32_t base;
    uint8_t *bytes;
    size_t count, capacity;
    SkseX86HeapBlock *blocks;
} SkseX86Heap;

/* The caller reserves a disjoint virtual range and owns the heap for the
 * lifetime of the plugin group. The arena and its metadata are allocated lazily. */
uint8_t *skse_x86_heap_memory(SkseX86Heap *heap, uint32_t address, size_t length, bool write);
/* A word-aligned scalar load may start in a live general allocation and
 * reach into its alignment reserve. Only logical bytes are copied; any
 * reserve bytes read as zero. This never grants bulk access or writes. */
bool skse_x86_heap_read_word_tail(const SkseX86Heap *heap, uint32_t address, uint32_t *word);
uint32_t skse_x86_heap_allocate(SkseX86Heap *heap, uint32_t size, bool clear);
uint32_t skse_x86_heap_allocate_owned(SkseX86Heap *heap, uint32_t size, bool clear, SkseX86HeapOwner owner);
/* Exact base of a general allocation. Returns UINT32_MAX when absent. */
uint32_t skse_x86_heap_size(const SkseX86Heap *heap, uint32_t address);
bool skse_x86_heap_release(SkseX86Heap *heap, uint32_t address);
bool skse_x86_heap_release_owned(SkseX86Heap *heap, uint32_t address, SkseX86HeapOwner owner);
uint32_t skse_x86_heap_resize(SkseX86Heap *heap, uint32_t address, uint32_t size, bool clear, bool in_place);
void skse_x86_heap_close(SkseX86Heap *heap);
#endif
