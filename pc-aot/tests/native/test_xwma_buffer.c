/* Authored packet/resampler stand-ins around the actual receive/decode bodies.
 * This checks buffer ownership and ordering, not codec or device audio quality. */
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AVERROR(e) (-(e))
#define AVERROR_EOF (-12345)
#define LIBAVUTIL_VERSION_MAJOR 58
#define AV_SAMPLE_FMT_S16 1
typedef struct { int channels; } AVChannelLayout;
typedef struct { uint8_t *data; int size; int64_t pts, dts; } AVPacket;
typedef struct { int nb_samples, format, sample_rate; AVChannelLayout ch_layout; uint8_t **extended_data; } AVFrame;
typedef struct { int channels; } SwrContext;
typedef struct { int pending, ended, packet_index; } Codec;
typedef struct {
    Codec *codec; AVFrame *frame; SwrContext *swr;
    int channels, rate, block_align, finished; int64_t next_pts;
} XWmaDecoder;
static SwrContext resampler;
static int wanted, available, seed, mode, resets, unrefs;
static uint64_t digest = UINT64_C(1469598103934665603), calls, copied, successes, cases, streaming_calls;
static size_t live_size, size_limit;
static void *live;
static int deny_growth, deny_shrink, failed_growth, failed_shrink;
static void hash(uint64_t n) { digest = (digest ^ n) * UINT64_C(1099511628211); }
static void *pcm_realloc(void *p, size_t n) {
    assert(p == live); assert(n); ++calls;
    if ((size_limit && n > size_limit) || (deny_growth && p && n > live_size)) {
        ++failed_growth; errno = ENOMEM; return NULL;
    }
    if (deny_shrink && p && n < live_size) { ++failed_shrink; errno = ENOMEM; return NULL; }
    /* Always move: exercise pointer invalidation and count worst-case copies. */
    void *q = malloc(n); assert(q);
    if (p) { size_t keep = n < live_size ? n : live_size; memcpy(q, p, keep); copied += keep; free(p); }
    live = q; live_size = n; ++successes;
    return q;
}
static void pcm_free(void *p) { assert(p == live); free(p); live = NULL; live_size = 0; }
static int avcodec_is_open(Codec *c) { return c != NULL; }
static AVPacket *av_packet_alloc(void) { return calloc(1, sizeof(AVPacket)); }
static int av_new_packet(AVPacket *p, int n) { p->data = calloc(1, (size_t)n); p->size = n; return p->data ? 0 : -1; }
static void av_packet_unref(AVPacket *p) { free(p->data); p->data = NULL; }
static void av_packet_free(AVPacket **p) { av_packet_unref(*p); free(*p); *p = NULL; }
static int avcodec_send_packet(Codec *c, AVPacket *p) {
    if (!p) { c->ended = 1; c->pending = 1; wanted = available = 17; seed = 51; return 0; }
    assert(!c->pending); hash((uint64_t)p->pts); hash((uint64_t)p->dts);
    ++c->packet_index;
    if (mode == 1 && c->packet_index == 2) return -7;
    wanted = mode == 7 && c->packet_index % 3 == 0 ? 0 : 32 + p->data[0] * 16;
    available = wanted - p->data[p->size - 1] % 7;
    seed = p->data[0]; c->pending = 1; return 0;
}
static int avcodec_receive_frame(Codec *c, AVFrame *f) {
    if (!c->pending) return c->ended ? AVERROR_EOF : AVERROR(EAGAIN);
    if (mode == 2 && c->packet_index == 2) return -8;
    c->pending = 0; f->nb_samples = available; return 0;
}
static void av_frame_unref(AVFrame *f) { (void)f; ++unrefs; }
static void av_channel_layout_default(AVChannelLayout *l, int n) { l->channels = n; }
static void av_channel_layout_uninit(AVChannelLayout *l) { (void)l; }
static int swr_alloc_set_opts2(SwrContext **s, AVChannelLayout *out, int fmt, int rate,
                             AVChannelLayout *in, int infmt, int inrate, int flags, void *ctx) {
    (void)fmt; (void)rate; (void)in; (void)infmt; (void)inrate; (void)flags; (void)ctx;
    resampler.channels = out->channels; *s = &resampler; return mode == 4 ? -1 : 0;
}
static int swr_init(SwrContext *s) { (void)s; return 0; }
static int swr_get_out_samples(SwrContext *s, int n) {
    (void)s; (void)n; return mode == 5 ? 0 : mode == 6 ? -1 : wanted;
}
static int swr_convert(SwrContext *s, uint8_t **out, int count, const uint8_t **in, int n) {
    (void)in; (void)n; if (mode == 3) return -9;
    assert(available <= count);
    int16_t *p = (int16_t *)*out;
    for (int i = 0; i < available * s->channels; ++i) p[i] = (int16_t)((seed + i * 13) & 32767);
    return available;
}
static void xwma_reset(XWmaDecoder *d) {
    ++resets; memset(d->codec, 0, sizeof(*d->codec)); d->swr = NULL; d->finished = 0; d->next_pts = 0;
}
#define realloc pcm_realloc
#define free pcm_free
/* ACTUAL_DECODE */
#undef realloc
#undef free

