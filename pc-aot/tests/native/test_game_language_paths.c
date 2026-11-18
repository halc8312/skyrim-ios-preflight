/* Included after the real guest path/INI reader in test_game_language.py. */
static void write_profile(const char *path, const char *bytes) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(bytes, 1, strlen(bytes), f) == strlen(bytes)); assert(!fclose(f));
}
static void apply_language(const char *path, unsigned japanese) {
    FILE *f = fopen(path, "rb"); assert(f);
    char bytes[4096]; size_t count = fread(bytes, 1, sizeof bytes, f);
    assert(count < sizeof bytes && !ferror(f)); assert(!fclose(f));
    SkyrimGameLanguage language = {.text=japanese}; size_t updatedCount;
    char *updated = skyrim_game_language_ini(bytes, count, language, &updatedCount);
    assert(updated && updatedCount == strlen(updated));
    write_profile(path, updated); free(updated);
}
static void guest_reads(const char *language, const char *setting, const char *value) {
    assert(!pthread_mutex_lock(&g_ini_lock));
    IniFile *profile = ini_get("C:\\Users\\Player\\Documents\\My Games\\Skyrim\\Skyrim.INI");
    assert(profile && ini_find(profile, "General", "sLanguage"));
    assert(!strcmp(ini_find(profile, "General", "sLanguage"), language));
    if (setting) assert(!strcmp(ini_find(profile, "General", setting), value));
    assert(!pthread_mutex_unlock(&g_ini_lock));
}
int main(int argc, char **argv) {
    assert(argc == 3); // authored profile root and its Skyrim settings directory
    assert(strlen(argv[1]) < sizeof g_save_dir); strcpy(g_save_dir, argv[1]);
    assert(strlen(argv[2]) < sizeof g_game_dir); strcpy(g_game_dir, argv[2]);
    const char *folder = argv[2];
    char upper[PATH_MAX], lower[PATH_MAX], other[PATH_MAX], selected[PATH_MAX];
    assert(snprintf(upper, sizeof upper, "%s/Skyrim.INI", folder) > 0);
    assert(snprintf(lower, sizeof lower, "%s/Skyrim.ini", folder) > 0);
    assert(snprintf(other, sizeof other, "%s/sKyRiM.iNi", folder) > 0);
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == 0);
    assert(!strcmp(selected, upper));
    write_profile(lower, "[General]\nsLanguage=ENGLISH\nFromLower=99\n");
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == 1);
    assert(!strcmp(selected, lower)); apply_language(selected, 1);
    dircache_forget(folder); guest_reads("JAPANESE", "FromLower", "99");
    // Reproduce build10: a lower-case Japanese INI does not supersede the
    // pre-existing canonical English INI that the real guest requests.
    write_profile(upper, "[General]\nsLanguage=ENGLISH\nFromUpper=42\n");
    dircache_forget(folder); guest_reads("ENGLISH", "FromUpper", "42");
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == 1);
    assert(!strcmp(selected, upper)); apply_language(selected, 1);
    guest_reads("JAPANESE", "FromUpper", "42");
    // The unused lowercase file stays in place and retains its contents.
    FILE *f = fopen(lower, "rb"); assert(f); char bytes[4096];
    size_t size = fread(bytes, 1, sizeof bytes - 1, f); bytes[size] = 0; assert(!fclose(f));
    assert(strstr(bytes, "sLanguage=JAPANESE") && strstr(bytes, "FromLower=99"));
    apply_language(selected, 0); guest_reads("ENGLISH", "FromUpper", "42");
    assert(!unlink(upper) && !unlink(lower));
    write_profile(other, "[General]\nsLanguage=ENGLISH\nOnlyMixed=1\n");
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == 1);
    assert(!strcmp(selected, other)); apply_language(selected, 1);
    dircache_forget(folder); guest_reads("JAPANESE", "OnlyMixed", "1");
    write_profile(lower, "[General]\nsLanguage=ENGLISH\n");
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == -1);
    assert(!symlink("Skyrim.ini", upper));
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == -1);
    assert(!unlink(upper)); assert(!mkdir(upper, 0700));
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == -1);
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_DEFAULT, selected, sizeof selected) == 0);
    assert(strstr(selected, "/Skyrim_default.ini"));
    char defaults[PATH_MAX]; assert(snprintf(defaults, sizeof defaults, "%s/Skyrim_Default.ini", folder) > 0);
    write_profile(defaults, "original defaults stay unchanged\n");
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_DEFAULT, selected, sizeof selected) == 1);
    assert(!strcmp(selected, defaults));
    assert(skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, selected, 4) == -1);
    assert(skyrim_game_language_ini_path("relative", SKYRIM_GAME_INI_PROFILE, selected, sizeof selected) == -1);
    assert(skyrim_game_language_ini_path(folder, 2, selected, sizeof selected) == -1);
    puts("real guest case-sensitive INI path PASS"); return 0;
}
