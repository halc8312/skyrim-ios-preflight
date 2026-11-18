/* Deterministic authored SDL sink and clock around the real engine loop. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef uint32_t Uint32;
typedef struct { int unused; } CPU;
#define PASS_FRAMES_MS 10
static int g_rate=48000,g_running=1,g_dev=1;
static CPU cpu;
static uint64_t clock_us, next_pull_ns, period_ns, queued, pull_bytes;
static uint64_t underruns, sleep_calls, queue_queries, sleep_total, mix_count, queued_count;
static uint64_t digest, last_packet, cleared, paused, stall_notice, resume_notice;
static unsigned samples, phase_us, scenario, work_us, oversleep_us, interrupt_every;
static CPU *rt_new_cpu(void) {return &cpu;}
static void rt_thread_init(CPU *c) {assert(c==&cpu);}
static uint64_t now_us(void) {return clock_us;}
static void update_device(void) {
    while(next_pull_ns<=clock_us*1000) {
        uint64_t time=next_pull_ns/1000;
        int suspended=scenario==1 && time>=500000 && time<1400000;
        int stopped=scenario==2 && time>=300000 && time<700000;
        if(!suspended) {
            if(queued<pull_bytes && mix_count>5 && !stopped && scenario==0) ++underruns;
            queued=queued<pull_bytes ? 0 : queued-pull_bytes;
        }
        next_pull_ns+=period_ns;
    }
    g_running=!(scenario==2 && clock_us>=300000 && clock_us<700000);
}
static Uint32 SDL_GetQueuedAudioSize(int dev) {
    assert(dev==1);++queue_queries;update_device();assert(queued<=UINT32_MAX);return (Uint32)queued;
}
static const char *SDL_GetCurrentAudioDriver(void) {return "authored";}
static int SDL_GetAudioDeviceStatus(int dev) {(void)dev;return 1;}
static void SDL_ClearQueuedAudio(int dev) {assert(dev==1);queued=0;++cleared;}
static void SDL_PauseAudioDevice(int dev,int value) {assert(dev==1 && !value);++paused;}
static int SDL_QueueAudio(int dev,const void *data,Uint32 bytes) {
    assert(dev==1 && bytes==(unsigned)(g_rate*PASS_FRAMES_MS/1000)*2*sizeof(float));
    const float *pcm=data;
    uint64_t packet=(uint64_t)pcm[0];
    assert(packet>last_packet && packet==mix_count && pcm[bytes/sizeof(float)-1]==pcm[0]);
    last_packet=packet;digest=digest*131+packet;queued+=bytes;++queued_count;return 0;
}
static void engine_pass(CPU *c,int frames,float *out) {
    assert(c==&cpu && frames==g_rate*PASS_FRAMES_MS/1000);
    ++mix_count;for(int i=0;i<frames*2;i++)out[i]=(float)mix_count;
    clock_us+=work_us;update_device();
}
static int fixture_usleep(useconds_t amount) {
    /* Recovery's existing timer may request >10 ms after an interrupted sleep.
     * Queue-delay bounds are tested separately; do not impose a new contract. */
    assert(amount && amount<=1000000);++sleep_calls;sleep_total+=amount;
    if(interrupt_every && sleep_calls%interrupt_every==0)clock_us+=amount/2;
    else clock_us+=amount+oversleep_us;
    update_device();assert(clock_us<UINT64_C(30000000));return 0;
}
static void fixture_log(const char *format,...) {
    if(strstr(format,"stalled"))++stall_notice;
    if(strstr(format,"resumed"))++resume_notice;
}
#define LOG fixture_log
#define usleep fixture_usleep
/* ACTUAL_HELPER_AND_ENGINE */
#undef usleep

int main(int argc,char **argv) {
    assert(argc==8);
    g_rate=atoi(argv[1]);samples=(unsigned)atoi(argv[2]);phase_us=(unsigned)atoi(argv[3]);
    scenario=(unsigned)atoi(argv[4]);work_us=(unsigned)atoi(argv[5]);
    oversleep_us=(unsigned)atoi(argv[6]);interrupt_every=(unsigned)atoi(argv[7]);
    if(scenario==3)g_dev=0;
    clock_us=7;pull_bytes=(uint64_t)samples*2*sizeof(float);
    period_ns=(uint64_t)samples*1000000000/g_rate;next_pull_ns=period_ns+(uint64_t)phase_us*1000;
#if FIXTURE_NEW
    ios_audio_device_samples=samples;
    ios_audio_device_rate=g_rate;
    assert(ios_audio_backpressure_delay(0,480,48000)==1000);
    assert(ios_audio_backpressure_delay(UINT32_MAX,480,48000)==(samples<=1440?5000:1000));
    assert(ios_audio_backpressure_delay(UINT32_MAX,-1,48000)==1000);
    assert(ios_audio_backpressure_delay(UINT32_MAX,480,0)==1000);
    unsigned saved=ios_audio_device_samples;ios_audio_device_samples=0;
    assert(ios_audio_backpressure_delay(UINT32_MAX,480,48000)==1000);
    ios_audio_device_samples=saved;
#endif
    engine_main(NULL);
    assert(mix_count==300);
    if(scenario==0)assert(queued_count==300 && !cleared && !paused && !stall_notice && !resume_notice);
    if(scenario==1)assert(stall_notice==1 && resume_notice==1 && cleared && paused);
    if(scenario==2)assert(queued_count==300 && !stall_notice && !resume_notice);
    if(scenario==3)assert(!queued_count && !queue_queries && !cleared && !paused);
    printf("digest=%llu packets=%llu underflows=%llu sleeps=%llu queries=%llu time_us=%llu stall=%llu resume=%llu clears=%llu restarts=%llu\n",
        (unsigned long long)digest,(unsigned long long)queued_count,(unsigned long long)underruns,
        (unsigned long long)sleep_calls,(unsigned long long)queue_queries,(unsigned long long)clock_us,
        (unsigned long long)stall_notice,(unsigned long long)resume_notice,
        (unsigned long long)cleared,(unsigned long long)paused);
    return 0;
}
