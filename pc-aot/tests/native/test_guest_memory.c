#include "guest_memory.h"
#include <stdio.h>
#include <stdint.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    CHECK(gm_range_valid(0, GM_GUEST_SIZE));
    CHECK(gm_range_valid(GM_GUEST_SIZE - 1, 1));
    CHECK(!gm_range_valid(GM_GUEST_SIZE - 1, 2));
    CHECK(!gm_range_valid(GM_GUEST_SIZE, 0));
    CHECK(!gm_range_valid(UINT64_MAX, 1));
    CHECK(!gm_range_valid(1, UINT64_MAX));
    gm_space s = {0}; int error = 0;
    CHECK(gm_reserve(&s, &error)); CHECK(s.page_size > 0);
    CHECK(!gm_reserve(&s, &error)); /* live reservation cannot be overwritten */
    CHECK(!gm_pointer(&s, 0, 1)); CHECK(!gm_commit_page(&s, 1, &error));
    CHECK(!gm_commit_page(&s, GM_GUEST_SIZE, &error));
    uint64_t high = GM_GUEST_SIZE - s.page_size;
    CHECK(gm_commit_page(&s, high, &error)); CHECK(gm_commit_page(&s, high, &error)); CHECK(s.page_count == 1);
    unsigned char *p = gm_pointer(&s, high, s.page_size); CHECK(p);
    CHECK(p[0] == 0 && p[s.page_size - 1] == 0);
    p[0] = 13; p[s.page_size - 1] = 29; CHECK(p[0] == 13 && p[s.page_size - 1] == 29);
    uint32_t offset = 0; CHECK(gm_guest_offset(&s, p + s.page_size - 1, &offset)); CHECK(offset == UINT32_MAX);
    CHECK(!gm_pointer(&s, high, s.page_size + 1)); CHECK(!gm_pointer(&s, high, 0));
    CHECK(!gm_guest_offset(&s, (void *)((uintptr_t)s.base + GM_GUEST_SIZE), &offset));
    CHECK(!gm_guest_offset(&s, (void *)((uintptr_t)s.base - 1), &offset));
    for (size_t i = 0; i < GM_MAX_PAGES - 1; ++i) CHECK(gm_commit_page(&s, i * s.page_size, &error));
    CHECK(s.page_count == GM_MAX_PAGES);
    CHECK(!gm_commit_page(&s, (GM_MAX_PAGES - 1) * s.page_size, &error));
    CHECK(gm_release(&s, &error)); CHECK(!s.base && !s.page_count); CHECK(!gm_pointer(&s, 0, 1)); CHECK(gm_release(&s, &error));
    gm_report report; CHECK(gm_run_probe(&report));
    CHECK(report.reserved_bytes == GM_GUEST_SIZE); CHECK(report.touched_bytes == 3 * report.page_size);
    printf("PASS 4 GiB reservation, three sparse pages (%zu bytes/page), bounds, roundtrip, release\n", report.page_size);
    return 0;
}
