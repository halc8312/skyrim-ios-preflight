/* SPDX-License-Identifier: GPL-3.0-only */
#include "game_language.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *const textFiles[] = {
    "Strings/skyrim_Japanese.STRINGS", "Strings/skyrim_Japanese.DLSTRINGS",
    "Strings/skyrim_Japanese.ILSTRINGS", "Strings/Update_Japanese.STRINGS",
    "Strings/Update_Japanese.DLSTRINGS", "Strings/Update_Japanese.ILSTRINGS",
    "Interface/Translate_JAPANESE.txt", "Interface/fonts_jp.swf",
    "Interface/FontConfig_Japanese.txt"
};
static const char *const voiceFiles[] = {
    "Skyrim - Voices_Japanese.bsa", "Skyrim - VoicesExtra_Japanese.bsa"
};

static int regularFile(const char *root, const char *name, uint64_t size) {
    char path[PATH_MAX]; struct stat st;
    int n = snprintf(path, sizeof path, "%s/Data/%s", root, name);
    return n > 0 && (size_t)n < sizeof path && !lstat(path, &st) &&
        S_ISREG(st.st_mode) && st.st_size >= 0 && (uint64_t)st.st_size == size;
}
static int directory(const char *root, const char *name) {
    char path[PATH_MAX]; struct stat st;
    int n = snprintf(path, sizeof path, "%s/%s", root, name);
    return n > 0 && (size_t)n < sizeof path && !lstat(path, &st) && S_ISDIR(st.st_mode);
}
static unsigned pack(const char *root, const char *marker, const char *magic,
                     const char *const *files, size_t count, int text) {
    if (!root || root[0] != '/') return SKYRIM_GAME_PACK_MISSING;
    char path[PATH_MAX]; struct stat st;
    int n = snprintf(path, sizeof path, "%s/Data/%s", root, marker);
    if (n < 0 || (size_t)n >= sizeof path) return SKYRIM_GAME_PACK_INCOMPLETE;
    if (lstat(path, &st)) return errno == ENOENT ? SKYRIM_GAME_PACK_MISSING : SKYRIM_GAME_PACK_INCOMPLETE;
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size >= 256 ||
        !directory(root, "Data") || (text &&
        (!directory(root, "Data/Strings") || !directory(root, "Data/Interface"))))
        return SKYRIM_GAME_PACK_INCOMPLETE;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return SKYRIM_GAME_PACK_INCOMPLETE;
    char data[256]; size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t r = read(fd, data + got, (size_t)st.st_size - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
    }
    struct stat after;
    int stable = !fstat(fd, &after) && st.st_dev == after.st_dev && st.st_ino == after.st_ino &&
        st.st_size == after.st_size && st.st_mtime == after.st_mtime;
    int closed = !close(fd);
    size_t magicSize = strlen(magic);
    if (!stable || !closed || got != (size_t)st.st_size || got < magicSize ||
        memcmp(data, magic, magicSize) || memchr(data, 0, got)) return SKYRIM_GAME_PACK_INCOMPLETE;
    data[got] = 0;
    const char *p = data + magicSize;
    for (size_t i = 0; i < count; ++i) {
        uint64_t size = 0; unsigned digits = 0;
        while (*p >= '0' && *p <= '9') {
            unsigned digit = (unsigned)(*p++ - '0');
            if (++digits > 10 || size > (UINT64_C(4294967295) - digit) / 10)
                return SKYRIM_GAME_PACK_INCOMPLETE;
            size = size * 10 + digit;
        }
        if (!digits || *p++ != '\n' || !size || !regularFile(root, files[i], size))
            return SKYRIM_GAME_PACK_INCOMPLETE;
    }
    return *p ? SKYRIM_GAME_PACK_INCOMPLETE : SKYRIM_GAME_PACK_READY;
}
void skyrim_game_language_probe(const char *root, SkyrimGameLanguagePacks *packs) {
    if (!packs) return;
    packs->text_status = pack(root, "SkyrimIOS-Japanese-v1.ready", "SKYRIMIOS-JAPANESE-TEXT-1\n",
        textFiles, sizeof textFiles / sizeof textFiles[0], 1);
    packs->voices_status = pack(root, "SkyrimIOS-Japanese-Voices-v1.ready", "SKYRIMIOS-JAPANESE-VOICES-1\n",
        voiceFiles, sizeof voiceFiles / sizeof voiceFiles[0], 0);
    // The pinned runtime adapter supports the audited Japanese voice profile.
    // Its independent decoder regression is tests/test_xwma_voice_profile.py;
    // this probe checks installation completeness only, not audio decoding.
}
SkyrimGameLanguage skyrim_game_language_select(unsigned text, unsigned voices,
                                               SkyrimGameLanguagePacks packs, unsigned arabic_mod) {
    SkyrimGameLanguage state = {0};
    state.arabic_mod = arabic_mod;
    state.requested_text = text == SKYRIM_GAME_JAPANESE;
    state.requested_voices = voices == SKYRIM_GAME_JAPANESE;
    state.text_reason = state.requested_text ? packs.text_status : SKYRIM_GAME_PACK_READY;
    state.voices_reason = state.requested_voices ? packs.voices_status : SKYRIM_GAME_PACK_READY;
    state.text = state.requested_text && state.text_reason == SKYRIM_GAME_PACK_READY;
    state.voices = state.requested_voices && state.voices_reason == SKYRIM_GAME_PACK_READY;
    if (state.arabic_mod == SKYRIM_ARABIC_MOD_READY) {
        // The reviewed translation replaces the English tables and font configuration.
        state.text = SKYRIM_GAME_ENGLISH;
        state.text_reason = SKYRIM_GAME_PACK_READY;
    }
    return state;
}

