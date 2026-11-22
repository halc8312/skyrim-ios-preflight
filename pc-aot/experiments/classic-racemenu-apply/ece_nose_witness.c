/* SPDX-License-Identifier: GPL-3.0-only */
#include "ece_nose_witness.h"
#include "ece_head_geometry.h"

enum { MENU_TICK = 0xA5CE2B, MENU_VTABLE = 0x10E7404, NOSE_CALL = 0x885611, HEAD_CALL = 0x5A48D2,
       PLAYER = 0x1B2E8E4, PLAYER_VTABLE = 0x10D201C, NORD = 0x13746, FEMALE_HEAD = 0x51623,
       RUNTIME_TRI_VTABLE = 0x10BE9D4 };

typedef struct {
    EceNoseRead read;
    EceNoseSourceBegin begin;
    EceNoseSourceFinish finish;
    EceNoseReport report;
    void *context;
    EceNoseWitnessReport output;
    uint32_t menu, actor, preview, race, face, geometry, entry, tri;
    bool applying, head, lookup, looked_up, parse_started, morph, invalid;
} EceWitness;

static _Thread_local EceWitness witness;

bool ece_nose_witness_arm(EceNoseRead read, EceNoseSourceBegin begin, EceNoseSourceFinish finish,
                         EceNoseReport report, void *context) {
    if (witness.read || !read || !report || (!!begin != !!finish)) return false;
    witness = (EceWitness){.read = read, .begin = begin, .finish = finish, .report = report, .context = context};
    return true;
}

static bool field(uint32_t object, uint32_t offset, void *out, size_t size) {
    return object >= 0x10000 && object <= UINT32_MAX - offset - (size - 1) &&
           witness.read(witness.context, object + offset, out, size);
}

