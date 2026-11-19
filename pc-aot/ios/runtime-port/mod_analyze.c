/* SPDX-License-Identifier: GPL-3.0-only
 * Header-only inspection of MOD files and classic saves. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "mod_analyze.h"
#include "mod_plugins.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint16_t be16(const unsigned char *p) { return (uint16_t)(p[0] << 8 | p[1]); }

size_t skyrim_dds_level_bytes(unsigned format, unsigned w, unsigned h) {
    if (!w) w = 1;
    if (!h) h = 1;
    size_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    switch (format) {
    case SKYRIM_DDS_BC1: case SKYRIM_DDS_BC4: return bw * bh * 8;
    case SKYRIM_DDS_BC2: case SKYRIM_DDS_BC3: case SKYRIM_DDS_BC5:
    case SKYRIM_DDS_BC6H: case SKYRIM_DDS_BC7: return bw * bh * 16;
    case SKYRIM_DDS_RGBA8: case SKYRIM_DDS_BGRA8: case SKYRIM_DDS_BGRX8: return (size_t)w * h * 4;
    case SKYRIM_DDS_BGR8: return (size_t)w * h * 3;
    default: return 0;
    }
}

int skyrim_dds_info(const unsigned char *b, size_t n, SkyrimDDSInfo *out) {
    memset(out, 0, sizeof *out);
    if (n < 128 || memcmp(b, "DDS ", 4) || le32(b + 4) != 124) return -1;
    out->height = le32(b + 12); out->width = le32(b + 16);
    out->mips = le32(b + 28) ? le32(b + 28) : 1;
    if (!(le32(b + 8) & 0x20000)) out->mips = 1;          /* DDSD_MIPMAPCOUNT absent */
    uint32_t pf_flags = le32(b + 80), bits = le32(b + 88), rmask = le32(b + 92), amask = le32(b + 104);
    uint32_t caps2 = le32(b + 112);
    out->cube_or_volume = (caps2 & 0x200) || (caps2 & 0x200000);
    out->data_offset = 128;
    if (pf_flags & 4) {
        const unsigned char *cc = b + 84;
        if (!memcmp(cc, "DX10", 4)) {
            if (n < 148) return -1;
            out->dx10 = 1; out->data_offset = 148;
            uint32_t dxgi = le32(b + 128), dim = le32(b + 132), misc = le32(b + 136);
            if (dim == 4 || (misc & 4)) out->cube_or_volume = 1;
            switch (dxgi) {
            case 70: case 71: case 72: out->format = SKYRIM_DDS_BC1; break;
            case 73: case 74: case 75: out->format = SKYRIM_DDS_BC2; break;
            case 76: case 77: case 78: out->format = SKYRIM_DDS_BC3; break;
            case 79: case 80: case 81: out->format = SKYRIM_DDS_BC4; break;
            case 82: case 83: case 84: out->format = SKYRIM_DDS_BC5; break;
            case 94: case 95: case 96: out->format = SKYRIM_DDS_BC6H; break;
            case 97: case 98: case 99: out->format = SKYRIM_DDS_BC7; break;
            case 27: case 28: case 29: out->format = SKYRIM_DDS_RGBA8; break;
            case 87: case 90: case 91: out->format = SKYRIM_DDS_BGRA8; break;
            case 88: case 92: case 93: out->format = SKYRIM_DDS_BGRX8; break;
            default: out->format = SKYRIM_DDS_OTHER;
            }
        } else if (!memcmp(cc, "DXT1", 4)) out->format = SKYRIM_DDS_BC1;
        else if (!memcmp(cc, "DXT2", 4) || !memcmp(cc, "DXT3", 4)) out->format = SKYRIM_DDS_BC2;
        else if (!memcmp(cc, "DXT4", 4) || !memcmp(cc, "DXT5", 4)) out->format = SKYRIM_DDS_BC3;
        else if (!memcmp(cc, "ATI1", 4) || !memcmp(cc, "BC4U", 4)) out->format = SKYRIM_DDS_BC4;
        else if (!memcmp(cc, "ATI2", 4) || !memcmp(cc, "BC5U", 4)) out->format = SKYRIM_DDS_BC5;
        else out->format = SKYRIM_DDS_OTHER;
    } else if ((pf_flags & 0x40) && bits == 32) {
        out->format = rmask == 0x00ff0000u ? (amask ? SKYRIM_DDS_BGRA8 : SKYRIM_DDS_BGRX8)
                    : rmask == 0x000000ffu ? SKYRIM_DDS_RGBA8 : SKYRIM_DDS_OTHER;
    } else if ((pf_flags & 0x40) && bits == 24 && rmask == 0x00ff0000u) out->format = SKYRIM_DDS_BGR8;
    else out->format = SKYRIM_DDS_OTHER;
    return 0;
}