int skyrim_game_language_ini_path(const char *directory, unsigned kind,
                                 char *path, size_t capacity) {
    if (!directory || directory[0] != '/' || !path || !capacity || kind > SKYRIM_GAME_INI_DEFAULT)
        return -1;
    const char *name = kind == SKYRIM_GAME_INI_PROFILE ? "Skyrim.INI" : "Skyrim_default.ini";
    int n = snprintf(path, capacity, "%s/%s", directory, name);
    if (n < 0 || (size_t)n >= capacity) return -1;
    struct stat st;
    // Match the guest's exact-path-first resolve_ci behavior. Existing mixed
    // case aliases must not cause us to write a second, unused profile file.
    if (!lstat(path, &st)) return S_ISREG(st.st_mode) ? 1 : -1;
    if (errno != ENOENT) return -1;
    DIR *dir = opendir(directory);
    if (!dir) return errno == ENOENT ? 0 : -1;
    unsigned matches = 0;
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry) { error = errno; break; }
        if (strcasecmp(entry->d_name, name)) continue;
        if (++matches > 1) break;
        n = snprintf(path, capacity, "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= capacity) { error = ENAMETOOLONG; break; }
    }
    if (closedir(dir)) error = errno;
    if (error || matches > 1) return -1;
    if (!matches) return 0;
    return !lstat(path, &st) && S_ISREG(st.st_mode) ? 1 : -1;
}

SkyrimGameIniSource skyrim_game_language_plan_profile(const char *game, const char *folder,
    SkyrimGameLanguage *language, char *target, char *defaults, size_t capacity) {
    if (!language) return SKYRIM_PROFILE_INI_ERROR;
    int found = skyrim_game_language_ini_path(folder, SKYRIM_GAME_INI_PROFILE, target, capacity);
    if (found < 0) return SKYRIM_PROFILE_INI_ERROR;
    if (found) return SKYRIM_PROFILE_INI_EXISTING;
    found = skyrim_game_language_ini_path(game, SKYRIM_GAME_INI_DEFAULT, defaults, capacity);
    if (found < 0) return SKYRIM_PROFILE_INI_ERROR;
    if (found) return SKYRIM_PROFILE_INI_FROM_DEFAULT;

    /* A partial transfer can boot with the same defaults as the first launch.
     * Creating a three-key profile here would discard the template's archives. */
    language->text = SKYRIM_GAME_ENGLISH;
    language->voices = SKYRIM_GAME_ENGLISH;
    if (language->arabic_mod == SKYRIM_ARABIC_MOD_READY)
        language->arabic_mod = SKYRIM_ARABIC_MOD_NEEDS_INI;
    if (language->requested_text) language->text_reason = SKYRIM_GAME_MISSING_DEFAULT_INI;
    if (language->requested_voices) language->voices_reason = SKYRIM_GAME_MISSING_DEFAULT_INI;
    return SKYRIM_PROFILE_INI_DEFERRED;
}