static uint32_t word(uint32_t object, uint32_t offset) {
    uint8_t b[4];
    if (!field(object, offset, b, sizeof b)) return 0;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint8_t byte(uint32_t object, uint32_t offset) {
    uint8_t b = 0;
    return field(object, offset, &b, sizeof b) ? b : 0;
}

static bool same_player(void) {
    const uint32_t row = word(witness.menu, 0xE4);
    const uint32_t rows = word(witness.menu, 0xCC);
    return witness.actor && word(PLAYER, 0) == witness.actor && word(witness.actor, 0) == PLAYER_VTABLE &&
           row < word(witness.menu, 0xD4) && row <= (UINT32_MAX - rows) / 20 && word(rows, row * 20) == witness.race &&
           word(witness.actor, 0x24) == witness.preview && word(witness.actor, 0x130) == witness.race &&
           byte(witness.preview, 0x12) == 0x2B && (byte(witness.preview, 0x24) & 1) &&
           word(witness.preview, 0xC4) == witness.race &&
           byte(witness.race, 0x12) == 0x0E && word(witness.race, 0x0C) == NORD &&
           word(witness.menu, 0) == MENU_VTABLE && (byte(witness.menu, 0x10) & 0x40) &&
           word(witness.menu, 0xF4) == 1;
}

void ece_nose_witness_disarm(void) {
    if (!witness.menu) witness = (EceWitness){0};
}

bool ece_nose_menu(uint32_t caller, uint32_t menu) {
    if (!witness.read || caller != MENU_TICK || word(menu, 0) != MENU_VTABLE || !(byte(menu, 0x10) & 0x40))
        return false;
    if (witness.menu) {
        witness.invalid = true;
        return false;
    }
    witness.menu = menu;
    return true;
}

void ece_nose_menu_end(bool active) {
    if (!active) return;
    EceWitness finished = witness;
    witness = (EceWitness){0};
    if (!finished.invalid) finished.report(finished.context, finished.output);
}

bool ece_nose_begin(uint32_t menu, uint32_t preview, uint32_t descriptor_race) {
    if (!witness.menu || witness.menu != menu || witness.applying || witness.output.female_nord) return false;
    witness.actor = word(PLAYER, 0);
    witness.preview = preview;
    witness.race = descriptor_race;
    if (!same_player()) return false;
    witness.output.female_nord = witness.applying = true;
    return true;
}

void ece_nose_end(bool active) {
    if (!active) return;
    if (!same_player()) witness.output = (EceNoseWitnessReport){0};
    witness.applying = false;
}

void ece_nose_face(uint32_t caller, uint32_t menu, uint32_t actor, uint32_t base, uint32_t face) {
    if (witness.applying && caller == NOSE_CALL && menu == witness.menu &&
        actor == witness.actor && base == witness.preview && !witness.face && same_player())
        witness.face = face;
}

void ece_nose_parts(uint32_t base, uint32_t face, uint32_t list, uint32_t count) {
    if (!witness.applying || witness.output.layout_checked || base != witness.preview ||
        !face || face != witness.face || !same_player()) return;
    EceHeadLayout layout;
    witness.output.layout_checked = true;
    witness.output.layout_complete = ece_head_layout(witness.read, witness.context, face, list, count, &layout) &&
                                     layout.count != 0;
}

bool ece_nose_head(uint32_t caller, uint32_t face, uint32_t part, uint32_t geometry) {
    if (!witness.applying || witness.head || witness.output.head || caller != HEAD_CALL ||
        !witness.face || face != witness.face || face == geometry ||
        byte(part, 0x12) != 0x0C || word(part, 0x0C) != FEMALE_HEAD || word(part, 0x3C) != 1)
        return false;
    witness.output.head = witness.head = true;
    witness.output.geometry = geometry != 0;
    witness.geometry = geometry;
    return true;
}

void ece_nose_head_end(bool active) {
    if (!active) return;
    witness.head = false;
    witness.entry = witness.tri = witness.geometry = 0;
}

static bool equal_string(uint32_t address, const char *expected, bool path) {
    for (uint32_t i = 0;; ++i) {
        char c;
        if (!field(address, i, &c, 1)) return false;
        if (path && c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (path && c == '/') c = '\\';
        if (c != *expected) return false;
        if (!c) return true;
        ++expected;
    }
}

static bool tri_path(uint32_t path) {
    static const char relative[] = "actors\\character\\character assets\\femaleheadchargen.tri";
    static const char rooted[] = "meshes\\actors\\character\\character assets\\femaleheadchargen.tri";
    return equal_string(path, relative, true) || equal_string(path, rooted, true);
}

bool ece_nose_lookup(uint32_t path) {
    if (!witness.head || !witness.geometry || witness.lookup || witness.looked_up || !tri_path(path))
        return false;
    witness.output.selected_path = witness.lookup = witness.looked_up = true;
    return true;
}

void ece_nose_lookup_end(bool active, uint32_t result) {
    if (!active) return;
    witness.lookup = false;
    if (!result || result != witness.entry) witness.entry = witness.tri = 0;
}

uint64_t ece_nose_parse(const struct CPU *cpu, uint32_t path, uint32_t stream) {
    if (!witness.lookup || witness.invalid || witness.parse_started || !witness.begin || !stream) return 0;
    witness.parse_started = true;
    if (!tri_path(path)) return 0;
    return witness.begin(witness.context, cpu, path, stream);
}

bool ece_nose_parsed(const struct CPU *cpu, uint64_t receipt, bool parsed) {
    if (!receipt || !witness.lookup) return false;
    return witness.finish(witness.context, cpu, receipt) && parsed && !witness.invalid;
}

void ece_nose_constructed(bool fresh, uint32_t entry) {
    if (fresh && witness.lookup) {
        const uint32_t tri = word(entry, 0x2C);
        if (tri && word(tri, 0) == RUNTIME_TRI_VTABLE) {
            witness.entry = entry;
            witness.tri = tri;
        }
    }
}

void ece_nose_retiring(uint32_t entry) {
    if (witness.entry == entry) witness.entry = witness.tri = 0;
}

bool ece_nose_morph(uint32_t tri, uint32_t geometry) {
    if (!witness.head || witness.lookup || witness.morph || !tri || !geometry ||
        geometry != witness.geometry || !witness.output.selected_path)
        return false;
    witness.morph = true;
    witness.output.fresh_tri = tri == witness.tri;
    return true;
}

void ece_nose_morph_record(bool active, uint32_t name, bool matched) {
    if (active && matched &&
        (equal_string(name, "NoseUp", false) || equal_string(name, "NoseDown", false)))
        witness.output.morph_record = true;
}