long skyrim_nif_stream_version(const unsigned char *b, size_t n) {
    const char *magic = "Gamebryo File Format, Version ";
    size_t m = strlen(magic);
    if (n < m + 20 || memcmp(b, magic, m)) return -1;
    const unsigned char *nl = memchr(b, '\n', n < 128 ? n : 128);
    if (!nl) return -1;
    size_t p = (size_t)(nl - b) + 1;
    if (p + 17 > n || le32(b + p) != 0x14020007u) return -1;   /* 20.2.0.7: Skyrim family */
    /* version, endian byte, user version, block count, user version 2 */
    return (long)le32(b + p + 13);
}

long skyrim_bsa_version(const unsigned char *b, size_t n) {
    if (n < 8 || memcmp(b, "BSA\0", 4)) return -1;
    return (long)le32(b + 4);
}

/* ---- SKSE use in compiled Papyrus (.pex) ------------------------------- */
/* Script names that exist only with SKSE (classic 1.7.x). */
static const char *const k_skse_scripts[] = {"SKSE", "StringUtil", "UI", "UICallback", "Input", "ModEvent",
    "ColorComponent", "ColorForm", "NetImmerse", "ArmorAddon", "HeadPart", "EquipSlot", "TextureSet",
    "SoundDescriptor", "TreeObject", "ActorValueInfo", "Art", "CombatStyle", "DefaultObjectManager", NULL};
/* Common SKSE functions added to game scripts. */
static const char *const k_skse_functions[] = {"GetModCount", "GetModName", "GetModByName", "GetModDependencyCount",
    "GetNumItems", "GetNthForm", "GetNumKeywords", "GetNthKeyword", "GetWornForm", "GetEquippedObject",
    "GetNumParts", "QueueNiNodeUpdate", "GetModelPath", "SetModelPath", "GetIconPath", "GetMessageIconPath",
    "GetWeaponType", "GetBaseDamage", "SetBaseDamage", "GetCritDamage", "GetSlotMask", "SetSlotMask",
    "AddSlotToMask", "GetArmorRating", "SetArmorRating", "GetNumArmorAddons", "GetNthArmorAddon",
    "GetNumEffects", "GetNthEffectMagnitude", "GetNthEffectMagicEffect", "GetEffectiveMagickaCost",
    "GetPerkPoints", "SetPerkPoints", "GetCameraState", "IsKeyPressed", "TapKey", "HoldKey", "ReleaseKey",
    "GetNumKeysPressed", "GetNthKeyPressed", "GetMappedKey", "GetMappedControl", "RegisterForKey",
    "RegisterForControl", "RegisterForMenu", "RegisterForModEvent", "SendModEvent", "RegisterForCameraState",
    "RegisterForCrosshairRef", "RegisterForActorAction", "GetCurrentCrosshairRef", "GetName", "SetName",
    "GetWeight", "SetWeight", "SetGoldValue", "GetLightRadius", "GetResult", "GetWorkbenchKeyword",
    "GetNumIngredients", "GetNthIngredient", "GetCurrentMusicType", "GetHairColor", "GetSkinColor",
    "GetEnchantment", "SetEnchantment", "GetNthSpell", "GetSpellCount", "GetNthHeadPart", "Split",
    "GetINIFloat", "GetINIInt", "GetINIBool", "GetINIString", "SetINIFloat", "SetINIInt", "SetINIBool",
    "SetINIString", "Invoke", "InvokeString", "InvokeInt", "InvokeBool", "InvokeFloat", "OpenCustomMenu",
    "UsingGamepad", "GetVersion", "GetVersionMinor", "GetPluginVersion", "LeftShift", "RightShift",
    "LogicalAnd", "LogicalOr", "LogicalXor", "LogicalNot", "GetLength", "GetNthChar", "Substring", "Find",
    "AsOrd", "AsChar", "IsLetter", "IsDigit", "IsMenuOpen", "IsTextInputEnabled", NULL};
/* Functions provided by the bundled SKSE compatibility layer when enabled
 * (vendor/skydream runtime/skse_compat_values.c). */