static void run_case(int channels, int packets, int pattern, int failure_mode, int limit_kind, int split) {
    uint64_t initial_calls = calls;
    Codec codec = {0}; AVFrame frame = {0};
    XWmaDecoder d = {.codec=&codec, .frame=&frame, .channels=channels, .rate=48000, .block_align=2};
    uint8_t data[514]; assert(packets * 2 <= (int)sizeof(data));
    for (int i=0;i<packets;++i) { data[i*2] = pattern ? (uint8_t)(i*37) : 0; data[i*2+1] = pattern ? (uint8_t)(i*7+3) : 0; }
    mode = failure_mode; deny_growth = limit_kind == 1; deny_shrink = limit_kind == 2;
    size_limit = limit_kind == 3 ? 128 : 0;
    resets = unrefs = 0;
    for (int repeat=0;repeat<2;++repeat) {
        int stride = split ? 2 : packets*2;
        for (int offset=0;offset<packets*2;offset+=stride) {
            int16_t *pcm = (void *)1; uint32_t frames = UINT32_MAX;
            int end = limit_kind == 3 ? 0 : offset+stride == packets*2;
            errno = EINTR;
            int rc = xwma_decode(&d, data+offset, (size_t)stride, (unsigned)(stride/2), end, &pcm, &frames);
            hash((unsigned)errno);
            hash((uint64_t)(int64_t)rc); hash(frames); hash((uint64_t)d.next_pts); hash(d.finished);
            if (rc < 0) { assert(!pcm && !frames && !live); }
            else {
                assert(frames <= live_size / sizeof(int16_t) / channels);
                for (size_t i=0;i<(size_t)frames*channels;++i) hash((uint16_t)pcm[i]);
                if (pcm) pcm_free(pcm);
            }
            ++cases;
        }
    }
    hash(resets); hash(unrefs); assert(!live);
    if (split) streaming_calls += calls - initial_calls;
}
int main(void) {
    const int counts[] = {1,2,3,17,257};
    for(int ch=1;ch<=8;++ch) for(unsigned n=0;n<sizeof(counts)/sizeof(*counts);++n)
        for(int pattern=0;pattern<2;++pattern) for(int split=0;split<2;++split)
            run_case(ch,counts[n],pattern,0,0,split);
    /* Error cleanup, EOS reuse, zero output, exact-fit fallback and failed trim. */
    for(int failure=1;failure<=6;++failure) run_case(2,3,1,failure,0,0);
    run_case(2,3,1,0,1,0);
    run_case(2,17,1,0,2,0);
    run_case(1,3,0,7,3,0);
    /* Reject a frame-count overflow before allocation or pointer arithmetic. */
    Codec codec = {.pending=1}; AVFrame frame = {0};
    XWmaDecoder d = {.codec=&codec, .frame=&frame, .swr=&resampler, .channels=8};
    int16_t *pcm = NULL; uint32_t frames = UINT32_MAX;
    mode = 0; wanted = available = 1;
    uint64_t before = calls;
    /* OVERFLOW_RECEIVE */
    assert(rc < 0 && !pcm && frames == UINT32_MAX && calls == before);
    printf("{\"cases\":%" PRIu64 ",\"digest\":\"%016" PRIx64 "\",\"alloc_calls\":%" PRIu64
           ",\"copied_bytes\":%" PRIu64 ",\"streaming_calls\":%" PRIu64 ",\"growth_failures\":%d,\"shrink_failures\":%d}\n",
           cases,digest,calls,copied,streaming_calls,failed_growth,failed_shrink);
    return 0;
}
