/* Authored TES4 headers only; no game files. */
#include "mod_plugins.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { unsigned char b[300000]; size_t n; } Buf;
static void put(Buf *b, const void *p, size_t n) { assert(b->n + n <= sizeof b->b); memcpy(b->b + b->n, p, n); b->n += n; }
static void u16(Buf *b, unsigned v) { unsigned char x[2] = {v & 255, v >> 8 & 255}; put(b, x, 2); }
static void u32(Buf *b, unsigned v) { unsigned char x[4] = {v & 255, v >> 8 & 255, v >> 16 & 255, v >> 24 & 255}; put(b, x, 4); }
static void sub(Buf *b, const char *type, const void *data, size_t n) {
    if (n > 65535) { put(b, "XXXX", 4); u16(b, 4); u32(b, (unsigned)n); put(b, type, 4); u16(b, 0); }
    else { put(b, type, 4); u16(b, (unsigned)n); }
    put(b, data, n);
}
/* masters: NULL-terminated list. onam: bytes of a large ONAM subrecord. */
static Buf *plugin(unsigned flags, const char *const *masters, size_t onam) {
    static Buf body; static Buf out;
    body.n = out.n = 0;
    unsigned char hedr[12] = {0};
    sub(&body, "HEDR", hedr, sizeof hedr);
    sub(&body, "CNAM", "author", 7);
    for (; masters && *masters; ++masters) {
        sub(&body, "MAST", *masters, strlen(*masters) + 1);
        unsigned char data[8] = {0};
        sub(&body, "DATA", data, 8);
    }
    if (onam) { static unsigned char big[200000]; assert(onam <= sizeof big); sub(&body, "ONAM", big, onam); }
    put(&out, "TES4", 4); u32(&out, (unsigned)body.n); u32(&out, flags); u32(&out, 0); u32(&out, 0);
    u16(&out, 43); u16(&out, 0);
    put(&out, body.b, body.n);
    return &out;
}

static SkyrimPluginHeader headers[300];
static SkyrimPluginEntry entries[300];
static size_t entry_count;
static void add(const char *name, int enabled, unsigned flags, const char *const *masters) {
    Buf *b = plugin(flags, masters, 0);
    SkyrimPluginEntry *e = &entries[entry_count];
    e->name = name; e->enabled = enabled;
    e->header_status = skyrim_plugin_parse_header(b->b, b->n, &headers[entry_count]);
    e->header = &headers[entry_count];
    ++entry_count;
}
static void reset(void) {
    for (size_t i = 0; i < entry_count; ++i) skyrim_plugin_header_free(&headers[i]);
    entry_count = 0;
}
static size_t order[300]; static SkyrimPluginState state[300]; static int problem[300];
static size_t run(void) { return skyrim_plugin_order(entries, entry_count, order, state, problem); }
static const char *loaded_names(size_t n) {
    static char text[8192]; text[0] = 0;
    for (size_t k = 0; k < n; ++k) { strcat(text, entries[order[k]].name); strcat(text, k + 1 < n ? "," : ""); }
    return text;
}
#define EXPECT_ORDER(n, s) do { const char *got = loaded_names(n); if (strcmp(got, s)) { fprintf(stderr, "order %s != %s\n", got, s); abort(); } } while (0)