static const char *const k_compat_functions[] = {"GetVersion", "GetVersionMinor", "GetVersionBeta",
    "GetVersionRelease", "GetPluginVersion", "LeftShift", "RightShift", "LogicalAnd", "LogicalOr",
    "LogicalXor", "LogicalNot", "Log", "GetLength", "GetNthChar", "IsLetter", "IsDigit", "IsPunctuation",
    "IsPrintable", "AsOrd", "AsChar", "Find", "Substring", "GetType", "IsMenuOpen", "GetBool", "SetBool",
    "InvokeBool", "InvokeBoolA", "GetInt", "SetInt", "InvokeInt", "InvokeIntA", "GetFloat", "SetFloat",
    "InvokeFloat", "InvokeFloatA", "GetString", "SetString", "InvokeString", "InvokeStringA",
    "IsTextInputEnabled", "RegisterForMenu", "UnregisterForMenu", "UnregisterForAllMenus", "RegisterForKey",
    "UnregisterForKey", "UnregisterForAllKeys", "RegisterForModEvent", "UnregisterForModEvent",
    "UnregisterForAllModEvents", "SendModEvent", "GetMappedControl", "UsingGamepad", "GetINIFloat",
    "GetINIInt", "SetINIFloat", NULL};
static int in_list(const char *const *list, const char *s, size_t n) {
    for (int i = 0; list[i]; ++i) if (strlen(list[i]) == n && !strncasecmp(list[i], s, n)) return 1;
    return 0;
}
static void add_missing(SkyrimModReport *r, const char *s, size_t n) {
    char name[64];
    if (n >= sizeof name) return;
    memcpy(name, s, n); name[n] = 0;
    size_t used = strlen(r->skse_missing);
    /* Avoid duplicates: whole-word search in the comma list. */
    for (const char *p = r->skse_missing; (p = strcasestr(p, name)); p += n)
        if ((p == r->skse_missing || p[-1] == ' ') && (p[n] == ',' || !p[n])) return;
    if (used + n + 3 >= sizeof r->skse_missing) return;
    if (used) strcat(r->skse_missing, ", ");
    strcat(r->skse_missing, name);
}
static void scan_pex(const unsigned char *b, size_t n, SkyrimModReport *r) {
    if (n < 18 || b[0] != 0xFA || b[1] != 0x57 || b[2] != 0xC0 || b[3] != 0xDE) return;
    size_t p = 16;                                   /* magic, version, game id, time */
    for (int i = 0; i < 3; ++i) { if (p + 2 > n) return; p += 2 + be16(b + p); }
    if (p + 2 > n) return;
    unsigned count = be16(b + p); p += 2;
    int skse = 0, mcm = 0, has_skse_class = 0;
    size_t starts[4096]; size_t lens[4096]; unsigned kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (p + 2 > n) return;
        size_t len = be16(b + p); p += 2;
        if (p + len > n) return;
        const char *s = (const char *)b + p;
        if (len == 14 && !strncasecmp(s, "SKI_ConfigBase", 14)) mcm = 1;
        if (in_list(k_skse_scripts, s, len)) has_skse_class = 1;
        if (in_list(k_skse_functions, s, len) && kept < 4096) { starts[kept] = p; lens[kept] = len; ++kept; }
        p += len;
    }
    /* A known SKSE function name alone may be the script's own function, so
     * require an SKSE-only script name too, or several distinct functions. */
    if (has_skse_class || kept >= 3) {
        skse = 1;
        for (unsigned i = 0; i < kept; ++i)
            if (!in_list(k_compat_functions, (const char *)b + starts[i], lens[i]))
                add_missing(r, (const char *)b + starts[i], lens[i]);
    }
    r->skse_scripts += (unsigned)skse;
    r->mcm_scripts += (unsigned)mcm;
}

