/* Actual patched engine loop, authored SDL queue and notification provider. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
typedef uint32_t Uint32;
typedef struct { int unused; } CPU;
static CPU cpu;
static unsigned queued, clears, paused, pauses, resumes, events, passes;
static int inactive;
static int g_dev = 1, g_rate = 48000, g_running = 1;
#define PASS_FRAMES_MS 10
#define LOG(...) ((void)0)
static CPU *rt_new_cpu(void) { return &cpu; }
static void rt_thread_init(CPU *c) { assert(c == &cpu); }
static uint64_t now_us(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static unsigned SDL_GetQueuedAudioSize(int dev) { assert(dev == 1); return queued; }
static void SDL_ClearQueuedAudio(int dev) { assert(dev == 1); ++clears; queued = 0; }
static void SDL_PauseAudioDevice(int dev, int pause) {
    assert(dev == 1); paused = !!pause;
    if (pause) { ++pauses; assert(queued > 0); }
    else ++resumes;
}
static void SDL_QueueAudio(int dev, const void *bytes, unsigned size) {
    assert(dev == 1 && bytes && size > 0 && !paused);
    queued += size; inactive = 1;
}
static int skyrim_lifecycle_audio_should_pause(void) { return inactive; }
static void skyrim_lifecycle_audio_wait(void) {
    assert(paused && queued > 0 && g_running == 1);
    inactive = 0; /* Explicit authored resume, no injected PCM/cursor changes. */
}
static void skyrim_lifecycle_audio_event(int pause) {
    assert(pause == (events == 0)); ++events;
}
static void engine_pass(CPU *c, int frames, float *out) {
    assert(c == &cpu && frames == 480 && out && !paused);
    if (++passes == 2) {
        assert(queued == 480 * 2 * sizeof(float));
        assert(!clears && pauses == 1 && resumes == 1 && events == 2 && g_running == 1);
        free(out); pthread_exit(NULL);
    }
}
/* ACTUAL_HELPER */
/* ACTUAL_ENGINE */
int main(void) {
    pthread_t worker; assert(!pthread_create(&worker, NULL, engine_main, NULL));
    assert(!pthread_join(worker, NULL)); assert(passes == 2);
    puts("lifecycle queued_audio_preserved=PASS guest_running_preserved=PASS real_sdl=0");
}