static void parsing(void) {
    SkyrimPluginHeader h;
    const char *masters[] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", NULL};
    Buf *b = plugin(1, masters, 0);
    assert(skyrim_plugin_header_size(b->b, 8) == b->n);
    assert(skyrim_plugin_parse_header(b->b, b->n, &h) == SKYRIM_PLUGIN_HEADER_OK);
    assert(h.flags == 1 && h.version == 43 && h.master_count == 3);
    assert(!strcmp(skyrim_plugin_master(&h, 0), "Skyrim.esm"));
    assert(!strcmp(skyrim_plugin_master(&h, 2), "Dawnguard.esm"));
    assert(!skyrim_plugin_master(&h, 3));
    skyrim_plugin_header_free(&h);
    assert(skyrim_plugin_parse_header(b->b, b->n - 1, &h) == SKYRIM_PLUGIN_HEADER_TRUNCATED);
    assert(!h.masters && !h.master_count);

    /* Large ONAM uses XXXX; masters before and after it are both kept. */
    b = plugin(1, masters, 70000);
    assert(skyrim_plugin_parse_header(b->b, b->n, &h) == SKYRIM_PLUGIN_HEADER_OK && h.master_count == 3);
    skyrim_plugin_header_free(&h);

    b = plugin(0x00040000u, masters, 0);
    assert(skyrim_plugin_parse_header(b->b, b->n, &h) == SKYRIM_PLUGIN_HEADER_COMPRESSED);
    b = plugin(0, masters, 0); memcpy(b->b, "TES3", 4);
    assert(skyrim_plugin_parse_header(b->b, b->n, &h) == SKYRIM_PLUGIN_HEADER_NOT_TES4);
    assert(!skyrim_plugin_header_size(b->b, b->n));

    /* Unterminated master name; subrecord past the end; dangling XXXX. */
    Buf bad = {.n = 0};
    put(&bad, "TES4", 4); u32(&bad, 10); u32(&bad, 0); u32(&bad, 0); u32(&bad, 0); u16(&bad, 43); u16(&bad, 0);
    put(&bad, "MAST", 4); u16(&bad, 4); put(&bad, "abcd", 4);
    assert(skyrim_plugin_parse_header(bad.b, bad.n, &h) == SKYRIM_PLUGIN_HEADER_BAD_MASTER);
    bad.n = 0;
    put(&bad, "TES4", 4); u32(&bad, 10); u32(&bad, 0); u32(&bad, 0); u32(&bad, 0); u16(&bad, 43); u16(&bad, 0);
    put(&bad, "CNAM", 4); u16(&bad, 9); put(&bad, "abcd", 4);
    assert(skyrim_plugin_parse_header(bad.b, bad.n, &h) == SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD);
    bad.n = 0;
    put(&bad, "TES4", 4); u32(&bad, 10); u32(&bad, 0); u32(&bad, 0); u32(&bad, 0); u16(&bad, 43); u16(&bad, 0);
    put(&bad, "XXXX", 4); u16(&bad, 4); u32(&bad, 5);
    assert(skyrim_plugin_parse_header(bad.b, bad.n, &h) == SKYRIM_PLUGIN_HEADER_BAD_SUBRECORD);
    /* Declared size beyond the cap is refused before allocation. */
    bad.n = 0; put(&bad, "TES4", 4); u32(&bad, 0x7fffffffu);
    assert(!skyrim_plugin_header_size(bad.b, bad.n));
    assert(skyrim_plugin_parse_header(bad.b, bad.n, &h) == SKYRIM_PLUGIN_HEADER_TOO_LARGE);
    puts("header parsing PASS");
}

static void names(void) {
    assert(skyrim_plugin_name_valid("Unofficial Skyrim Patch.esp"));
    assert(skyrim_plugin_name_valid("a.ESM"));
    assert(!skyrim_plugin_name_valid("a.esl") && !skyrim_plugin_name_valid(".esp"));
    assert(!skyrim_plugin_name_valid("dir/a.esp") && !skyrim_plugin_name_valid("dir\\a.esp"));
    assert(!skyrim_plugin_name_valid("\xe6\x97\xa5\xe6\x9c\xac.esp"));   /* non-ASCII */
    assert(!skyrim_plugin_name_valid("#a.esp") && !skyrim_plugin_name_valid("a .esp"));
    assert(skyrim_plugin_is_base("SKYRIM.ESM") && skyrim_plugin_is_base("update.esm") && !skyrim_plugin_is_base("Dawnguard.esm"));
    assert(skyrim_plugin_default_rank("dawnguard.esm", 1) < skyrim_plugin_default_rank("Dragonborn.esm", 1));
    assert(skyrim_plugin_default_rank("Dragonborn.esm", 1) < skyrim_plugin_default_rank("Other.esm", 1));
    assert(skyrim_plugin_default_rank("Other.esm", 1) < skyrim_plugin_default_rank("Other.esp", 0));
    puts("names PASS");
}