/* ---- walk -------------------------------------------------------------- */
static unsigned char *read_head(const char *path, size_t want, size_t *got) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char *b = malloc(want);
    *got = b ? fread(b, 1, want, f) : 0;
    fclose(f);
    return b;
}
static void inspect(const char *path, const char *name, size_t size, int top_level, SkyrimModReport *r) {
    const char *dot = strrchr(name, '.');
    if (!dot) return;
    size_t got = 0;
    if (!strcasecmp(dot, ".dds")) {
        unsigned char *b = read_head(path, 148, &got);
        SkyrimDDSInfo info;
        ++r->textures;
        if (b && !skyrim_dds_info(b, got, &info)) {
            unsigned w = info.width, h = info.height, m = info.mips;
            if (info.dx10) ++r->dx10_textures;
            if (info.format == SKYRIM_DDS_BC7 || info.format == SKYRIM_DDS_BC6H) ++r->bc7_textures;
            if (w >= 4096 || h >= 4096) ++r->textures_4k;
            if (m <= 1 && (w > 256 || h > 256)) ++r->textures_no_mips;
            if (w > r->max_texture) r->max_texture = w;
            if (h > r->max_texture) r->max_texture = h;
            uint64_t bytes = 0;
            for (unsigned i = 0; i < m && i < 16; ++i) bytes += skyrim_dds_level_bytes(info.format, w >> i, h >> i);
            r->texture_bytes += bytes ? bytes : size;
            if (info.format == SKYRIM_DDS_OTHER || info.format == SKYRIM_DDS_UNKNOWN) ++r->textures_unknown;
        } else ++r->textures_unknown;
        free(b);
    } else if (!strcasecmp(dot, ".nif")) {
        unsigned char *b = read_head(path, 256, &got);
        long v = b ? skyrim_nif_stream_version(b, got) : -1;
        ++r->meshes;
        if (v == 100) ++r->se_meshes; else if (v != 83 && v >= 0) ++r->foreign_meshes;
        free(b);
    } else if (!strcasecmp(dot, ".bsa") && top_level) {
        unsigned char *b = read_head(path, 8, &got);
        long v = b ? skyrim_bsa_version(b, got) : -1;
        ++r->bsas;
        if (v == 105) ++r->se_bsas; else if (v != 104) ++r->foreign_bsas;
        free(b);
    } else if ((!strcasecmp(dot, ".esp") || !strcasecmp(dot, ".esm")) && top_level) {
        unsigned char *b = read_head(path, 24, &got);
        ++r->plugins;
        if (b && got >= 24 && !memcmp(b, "TES4", 4)) {
            if (le16(b + 20) >= 44) ++r->se_plugins;
            if (le32(b + 8) & 0x200) ++r->esl;
        }
        free(b);
    } else if (!strcasecmp(dot, ".esl") && top_level) {
        ++r->esl;
    } else if (!strcasecmp(dot, ".pex") && size <= (8u << 20)) {
        unsigned char *b = read_head(path, size, &got);
        ++r->scripts;
        if (b) scan_pex(b, got, r);
        free(b);
    } else if (!strcasecmp(dot, ".dll") && strcasestr(path, "/skse/")) {
        ++r->skse_dlls;
    }
}
static int walk(const char *dir, int depth, SkyrimModReport *r) {
    if (depth > 32) return 0;
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096]; struct stat st;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path) continue;
        if (lstat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) walk(path, depth + 1, r);
        else if (S_ISREG(st.st_mode)) inspect(path, e->d_name, (size_t)st.st_size, depth == 0, r);
    }
    closedir(d);
    return 0;
}
void skyrim_mod_analyze_add_file(const char *path, const char *destination, SkyrimModReport *report) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return;
    const char *slash = strrchr(destination, '/');
    inspect(path, slash ? slash + 1 : destination, (size_t)st.st_size, slash == NULL, report);
}
void skyrim_mod_analyze_add_dir(const char *dir, int top_level, SkyrimModReport *report) {
    walk(dir, top_level ? 0 : 1, report);
}
int skyrim_mod_analyze(const char *data_dir, SkyrimModReport *report) {
    memset(report, 0, sizeof *report);
    return walk(data_dir, 0, report);
}

/* ---- classic save header ----------------------------------------------- */
int skyrim_save_plugins(const char *path, char **names_out, unsigned *count_out) {
    *names_out = NULL; *count_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char head[17];
    int status = -1;
    if (fread(head, 1, 17, f) == 17 && !memcmp(head, "TESV_SAVEGAME", 13)) {
        uint32_t header_size = le32(head + 13);
        unsigned char *h = header_size < 65536 ? malloc(header_size) : NULL;
        if (h && fread(h, 1, header_size, f) == header_size && header_size >= 8) {
            uint32_t version = le32(h);
            size_t p = 8;                                  /* version, save number */
            int ok = 1;
            for (int i = 0; i < 4 && ok; ++i) {            /* name, level+location, date, race id */
                if (i == 1) { p += 4; }
                if (p + 2 > header_size) { ok = 0; break; }
                p += 2 + le16(h + p);
            }
            p += 2 + 4 + 4 + 8;                             /* sex, exp, level-up exp, filetime */
            if (ok && p + 8 <= header_size && version <= 9) {
                uint32_t w = le32(h + p), hgt = le32(h + p + 4);
                uint64_t shot = (uint64_t)w * hgt * 3;
                if (shot < (64u << 20) && !fseek(f, (long)shot, SEEK_CUR)) {
                    unsigned char pre[6];
                    if (fread(pre, 1, 6, f) == 6) {
                        unsigned count = pre[5];
                        char *names = malloc((size_t)count * 258 + 1), *o = names;
                        status = names ? 0 : -1;
                        for (unsigned i = 0; i < count && !status; ++i) {
                            unsigned char lb[2];
                            if (fread(lb, 1, 2, f) != 2) { status = -1; break; }
                            unsigned len = le16(lb);
                            if (len > 256 || fread(o, 1, len, f) != len) { status = -1; break; }
                            o[len] = 0; o += len + 1;
                        }
                        if (!status) { *names_out = names; *count_out = count; } else free(names);
                    }
                }
            }
        }
        free(h);
    }
    fclose(f);
    return status;
}
