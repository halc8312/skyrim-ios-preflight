/* Public mixer bodies are inserted by the driver. All PCM/graphs are authored. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef struct { int unused; } CPU;
typedef struct { int unused; } XWmaDecoder;
/* ACTUAL_TYPES */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Voice *g_voices[MAX_VOICES], *g_master;
static uint32_t g_engine_cb[16];
static int g_nengine_cb, g_rate = 48000;
static uint8_t memory[4 * 1024 * 1024];
#define MEM memory
static uint16_t word16(uint32_t at) { uint16_t n; memcpy(&n, MEM + at, 2); return n; }
static uint32_t word32(uint32_t at) { uint32_t n; memcpy(&n, MEM + at, 4); return n; }
static float wordf(uint32_t at) { float n; memcpy(&n, MEM + at, 4); return n; }
#define M8(at) MEM[(uint32_t)(at)]
#define M16(at) word16((uint32_t)(at))
#define M32(at) word32((uint32_t)(at))
#define MF32(at) wordf((uint32_t)(at))
static void put32(uint32_t at, uint32_t v) { memcpy(MEM + at, &v, 4); }
static uint64_t digest = UINT64_C(14695981039346656037), route_count, callback_count;
static unsigned current_case, current_pass, active_slots;
static void hash_bytes(const void *p, size_t size) {
    const uint8_t *b = p;
    for (size_t i = 0; i < size; ++i) digest = (digest ^ b[i]) * UINT64_C(1099511628211);
}
static void hash_u64(uint64_t v) { hash_bytes(&v, sizeof v); }
static void xwma_destroy(XWmaDecoder *p) { assert(p == NULL); }
static uint32_t rt_invoke(CPU *c, uint32_t fn, int nargs, const uint32_t *args) {
    (void)c;
    assert(pthread_mutex_trylock(&g_lock) == 0);
    pthread_mutex_unlock(&g_lock);
    hash_u64(fn); hash_u64((unsigned)nargs);
    hash_bytes(args, (size_t)nargs * 4);
    ++callback_count;
    if (fn == 1000 && g_voices[1]) {
        // Graph changes in pass-start callbacks must affect this very pass.
        g_voices[1]->stage = (int)((current_case + current_pass * 17) % 64);
        g_voices[1]->volume = current_pass == 1 ? 0.375f : 0.75f;
    }
    if (fn == 1001 && current_pass == 2 && active_slots > 2) {
        // End callbacks may retire a voice; actual engine_pass performs cleanup.
        Voice *v = g_voices[active_slots - 1];
        if (v && v != g_master) {
            for (unsigned i = 0; i < active_slots; ++i)
                if (g_voices[i]) for (int s = 0; s < g_voices[i]->nsends; ++s)
                    if (g_voices[i]->send[s] == v) g_voices[i]->send[s] = NULL;
            v->dead = 1;
        }
    }
    return 0;
}
/* ACTUAL_MIXER */

