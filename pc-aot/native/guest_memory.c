#include "guest_memory.h"
#include <string.h>
#include <errno.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

int gm_range_valid(uint64_t offset, uint64_t length) {
    return offset < GM_GUEST_SIZE && length <= GM_GUEST_SIZE - offset;
}
static int fail(int *error, int value) { if (error) *error = value; return 0; }
int gm_reserve(gm_space *s, int *error) {
    if (error) *error = 0;
    if (!s || s->base || sizeof(size_t) < 8) return fail(error, EINVAL);
    memset(s, 0, sizeof(*s));
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info); s->page_size = info.dwPageSize;
    s->base = VirtualAlloc(NULL, (SIZE_T)GM_GUEST_SIZE, MEM_RESERVE, PAGE_NOACCESS);
    if (!s->base) return fail(error, (int)GetLastError());
#else
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return fail(error, EINVAL);
    s->page_size = (size_t)page;
    s->base = mmap(NULL, (size_t)GM_GUEST_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (s->base == MAP_FAILED) { s->base = NULL; return fail(error, errno); }
#endif
    return 1;
}
int gm_commit_page(gm_space *s, uint64_t offset, int *error) {
    if (error) *error = 0;
    if (!s || !s->base || !s->page_size || offset % s->page_size || !gm_range_valid(offset, s->page_size)) return fail(error, EINVAL);
    for (size_t i = 0; i < s->page_count; ++i) if (s->pages[i] == offset) return 1;
    if (s->page_count == GM_MAX_PAGES) return fail(error, ENOMEM);
    void *p = (unsigned char *)s->base + (size_t)offset;
#ifdef _WIN32
    if (VirtualAlloc(p, s->page_size, MEM_COMMIT, PAGE_READWRITE) != p) return fail(error, (int)GetLastError());
#else
    if (mprotect(p, s->page_size, PROT_READ | PROT_WRITE)) return fail(error, errno);
#endif
    s->pages[s->page_count++] = offset;
    return 1;
}
void *gm_pointer(gm_space *s, uint64_t offset, size_t length) {
    if (!s || !s->base || !length || !gm_range_valid(offset, length)) return NULL;
    /* Each checked access must fit in one explicitly committed page. */
    for (size_t i = 0; i < s->page_count; ++i)
        if (offset >= s->pages[i] && offset - s->pages[i] < s->page_size && length <= s->page_size - (offset - s->pages[i]))
            return (unsigned char *)s->base + (size_t)offset;
    return NULL;
}
int gm_guest_offset(const gm_space *s, const void *pointer, uint32_t *offset) {
    if (!s || !s->base || !pointer || !offset) return 0;
    uintptr_t base = (uintptr_t)s->base, p = (uintptr_t)pointer;
    if (p < base || (uint64_t)(p - base) >= GM_GUEST_SIZE) return 0;
    *offset = (uint32_t)(p - base); return 1;
}
int gm_release(gm_space *s, int *error) {
    if (error) *error = 0;
    if (!s) return fail(error, EINVAL);
    if (!s->base) return 1;
#ifdef _WIN32
    if (!VirtualFree(s->base, 0, MEM_RELEASE)) return fail(error, (int)GetLastError());
#else
    if (munmap(s->base, (size_t)GM_GUEST_SIZE)) return fail(error, errno);
#endif
    memset(s, 0, sizeof(*s)); return 1;
}
int gm_run_probe(gm_report *r) {
    if (!r) return 0;
    memset(r, 0, sizeof(*r)); gm_space s = {0};
    snprintf(r->stage, sizeof(r->stage), "reserve");
    if (!gm_reserve(&s, &r->os_error)) return 0;
    r->reserved_bytes = GM_GUEST_SIZE; r->page_size = s.page_size;
    uint64_t offsets[3] = { 0, UINT64_C(2147483648), GM_GUEST_SIZE - s.page_size };
    int ok = 1;
    for (size_t i = 0; i < 3; ++i) {
        snprintf(r->stage, sizeof(r->stage), "commit");
        if (!gm_commit_page(&s, offsets[i], &r->os_error)) { ok = 0; break; }
        volatile unsigned char *p = (volatile unsigned char *)gm_pointer(&s, offsets[i], s.page_size);
        snprintf(r->stage, sizeof(r->stage), "touch/roundtrip");
        if (!p) { ok = 0; break; }
        /* Touch only these three OS pages; never walk the reservation. */
        p[0] = (unsigned char)(0x31 + i); p[s.page_size - 1] = (unsigned char)(0x71 + i);
        r->touched_bytes += s.page_size;
        uint32_t guest = 0;
        if (p[0] != (unsigned char)(0x31 + i) || p[s.page_size - 1] != (unsigned char)(0x71 + i) ||
            !gm_guest_offset(&s, (const void *)p, &guest) || guest != offsets[i]) { ok = 0; break; }
    }
    if (ok && (gm_pointer(&s, GM_GUEST_SIZE - 1, 2) || gm_range_valid(UINT64_MAX, 2) || gm_pointer(&s, s.page_size, 1))) {
        snprintf(r->stage, sizeof(r->stage), "bounds"); ok = 0;
    }
    int release_error = 0;
    if (!gm_release(&s, &release_error)) { snprintf(r->stage, sizeof(r->stage), "release"); r->os_error = release_error; return 0; }
    if (ok) snprintf(r->stage, sizeof(r->stage), "passed/released");
    return ok;
}
