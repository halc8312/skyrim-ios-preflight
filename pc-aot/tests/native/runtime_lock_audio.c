#include <stdio.h>
#include <stdlib.h>
#include "runtime_lock_metrics_fixture.h"

typedef struct { int result; unsigned resets; } XWmaDecoder;
/* ACTUAL_TYPES */
typedef struct { uint32_t args[4], result; Voice *voice; } CPU;
#define SHIM(name) static void name(CPU *c)
#define ARG(n) c->args[n]
#define V c->voice
#define RET(value, nargs) do { c->result = (uint32_t)(value); return; } while (0)
#define LOG(...) ((void)0)
static uint32_t memory[4096];
#define MEM ((uint8_t *)memory)
#define M32(at) memory[(uint32_t)(at) / 4]
static pthread_mutex_t g_lock;
static int g_running;
static int waiter_entered;
static FIXTURE_TLS int watch_wait;
static int fixture_blocking_lock(pthread_mutex_t *mutex) {
    if (watch_wait) __atomic_store_n(&waiter_entered, 1, __ATOMIC_RELEASE);
    return pthread_mutex_lock(mutex);
}
#define pthread_mutex_lock fixture_blocking_lock
/* ACTUAL_AUDIO_HELPER */
#undef pthread_mutex_lock

/* ACTUAL_ADPCM */
static int xwma_decode(XWmaDecoder *d, const uint8_t *data, size_t bytes,
                      unsigned packets, int end, int16_t **pcm, uint32_t *frames) {
    assert(ios_audio_lock_state.depth != 0);
    assert(data == MEM + 512 && bytes == 8 && packets == 1 && end == 1);
    if (d->result < 0) { *pcm = NULL; *frames = 0; return d->result; }
    *pcm = malloc(4 * sizeof(int16_t)); assert(*pcm);
    for (unsigned i = 0; i < 4; ++i) (*pcm)[i] = (int16_t)(13 * i - 17);
    *frames = 4;
    return 0;
}
static void xwma_reset(XWmaDecoder *d) {
    assert(ios_audio_lock_state.depth != 0);
    if (d) ++d->resets;
    errno = EDOM;
}
/* ACTUAL_XWMA_OBSERVER */
/* ACTUAL_SUBMIT */
/* ACTUAL_FLUSH */
/* ACTUAL_GET_STATE */

static void assert_released(void) {
    assert(ios_audio_lock_state.depth == 0);
    assert(atomic_load_explicit(&ios_audio_lock_owner, memory_order_relaxed) == 0);
    assert(pthread_mutex_trylock(&g_lock) == 0);
    assert(pthread_mutex_unlock(&g_lock) == 0);
}

static void *state_reader(void *argument) {
    CPU *cpu = argument;
    watch_wait = 1;
    xs_GetState(cpu);
    assert_released();
    return NULL;
}