static int equals(const char *s, size_t n, const char *literal) {
    return n == strlen(literal) && !strncasecmp(s, literal, n);
}
static unsigned iniSection(const char *p, const char *end) {
    const char *close = memchr(p, ']', (size_t)(end - p));
    if (!close) return 0;
    ++p;
    while (p < close && isspace((unsigned char)*p)) ++p;
    while (close > p && isspace((unsigned char)close[-1])) --close;
    if (equals(p, (size_t)(close - p), "General")) return 1;
    if (equals(p, (size_t)(close - p), "Fonts")) return 2;
    if (equals(p, (size_t)(close - p), "Archive")) return 3;
    return 0;
}
static const char *iniValue(const char *p, const char *end, const char *key) {
    if (p == end || *p == ';' || *p == '#') return NULL;
    const char *eq = memchr(p, '=', (size_t)(end - p));
    if (!eq) return NULL;
    const char *nameEnd = eq;
    while (nameEnd > p && isspace((unsigned char)nameEnd[-1])) --nameEnd;
    return equals(p, (size_t)(nameEnd - p), key) ? eq + 1 : NULL;
}
static const char *lineStart(const char *source, size_t begin, size_t end) {
    const char *p = source + begin;
    if (!begin && end >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) p += 3;
    while (p < source + end && (*p == ' ' || *p == '\t')) ++p;
    return p;
}
/* Change only the two known voice archives; retain every unrelated archive,
 * in order. Missing voice entries are appended and duplicate aliases removed. */