static void ordering(void) {
    const char *base[] = {"Skyrim.esm", "Update.esm", NULL};
    const char *needs_master[] = {"Skyrim.esm", "Master.esm", NULL};
    const char *needs_patch[] = {"Patch.esp", NULL};
    const char *needs_a[] = {"A.esp", NULL};
    const char *needs_b[] = {"B.esp", NULL};
    const char *needs_c[] = {"c.ESP", NULL};

    /* User put an ESP and a dependent above their masters: masters move up,
     * independent user order is kept. Master names match case-insensitively. */
    reset();
    add("Patch.esp", 1, 0, needs_master);
    add("Second.esp", 1, 0, base);
    add("Master.esm", 1, 1, base);
    add("Child.esp", 1, 0, needs_patch);
    size_t n = run();
    assert(n == 4); EXPECT_ORDER(n, "Master.esm,Patch.esp,Second.esp,Child.esp");
    reset();
    add("Child.esp", 1, 0, needs_patch);
    add("patch.ESP", 1, 0, base);
    n = run(); EXPECT_ORDER(n, "patch.ESP,Child.esp");

    /* Disabled or absent master blocks the dependent and its dependents. */
    reset();
    add("Master.esm", 0, 1, base);
    add("Patch.esp", 1, 0, needs_master);
    add("Child.esp", 1, 0, needs_patch);
    add("Free.esp", 1, 0, base);
    n = run();
    EXPECT_ORDER(n, "Free.esp");
    assert(state[0] == SKYRIM_PLUGIN_OFF && state[1] == SKYRIM_PLUGIN_MISSING_MASTER && problem[1] == 1);
    assert(state[2] == SKYRIM_PLUGIN_MISSING_MASTER && problem[2] == 0);

    /* An ESM cannot use an ESP master: ESM-flag files always load first. */
    reset();
    add("A.esp", 1, 0, base);
    add("Bad.esm", 1, 1, needs_a);
    n = run(); EXPECT_ORDER(n, "A.esp");
    assert(state[1] == SKYRIM_PLUGIN_MASTER_AFTER);

    /* Cycle A <-> B is refused, their dependent too; unrelated plugins load. */
    reset();
    add("A.esp", 1, 0, needs_b);
    add("B.esp", 1, 0, needs_a);
    add("C.esp", 1, 0, base);
    add("D.esp", 1, 0, needs_c);
    add("E.esp", 1, 0, needs_a);
    n = run(); EXPECT_ORDER(n, "C.esp,D.esp");
    assert(state[0] == SKYRIM_PLUGIN_CYCLE && state[1] == SKYRIM_PLUGIN_CYCLE);
    assert(state[4] == SKYRIM_PLUGIN_CYCLE || state[4] == SKYRIM_PLUGIN_MISSING_MASTER);

    /* Self reference, unreadable header, base names, invalid names, duplicates. */
    reset();
    const char *self[] = {"Self.esp", NULL};
    add("Self.esp", 1, 0, self);
    add("Skyrim.esm", 1, 1, NULL);
    add("Dup.esp", 1, 0, base);
    add("dup.ESP", 0, 0, base);
    add("ok.esp", 1, 0, base);
    entries[entry_count] = (SkyrimPluginEntry){"Broken.esp", 1, SKYRIM_PLUGIN_HEADER_TRUNCATED, NULL}; ++entry_count;
    entries[entry_count] = (SkyrimPluginEntry){"\xe6\x97\xa5.esp", 1, SKYRIM_PLUGIN_HEADER_OK, &headers[4]}; ++entry_count;
    n = run(); EXPECT_ORDER(n, "ok.esp");
    assert(state[0] == SKYRIM_PLUGIN_MISSING_MASTER && state[1] == SKYRIM_PLUGIN_BAD_NAME);
    assert(state[2] == SKYRIM_PLUGIN_DUPLICATE && state[3] == SKYRIM_PLUGIN_OFF);
    assert(state[5] == SKYRIM_PLUGIN_UNREADABLE && state[6] == SKYRIM_PLUGIN_BAD_NAME);
    entry_count = 5; /* the two literal entries reuse headers; avoid double free */

    /* Slot limit: 253 loaded besides Skyrim.esm / Update.esm. */
    reset();
    static char names[260][32];
    for (int i = 0; i < 260; ++i) { snprintf(names[i], sizeof names[i], "P%03d.esp", i); add(names[i], 1, 0, base); }
    n = run();
    assert(n == 253 && state[252] == SKYRIM_PLUGIN_LOAD && state[253] == SKYRIM_PLUGIN_LIMIT && state[259] == SKYRIM_PLUGIN_LIMIT);
    reset();
    puts("ordering PASS");
}

