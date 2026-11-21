/* The app's Fuz hooks with an authored guest memory, TEB and BSIStream. The
 * game resource lookup runs through the app's rt_call header. */
#define _DEFAULT_SOURCE
#include "mod_fuz_host.h"
#include "classic_vm_epoch.h"
#include "funcs_exe.h"
#include "lift_prelude.h"

#include <assert.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

uint8_t *MEM;

enum {
    SLOT = 2,
    TEB = SKYRIM_FUZ_TEB_BASE + SLOT * SKYRIM_FUZ_TEB_STRIDE,
    STACK_LO = SKYRIM_FUZ_STACK_BASE + SLOT * SKYRIM_FUZ_STACK_SIZE,
    STACK_HI = STACK_LO + SKYRIM_FUZ_STACK_SIZE - SKYRIM_FUZ_STACK_TOP_RESERVE,
    LIVE = STACK_HI - 0x1000, FRAME = LIVE + SKYRIM_FUZ_LIFT_FRAME_FROM_SLOT,
    HEAP = 0x20000000, TITLE_A = HEAP, TITLE_B = HEAP + 0x40,
    RESPONSE_A = HEAP + 0x100, RESPONSE_B = HEAP + 0x140, VOICE_A = HEAP + 0x200, VOICE_B = HEAP + 0x300,
    MANAGER = HEAP + 0x400, SELECTION = HEAP + 0x440, NODE = HEAP + 0x480, TOPIC = HEAP + 0x4c0,
    BSISTREAM_VTABLE = 0x0111eaf8, STREAM = 0x5000
};

static const char line[] = "The gate remains closed until sunrise";  /* 6 words: 6/2+1 = 4 s */
static const char voiced[] = "Data\\Sound\\Voice\\Fixture.esp\\Narrator\\Voiced.wav";
static const char silent[] = "Data\\Sound\\Voice\\Fixture.esp\\Narrator\\Silent.wav";
static const char game_stock[] = "Data\\Sound\\Voice\\Fuz Ro Doh\\Stock_4.xwm";

static struct {
    const char *const *present;
    const char *unreadable;
    unsigned probes, calls, destroys;
    bool reenter;
    PcGuestModHooks *table;
} catalog;

static uint32_t argument(const CPU *cpu, unsigned index) { return M32(cpu->r[4] + 4 * (index + 1)); }

/* The two reviewed routines, reached by original address as rt_call would. */
void rt_call(CPU *cpu, uint32_t target) {
    ++catalog.calls;
    assert(cpu->fs_base == TEB);
    const uint32_t object = cpu->r[1];
    if (target == 0x00B08630u) {
        const char *requested = (const char *)MEM + argument(cpu, 0);
        ++catalog.probes;
        if (catalog.reenter) {
            SkyrimFuzLiftChoice inner = {0};
            catalog.table->fuz_menu.choose_menu_voice(cpu, LIVE, RESPONSE_B, &inner);
            assert(!inner.assigned_path[0]);
        }
        M32(object) = BSISTREAM_VTABLE;
        M32(object + 4) = 0;
        M8(object + 8) = 0;
        M32(object + 12) = argument(cpu, 0);
        for (const char *const *p = catalog.present; p && *p; ++p) {
            if (strcmp(*p, requested)) continue;
            M32(object + 4) = STREAM;
            M8(object + 8) = !(catalog.unreadable && !strcmp(catalog.unreadable, requested));
        }
        cpu->r[0] = object;
        cpu->r[4] += 12;
    } else if (target == 0x00B08970u) {
        assert(M32(object) == BSISTREAM_VTABLE);
        ++catalog.destroys;
        memset(MEM + object, 0, 16);
        cpu->r[0] = object;
        cpu->r[4] += 8;
    } else {
        abort();
    }
}

static void set_response(uint32_t response, uint32_t title) {
    M32(response) = title;
    M32(response + 4) = (uint32_t)(sizeof line - 1) | ((uint32_t)sizeof line << 16);
    M32(response + 0x10) = 0;
}

/* The staged constructor: hook, then the original assignment, then the note. */
static bool assign(PcGuestModHooks *t, const CPU *cpu, uint32_t response, const char *path, uint32_t voice) {
    M32(LIVE) = FRAME;
    memset(MEM + FRAME, 0x5b, SKYRIM_FUZ_LIFT_FRAME_BYTES);
    strcpy((char *)MEM + FRAME, path);
    catalog.probes = catalog.destroys = 0;
    SkyrimFuzLiftChoice choice = {0};
    t->fuz_menu.choose_menu_voice(cpu, LIVE, response, &choice);
    assert(catalog.probes == catalog.destroys);
    strcpy((char *)MEM + voice, (const char *)MEM + FRAME);
    M32(response + 0x10) = voice;
    t->fuz_menu.note_menu_assignment(response, &choice);
    return choice.assigned_path[0] != 0;
}

