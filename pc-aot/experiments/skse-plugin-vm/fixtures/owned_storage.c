/* SPDX-License-Identifier: GPL-3.0-only */
#include "owned_storage.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { StorageUID = 0x424510a2, ModOrder = 0x4d4f4453, Integers = 0x494e5456 };
static const char int_value[] = "1 0 1 classicvalue -731";

bool owned_storage_read_initializers(const char *path, uint8_t bytes[8]) {
    FILE *source = fopen(path, "rb");
    if (!source) return false;
    const bool exact = fread(bytes, 1, 8, source) == 8 && fgetc(source) == EOF && !ferror(source);
    fclose(source);
    return exact;
}

static void store_word(uint8_t *out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(value >> (8 * i));
}

/* Offsets follow the pinned Classic DataHandler and ModInfo layout. */
static void write_mod_catalog(uint8_t *image, uint32_t base, bool swapped) {
    enum { List = 0x694, Count = 0x69c, Active = 0x6a0, First = 0xb00, Second = 0xc40, Tail = 0xe00 };
    uint32_t first = base + (swapped ? Second : First), second = base + (swapped ? First : Second);
    store_word(image + List, first);
    store_word(image + List + 4, base + Tail);
    store_word(image + Tail, second);
    store_word(image + Count, 2);
    store_word(image + Active, first);
    store_word(image + Active + 4, second);
    memcpy(image + First + 0x20, "Skyrim.esm", sizeof "Skyrim.esm");
    memcpy(image + Second + 0x20, "OwnedFixture.esp", sizeof "OwnedFixture.esp");
}

void owned_storage_catalog(OwnedStorageCatalog *catalog, bool swapped, SkseX86ReadView views[2]) {
    *catalog = (OwnedStorageCatalog){0};
    const uint32_t base = 0x14000000;
    store_word(catalog->handle, base);
    write_mod_catalog(catalog->mods, base, swapped);
    views[0] = (SkseX86ReadView){0x012e2cb8, catalog->handle, sizeof catalog->handle};
    views[1] = (SkseX86ReadView){base, catalog->mods, sizeof catalog->mods};
}

void owned_storage_probe(const char *dll, const uint8_t initializers[8], bool swapped,
                         SkseTrialRecordProbe *probe) {
    // This owned DLL can spend more than the generic default on formatting
    // its private diagnostics during a single serialization callback.
    if (!probe->callback_work_limit) probe->callback_work_limit = 250000;
    uint8_t words[8];
    memcpy(words, initializers, sizeof words);
    OwnedStorageCatalog catalog;
    SkseX86ReadView views[4] = {{0x0124166c, words, 4}, {0x012b5b80, words + 4, 4}};
    owned_storage_catalog(&catalog, swapped, views + 2);
    const uint32_t sites[] = {0x006a9e90, 0x008cab54, 0x006f02ff};
    const SkseLocalProbeSetup setup = {
        .guest_base = swapped ? 0x12000000 : 0, .read_views = views, .read_view_count = sizeof views / sizeof views[0],
        .capture_papyrus_startup = true, .hook_probe_sites = sites,
        .hook_probe_count = sizeof sites / sizeof sites[0], .record_probe = probe
    };
    SkseLocalDllReport report;
    assert(skse_probe_local_dll_with_setup(dll, 4427, &setup, &report) == SKSE_PROBE_FILE_CHECKED);
    if (probe->status != SKSE_SESSION_OK && probe->status != SKSE_SESSION_UID_MISMATCH) {
        const SkseX86Result *fault = &report.trial.serialization_fault;
        fprintf(stderr, "Co-save replay phase=%u x86=%u ip=%08x address=%08x service=%08x work=%u\n",
                report.trial.serialization_callbacks, fault->status, fault->instruction_va,
                fault->fault_address, fault->failed_service, fault->work);
    }
    assert(report.trial.status == SKSE_TRIAL_ENGINE_HOOKS_OBSERVED);
    if (probe->status == SKSE_SESSION_UID_MISMATCH) assert(!report.trial.serialization_callbacks);
    else assert(probe->status == SKSE_SESSION_OK && report.trial.serialization_callbacks == 3);
}

SksePluginRecordSet *owned_storage_add_integer(const SksePluginRecordSet *catalog) {
    SksePluginRecord authored[2] = {{0}, {Integers, 1, sizeof int_value - 1, int_value}};
    assert(skse_plugin_record_set_at(catalog, 0, authored));
    SksePluginRecordSet *input = NULL;
    assert(skse_plugin_record_set_create(StorageUID, authored, 2, &input) == SKSE_SESSION_OK);
    return input;
}

void owned_storage_expect(const SksePluginRecordSet *set, bool swapped, bool integer) {
    SksePluginRecord saved;
    assert(skse_plugin_record_set_uid(set) == StorageUID);
    assert(skse_plugin_record_set_count(set) == (integer ? 2u : 1u) && skse_plugin_record_set_at(set, 0, &saved));
    const char *a = swapped ? "OwnedFixture.esp" : "Skyrim.esm";
    const char *b = swapped ? "Skyrim.esm" : "OwnedFixture.esp";
    const uint8_t *row = saved.data;
    const size_t first = strlen(a), second = strlen(b);
    assert(saved.type == ModOrder && saved.version == 0 && saved.length == 5 + first + second);
    assert(row[0] == 2 && row[1] == first && row[2] == 0 && !memcmp(row + 3, a, first));
    const uint8_t *next = row + 3 + first;
    assert(next[0] == second && next[1] == 0 && !memcmp(next + 2, b, second));
    if (integer) {
        assert(skse_plugin_record_set_at(set, 1, &saved) && saved.type == Integers && saved.version == 1);
        assert(saved.length == sizeof int_value - 1 && !memcmp(saved.data, int_value, saved.length));
    }
}