static uint32_t rng_state;
static uint32_t rnd(void) { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; return rng_state ^= rng_state << 5; }
static void setup(unsigned count, unsigned serial) {
    current_case = serial; active_slots = count;
    rng_state = serial + 712371;
    g_nengine_cb = 2; g_engine_cb[0] = 32; g_engine_cb[1] = 40;
    put32(32, 128); put32(40, 160);
    put32(128, 1000); put32(132, 1001); put32(160, 1002); put32(164, 1003);
    g_master = NULL;
    for (unsigned i = 0; i < count; ++i) {
        if (serial % 3 == 0 && i % 7 == 3) continue; // Empty slots among live voices.
        Voice *v = calloc(1, sizeof *v); assert(v);
        g_voices[i] = v; v->guest = i + 1;
        v->kind = i == 0 ? V_MASTER : i % 5 == 0 ? V_SOURCE : V_SUBMIX;
        v->dead = i > 1 && i % 23 == 0;
        v->stage = (int)(rnd() % 69) - 2; // Includes ignored negative and >=64 stages.
        if (serial == 128) v->stage = 0;
        if (serial == 129) v->stage = 63;
        if (serial == 130) v->stage = (int)(i % 64);
        if (serial == 131) v->stage = INT_MAX;
        v->in_ch = 1 + (int)(rnd() % MAX_CH); v->rate = 24000 + (int)(rnd() % 3) * 24000;
        v->volume = (float)(rnd() % 13) / 16.0f; v->ratio = 0.5f + (float)(rnd() % 5) / 4.0f;
        v->running = i % 11 != 0;
        for (int ch = 0; ch < MAX_CH; ++ch) v->chvol[ch] = (float)(ch + 3) / 11.0f;
        if (v->kind != V_SOURCE) {
            v->acc = calloc(MAX_CH * 64, sizeof(float)); assert(v->acc);
        } else {
            v->fmt = FMT_PCM16; v->bpf = v->in_ch * 2; v->qn = 2;
            v->callback = 512 + i * 4;
            put32(v->callback, 8192 + i * 32);
            for (unsigned m = 0; m < 7; ++m) put32(8192 + i * 32 + 4 * m, 2000 + i * 8 + m);
            for (int b = 0; b < 2; ++b) {
                XBuf *x = &v->q[b];
                x->data = 65536 + i * 1024 + (unsigned)b * 512;
                x->nframes = x->end = 23; x->ctx = i * 2 + (unsigned)b;
                x->flags = b == 1 ? 0x40 : 0;
                x->loops_left = b == 0 ? 2 : 0; x->loop_begin = 3; x->loop_len = 7;
                for (int sample = 0; sample < 23 * v->in_ch; ++sample) {
                    int16_t pcm = (int16_t)rnd();
                    memcpy(MEM + x->data + sample * 2, &pcm, 2);
                }
            }
        }
        if (i == 0) g_master = v;
    }
    for (unsigned i = 1; i < count; ++i) {
        Voice *v = g_voices[i]; if (!v) continue;
        v->nsends = i % 4 == 0 ? 0 : 2;
        for (int send = 0; send < v->nsends; ++send) {
            Voice *dest = g_master;
            for (unsigned j = (i + 1); j < count; ++j) {
                Voice *candidate = g_voices[j];
                if (candidate && !candidate->dead && candidate->kind == V_SUBMIX && candidate->stage > v->stage) { dest = candidate; break; }
            }
            v->send[send] = send == 0 ? dest : g_master;
            v->matrix_set[send] = i % 3 == 0;
            for (unsigned k = 0; k < MAX_CH * MAX_CH; ++k)
                v->matrix[send][k] = (float)((int)(rnd() % 9) - 4) / 32.0f;
        }
    }
}
static void cleanup(void) {
    for (unsigned i = 0; i < MAX_VOICES; ++i) {
        Voice *v = g_voices[i]; if (!v) continue;
        free(v->acc); free(v); g_voices[i] = NULL;
    }
    g_master = NULL;
}
int main(void) {
    CPU cpu = {0}; float out[2 * 64];
    for (unsigned scenario = 0; scenario < 132; ++scenario) {
        unsigned count = scenario < 128 ? scenario % 47 : MAX_VOICES;
        setup(count, scenario);
        for (current_pass = 0; current_pass < 4; ++current_pass) {
            int frames = 1 + (int)((scenario * 7 + current_pass * 11) % 48);
            memset(out, 0x85, sizeof out);
            engine_pass(&cpu, frames, out);
            hash_bytes(out, sizeof out);
            hash_u64((unsigned)g_nev);
            for (unsigned i = 0; i < count; ++i) {
                Voice *v = g_voices[i]; hash_u64(v != NULL);
                if (!v) continue;
                hash_u64(v->played); hash_u64((unsigned)v->qn);
                if (v->acc) hash_bytes(v->acc, MAX_CH * 64 * sizeof(float));
            }
        }
        cleanup();
    }
    assert(route_count > 10000 && callback_count > 1000);
    printf("pcm_graph_cases=132 callback_lock=PASS routes=%" PRIu64 " callbacks=%" PRIu64 " digest=%016" PRIx64 "\n",
           route_count, callback_count, digest);
}
