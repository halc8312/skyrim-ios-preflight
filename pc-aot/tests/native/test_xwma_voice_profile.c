/* Exercise actual adapter setup/reset against an observable codec boundary.
 * Audio decoding itself is covered by the optional real-FFmpeg differential. */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LIBAVUTIL_VERSION_MAJOR 58
#define AV_INPUT_BUFFER_PADDING_SIZE 64
enum AVCodecID { AV_CODEC_ID_WMAV2, AV_CODEC_ID_WMAPRO };
typedef struct { int num, den; } AVRational;
typedef struct { int nb_channels; } AVChannelLayout;
typedef struct { int unused; } AVFrame;
typedef struct { int unused; } SwrContext;
typedef struct { int unused; } AVCodec;
typedef struct {
    int sample_rate, block_align, bits_per_coded_sample, extradata_size;
    int64_t bit_rate;
    AVRational pkt_timebase;
    AVChannelLayout ch_layout;
    uint8_t *extradata;
} AVCodecContext;
typedef struct XWmaDecoder XWmaDecoder;
static int opened, freed, flushed, fail_open;
static AVCodec implementation;
static const AVCodec *avcodec_find_decoder(enum AVCodecID id) {
    (void)id; return &implementation;
}
static AVCodecContext *avcodec_alloc_context3(const AVCodec *codec) {
    (void)codec; return calloc(1, sizeof(AVCodecContext));
}
static int avcodec_open2(AVCodecContext *c, const AVCodec *codec, void *options) {
    (void)c; (void)codec; (void)options; ++opened; return fail_open ? -1 : 0;
}
static void avcodec_free_context(AVCodecContext **c) {
    if (*c) { free((*c)->extradata); free(*c); ++freed; *c = NULL; }
}
static void avcodec_flush_buffers(AVCodecContext *c) { assert(c); ++flushed; }
static int avcodec_is_open(AVCodecContext *c) { return c != NULL; }
static void av_channel_layout_default(AVChannelLayout *c, int channels) {
    c->nb_channels = channels;
}
static void *av_mallocz(size_t bytes) { return calloc(1, bytes); }
static AVFrame *av_frame_alloc(void) { return calloc(1, sizeof(AVFrame)); }
static void av_frame_free(AVFrame **frame) { free(*frame); *frame = NULL; }
static void swr_free(SwrContext **swr) { free(*swr); *swr = NULL; }
void xwma_destroy(XWmaDecoder *d);

/* ACTUAL_SETUP_RESET */

static XWmaDecoder *make(unsigned tag, int channels, int rate, int avg,
                        int align, int bits, const uint8_t *extra, size_t length) {
    XWmaDecoder *d = xwma_create(tag, channels, rate, avg, align, bits, extra, length);
    assert(d && d->codec); return d;
}

int main(void) {
    XWmaDecoder *d = make(0x161, 1, 32000, 2500, 1280, 16, NULL, 0);
    assert(d->codec->extradata_size == 6 && d->codec->extradata[4] == 0x17);
    int before_open = opened, before_free = freed, before_flush = flushed;
    d->finished = 1; d->next_pts = 1234;
    xwma_reset(d);
    assert(opened == before_open + 1 && freed == before_free + 1);
    assert(flushed == before_flush && d->codec->extradata[4] == 0x17);
    assert(!d->finished && !d->next_pts);
    before_open = opened;
    assert(xwma_set_rate(d, 32000) == 0 && opened == before_open);
    assert(xwma_set_rate(d, 0) < 0 && d->rate == 32000);
    assert(xwma_set_rate(d, 44100) == 0 && d->codec->extradata[4] == 31);
    assert(xwma_set_rate(d, 32000) == 0 && d->codec->extradata[4] == 0x17);
    fail_open = 1; xwma_reset(d); fail_open = 0;
    assert(!d->codec); xwma_destroy(d);

    /* Near misses and bitrate aliases must keep the original interpretation. */
    const int formats[][5] = {
        {1, 44100, 4000, 1487, 16}, {2, 44100, 6000, 2230, 16},
        {2, 32000, 2500, 1280, 16}, {1, 22050, 2500, 1280, 16},
        {1, 32000, 2501, 1280, 16}, {1, 32000, 2500, 1279, 16},
        {1, 32000, 2500, 1280, 8}, {1, 32000, 6000, 1280, 16}
    };
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        const int *f = formats[i];
        d = make(0x161, f[0], f[1], f[2], f[3], f[4], NULL, 0);
        assert(d->codec->extradata[4] == 31);
        before_open = opened; before_flush = flushed;
        xwma_reset(d);
        assert(opened == before_open && flushed == before_flush + 1);
        xwma_destroy(d);
    }
    d = make(0x161, 2, 44100, 4000, 1485, 16, NULL, 0);
    assert(d->codec->extradata[4] == 31);
    before_open = opened; before_flush = flushed;
    xwma_reset(d);
    assert(opened == before_open + 1 && flushed == before_flush);
    xwma_destroy(d);
    /* Caller-supplied codec options always win, including explicit 0x1f. */
    const uint8_t explicit_options[] = {1, 2, 3, 4, 31, 0};
    d = make(0x161, 1, 32000, 2500, 1280, 16, explicit_options, 6);
    assert(!memcmp(d->codec->extradata, explicit_options, 6));
    before_open = opened; before_flush = flushed;
    xwma_reset(d);
    assert(opened == before_open && flushed == before_flush + 1);
    xwma_destroy(d);
    d = make(0x162, 1, 32000, 2500, 1280, 16, NULL, 0);
    assert(d->codec->extradata_size == 18 && d->codec->extradata[14] == 224);
    xwma_destroy(d);
    assert(!xwma_create(0x160, 1, 32000, 2500, 1280, 16, NULL, 0));
    assert(!xwma_create(0x161, 1, 32000, 2500, 1280, 16, NULL, 6));
    assert(opened == freed);
    return 0;
}