static char *voiceArchives(const char *source, size_t bytes, unsigned japanese) {
    const char *value = NULL, *end = NULL;
    unsigned section = 0;
    for (size_t offset = 0; offset < bytes;) {
        size_t begin = offset;
        while (offset < bytes && source[offset] != '\n') ++offset;
        if (offset < bytes) ++offset;
        const char *p = lineStart(source, begin, offset);
        if (p < source + offset && *p == '[') section = iniSection(p, source + offset);
        else if (section == 3 && (value = iniValue(p, source + offset, "sResourceArchiveList2"))) {
            end = source + offset;
            break;
        }
    }
    const char *english[] = {"Skyrim - Voices.bsa", "Skyrim - VoicesExtra.bsa"};
    const char *japaneseNames[] = {"Skyrim - Voices_Japanese.bsa", "Skyrim - VoicesExtra_Japanese.bsa"};
    const char *const *names = japanese == SKYRIM_GAME_JAPANESE ? japaneseNames : english;
    // Canonical comma-space separators can expand a tightly packed list.
    char *out = malloc(bytes * 2 + 128);
    if (!out) return NULL;
    size_t used = 0; unsigned seen[2] = {0};
    while (value && value < end) {
        const char *next = memchr(value, ',', (size_t)(end - value));
        const char *tokenEnd = next ? next : end;
        while (value < tokenEnd && isspace((unsigned char)*value)) ++value;
        while (tokenEnd > value && isspace((unsigned char)tokenEnd[-1])) --tokenEnd;
        size_t length = (size_t)(tokenEnd - value);
        for (unsigned i = 0; i < 2; ++i) {
            if (equals(value, length, english[i]) || equals(value, length, japaneseNames[i])) {
                if (seen[i]++) length = 0;
                else { value = names[i]; length = strlen(value); }
                break;
            }
        }
        if (length) {
            if (used) { memcpy(out + used, ", ", 2); used += 2; }
            memcpy(out + used, value, length); used += length;
        }
        value = next ? next + 1 : end;
    }
    for (unsigned i = 0; i < 2; ++i) if (!seen[i]) {
        if (used) { memcpy(out + used, ", ", 2); used += 2; }
        size_t length = strlen(names[i]); memcpy(out + used, names[i], length); used += length;
    }
    out[used] = 0;
    return out;
}
static int setting(char *out, size_t capacity, size_t *used, const char *key,
                   const char *value, const char *newline) {
    if (*used && out[*used - 1] != '\n') {
        size_t n = strlen(newline);
        if (n >= capacity - *used) return 0;
        memcpy(out + *used, newline, n); *used += n;
    }
    int n = snprintf(out + *used, capacity - *used, "%s=%s%s", key, value, newline);
    if (n < 0 || (size_t)n >= capacity - *used) return 0;
    *used += (size_t)n;
    return 1;
}
char *skyrim_game_language_ini(const char *source, size_t bytes,
                             SkyrimGameLanguage language, size_t *out_bytes) {
    if (out_bytes) *out_bytes = 0;
    if (!out_bytes || !source || bytes > 1024 * 1024 || memchr(source, 0, bytes)) return NULL;
    const size_t capacity = bytes * 2 + 1024;
    char *out = malloc(capacity);
    if (!out) return NULL;
    char *archives = voiceArchives(source, bytes, language.voices);
    if (!archives) { free(out); return NULL; }
    size_t used = 0, offset = 0; unsigned section = 0;
    unsigned written[3] = {0};
    const char *newline = memchr(source, '\r', bytes) ? "\r\n" : "\n";
    const char *sections[] = {"General", "Fonts", "Archive"};
    const char *keys[] = {"sLanguage", "sFontConfigFile", "sResourceArchiveList2"};
    const char *values[] = {
        language.text == SKYRIM_GAME_JAPANESE ? "JAPANESE" : "ENGLISH",
        language.text == SKYRIM_GAME_JAPANESE ? "Interface\\FontConfig_Japanese.txt" : "Interface\\FontConfig.txt",
        archives
    };
    while (offset < bytes) {
        size_t begin = offset;
        while (offset < bytes && source[offset] != '\n') ++offset;
        if (offset < bytes) ++offset;
        const char *p = lineStart(source, begin, offset), *end = source + offset;
        int drop = 0;
        if (p < end && *p == '[') {
            if (section && !written[section - 1]) {
                if (!setting(out, capacity, &used, keys[section - 1], values[section - 1], newline)) goto fail;
                written[section - 1] = 1;
            }
            section = iniSection(p, end);
        } else if (section) drop = iniValue(p, end, keys[section - 1]) != NULL;
        if (!drop) {
            // The bound Win32 INI reader does not strip a UTF-8 BOM before
            // recognizing the first section. Emit a plain UTF-8 profile.
            size_t rawBegin = begin;
            if (!begin && offset >= 3 && !memcmp(source, "\xef\xbb\xbf", 3)) rawBegin += 3;
            memcpy(out + used, source + rawBegin, offset - rawBegin); used += offset - rawBegin;
        }
        else if (!written[section - 1]) {
            if (!setting(out, capacity, &used, keys[section - 1], values[section - 1], newline)) goto fail;
            written[section - 1] = 1;
        }
    }
    if (section && !written[section - 1]) {
        if (!setting(out, capacity, &used, keys[section - 1], values[section - 1], newline)) goto fail;
        written[section - 1] = 1;
    }
    if (used && out[used - 1] != '\n') { strcpy(out + used, newline); used += strlen(newline); }
    for (unsigned i = 0; i < 3; ++i) if (!written[i]) {
        int n = snprintf(out + used, capacity - used, "[%s]%s%s=%s%s", sections[i], newline, keys[i], values[i], newline);
        if (n < 0 || (size_t)n >= capacity - used) goto fail;
        used += (size_t)n;
    }
    free(archives); out[used] = 0; *out_bytes = used;
    return out;
fail:
    free(archives); free(out); return NULL;
}