static void expect_counts(uint64_t substituted, uint64_t kept, uint64_t missing, uint64_t errors) {
    SkyrimFuzHostCounts c;
    skyrim_fuz_app_counts(&c);
    assert(c.substituted == substituted && c.kept == kept && c.missing == missing && c.errors == errors);
}

static void select_response(uint32_t response) {
    M32(MANAGER + 0x1c) = SELECTION;
    M32(SELECTION + 0x20) = NODE;
    M32(SELECTION + 0x18) = TOPIC;
    M32(TOPIC + 0x24) = 0;
    M32(NODE) = response;
}

int main(void) {
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(MEM != MAP_FAILED && !mprotect(MEM, SKYRIM_FUZ_GUEST_LOW_GUARD, PROT_NONE));
    M32(TEB + 4) = STACK_HI;
    M32(TEB + 8) = STACK_LO;
    strcpy((char *)MEM + TITLE_A, line);
    strcpy((char *)MEM + TITLE_B, line);
    set_response(RESPONSE_A, TITLE_A);
    set_response(RESPONSE_B, TITLE_B);
    const CPU cpu = {.fs_base = TEB, .r = {[3] = 0x12345678}};

    PcGuestModHooks table = {.version = PC_GUEST_MOD_HOOKS_ABI, .bytes = sizeof table,
                             .groups = PC_GUEST_MOD_HOOK_FUZ_MENU | PC_GUEST_MOD_HOOK_FUZ_WORLD};
    skyrim_fuz_app_fill(&table);
    assert(pc_guest_mod_hooks_table_valid(&table));
    catalog.table = &table;
    uint32_t limit, base;
    assert(skyrim_fuz_app_stack(TEB, &limit, &base) && limit == STACK_LO && base == STACK_HI);
    assert(!skyrim_fuz_app_stack(TEB + 4, &limit, &base) && !skyrim_fuz_app_stack(SKYRIM_FUZ_TEB_BASE - 0x2000, &limit, &base));
    assert(!skyrim_fuz_app_stack(SKYRIM_FUZ_TEB_BASE + SKYRIM_FUZ_STACK_SLOTS * SKYRIM_FUZ_TEB_STRIDE, &limit, &base));

    /* Not configured: nothing is looked up or written. */
    static const char *const stock_and_voiced[] = {"Sound/Voice/Fuz Ro Doh/Stock_4.xwm",
                                                   "Sound/Voice/Fixture.esp/Narrator/Voiced.fuz", NULL};
    static const char *const packed_and_voiced[] = {"Sound/Voice/Fuz Ro Doh/Stock_4.fuz",
                                                    "Sound/Voice/Fixture.esp/Narrator/Voiced.fuz", NULL};
    catalog.present = stock_and_voiced;
    assert(!assign(&table, &cpu, RESPONSE_B, silent, VOICE_B) && !catalog.calls);
    assert(!strcmp((const char *)MEM + VOICE_B, silent));
    expect_counts(0, 0, 0, 0);

    SkyrimFuzSettings settings = {.words_per_second = 2, .skip_empty_responses = 1};
    assert(skyrim_fuz_app_configure(&settings) == 0 && skyrim_fuz_app_configure(&settings) != 0);
    assert(skyrim_vm_epoch_subscribe(skyrim_fuz_app_retire, NULL));
    skyrim_vm_epoch_registered(0x7000);

    /* The 6.1 package only has a .fuz file. Assignment still uses the .xwm name. */
    catalog.present = packed_and_voiced;
    assert(!assign(&table, &cpu, RESPONSE_A, voiced, VOICE_A));
    assert(!strcmp((const char *)MEM + VOICE_A, voiced));
    expect_counts(0, 1, 0, 0);
    assert(assign(&table, &cpu, RESPONSE_B, silent, VOICE_B));
    assert(!strcmp((const char *)MEM + VOICE_B, game_stock));
    assert(catalog.probes == 5);
    expect_counts(1, 1, 0, 0);
    select_response(RESPONSE_A);
    SkyrimFuzMenuScope scope = table.fuz_menu.capture_menu(MANAGER, NODE, TITLE_A);
    assert(scope.response == RESPONSE_A && !scope.serial);
    assert(!table.fuz_menu.force_menu(&scope, 0x00675caa, TITLE_A));
    select_response(RESPONSE_B);
    scope = table.fuz_menu.capture_menu(MANAGER, NODE, TITLE_B);
    assert(scope.serial == 1 && table.fuz_menu.force_menu(&scope, 0x00675caa, TITLE_B));
    assert(!table.fuz_menu.force_menu(&scope, 0x00675c06, TITLE_B));  /* another caller */
    SkyrimFuzWorldScope world = table.fuz_world.capture_world(RESPONSE_B, VOICE_B);
    assert(world.serial == 1);
    assert(!table.fuz_world.capture_world(RESPONSE_A, VOICE_A).serial);

    /* A revert clears the marks; a later mark gets a new serial. */
    skyrim_vm_epoch_before_revert(SKYRIM_VM_REVERT_LOAD);
    skyrim_vm_epoch_after_load(1);
    scope = table.fuz_menu.capture_menu(MANAGER, NODE, TITLE_B);
    assert(!scope.serial && !table.fuz_menu.force_menu(&scope, 0x00675caa, TITLE_B));
    assert(!table.fuz_world.capture_world(RESPONSE_B, VOICE_B).serial);
    catalog.present = stock_and_voiced;
    assert(assign(&table, &cpu, RESPONSE_B, silent, VOICE_B));
    assert(catalog.probes == 4);
    assert(table.fuz_menu.capture_menu(MANAGER, NODE, TITLE_B).serial == 2);
    table.fuz_menu.forget_response(RESPONSE_B);
    assert(!table.fuz_menu.capture_menu(MANAGER, NODE, TITLE_B).serial);
    expect_counts(2, 1, 0, 0);

    /* No stock clip: the original path stays. An unreadable stock clip is a lookup error. */
    static const char *const nothing[] = {NULL};
    catalog.present = nothing;
    assert(!assign(&table, &cpu, RESPONSE_B, silent, VOICE_B));
    assert(!strcmp((const char *)MEM + VOICE_B, silent));
    expect_counts(2, 1, 1, 0);
    catalog.present = stock_and_voiced;
    catalog.unreadable = stock_and_voiced[0];
    assert(!assign(&table, &cpu, RESPONSE_B, silent, VOICE_B));
    assert(catalog.probes == 4);
    expect_counts(2, 1, 1, 1);
    catalog.unreadable = NULL;

    /* Refused before any guest call: another TEB, a stack too small, a reentrant call,
     * and a response below the guarded low pages. */
    unsigned calls = catalog.calls;
    CPU other = cpu;
    other.fs_base = TEB + SKYRIM_FUZ_TEB_STRIDE;  /* its TEB words do not match the slot */
    SkyrimFuzLiftChoice choice = {0};
    table.fuz_menu.choose_menu_voice(&other, LIVE, RESPONSE_B, &choice);
    table.fuz_menu.choose_menu_voice(&cpu, STACK_LO + 0x100, RESPONSE_B, &choice);
    table.fuz_menu.choose_menu_voice(&cpu, STACK_HI - 8, RESPONSE_B, &choice);
    assert(catalog.calls == calls && !choice.assigned_path[0]);
    expect_counts(2, 1, 1, 4);
    M32(LIVE) = FRAME;
    strcpy((char *)MEM + FRAME, silent);
    table.fuz_menu.choose_menu_voice(&cpu, LIVE, 0x100, &choice);
    assert(!choice.assigned_path[0] && !strcmp((const char *)MEM + FRAME, silent));
    catalog.reenter = true;
    assert(assign(&table, &cpu, RESPONSE_B, silent, VOICE_B));
    catalog.reenter = false;
    SkyrimFuzHostCounts counts;
    skyrim_fuz_app_counts(&counts);
    assert(counts.substituted == 3 && counts.kept == 1 && counts.missing == 1 && counts.errors == 9);  /* one refusal per nested probe */

    /* The report is numeric and matches the upload allowlist. */
    fflush(stderr);
    char path[] = "/tmp/fuz-report-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    const int saved = dup(2);
    dup2(fd, 2);
    skyrim_fuz_app_report(1);
    fflush(stderr);
    dup2(saved, 2);
    char report[256] = {0};
    assert(pread(fd, report, sizeof report - 1, 0) > 0);
    close(fd);
    unlink(path);
    regex_t pattern;
    assert(!regcomp(&pattern, "^IOS_FUZ schema=1 enabled=1 substituted=3 kept=1 missing=1 errors=9\n$", REG_EXTENDED));
    assert(!regexec(&pattern, report, 0, NULL, 0));
    regfree(&pattern);
    puts("Fuz app hooks kept original voices, substituted stock clips and cleared marks on revert.");
    return 0;
}
