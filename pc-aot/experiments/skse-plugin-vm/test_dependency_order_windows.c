/* SPDX-License-Identifier: GPL-3.0-only */
/* The authored leaf copies its whole attach/detach sequence to the caller at final unload. */
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <windows.h>

static HMODULE fixture(const wchar_t *file) {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return NULL;
    wchar_t *name = wcsrchr(path, L'\\');
    if (!name || wcscpy_s(name + 1, MAX_PATH - (size_t)(name + 1 - path), file)) return NULL;
    return LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

int main(void) {
    HMODULE root = fixture(L"fixture.dll");
    HMODULE leaf = GetModuleHandleW(L"PlanLeaf.dll"); /* No extra loader reference. */
    if (!root || !leaf) return 1;
    void (__cdecl *observe)(uint32_t *) = (void (__cdecl *)(uint32_t *))GetProcAddress(leaf, "PlanObserve");
    if (!observe) return 2;
    uint32_t recorded = 0;
    observe(&recorded);
    if (!FreeLibrary(root) || GetModuleHandleW(L"PlanLeaf.dll")) return 3;
    printf("%u\n", recorded);
    return 0;
}
