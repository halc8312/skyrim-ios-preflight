/* SPDX-License-Identifier: GPL-3.0-only */
#include "highheel_identity.h"
#include "mod_plugins.h"
#include "probe_asset_file.h"

#include <stdlib.h>
#include <string.h>

enum {
    HIGHHEEL_LOCAL_SETTING = 0x800,
    PLUGIN_HEADER_LIMIT = 64 * 1024,
    PROBE_FILE_MISSING = 2,
    PROBE_PATH_MISSING = 3,
    PROBE_TOO_LARGE = 223
};

static const char owner_name[] = "hdtHighHeel.esm";
static const struct {
    const char *path;
    size_t size;
    const char *digest;
} assets[] = {
    {owner_name, 1934, "7c7b6bb6ba4585e9f55a5b0459bb275a2f6d7f37542f3f12239f5b1c32c52c32"},
    {"Scripts/hdtHighHeelEffect.pex", 5875, "01e4816a1ea6c5bcb6a4c3bf62c9cdca6574600ddc441b95341b4942df3bdf03"}
};

static bool name_matches(const uint8_t *name, size_t length, const char *expected) {
    return !skse_classic_mod_name_compare(name, length, (const uint8_t *)expected, strlen(expected));
}

static bool equal_digest(const uint8_t digest[32], const char *expected) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        if (hex[digest[i] >> 4] != expected[2 * i] || hex[digest[i] & 15] != expected[2 * i + 1]) return false;
    }
    return true;
}

HighHeelIdentityResult highheel_identity_check_assets(int data, HighHeelDigest digest) {
    if (data < 0 || !digest) return HIGHHEEL_IDENTITY_UNREADABLE;
    for (size_t i = 0; i < sizeof assets / sizeof assets[0]; ++i) {
        uint8_t *bytes, hash[32];
        size_t size;
        uint32_t error = skse_probe_asset_read_unique(&data, assets[i].path, assets[i].size, &bytes, &size);
        if (error == PROBE_TOO_LARGE || error == PROBE_FILE_MISSING || error == PROBE_PATH_MISSING)
            return HIGHHEEL_IDENTITY_ASSET_DIFFERENT;
        if (error) return HIGHHEEL_IDENTITY_UNREADABLE;
        const bool hashed = size == assets[i].size && digest(bytes, size, hash);
        free(bytes);
        if (size != assets[i].size) return HIGHHEEL_IDENTITY_ASSET_DIFFERENT;
        if (!hashed) return HIGHHEEL_IDENTITY_UNREADABLE;
        if (!equal_digest(hash, assets[i].digest)) return HIGHHEEL_IDENTITY_ASSET_DIFFERENT;
    }
    return HIGHHEEL_IDENTITY_MATCH;
}

static HighHeelIdentityResult verify_later_plugin(int data, const SkseLoadedMod *row) {
    const char *name = (const char *)row->name;
    if (!skyrim_plugin_name_valid(name)) return HIGHHEEL_IDENTITY_UNREADABLE;
    uint8_t *bytes;
    size_t size;
    if (skse_probe_asset_read_prefix_unique(&data, name, PLUGIN_HEADER_LIMIT, &bytes, &size))
        return HIGHHEEL_IDENTITY_UNREADABLE;
    SkyrimPluginHeader header;
    const int status = skyrim_plugin_parse_header(bytes, size, &header);
    free(bytes);
    if (status != SKYRIM_PLUGIN_HEADER_OK) return HIGHHEEL_IDENTITY_UNREADABLE;
    HighHeelIdentityResult result = HIGHHEEL_IDENTITY_MATCH;
    const char *master = header.masters;
    for (unsigned i = 0; i < header.master_count; ++i) {
        size_t length = strlen(master);
        if (name_matches((const uint8_t *)master, length, owner_name)) {
            result = HIGHHEEL_IDENTITY_POSSIBLE_OVERRIDE;
            break;
        }
        if (!skyrim_plugin_name_valid(master)) {
            result = HIGHHEEL_IDENTITY_UNREADABLE;
            break;
        }
        master += length + 1;
    }
    skyrim_plugin_header_free(&header);
    return result;
}

HighHeelIdentityResult highheel_identity_join(int data, const ClassicModCatalog *catalog,
                                              HighHeelDigest digest, uint32_t *setting_id) {
    if (data < 0 || !catalog || !digest || !setting_id) return HIGHHEEL_IDENTITY_UNREADABLE;
    size_t count;
    const SkseLoadedMod *rows = classic_mod_catalog_entries(catalog, &count);
    if (!count || !name_matches(rows[0].name, rows[0].length, "Skyrim.esm"))
        return HIGHHEEL_IDENTITY_UNREADABLE;
    size_t owner = 1;
    while (owner < count && !name_matches(rows[owner].name, rows[owner].length, owner_name)) ++owner;
    if (owner == count) return HIGHHEEL_IDENTITY_NOT_LOADED;

    HighHeelIdentityResult result = highheel_identity_check_assets(data, digest);
    for (size_t i = owner + 1; i < count && result == HIGHHEEL_IDENTITY_MATCH; ++i)
        result = verify_later_plugin(data, &rows[i]);
    if (result == HIGHHEEL_IDENTITY_MATCH) *setting_id = (uint32_t)rows[owner].index << 24 | HIGHHEEL_LOCAL_SETTING;
    return result;
}
