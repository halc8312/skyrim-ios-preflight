/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_heap.h"
#include <stdlib.h>
#include <string.h>

enum { ALIGNMENT = 16, INITIAL_HEAP_OBJECTS = 128 };
#define LAST_PRIVATE_HANDLE UINT32_C(0xe1ffffff)

static uint32_t aligned(uint32_t count) {
    return count ? (count + ALIGNMENT - 1u) & ~(ALIGNMENT - 1u) : ALIGNMENT;
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

uint8_t *skse_x86_heap_memory(SkseX86Heap *heap, uint32_t address, size_t length, bool write) {
    if (!heap || !heap->bytes || address < heap->base || !length) return NULL;
    uint32_t offset = address - heap->base;
    size_t index = at_or_before(heap, offset);
    if (index == heap->count) return NULL;
    SkseX86HeapBlock block = heap->blocks[index];
    uint32_t within = offset - block.offset;
    const uint32_t accessible = block.size ? block.size : 1;
    if (within >= accessible || length > accessible - within || (write && block.owner == SKSE_X86_HEAP_ENVIRONMENT))
        return NULL;
    return heap->bytes + offset;
}

bool skse_x86_heap_read_word_tail(const SkseX86Heap *heap, uint32_t address, uint32_t *word) {
    if (!heap || !heap->bytes || !word || (address & 3) || address < heap->base) return false;
    const uint32_t offset = address - heap->base;
    const size_t index = at_or_before(heap, offset);
    if (index == heap->count) return false;
    const SkseX86HeapBlock block = heap->blocks[index];
    const uint32_t within = offset - block.offset;
    if (block.owner == SKSE_X86_HEAP_ENVIRONMENT || within >= block.size || within + 4 > aligned(block.size))
        return false;
    const uint32_t available = block.size - within;
    if (available >= 4) return false;
    uint32_t value = 0;
    for (uint32_t i = 0; i < available; ++i) value |= (uint32_t)heap->bytes[offset + i] << (8 * i);
    *word = value;
    return true;
}

uint32_t skse_x86_heap_allocate(SkseX86Heap *heap, uint32_t size, bool clear) {
    return skse_x86_heap_allocate_owned(heap, size, clear, SKSE_X86_HEAP_GENERAL);
}

uint32_t skse_x86_heap_allocate_owned(SkseX86Heap *heap, uint32_t size, bool clear, uint32_t owner) {
    if (!heap || !heap->base || size > SKSE_X86_HEAP_BYTES || heap->count == SKSE_X86_HEAP_OBJECTS) return 0;
    if (owner > SKSE_X86_HEAP_ENVIRONMENT && !skse_x86_heap_private_live(heap, owner)) return 0;
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
    if (heap->count == heap->capacity) {
        size_t capacity = heap->capacity ? heap->capacity * 2 : INITIAL_HEAP_OBJECTS;
        if (capacity > SKSE_X86_HEAP_OBJECTS) capacity = SKSE_X86_HEAP_OBJECTS;
        SkseX86HeapBlock *blocks = realloc(heap->blocks, capacity * sizeof *blocks);
        if (!blocks) return 0;
        heap->blocks = blocks;
        heap->capacity = capacity;
    }
    if (index < heap->count)
        memmove(&heap->blocks[index + 1], &heap->blocks[index], (heap->count - index) * sizeof(heap->blocks[0]));
    heap->blocks[index] = (SkseX86HeapBlock){offset, size, owner};
    ++heap->count;
    if (clear) memset(heap->bytes + offset, 0, size ? size : 1);
    return heap->base + offset;
}

uint32_t skse_x86_heap_size(const SkseX86Heap *heap, uint32_t address) {
    return skse_x86_heap_size_owned(heap, address, SKSE_X86_HEAP_GENERAL);
}

uint32_t skse_x86_heap_size_owned(const SkseX86Heap *heap, uint32_t address, uint32_t owner) {
    if (!heap || !heap->base) return UINT32_MAX;
    const size_t index = exact_block(heap, address);
    if (index == heap->count || heap->blocks[index].owner != owner) return UINT32_MAX;
    return heap->blocks[index].size;
}

bool skse_x86_heap_release(SkseX86Heap *heap, uint32_t address) {
    return skse_x86_heap_release_owned(heap, address, SKSE_X86_HEAP_GENERAL);
}

bool skse_x86_heap_release_owned(SkseX86Heap *heap, uint32_t address, uint32_t owner) {
    if (!heap || !heap->base) return false;
    size_t index = exact_block(heap, address);
    if (index == heap->count || heap->blocks[index].owner != owner) return false;
    --heap->count;
    if (index < heap->count)
        memmove(&heap->blocks[index], &heap->blocks[index + 1], (heap->count - index) * sizeof(heap->blocks[0]));
    return true;
}

uint32_t skse_x86_heap_resize(SkseX86Heap *heap, uint32_t address, uint32_t size, bool clear, bool in_place) {
    return skse_x86_heap_resize_owned(heap, address, size, clear, in_place, SKSE_X86_HEAP_GENERAL);
}

uint32_t skse_x86_heap_resize_owned(SkseX86Heap *heap, uint32_t address, uint32_t size,
                                    bool clear, bool in_place, uint32_t owner) {
    if (!heap || !heap->base || size > SKSE_X86_HEAP_BYTES) return 0;
    size_t index = exact_block(heap, address);
    if (index == heap->count || heap->blocks[index].owner != owner) return 0;
    SkseX86HeapBlock block = heap->blocks[index];
    const uint32_t end = index + 1 == heap->count ? SKSE_X86_HEAP_BYTES : heap->blocks[index + 1].offset;
    if (aligned(size) <= end - block.offset) {
        if (clear && size > block.size)
            memset(heap->bytes + block.offset + block.size, 0, size - block.size);
        heap->blocks[index].size = size;
        return address;
    }
    if (in_place) return 0;
    uint32_t replacement = skse_x86_heap_allocate_owned(heap, size, clear, owner);
    if (!replacement) return 0;
    size_t preserved = size < block.size ? size : block.size;
    memcpy(heap->bytes + (replacement - heap->base), heap->bytes + block.offset, preserved);
    skse_x86_heap_release_owned(heap, address, owner);
    return replacement;
}

static size_t private_slot(const SkseX86Heap *heap, uint32_t handle) {
    for (size_t i = 0; i < SKSE_X86_PRIVATE_HEAPS; ++i)
        if (heap->private_heaps[i] == handle) return i;
    return SKSE_X86_PRIVATE_HEAPS;
}

uint32_t skse_x86_heap_create_private(SkseX86Heap *heap) {
    if (!heap || !heap->base || heap->last_private_handle == LAST_PRIVATE_HANDLE) return 0;
    const size_t slot = private_slot(heap, 0);
    if (slot == SKSE_X86_PRIVATE_HEAPS) return 0;
    if (!heap->last_private_handle) heap->last_private_handle = SKSE_X86_PROCESS_HEAP;
    return heap->private_heaps[slot] = ++heap->last_private_handle;
}

bool skse_x86_heap_private_live(const SkseX86Heap *heap, uint32_t handle) {
    return heap && handle > SKSE_X86_PROCESS_HEAP && private_slot(heap, handle) < SKSE_X86_PRIVATE_HEAPS;
}

bool skse_x86_heap_destroy_private(SkseX86Heap *heap, uint32_t handle) {
    if (!heap || handle <= SKSE_X86_PROCESS_HEAP) return false;
    const size_t slot = private_slot(heap, handle);
    if (slot == SKSE_X86_PRIVATE_HEAPS) return false;
    size_t remaining = 0;
    for (size_t i = 0; i < heap->count; ++i)
        if (heap->blocks[i].owner != handle) heap->blocks[remaining++] = heap->blocks[i];
    heap->count = remaining;
    heap->private_heaps[slot] = 0;
    return true;
}

void skse_x86_heap_close(SkseX86Heap *heap) {
    if (!heap) return;
    free(heap->blocks);
    free(heap->bytes);
    memset(heap, 0, sizeof *heap);
}
