#ifndef SKYRIM_GUEST_MEMORY_H
#define SKYRIM_GUEST_MEMORY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define GM_GUEST_SIZE UINT64_C(4294967296)
#define GM_MAX_PAGES 16
typedef struct { void *base; size_t page_size; uint64_t pages[GM_MAX_PAGES]; size_t page_count; } gm_space;
typedef struct { uint64_t reserved_bytes, touched_bytes; size_t page_size; int os_error; char stage[64]; } gm_report;
int gm_range_valid(uint64_t offset, uint64_t length);
/* Caller must zero-initialize space. A live reservation is never overwritten. */
int gm_reserve(gm_space *space, int *os_error);
int gm_commit_page(gm_space *space, uint64_t offset, int *os_error);
void *gm_pointer(gm_space *space, uint64_t offset, size_t length);
int gm_guest_offset(const gm_space *space, const void *pointer, uint32_t *offset);
int gm_release(gm_space *space, int *os_error);
int gm_run_probe(gm_report *report);
#ifdef __cplusplus
}
#endif
#endif
