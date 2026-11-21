/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "probe_local_file.h"

/* All mod names and record values belong to this authored fixture. The
 * launcher separately verifies the pinned LE executable before supplying
 * its two read-only initializer words. The DLL is the owner's original. */
typedef struct {
    uint8_t handle[4], mods[0x1000];
} OwnedStorageCatalog;

void owned_storage_catalog(OwnedStorageCatalog *, bool swapped, SkseX86ReadView views[2]);
bool owned_storage_read_initializers(const char *path, uint8_t bytes[8]);
void owned_storage_probe(const char *dll, const uint8_t initializers[8], bool swapped,
                         SkseTrialRecordProbe *probe);
SksePluginRecordSet *owned_storage_add_integer(const SksePluginRecordSet *catalog);
void owned_storage_expect(const SksePluginRecordSet *set, bool swapped, bool integer);
