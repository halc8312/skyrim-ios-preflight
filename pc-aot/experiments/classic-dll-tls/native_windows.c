/* SPDX-License-Identifier: GPL-3.0-only */
/* Windows assigns TLS and issues notifications on the genuine x86 loader path. */
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include "trace.h"

struct Module {
    uint32_t (__cdecl *value)(void);
    uint32_t (__cdecl *count)(void);
    uint32_t (__cdecl *set)(uint32_t, uint32_t);
};

struct ThreadCase {
    struct Module *module;
    uint32_t new_value, new_count, before, before_count, returned, after, after_count;
};

static DWORD WINAPI run_case(void *ctx) {
    struct ThreadCase *c = ctx;
    c->before = c->module->value();
    c->before_count = c->module->count();
    c->returned = c->module->set(c->new_value, c->new_count);
    c->after = c->module->value();
    c->after_count = c->module->count();
    return 0;
}

static void print_case(unsigned id, const struct ThreadCase *c) {
    printf("context %u %08" PRIx32 " %" PRIu32 " %08" PRIx32 " %08" PRIx32 " %" PRIu32 "\n",
           id, c->before, c->before_count, c->returned, c->after, c->after_count);
}

int main(void) {
    HMODULE module = LoadLibraryExW(L"OwnedTls.dll", NULL,
                                    LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        fprintf(stderr, "Could not load the authored sibling DLL (%lu)\n", GetLastError());
        return 1;
    }
    struct TlsTestTrace trace = {0};
    struct Module api = {0};
    void (__cdecl *bind)(struct TlsTestTrace *);
    bind = (void *)GetProcAddress(module, "BindTrace");
    api.value = (void *)GetProcAddress(module, "ReadValue");
    api.count = (void *)GetProcAddress(module, "ReadCount");
    api.set = (void *)GetProcAddress(module, "SetPair");
    if (!bind || !api.value || !api.count || !api.set) return 2;
    bind(&trace);

    const uint8_t *base = (const uint8_t *)module;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *pe = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *directory = &pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (pe->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 || directory->Size != sizeof(IMAGE_TLS_DIRECTORY32))
        return 3;
    const IMAGE_TLS_DIRECTORY32 *tls = (const IMAGE_TLS_DIRECTORY32 *)(base + directory->VirtualAddress);
    printf("Windows assigned module slot %" PRIu32 "\n", *(uint32_t *)(uintptr_t)tls->AddressOfIndex);
    if (*(uint32_t *)(uintptr_t)tls->AddressOfIndex == UINT32_MAX ||
        trace.used != 3 || api.value() != TLS_TEST_INITIAL || api.count() != 0) {
        fprintf(stderr, "Windows did not initialize this test module's static TLS\n");
        FreeLibrary(module);
        return 6;
    }

    struct ThreadCase cases[] = {
        {.module = &api, .new_value = 0x11111111, .new_count = 4},
        {.module = &api, .new_value = 0x22222222, .new_count = 8},
        {.module = &api, .new_value = 0x33333333, .new_count = 12},
    };
    run_case(&cases[0]); /* The loading thread. */
    for (unsigned i = 1; i <= 2; ++i) {
        HANDLE thread = CreateThread(NULL, 0, run_case, &cases[i], 0, NULL);
        if (!thread || WaitForSingleObject(thread, 10000) != WAIT_OBJECT_0 || !CloseHandle(thread)) return 4;
    }
    for (unsigned i = 0; i < 3; ++i) print_case(i, &cases[i]);
    printf("loading context after threads %08" PRIx32 " %" PRIu32 "\n", api.value(), api.count());
    if (!FreeLibrary(module) || trace.used > TLS_TEST_EVENTS) return 5;
    for (uint32_t i = 0; i < trace.used; ++i) {
        const struct TlsTestEvent *ev = &trace.events[i];
        printf("notify %" PRIu32 " %" PRIu32 " %08" PRIx32 " %" PRIu32 "\n",
               ev->source, ev->reason, ev->value, ev->count);
    }
    return 0;
}