/* USLEEP 3.0.16 is an ESP whose masters are the base game and the three official DLC. */
static void usleep_dlc_masters(void) {
    const char *usleep[] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm", NULL};
    const char *shouting[] = {"SKYRIM.ESM", "update.esm", "DAWNGUARD.ESM", "hearthfires.esm", "DRAGONBORN.ESM", NULL};
    const char *base[] = {"Skyrim.esm", NULL};
    const char *needs_usleep[] = {"Unofficial Skyrim Legendary Edition Patch.esp", NULL};
    size_t n;

    /* All three DLC enabled, listed after the patch: the DLC move above it. */
    reset();
    add("Unofficial Skyrim Legendary Edition Patch.esp", 1, 0, usleep);
    add("Dragonborn.esm", 1, 1, base);
    add("Dawnguard.esm", 1, 1, base);
    add("HearthFires.esm", 1, 1, base);
    add("Dependent.esp", 1, 0, needs_usleep);
    n = run();
    EXPECT_ORDER(n, "Dragonborn.esm,Dawnguard.esm,HearthFires.esm,Unofficial Skyrim Legendary Edition Patch.esp,Dependent.esp");

    /* Master names match without regard to case. */
    reset();
    add("Unofficial Skyrim Legendary Edition Patch.esp", 1, 0, shouting);
    add("Dragonborn.esm", 1, 1, base);
    add("Dawnguard.esm", 1, 1, base);
    add("HearthFires.esm", 1, 1, base);
    n = run(); assert(n == 4 && state[0] == SKYRIM_PLUGIN_LOAD);

    /* One DLC switched off blocks the patch and its dependent; the others load. */
    reset();
    add("Dawnguard.esm", 1, 1, base);
    add("HearthFires.esm", 1, 1, base);
    add("Dragonborn.esm", 0, 1, base);
    add("Unofficial Skyrim Legendary Edition Patch.esp", 1, 0, usleep);
    add("Dependent.esp", 1, 0, needs_usleep);
    n = run();
    EXPECT_ORDER(n, "Dawnguard.esm,HearthFires.esm");
    assert(state[2] == SKYRIM_PLUGIN_OFF && state[3] == SKYRIM_PLUGIN_MISSING_MASTER && problem[3] == 4);
    assert(state[4] == SKYRIM_PLUGIN_MISSING_MASTER && problem[4] == 0);

    /* A DLC that is not installed at all is also a missing master. */
    reset();
    add("Dawnguard.esm", 1, 1, base);
    add("Dragonborn.esm", 1, 1, base);
    add("Unofficial Skyrim Legendary Edition Patch.esp", 1, 0, usleep);
    n = run();
    EXPECT_ORDER(n, "Dawnguard.esm,Dragonborn.esm");
    assert(state[2] == SKYRIM_PLUGIN_MISSING_MASTER && problem[2] == 3);
    reset();
    puts("usleep dlc masters PASS");
}

static void list_text(void) {
    const char *base[] = {"Skyrim.esm", NULL};
    reset();
    add("B.esp", 1, 0, base);
    add("Dawnguard.esm", 1, 1, base);
    size_t n = run(), bytes = 0;
    char *text = skyrim_plugin_list_text(entries, order, n, &bytes);
    const char *expect = "# Managed by SkyrimIOS Settings > MOD. Changes here are replaced at launch.\r\n"
        "Skyrim.esm\r\nUpdate.esm\r\nDawnguard.esm\r\nB.esp\r\n";
    assert(text && bytes == strlen(expect) && !memcmp(text, expect, bytes));
    assert(skyrim_plugin_list_is_managed(text, bytes));
    assert(!skyrim_plugin_list_is_managed("Skyrim.esm\r\n", 12));
    free(text);
    text = skyrim_plugin_list_text(entries, order, 0, &bytes);
    assert(text && strstr(text, "Update.esm\r\n") && !strstr(text, "B.esp"));
    free(text);
    reset();
    puts("plugins.txt PASS");
}

static void touch(const char *dir, const char *name) {
    char path[2048]; snprintf(path, sizeof path, "%s/%s", dir, name);
    int fd = open(path, O_CREAT | O_WRONLY, 0600); assert(fd >= 0); close(fd);
}
static void paths(const char *root) {
    char out[2048], dir[512];
    assert(strlen(root) < 400);
    snprintf(dir, sizeof dir, "%s/paths", root); mkdir(dir, 0700);
    assert(skyrim_plugin_child_path(dir, "plugins.txt", out, sizeof out) == 0);
    assert(!strcmp(strrchr(out, '/') + 1, "plugins.txt"));
    touch(dir, "Plugins.TXT");
    assert(skyrim_plugin_child_path(dir, "plugins.txt", out, sizeof out) == 1);
    assert(!strcmp(strrchr(out, '/') + 1, "Plugins.TXT"));
    touch(dir, "plugins.txt");
    struct stat a, b; char p1[2048], p2[2048];
    snprintf(p1, sizeof p1, "%s/Plugins.TXT", dir); snprintf(p2, sizeof p2, "%s/plugins.txt", dir);
    if (!stat(p1, &a) && !stat(p2, &b) && a.st_ino != b.st_ino)   /* case-sensitive volume */
        assert(skyrim_plugin_child_path(dir, "plugins.txt", out, sizeof out) == -1);
    assert(skyrim_plugin_child_path("/nonexistent-dir-for-test", "Data", out, sizeof out) == 0);
    assert(skyrim_plugin_child_path(dir, "plugins.txt", out, 4) == -1);
    touch(dir, "DAWNGUARD.ESM");
    assert(skyrim_plugin_file_present(dir, "Dawnguard.esm"));
    assert(!skyrim_plugin_file_present(dir, "HearthFires.esm"));
    snprintf(p1, sizeof p1, "%s/Folder.esp", dir);
    assert(!mkdir(p1, 0700) && !skyrim_plugin_file_present(dir, "Folder.esp"));
    snprintf(p2, sizeof p2, "%s/Alias.esp", dir);
    assert(!symlink("DAWNGUARD.ESM", p2) && !skyrim_plugin_file_present(dir, "Alias.esp"));
    puts("case-insensitive paths PASS");
}

