/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_heap.h"
#include <stdlib.h>
#include <string.h>

enum { ALIGNMENT = 16 };

static uint32_t aligned(uint32_t count) {
    return (count + ALIGNMENT - 1u) & ~(ALIGNMENT - 1u);
}

static size_t at_or_before(const SkseX86Heap *heap, uint32_t offset) {
    size_t low = 0, high = heap->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (heap->blocks[middle].offset <= offset) low = middle + 1;
        else high = middle;
    }
    return low ? low - 1 : heap->count;
}

static size_t exact_block(const SkseX86Heap *heap, uint32_t address) {
    if (address < heap->base) return heap->count;
    uint32_t offset = address - heap->base;
    size_t index = at_or_before(heap, offset);
    return index < heap->count && heap->blocks[index].offset == offset ? index : heap->count;
}

uint8_t *skse_x86_heap_memory(SkseX86Heap *heap, uint32_t address, size_t length) {
    if (!heap || !heap->bytes || address < heap->base || !length) return NULL;
    uint32_t offset = address - heap->base;
    size_t index = at_or_before(heap, offset);
    if (index == heap->count) return NULL;
    SkseX86HeapBlock block = heap->blocks[index];
    uint32_t within = offset - block.offset;
    if (within >= block.size || length > block.size - within) return NULL;
    return heap->bytes + offset;
}

uint32_t skse_x86_heap_allocate(SkseX86Heap *heap, uint32_t size, bool clear) {
    if (!heap || !heap->base || size > SKSE_X86_HEAP_BYTES || heap->count == SKSE_X86_HEAP_OBJECTS) return 0;
    if (!size) size = 1;
    uint32_t span = aligned(size), offset = 0;
    size_t index = 0;
    for (; index < heap->count; ++index) {
        SkseX86HeapBlock block = heap->blocks[index];
        if (span <= block.offset - offset) break;
        offset = block.offset + aligned(block.size);
    }
    if (span > SKSE_X86_HEAP_BYTES - offset) return 0;
    if (!heap->bytes) {
        heap->bytes = calloc(1, SKSE_X86_HEAP_BYTES);
        if (!heap->bytes) return 0;
    }
    if (index < heap->count)
        memmove(&heap->blocks[index + 1], &heap->blocks[index], (heap->count - index) * sizeof(heap->blocks[0]));
    heap->blocks[index] = (SkseX86HeapBlock){offset, size};
    ++heap->count;
    if (clear) memset(heap->bytes + offset, 0, size);
    return heap->base + offset;
}

bool skse_x86_heap_release(SkseX86Heap *heap, uint32_t address) {
    if (!heap || !heap->base) return false;
    size_t index = exact_block(heap, address);
    if (index == heap->count) return false;
    --heap->count;
    if (index < heap->count)
        memmove(&heap->blocks[index], &heap->blocks[index + 1], (heap->count - index) * sizeof(heap->blocks[0]));
    return true;
}

uint32_t skse_x86_heap_resize(SkseX86Heap *heap, uint32_t address, uint32_t size, bool clear, bool in_place) {
    if (!heap || !heap->base || size > SKSE_X86_HEAP_BYTES) return 0;
    size_t index = exact_block(heap, address);
    if (index == heap->count) return 0;
    if (!size) size = 1;
    SkseX86HeapBlock block = heap->blocks[index];
    const uint32_t end = index + 1 == heap->count ? SKSE_X86_HEAP_BYTES : heap->blocks[index + 1].offset;
    if (aligned(size) <= end - block.offset) {
        if (clear && size > block.size)
            memset(heap->bytes + block.offset + block.size, 0, size - block.size);
        heap->blocks[index].size = size;
        return address;
    }
    if (in_place) return 0;
    uint32_t replacement = skse_x86_heap_allocate(heap, size, clear);
    if (!replacement) return 0;
    size_t preserved = size < block.size ? size : block.size;
    memcpy(heap->bytes + (replacement - heap->base), heap->bytes + block.offset, preserved);
    skse_x86_heap_release(heap, address);
    return replacement;
}

void skse_x86_heap_close(SkseX86Heap *heap) {
    if (!heap) return;
    free(heap->bytes);
    memset(heap, 0, sizeof *heap);
}