int main(void) {
    pthread_mutexattr_t attributes;
    assert(pthread_mutexattr_init(&attributes) == 0);
    assert(pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE) == 0);
    assert(pthread_mutex_init(&g_lock, &attributes) == 0);
    assert(pthread_mutexattr_destroy(&attributes) == 0);
    fixture_clear();
    errno = EINTR;
    assert(ios_audio_lock(SKYRIM_LOCK_OP_AUDIO_SUBMIT) == 0 && errno == EINTR);
    uint64_t outer_start = ios_audio_lock_state.start;
    unsigned clocks = fixture_clock_count;
    assert(ios_audio_lock(SKYRIM_LOCK_OP_AUDIO_OTHER) == 0 && errno == EINTR);
    assert(ios_audio_lock_state.depth == 2 && ios_audio_lock_state.start == outer_start);
    assert(fixture_clock_count == clocks && fixture_event_count == 0);
    assert(ios_audio_unlock() == 0 && errno == EINTR && fixture_event_count == 0);
    assert(ios_audio_unlock() == 0 && errno == EINTR);
    assert(fixture_count(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_HOLD, SKYRIM_LOCK_OP_AUDIO_SUBMIT) == 1);
    assert_released();

    Voice voice = {0}; XWmaDecoder decoder = {0};
    voice.wma = &decoder; voice.in_ch = 1; voice.rate = 32000; voice.bpf = 2;
    CPU cpu = {{0, 64, 128, 0}, 0, &voice};
    M32(64) = 0x40; M32(68) = 8; M32(72) = 512; M32(96) = 123;
    M32(132) = 1;
    voice.fmt = FMT_ADPCM; voice.block_align = 8; voice.spb = 4; voice.ncoef = 1;
    voice.adpcm_coef[1] = 1; /* authored coefficient 256, 0 */
    const uint8_t block[8] = {0, 16, 0, 100, 0, 50, 0, 0x10};
    memcpy(MEM + 512, block, sizeof block);
    fixture_clear(); errno = EINTR;
    xs_SubmitSourceBuffer(&cpu);
    assert(cpu.result == 0 && errno == EINTR && voice.qn == 1);
    const int16_t expected[4] = {50, 100, 116, 116};
    assert(voice.q[0].nframes == 4 && !memcmp(voice.q[0].decoded, expected, sizeof expected));
    assert(fixture_count(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_WORK, SKYRIM_LOCK_OP_ADPCM_DECODE) == 1);
    assert(fixture_count(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_HOLD, SKYRIM_LOCK_OP_AUDIO_SUBMIT) == 1);
    assert_released();
    xs_FlushSourceBuffers(&cpu);
    assert(cpu.result == 0 && voice.qn == 0 && decoder.resets == 1 && errno == EDOM);
    assert(fixture_count(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_WORK, SKYRIM_LOCK_OP_AUDIO_RESET) == 1);
    assert_released();

    voice.fmt = FMT_XWMA;
    xs_SubmitSourceBuffer(&cpu);
    assert(cpu.result == 0 && voice.qn == 1 && voice.q[0].decoded[3] == 22);
    cpu.args[1] = 256; xs_GetState(&cpu);
    assert(M32(256) == 123 && M32(260) == 1);
    cpu.args[1] = 64;
    decoder.result = -7;
    xs_SubmitSourceBuffer(&cpu);
    assert(cpu.result == 0x80004005u && voice.qn == 1);
    assert_released();
    xs_FlushSourceBuffers(&cpu); assert(voice.qn == 0 && decoder.resets == 2);
    voice.qn = MAX_QUEUE;
    xs_SubmitSourceBuffer(&cpu);
    assert(cpu.result == 0x88960001u && voice.qn == MAX_QUEUE);
    assert_released(); voice.qn = 0;

    fixture_clear();
    const uint64_t holder = skyrim_perf_thread_ordinal();
    assert(ios_audio_lock(SKYRIM_LOCK_OP_AUDIO_SUBMIT) == 0);
    cpu.args[1] = 256;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, state_reader, &cpu) == 0);
    fixture_wait_flag(&waiter_entered);
    assert(ios_audio_unlock() == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(fixture_count(SKYRIM_LOCK_AUDIO, SKYRIM_LOCK_WAIT, SKYRIM_LOCK_OP_AUDIO_GET_STATE) == 1);
    for (unsigned i = 0; i < fixture_event_count; ++i) if (fixture_events[i].phase == SKYRIM_LOCK_WAIT) {
        assert(fixture_events[i].owner_thread == holder && fixture_events[i].thread != holder);
        assert(fixture_events[i].owner_operation == SKYRIM_LOCK_OP_AUDIO_SUBMIT);
    }
    assert_released();
    assert(pthread_mutex_destroy(&g_lock) == 0);
    puts("audio_lock recursive=PASS adpcm=PASS submit_failure=PASS queue_full=PASS flush=PASS get_state_wait=PASS errno=PASS");
}