static void assert_bytes(const char *path, const char *text, size_t length) {
    FILE *file = fopen(path, "rb");
    assert(file);
    for (size_t i = 0; i < length; ++i) assert(fgetc(file) == (unsigned char)text[i]);
    assert(fgetc(file) == EOF && !ferror(file));
    assert(!fclose(file));
}
static void save_bytes(const char *path, const char *bytes, size_t length) {
    FILE *file = fopen(path, "wb"); assert(file);
    assert(fwrite(bytes, 1, length, file) == length && !fclose(file));
}
static void profile_list(const char *root) {
    char profile[512], docs[640], list[768];
    assert(strlen(root) < 400);
    snprintf(profile, sizeof profile, "%s/profile", root);
    snprintf(docs, sizeof docs, "%s/Documents", profile);
    snprintf(list, sizeof list, "%s/Skyrim/plugins.txt", docs);
    assert(!mkdir(profile, 0700));
    size_t bytes = 0;
    const char *base[] = {"Skyrim.esm", NULL};
    reset(); add("Authored.esp", 1, 0, base);
    size_t count = run();
    char *managed = skyrim_plugin_list_text(entries, order, count, &bytes);
    assert(managed);
    assert(!skyrim_plugin_apply_list(profile, NULL, 0));
    assert(access(docs, F_OK) != 0);
    assert(!skyrim_plugin_apply_list(profile, managed, bytes));
    assert_bytes(list, managed, bytes);
    assert(!skyrim_plugin_apply_list(profile, NULL, 0));
    assert(access(list, F_OK) != 0);

    const char original[] = "# Author's list\r\nDawnguard.esm\r\n# \xe9\n";
    save_bytes(list, original, sizeof original - 1);
    struct stat before, after;
    assert(!stat(list, &before) && !skyrim_plugin_apply_list(profile, NULL, 0) && !stat(list, &after));
    assert(before.st_ino == after.st_ino && before.st_mtime == after.st_mtime);
    assert(!skyrim_plugin_apply_list(profile, managed, bytes) && !skyrim_plugin_apply_list(profile, managed, bytes));
    assert(!skyrim_plugin_apply_list(profile, NULL, 0));
    assert_bytes(list, original, sizeof original - 1);
    assert(!skyrim_plugin_apply_list(profile, managed, bytes));
    save_bytes(list, "edited", 6);
    assert(!skyrim_plugin_apply_list(profile, NULL, 0)); assert_bytes(list, "edited", 6);
    save_bytes(list, "", 0);
    assert(!skyrim_plugin_apply_list(profile, managed, bytes) && !skyrim_plugin_apply_list(profile, NULL, 0));
    assert_bytes(list, "", 0);
    free(managed); reset();
    puts("profile restore PASS");
}

