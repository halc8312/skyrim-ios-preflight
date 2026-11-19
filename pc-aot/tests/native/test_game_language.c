/* Authored tiny packs and profile settings only; no game-derived data. */
#include "game_language.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *files[] = {
    "Strings/skyrim_Japanese.STRINGS", "Strings/skyrim_Japanese.DLSTRINGS",
    "Strings/skyrim_Japanese.ILSTRINGS", "Strings/Update_Japanese.STRINGS",
    "Strings/Update_Japanese.DLSTRINGS", "Strings/Update_Japanese.ILSTRINGS",
    "Interface/Translate_JAPANESE.txt", "Interface/fonts_jp.swf",
    "Interface/FontConfig_Japanese.txt"
};
static void path(char *out, size_t size, const char *root, const char *relative) {
    int n = snprintf(out, size, "%s/Data/%s", root, relative);
    assert(n > 0 && (size_t)n < size);
}
static void writeFile(const char *root, const char *relative, const void *data, size_t size) {
    char name[4096]; path(name, sizeof name, root, relative);
    FILE *f = fopen(name, "wb"); assert(f);
    assert(fwrite(data, 1, size, f) == size); assert(!fclose(f));
}
static void textMarker(const char *root) {
    static const char marker[] = "SKYRIMIOS-JAPANESE-TEXT-1\n1\n2\n3\n4\n5\n6\n7\n8\n9\n";
    writeFile(root, "SkyrimIOS-Japanese-v1.ready", marker, sizeof marker - 1);
}
static void expectPacks(const char *root, unsigned text, unsigned voices) {
    SkyrimGameLanguagePacks packs; skyrim_game_language_probe(root, &packs);
    assert(packs.text_status == text && packs.voices_status == voices);
}
static void testPacks(const char *root) {
    char name[4096];
    expectPacks(NULL, SKYRIM_GAME_PACK_MISSING, SKYRIM_GAME_PACK_MISSING);
    expectPacks("relative", SKYRIM_GAME_PACK_MISSING, SKYRIM_GAME_PACK_MISSING);
    expectPacks(root, SKYRIM_GAME_PACK_MISSING, SKYRIM_GAME_PACK_MISSING);
    path(name, sizeof name, root, ""); assert(!mkdir(name, 0700));
    path(name, sizeof name, root, "Strings"); assert(!mkdir(name, 0700));
    path(name, sizeof name, root, "Interface"); assert(!mkdir(name, 0700));
    for (unsigned i = 0; i < 9; ++i) writeFile(root, files[i], "123456789", i + 1);
    // All assets without the final marker still cannot enable Japanese.
    expectPacks(root, SKYRIM_GAME_PACK_MISSING, SKYRIM_GAME_PACK_MISSING);
    textMarker(root); expectPacks(root, SKYRIM_GAME_PACK_READY, SKYRIM_GAME_PACK_MISSING);
    for (unsigned i = 0; i < 9; ++i) {
        writeFile(root, files[i], "123456789", i);
        expectPacks(root, SKYRIM_GAME_PACK_INCOMPLETE, SKYRIM_GAME_PACK_MISSING);
        writeFile(root, files[i], "123456789", i + 1);
    }
    static const char *bad[] = {"", "SKYRIMIOS-JAPANESE-TEXT-2\n1\n2\n3\n4\n5\n6\n7\n8\n9\n",
        "SKYRIMIOS-JAPANESE-TEXT-1\n1\n", "SKYRIMIOS-JAPANESE-TEXT-1\n0\n",
        "SKYRIMIOS-JAPANESE-TEXT-1\n-1\n", "SKYRIMIOS-JAPANESE-TEXT-1\n4294967296\n",
        "SKYRIMIOS-JAPANESE-TEXT-1\n9999999999999999999\n",
        "SKYRIMIOS-JAPANESE-TEXT-1\n1\n2\n3\n4\n5\n6\n7\n8\n9",
        "SKYRIMIOS-JAPANESE-TEXT-1\n1\n2\n3\n4\n5\n6\n7\n8\n9\nEXTRA\n"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        writeFile(root, "SkyrimIOS-Japanese-v1.ready", bad[i], strlen(bad[i]));
        expectPacks(root, SKYRIM_GAME_PACK_INCOMPLETE, SKYRIM_GAME_PACK_MISSING);
    }
    char huge[256]; memset(huge, 'x', sizeof huge);
    writeFile(root, "SkyrimIOS-Japanese-v1.ready", huge, sizeof huge);
    expectPacks(root, SKYRIM_GAME_PACK_INCOMPLETE, SKYRIM_GAME_PACK_MISSING);
    textMarker(root);
    path(name, sizeof name, root, files[0]); assert(!unlink(name));
    assert(!symlink("skyrim_Japanese.DLSTRINGS", name));
    expectPacks(root, SKYRIM_GAME_PACK_INCOMPLETE, SKYRIM_GAME_PACK_MISSING);
    assert(!unlink(name)); writeFile(root, files[0], "1", 1);
    path(name, sizeof name, root, "SkyrimIOS-Japanese-v1.ready"); assert(!unlink(name));
    assert(!symlink("Interface/FontConfig_Japanese.txt", name));
    expectPacks(root, SKYRIM_GAME_PACK_INCOMPLETE, SKYRIM_GAME_PACK_MISSING);
    assert(!unlink(name)); textMarker(root);
    writeFile(root, "Skyrim - Voices_Japanese.bsa", "123", 3);
    writeFile(root, "Skyrim - VoicesExtra_Japanese.bsa", "1234", 4);
    static const char voiceMarker[] = "SKYRIMIOS-JAPANESE-VOICES-1\n3\n4\n";
    writeFile(root, "SkyrimIOS-Japanese-Voices-v1.ready", voiceMarker, sizeof voiceMarker - 1);
    expectPacks(root, SKYRIM_GAME_PACK_READY, SKYRIM_GAME_PACK_READY);
    writeFile(root, "Skyrim - VoicesExtra_Japanese.bsa", "1", 1);
    expectPacks(root, SKYRIM_GAME_PACK_READY, SKYRIM_GAME_PACK_INCOMPLETE);
    puts("pack completeness PASS");
}
static void testSelect(void) {
    for (unsigned t = 0; t <= SKYRIM_GAME_PACK_UNSUPPORTED; ++t)
        for (unsigned v = 0; v <= SKYRIM_GAME_PACK_UNSUPPORTED; ++v) {
            SkyrimGameLanguagePacks packs = {t, v};
            SkyrimGameLanguage en = skyrim_game_language_select(0, 0, packs);
            assert(!en.text && !en.voices && !en.text_reason && !en.voices_reason);
            SkyrimGameLanguage jp = skyrim_game_language_select(1, 1, packs);
            assert(jp.text == !t && jp.voices == !v && jp.text_reason == t && jp.voices_reason == v);
            SkyrimGameLanguage mixed = skyrim_game_language_select(1, 0, packs);
            assert(mixed.text == !t && !mixed.voices && !mixed.voices_reason);
        }
    SkyrimGameLanguage invalid = skyrim_game_language_select(99, 2, (SkyrimGameLanguagePacks){0,0});
    assert(!invalid.text && !invalid.voices && !invalid.requested_text && !invalid.requested_voices);
    puts("independent language selection PASS");
}
static char *transform(const char *input, SkyrimGameLanguage language) {
    size_t size = 999; char *result = skyrim_game_language_ini(input, strlen(input), language, &size);
    assert(result && size == strlen(result));
    size_t size2; char *again = skyrim_game_language_ini(result, size, language, &size2);
    assert(again && size == size2 && !memcmp(result, again, size)); free(again);
    return result;
}
static void testMissingTemplate(const char *game) {
    char folder[4096], target[4096], defaults[4096];
    int n = snprintf(folder, sizeof folder, "%s/Profile", game);
    assert(n > 0 && (size_t)n < sizeof folder);
    SkyrimGameLanguagePacks ready = {SKYRIM_GAME_PACK_READY, SKYRIM_GAME_PACK_READY};
    for (unsigned text = 0; text < 2; ++text) {
        for (unsigned voices = 0; voices < 2; ++voices) {
            SkyrimGameLanguage language = skyrim_game_language_select(text, voices, ready);
            assert(skyrim_game_language_plan_profile(game, folder, &language, target, defaults,
                sizeof target) == SKYRIM_PROFILE_INI_DEFERRED);
            assert(language.requested_text == text && language.requested_voices == voices);
            assert(!language.text && !language.voices);
            assert(language.text_reason == (text ? SKYRIM_GAME_MISSING_DEFAULT_INI : 0));
            assert(language.voices_reason == (voices ? SKYRIM_GAME_MISSING_DEFAULT_INI : 0));
            assert(access(target, F_OK) && access(defaults, F_OK));
        }
    }
    SkyrimGameLanguage language = skyrim_game_language_select(1, 1, ready);
    assert(!symlink("absent", defaults));
    assert(skyrim_game_language_plan_profile(game, folder, &language, target, defaults,
        sizeof target) == SKYRIM_PROFILE_INI_ERROR);
    assert(!unlink(defaults));
    FILE *file = fopen(defaults, "wb"); assert(file);
    assert(fputs("[General]\nsLanguage=ENGLISH\n", file) >= 0 && !fclose(file));
    assert(skyrim_game_language_plan_profile(game, folder, &language, target, defaults,
        sizeof target) == SKYRIM_PROFILE_INI_FROM_DEFAULT);
    assert(language.text && language.voices && !language.text_reason && !language.voices_reason);
    assert(!mkdir(folder, 0700) && !symlink("absent", target));
    assert(skyrim_game_language_plan_profile(game, folder, &language, target, defaults,
        sizeof target) == SKYRIM_PROFILE_INI_ERROR);
    assert(!unlink(target) && !unlink(defaults));
    file = fopen(target, "wb"); assert(file && !fclose(file));
    assert(skyrim_game_language_plan_profile(game, folder, &language, target, defaults,
        sizeof target) == SKYRIM_PROFILE_INI_EXISTING);
    puts("missing template deferral and recovery PASS");
}
static void testIni(void) {
    SkyrimGameLanguage en = {0}, text = {.text=1}, both = {.text=1,.voices=1};
    static const char original[] = "; authored profile\r\n[General]\r\nsLanguage=ENGLISH\r\nKeep=42\r\n"
        "[Fonts]\r\nsFontConfigFile=Interface\\FontConfig.txt\r\n"
        "[Archive]\r\nsResourceArchiveList2=Skyrim - Voices.bsa, Skyrim - VoicesExtra.bsa\r\n"
        "[Other]\r\nsLanguage=DoNotTouch\r\n";
    char *result = transform(original, en); assert(!strcmp(result, original)); free(result);
    result = transform(original, text);
    assert(strstr(result, "sLanguage=JAPANESE\r\nKeep=42"));
    assert(strstr(result, "sFontConfigFile=Interface\\FontConfig_Japanese.txt"));
    assert(strstr(result, "sResourceArchiveList2=Skyrim - Voices.bsa, Skyrim - VoicesExtra.bsa"));
    assert(strstr(result, "[Other]\r\nsLanguage=DoNotTouch"));
    char *restored = transform(result, en); assert(!strcmp(restored, original)); free(restored); free(result);
    result = transform("[General]\nKeep=42\n[Fonts]\nOtherFont=1\n[Archive]\nOtherArchive=1", both);
    assert(!strcmp(result, "[General]\nKeep=42\nsLanguage=JAPANESE\n"
        "[Fonts]\nOtherFont=1\nsFontConfigFile=Interface\\FontConfig_Japanese.txt\n"
        "[Archive]\nOtherArchive=1\nsResourceArchiveList2=Skyrim - Voices_Japanese.bsa, Skyrim - VoicesExtra_Japanese.bsa\n"));
    free(result);
    result = transform("\xef\xbb\xbf[GENERAL]\r\n sLANGUAGE =OLD\r\n[general]\r\nsLanguage=Duplicate\r\n"
        "[Archive]\r\nsResourceArchiveList2=Extra1.bsa,Skyrim - Voices.bsa, Extra2.bsa, Skyrim - VoicesExtra_Japanese.bsa, Skyrim - Voices_Japanese.bsa\r\n", both);
    assert(!strncmp(result, "[GENERAL]", 9));
    assert(!strstr(result, "OLD") && !strstr(result, "Duplicate"));
    assert(strstr(result, "Extra1.bsa, Skyrim - Voices_Japanese.bsa, Extra2.bsa, Skyrim - VoicesExtra_Japanese.bsa\r\n"));
    free(result);
    result = transform("[Archive]\nsResourceArchiveList2=Extra.bsa\n", en);
    assert(strstr(result, "Extra.bsa, Skyrim - Voices.bsa, Skyrim - VoicesExtra.bsa\n")); free(result);
    result = transform("", text); assert(strstr(result, "[General]\nsLanguage=JAPANESE\n")); free(result);
    size_t size = 99; assert(!skyrim_game_language_ini("a\0b", 3, en, &size) && !size);
    assert(!skyrim_game_language_ini(NULL, 0, en, &size));
    assert(!skyrim_game_language_ini("", 1024*1024+1, en, &size));
    // Separator expansion stays bounded even for a dense, authored list.
    char *large = malloc(200100); assert(large);
    strcpy(large, "[Archive]\nsResourceArchiveList2="); size_t used = strlen(large);
    for (unsigned i = 0; i < 100000; ++i) { large[used++] = 'a'; large[used++] = ','; }
    large[used] = 0; result = transform(large, both); free(result); free(large);
    unsigned random = 16;
    for (unsigned n = 0; n < 512; ++n) {
        char input[2048]; size_t length = n * 3;
        for (size_t i = 0; i < length; ++i) {
            random = random * 1664525u + 1013904223u;
            input[i] = (char)(1 + random % 127);
        }
        input[length] = 0; result = transform(input, both); free(result);
    }
    puts("profile preservation and switching PASS");
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        SkyrimGameLanguagePacks packs; skyrim_game_language_probe(argv[2], &packs);
        printf("text=%u voices=%u\n", packs.text_status, packs.voices_status); return 0;
    }
    assert(argc == 2); testPacks(argv[1]); testSelect(); testMissingTemplate(argv[1]); testIni(); return 0;
}
