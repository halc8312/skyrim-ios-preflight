/* Substitute instrumented game resources, a monotonic clock, and the final
 * dictionary insertion. The staged importer and real profile lookup follow. */
#include "classic_skse_translation_list.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <strings.h>
#include <time.h>
typedef int CPU;
static const char *g_save_dir, *expected_language, *guest_language;
static uint32_t translated_loader, mock_loader = 1, references = 2;
static pthread_mutex_t translation_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned ticks, skyui_requests, reserved, calls, inserted, trial, resource_ready;
#define M32(x) ((void)(x), mock_loader)
#define GPTR(x) ((void)(x), &references)
#define GSTR(x) ((void)(x), guest_language)

const char *skyrim_mobile_classic_text_language(void) { return expected_language; }
static int skse_skyui_enabled(void) { return trial; }
static uint32_t skse_ini_setting(CPU *c, const char *name) {
    (void)c; assert(!strcmp(name, "sLanguage:General")); return guest_language ? 1 : 0;
}
static uint32_t skse_virtual(CPU *c, uint32_t obj, unsigned slot, int count, const uint32_t *args) {
    (void)c; (void)obj; (void)count; (void)args;
    assert(slot == 3); ++references; return 1;
}
static int fake_clock(clockid_t clock, struct timespec *value) {
    assert(clock == CLOCK_MONOTONIC);
    value->tv_sec = ticks; value->tv_nsec = 0;
    return 0;
}
#define clock_gettime fake_clock

static int contains(const char *text, const char *part) {
    for (; *text; ++text) if (!strncasecmp(text, part, strlen(part))) return 1;
    return 0;
}
static uint32_t open_translation_stream(CPU *c, const char *path) {
    (void)c; assert(contains(path, "SkyUI_") && !reserved);
    ++skyui_requests;
    return reserved = resource_ready;
}
static void close_translation_stream(CPU *c, uint32_t stream) {
    (void)c; assert(reserved == stream && stream); reserved = 0;
}
static unsigned import_file(CPU *c, uint32_t translator, const char *path, uint32_t stream) {
    assert(translator && strstr(path, "Interface\\Translations\\") == path);
    const char *language = expected_language ? expected_language : guest_language;
    assert(language && contains(path, language));
    if (stream) { assert(contains(path, "SkyUI_")); close_translation_stream(c, stream); }
    ++calls;
    if (contains(path, "None_")) return 0;
    ++inserted;
    puts(path);
    return 1;
}