static void profile_dlc_inheritance(const char *root) {
    assert(strlen(root) < 400);
    char profile[512], docs[640], folder[768], list[896], other[512];
    snprintf(profile, sizeof profile, "%s/profile-dlc", root);
    snprintf(docs, sizeof docs, "%s/documents", profile);
    snprintf(folder, sizeof folder, "%s/SKYRIM", docs);
    snprintf(list, sizeof list, "%s/Plugins.TXT", folder);
    snprintf(other, sizeof other, "%s/profile-other", root);
    assert(!mkdir(profile, 0700) && !mkdir(other, 0700));

    unsigned bits = ~0u;
    assert(!skyrim_plugin_read_profile_dlc(profile, &bits) && !bits);
    assert(access(docs, F_OK) != 0);
    assert(!mkdir(docs, 0700) && !mkdir(folder, 0700));
    const char original[] = "# Owner's DLC settings\r\ndawnguard.ESM\r\n"
                            " HearthFires.esm\t\r\nDragonborn.esm";
    save_bytes(list, original, sizeof original - 1);
    struct stat before, after;
    assert(!stat(list, &before));
    assert(!skyrim_plugin_apply_list(profile, NULL, 0));
    assert(!stat(list, &after) && before.st_ino == after.st_ino && before.st_mtime == after.st_mtime);
    assert_bytes(list, original, sizeof original - 1);

    unsigned dawnguard = skyrim_plugin_dlc_bit("Dawnguard.esm");
    unsigned hearthfires = skyrim_plugin_dlc_bit("HearthFires.esm");
    unsigned dragonborn = skyrim_plugin_dlc_bit("Dragonborn.esm");
    unsigned all_dlc = dawnguard | hearthfires | dragonborn;
    assert(dawnguard && hearthfires && dragonborn && !skyrim_plugin_dlc_bit("HighResTexturePack01.esp"));
    assert(!skyrim_plugin_read_profile_dlc(profile, &bits) && bits == all_dlc);
    const char *base[] = {"Skyrim.esm", "Update.esm", NULL};
    const char *all_masters[] = {"Skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm", "Dragonborn.esm", NULL};
    reset();
    add("Authored DLC Patch.esp", 1, 0, all_masters);
    add("Dawnguard.esm", (bits & dawnguard) != 0, 1, base);
    add("HearthFires.esm", (bits & hearthfires) != 0, 1, base);
    size_t loaded = run();  /* A DLC mentioned in the list is not a file. */
    EXPECT_ORDER(loaded, "Dawnguard.esm,HearthFires.esm");
    assert(state[0] == SKYRIM_PLUGIN_MISSING_MASTER && problem[0] == 4);
    add("Dragonborn.esm", (bits & dragonborn) != 0, 1, base);
    loaded = run();
    EXPECT_ORDER(loaded, "Dawnguard.esm,HearthFires.esm,Dragonborn.esm,Authored DLC Patch.esp");

    entries[2].enabled = 0;  /* This profile explicitly disabled HearthFires. */
    loaded = run();
    EXPECT_ORDER(loaded, "Dawnguard.esm,Dragonborn.esm");
    assert(state[0] == SKYRIM_PLUGIN_MISSING_MASTER && problem[0] == 3 && state[2] == SKYRIM_PLUGIN_OFF);
    size_t length = 0;
    char *managed = skyrim_plugin_list_text(entries, order, loaded, &length);
    assert(managed && !skyrim_plugin_apply_list(profile, managed, length));
    assert_bytes(list, managed, length);
    assert(!skyrim_plugin_read_profile_dlc(other, &bits) && !bits);
    assert(!skyrim_plugin_read_profile_dlc(profile, &bits) && bits == all_dlc);
    assert(!skyrim_plugin_apply_list(profile, NULL, 0));
    assert_bytes(list, original, sizeof original - 1);
    free(managed);
    reset();
    puts("profile DLC inheritance PASS");
}

static void hair_pack_masters(void) {
    const char *base[] = {"Skyrim.esm", NULL};
    const char *patch_masters[] = {"Skyrim.esm", "ApachiiSkyHair.esm", "KS Hairdo's.esp", NULL};
    add("Authored Hair Patch.esp", 1, 0, patch_masters);
    add("KS Hairdo's.esp", 1, 0, base);
    add("ApachiiSkyHair.esm", 1, 1, base);
    EXPECT_ORDER(run(), "ApachiiSkyHair.esm,KS Hairdo's.esp,Authored Hair Patch.esp");
    entries[2].enabled = 0;
    size_t loaded = run();
    EXPECT_ORDER(loaded, "KS Hairdo's.esp");
    assert(state[0] == SKYRIM_PLUGIN_MISSING_MASTER);
    assert(!strcmp(skyrim_plugin_master(entries[0].header, (unsigned)problem[0]), "ApachiiSkyHair.esm"));
    reset();
    puts("hair pack masters PASS");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    parsing(); names(); ordering(); usleep_dlc_masters(); list_text(); paths(argv[1]); profile_list(argv[1]);
    profile_dlc_inheritance(argv[1]); hair_pack_masters();
    return 0;
}
