/* SPDX-License-Identifier: GPL-3.0-only */
#include "classic_pex.h"
#include <assert.h>
#include <stdio.h>

#define TEXT(literal) ((SksePexText){(const uint8_t *)(literal), sizeof(literal) - 1})

static void test_match(SksePexNative row, SksePexStaticCall site) {
    SkseClassicDeclaration declarations[] = {
        {.class_name = "dEmO", .name = "RUN", .result = 3, .count = 2, .is_static = 1,
         .arguments = {2, SKSE_CLASSIC_CATALOG_FORM | SKSE_CLASSIC_CATALOG_FORM_ARRAY | 0x3e}},
        {.class_name = "Other", .name = "Run", .result = 5, .count = 0, .is_static = 1}
    };
    SkseClassicCatalog catalog = {.rows = declarations, .count = 2, .limit = 2};
    const SkseClassicDeclaration *found;
    assert(skse_pex_match(&row, &catalog, &found) == SKSE_PEX_MATCH && found == declarations);
    assert(skse_pex_match_call(&site, &row, 1, &catalog, &found) == SKSE_PEX_MATCH && found == declarations);
    site.count++;
    assert(skse_pex_match_call(&site, &row, 1, &catalog, &found) == SKSE_PEX_DIFFERENT_SIGNATURE && !found);
    site.count--;
    const SksePexNative duplicates[] = {row, row};
    assert(skse_pex_match_call(&site, duplicates, 2, &catalog, &found) == SKSE_PEX_AMBIGUOUS && !found);
    site.script = TEXT("Other");
    assert(skse_pex_match_call(&site, &row, 1, &catalog, &found) == SKSE_PEX_MISSING && !found);
    SksePexNative changed = row;
    changed.script = TEXT("Unclaimed");
    assert(skse_pex_match(&changed, &catalog, &found) == SKSE_PEX_MISSING && !found);
    changed.script = TEXT("Other");
    assert(skse_pex_match(&changed, &catalog, NULL) == SKSE_PEX_DIFFERENT_SIGNATURE);
    changed = row;
    changed.global = false;
    assert(skse_pex_match(&changed, &catalog, NULL) == SKSE_PEX_DIFFERENT_SIGNATURE);
    changed = row;
    changed.count--;
    assert(skse_pex_match(&changed, &catalog, NULL) == SKSE_PEX_DIFFERENT_SIGNATURE);
    changed = row;
    changed.result = TEXT("bool");
    assert(skse_pex_match(&changed, &catalog, NULL) == SKSE_PEX_DIFFERENT_SIGNATURE);
    changed = row;
    changed.arguments[1] = TEXT("Actor");
    assert(skse_pex_match(&changed, &catalog, NULL) == SKSE_PEX_DIFFERENT_SIGNATURE);
    changed.arguments[1] = TEXT("ObjectReference[]");
    assert(skse_pex_match(&changed, &catalog, NULL) == SKSE_PEX_DIFFERENT_SIGNATURE);
    changed.arguments[1] = TEXT("OtherReference[]");
    assert(skse_pex_match(&changed, &catalog, NULL) == SKSE_PEX_UNMODELED_TYPE);
    declarations[1] = declarations[0];
    assert(skse_pex_match(&row, &catalog, NULL) == SKSE_PEX_AMBIGUOUS);
}

int main(int argc, char **argv) {
    const SksePexStatus expected[] = {
        SKSE_PEX_OK, SKSE_PEX_OK, SKSE_PEX_OK, SKSE_PEX_OK, SKSE_PEX_MALFORMED,
        SKSE_PEX_MALFORMED, SKSE_PEX_UNSUPPORTED, SKSE_PEX_MALFORMED, SKSE_PEX_MALFORMED, SKSE_PEX_LIMIT,
        SKSE_PEX_OK
    };
    assert(argc == 1 + (int)(sizeof expected / sizeof expected[0]));
    for (int i = 1; i < argc; ++i) {
        FILE *f = fopen(argv[i], "rb");
        uint8_t bytes[1024];
        assert(f);
        const size_t length = fread(bytes, 1, sizeof bytes, f);
        assert(feof(f) && !ferror(f) && fclose(f) == 0);
        SksePexNative row;
        SksePexStaticCall site;
        SksePexInventory inventory = {.natives = &row, .native_capacity = 1,
                                      .calls = &site, .call_capacity = 1};
        assert(skse_pex_inspect(bytes, length, &inventory) == expected[i - 1]);
        assert(inventory.native_count == (i <= 3 || i == 11 ? 1u : 0u));
        assert(inventory.call_count == (i <= 4 ? 1u : 0u));
        if (i <= 4) {
            assert(site.count == 2 && skse_pex_text_equal(site.script, "Demo") && skse_pex_text_equal(site.name, "Run"));
        }
        if (i == 3) {
            SkseClassicDeclaration entry = {0};
            SkseClassicCatalog catalog = {.rows = &entry};
            assert(skse_pex_text_equal(row.state, "Mode"));
            assert(skse_pex_match(&row, &catalog, NULL) == SKSE_PEX_NAMED_STATE);
        }
        if (i > 2) continue;
        assert(skse_pex_text_equal(row.name, "run") && skse_pex_text_equal(row.script, "demo"));
        assert(skse_pex_text_equal(row.result, "INT") && !row.state.length && row.global);
        assert(row.count == 2 && skse_pex_text_equal(row.arguments[0], "String"));
        assert(skse_pex_text_equal(row.arguments[1], "Actor[]"));
        test_match(row, site);
        for (size_t limit = 0; limit < length; ++limit) {
            inventory.native_count = inventory.call_count = SIZE_MAX;
            assert(skse_pex_inspect(bytes, limit, &inventory) != SKSE_PEX_OK &&
                   !inventory.native_count && !inventory.call_count);
        }
        inventory.native_capacity = 0;
        assert(skse_pex_inspect(bytes, length, &inventory) == SKSE_PEX_LIMIT &&
               !inventory.native_count && !inventory.call_count);
        inventory.native_capacity = 1;
        inventory.call_capacity = 0;
        assert(skse_pex_inspect(bytes, length, &inventory) == SKSE_PEX_LIMIT &&
               !inventory.native_count && !inventory.call_count);
        inventory.call_capacity = 1;
        for (size_t offset = 0; offset < length; ++offset) {
            bytes[offset] ^= 0x80;
            const SksePexStatus status = skse_pex_inspect(bytes, length, &inventory);
            assert(status == SKSE_PEX_OK || (!inventory.native_count && !inventory.call_count));
            bytes[offset] ^= 0x80;
        }
    }
}
