/* SPDX-License-Identifier: GPL-3.0-only
 * Import-time conversion of MOD textures and Special Edition archives.
 * Uses bcdec (MIT / public domain) to decode and stb_dxt (public domain) to
 * encode; both are pinned header-only checkouts under vendor/. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "mod_optimize.h"
#include "mod_analyze.h"
#include "mod_archive.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#include "bcdec.h"
#define STB_DXT_IMPLEMENTATION
#define STB_DXT_STATIC
#include "stb_dxt.h"

static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static unsigned lvl(unsigned v, unsigned i) { v >>= i; return v ? v : 1; }

/* Legacy (pre-DX10) header the classic engine's D3D9 loader understands. */
static void legacy_header(unsigned char *h, unsigned format, unsigned w, unsigned hgt, unsigned mips) {
    memset(h, 0, 128);
    memcpy(h, "DDS ", 4);
    put32(h + 4, 124);
    int compressed = format == SKYRIM_DDS_BC1 || format == SKYRIM_DDS_BC2 || format == SKYRIM_DDS_BC3;
    put32(h + 8, 0x1007u | (compressed ? 0x80000u : 0x8u) | (mips > 1 ? 0x20000u : 0));
    put32(h + 12, hgt); put32(h + 16, w);
    put32(h + 20, compressed ? (uint32_t)skyrim_dds_level_bytes(format, w, hgt) : w * 4u);
    put32(h + 28, mips);
    put32(h + 76, 32);
    if (compressed) {
        put32(h + 80, 4);
        memcpy(h + 84, format == SKYRIM_DDS_BC1 ? "DXT1" : format == SKYRIM_DDS_BC2 ? "DXT3" : "DXT5", 4);
    } else {
        put32(h + 80, format == SKYRIM_DDS_BGRA8 ? 0x41 : 0x40);
        put32(h + 88, 32);
        put32(h + 92, 0x00ff0000u); put32(h + 96, 0x0000ff00u); put32(h + 100, 0x000000ffu);
        put32(h + 104, format == SKYRIM_DDS_BGRA8 ? 0xff000000u : 0);
    }
    put32(h + 108, 0x1000u | (mips > 1 ? 0x400008u : 0));
}

/* Decode level 0 to tightly packed RGBA8 (w*h*4). */
static unsigned char *decode_rgba(const unsigned char *src, unsigned format, unsigned w, unsigned h) {
    size_t pw = (w + 3) & ~3u, ph = (h + 3) & ~3u;
    unsigned char *out = malloc((size_t)w * h * 4);
    if (!out) return NULL;
    if (format == SKYRIM_DDS_RGBA8 || format == SKYRIM_DDS_BGRA8 || format == SKYRIM_DDS_BGRX8 || format == SKYRIM_DDS_BGR8) {
        size_t px = format == SKYRIM_DDS_BGR8 ? 3 : 4;
        for (size_t i = 0; i < (size_t)w * h; ++i) {
            const unsigned char *s = src + i * px;
            unsigned char *d = out + i * 4;
            if (format == SKYRIM_DDS_RGBA8) { memcpy(d, s, 4); continue; }
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
            d[3] = format == SKYRIM_DDS_BGRA8 ? s[3] : 255;
        }
        return out;
    }
    unsigned char *pad = malloc(pw * ph * 4);
    if (!pad) { free(out); return NULL; }
    size_t block = skyrim_dds_level_bytes(format, 4, 4);
    const unsigned char *b = src;
    for (size_t by = 0; by < ph; by += 4)
        for (size_t bx = 0; bx < pw; bx += 4, b += block) {
            unsigned char *dst = pad + (by * pw + bx) * 4;
            int pitch = (int)(pw * 4);
            if (format == SKYRIM_DDS_BC1) bcdec_bc1(b, dst, pitch);
            else if (format == SKYRIM_DDS_BC2) bcdec_bc2(b, dst, pitch);
            else if (format == SKYRIM_DDS_BC3) bcdec_bc3(b, dst, pitch);
            else if (format == SKYRIM_DDS_BC7) bcdec_bc7(b, dst, pitch);
            else {
                unsigned char tmp[32];
                if (format == SKYRIM_DDS_BC4) bcdec_bc4(b, tmp, 4); else bcdec_bc5(b, tmp, 8);
                for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x) {
                    unsigned char *d = dst + y * pitch + x * 4;
                    if (format == SKYRIM_DDS_BC4) { d[0] = d[1] = d[2] = tmp[y * 4 + x]; d[3] = 255; continue; }
                    double nx = tmp[(y * 4 + x) * 2] / 127.5 - 1, ny = tmp[(y * 4 + x) * 2 + 1] / 127.5 - 1;
                    double nz = sqrt(fmax(0.0, 1.0 - nx * nx - ny * ny));
                    d[0] = tmp[(y * 4 + x) * 2]; d[1] = tmp[(y * 4 + x) * 2 + 1];
                    d[2] = (unsigned char)lround((nz * 0.5 + 0.5) * 255); d[3] = 255;
                }
            }
        }
    for (size_t y = 0; y < h; ++y) memcpy(out + y * w * 4, pad + y * pw * 4, (size_t)w * 4);
    free(pad);
    return out;
}

/* 2x2 box filter to half size (odd edges repeat). */
static unsigned char *half(const unsigned char *src, unsigned w, unsigned h, unsigned *nw, unsigned *nh) {
    *nw = w > 1 ? w / 2 : 1; *nh = h > 1 ? h / 2 : 1;
    unsigned char *out = malloc((size_t)*nw * *nh * 4);
    if (!out) return NULL;
    for (unsigned y = 0; y < *nh; ++y) for (unsigned x = 0; x < *nw; ++x) {
        unsigned x0 = x * 2 < w ? x * 2 : w - 1, x1 = x * 2 + 1 < w ? x * 2 + 1 : x0;
        unsigned y0 = y * 2 < h ? y * 2 : h - 1, y1 = y * 2 + 1 < h ? y * 2 + 1 : y0;
        for (int c = 0; c < 4; ++c) {
            unsigned s = src[((size_t)y0 * w + x0) * 4 + c] + src[((size_t)y0 * w + x1) * 4 + c] +
                         src[((size_t)y1 * w + x0) * 4 + c] + src[((size_t)y1 * w + x1) * 4 + c];
            out[((size_t)y * *nw + x) * 4 + c] = (unsigned char)((s + 2) / 4);
        }
    }
    return out;
}

static size_t encode_level(unsigned char *dst, const unsigned char *rgba, unsigned w, unsigned h, int alpha) {
    unsigned char *o = dst, block[64];
    for (unsigned by = 0; by < h; by += 4)
        for (unsigned bx = 0; bx < w; bx += 4) {
            for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 4; ++x) {
                unsigned sx = bx + x < w ? bx + x : w - 1, sy = by + y < h ? by + y : h - 1;
                memcpy(block + (y * 4 + x) * 4, rgba + ((size_t)sy * w + sx) * 4, 4);
            }
            stb_compress_dxt_block(o, block, alpha, STB_DXT_HIGHQUAL);
            o += alpha ? 16 : 8;
        }
    return (size_t)(o - dst);
}

static unsigned char *transcode(const unsigned char *src, const SkyrimDDSInfo *info, unsigned max_size, size_t *out_len) {
    unsigned w = info->width, h = info->height;
    unsigned char *img = decode_rgba(src, info->format, w, h);
    if (!img) return NULL;
    while (max_size && (w > max_size || h > max_size) && (w > 1 || h > 1)) {
        unsigned nw, nh;
        unsigned char *next = half(img, w, h, &nw, &nh);
        free(img);
        if (!(img = next)) return NULL;
        w = nw; h = nh;
    }
    int alpha = 0;
    for (size_t i = 0; i < (size_t)w * h && !alpha; ++i) alpha = img[i * 4 + 3] != 255;
    unsigned format = alpha ? SKYRIM_DDS_BC3 : SKYRIM_DDS_BC1, mips = 1;
    size_t total = 128;
    for (unsigned lw = w, lh = h;; ++mips) {
        total += skyrim_dds_level_bytes(format, lw, lh);
        if (lw == 1 && lh == 1) break;
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    unsigned char *out = malloc(total);
    if (!out) { free(img); return NULL; }
    legacy_header(out, format, w, h, mips);
    unsigned char *o = out + 128;
    for (unsigned i = 0; i < mips; ++i) {
        o += encode_level(o, img, w, h, alpha);
        if (i + 1 == mips) break;
        unsigned nw, nh;
        unsigned char *next = half(img, w, h, &nw, &nh);
        free(img);
        if (!(img = next)) { free(out); return NULL; }
        w = nw; h = nh;
    }
    free(img);
    *out_len = (size_t)(o - out);
    return out;
}

unsigned char *skyrim_texture_convert(const unsigned char *dds, size_t length, unsigned max_size,
                                      size_t *out_length, int *action) {
    SkyrimDDSInfo info;
    *action = SKYRIM_TEX_KEPT; *out_length = 0;
    if (skyrim_dds_info(dds, length, &info) || !info.width || !info.height ||
        info.width > 16384 || info.height > 16384) { *action = SKYRIM_TEX_ERROR; return NULL; }
    unsigned f = info.format, w = info.width, h = info.height, m = info.mips > 16 ? 16 : info.mips;
    if (info.cube_or_volume || f == SKYRIM_DDS_OTHER || f == SKYRIM_DDS_UNKNOWN || f == SKYRIM_DDS_BC6H) {
        *action = info.dx10 ? SKYRIM_TEX_UNSUPPORTED : SKYRIM_TEX_KEPT;
        return NULL;
    }
    size_t chain = 0;
    for (unsigned i = 0; i < m; ++i) chain += skyrim_dds_level_bytes(f, lvl(w, i), lvl(h, i));
    if (info.data_offset + skyrim_dds_level_bytes(f, w, h) > length) { *action = SKYRIM_TEX_ERROR; return NULL; }
    if (info.data_offset + chain > length) m = 1;                 /* truncated chain: rebuild */
    const unsigned char *data = dds + info.data_offset;
    unsigned big = w > h ? w : h;
    int over = max_size && big > max_size;
    int raw = f == SKYRIM_DDS_RGBA8 || f == SKYRIM_DDS_BGRA8 || f == SKYRIM_DDS_BGRX8 || f == SKYRIM_DDS_BGR8;
    int must = f == SKYRIM_DDS_BC7 || f == SKYRIM_DDS_BC5 || f == SKYRIM_DDS_BC4 || f == SKYRIM_DDS_BGR8 ||
               (raw && big >= 1024) || (m <= 1 && big > 256) || (over && m <= 1);
    if (must) {
        unsigned char *out = transcode(data, &info, max_size, out_length);
        *action = out ? SKYRIM_TEX_TRANSCODED : SKYRIM_TEX_ERROR;
        return out;
    }
    unsigned drop = 0;
    while (over && drop + 1 < m && (lvl(w, drop) > max_size || lvl(h, drop) > max_size)) ++drop;
    if (!drop && !info.dx10 && f != SKYRIM_DDS_RGBA8) return NULL;   /* already fine */
    size_t skip = 0, keep = 0;
    for (unsigned i = 0; i < m; ++i) {
        size_t b = skyrim_dds_level_bytes(f, lvl(w, i), lvl(h, i));
        if (i < drop) skip += b; else keep += b;
    }
    unsigned outf = f == SKYRIM_DDS_RGBA8 ? SKYRIM_DDS_BGRA8 : f;
    unsigned char *out = malloc(128 + keep);
    if (!out) { *action = SKYRIM_TEX_ERROR; return NULL; }
    legacy_header(out, outf, lvl(w, drop), lvl(h, drop), m - drop);
    memcpy(out + 128, data + skip, keep);
    if (f == SKYRIM_DDS_RGBA8)
        for (size_t i = 0; i < keep; i += 4) { unsigned char t = out[128 + i]; out[128 + i] = out[130 + i]; out[130 + i] = t; }
    *out_length = 128 + keep;
    *action = drop ? SKYRIM_TEX_TRIMMED : SKYRIM_TEX_REHEADERED;
    return out;
}

static unsigned char *read_all(const char *path, size_t limit, size_t *n) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    unsigned char *b = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size <= limit && (b = malloc((size_t)st.st_size + 1))) {
        size_t got = 0;
        while (got < (size_t)st.st_size) {
            ssize_t r = read(fd, b + got, (size_t)st.st_size - got);
            if (r <= 0) { free(b); b = NULL; break; }
            got += (size_t)r;
        }
        *n = got;
    }
    close(fd);
    return b;
}
static int write_replace(const char *path, const unsigned char *b, size_t n) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.skyrimios-tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) return errno;
    size_t done = 0;
    while (done < n) {
        ssize_t r = write(fd, b + done, n - done);
        if (r <= 0) { close(fd); unlink(tmp); return EIO; }
        done += (size_t)r;
    }
    if (close(fd) || rename(tmp, path)) { int e = errno; unlink(tmp); return e; }
    return 0;
}

int skyrim_texture_optimize(const char *path, unsigned max_size) {
    size_t n = 0, out_n = 0;
    unsigned char *b = read_all(path, 512u << 20, &n);
    if (!b) return SKYRIM_TEX_ERROR;
    int action;
    unsigned char *out = skyrim_texture_convert(b, n, max_size, &out_n, &action);
    free(b);
    if (out) {
        if (write_replace(path, out, out_n)) action = SKYRIM_TEX_ERROR;
        free(out);
    }
    return action;
}

/* ---- LZ4 ---------------------------------------------------------------- */
static long lz4_block(const unsigned char *s, size_t n, unsigned char *d, size_t cap, size_t prefix) {
    size_t i = 0, o = prefix;
    while (i < n) {
        unsigned token = s[i++];
        size_t lit = token >> 4;
        if (lit == 15) { unsigned char c; do { if (i >= n) return -1; c = s[i++]; lit += c; } while (c == 255); }
        if (i + lit > n || o + lit > cap) return -1;
        memcpy(d + o, s + i, lit); i += lit; o += lit;
        if (i == n) break;                              /* last sequence: literals only */
        if (i + 2 > n) return -1;
        size_t off = (size_t)s[i] | (size_t)s[i + 1] << 8; i += 2;
        size_t len = (token & 15) + 4;
        if ((token & 15) == 15) { unsigned char c; do { if (i >= n) return -1; c = s[i++]; len += c; } while (c == 255); }
        if (!off || off > o || o + len > cap) return -1;
        for (size_t k = 0; k < len; ++k, ++o) d[o] = d[o - off];
    }
    return (long)(o - prefix);
}
long skyrim_lz4_frame_decode(const unsigned char *s, size_t n, unsigned char *d, size_t cap) {
    if (n < 7 || le32(s) != 0x184D2204u) return -1;
    unsigned flg = s[4];
    if ((flg >> 6) != 1) return -1;
    size_t i = 6;
    if (flg & 0x08) i += 8;           /* content size */
    if (flg & 0x01) i += 4;           /* dictionary id */
    i += 1;                           /* header checksum */
    size_t o = 0;
    while (i + 4 <= n) {
        uint32_t size = le32(s + i); i += 4;
        if (!size) return (long)o;
        size_t bs = size & 0x7fffffffu;
        if (i + bs > n) return -1;
        if (size & 0x80000000u) {
            if (o + bs > cap) return -1;
            memcpy(d + o, s + i, bs); o += bs;
        } else {
            /* Blocks may reference earlier output (linked blocks): decode in place. */
            long r = lz4_block(s + i, bs, d, cap, o);
            if (r < 0) return -1;
            o += (size_t)r;
        }
        i += bs;
        if (flg & 0x10) i += 4;       /* block checksum */
    }
    return -1;
}

/* ---- Special Edition BSA -------------------------------------------------- */
int skyrim_bsa_extract_se(const char *bsa_path, const char *data_dir, unsigned *files_out) {
    *files_out = 0;
    int fd = open(bsa_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return errno; }
    uint64_t size = (uint64_t)st.st_size;
    unsigned char h[36];
    if (pread(fd, h, 36, 0) != 36 || memcmp(h, "BSA\0", 4) || le32(h + 4) != 105) { close(fd); return EINVAL; }
    uint32_t flags = le32(h + 12), folders = le32(h + 16), files = le32(h + 20);
    uint32_t name_total = le32(h + 28);
    if (folders > 100000 || files > 1000000 || !(flags & 1) || !(flags & 2)) { close(fd); return EINVAL; }
    /* Folder records, then per-folder name + file records, then file names. */
    size_t dir_bytes = (size_t)folders * 24;
    unsigned char *fr = malloc(dir_bytes ? dir_bytes : 1);
    if (!fr || pread(fd, fr, dir_bytes, 36) != (ssize_t)dir_bytes) { free(fr); close(fd); return EINVAL; }
    uint64_t pos = 36 + dir_bytes;
    typedef struct { char folder[256]; uint32_t size, offset; } Rec;
    Rec *recs = calloc(files ? files : 1, sizeof *recs);
    int e = recs ? 0 : ENOMEM;
    uint32_t r = 0;
    for (uint32_t f = 0; f < folders && !e; ++f) {
        uint32_t count = le32(fr + f * 24 + 8);
        unsigned char len;
        char folder[256];
        if (pread(fd, &len, 1, (off_t)pos) != 1 || !len || pread(fd, folder, len, (off_t)pos + 1) != len) { e = EINVAL; break; }
        folder[len - 1] = 0; pos += 1 + len;
        for (uint32_t k = 0; k < count && !e; ++k, ++r) {
            unsigned char rec[16];
            if (r >= files || pread(fd, rec, 16, (off_t)pos) != 16) { e = EINVAL; break; }
            memcpy(recs[r].folder, folder, len);
            recs[r].size = le32(rec + 8); recs[r].offset = le32(rec + 12);
            pos += 16;
        }
    }
    free(fr);
    char *names = e ? NULL : malloc(name_total + 1);
    if (!e && (!names || pread(fd, names, name_total, (off_t)pos) != (ssize_t)name_total)) e = EINVAL;
    const char *np = names, *nend = names ? names + name_total : NULL;
    unsigned char *packed = NULL, *plain = NULL;
    for (uint32_t i = 0; i < r && !e; ++i) {
        const char *nul = memchr(np, 0, (size_t)(nend - np));
        if (!nul) { e = EINVAL; break; }
        char rel[600], safe[600];
        snprintf(rel, sizeof rel, "%s\\%s", recs[i].folder, np);
        np = nul + 1;
        if (skyrim_zip_normalize(rel, strlen(rel), 1, safe, sizeof safe) != 1) continue;
        char out[4800];
        snprintf(out, sizeof out, "%s/%s", data_dir, safe);
        if (!access(out, F_OK)) continue;                       /* loose file wins */
        uint32_t sz = recs[i].size & 0x3fffffffu;
        int compressed = ((flags & 4) != 0) != ((recs[i].size & 0x40000000u) != 0);
        uint64_t at = recs[i].offset;
        if (at + sz > size || sz > (512u << 20)) { e = EINVAL; break; }
        free(packed); packed = malloc(sz ? sz : 1);
        if (!packed || pread(fd, packed, sz, (off_t)at) != (ssize_t)sz) { e = EINVAL; break; }
        size_t p = 0;
        if (flags & 0x100) { if (!sz) { e = EINVAL; break; } p = 1 + packed[0]; }
        if (p > sz) { e = EINVAL; break; }
        const unsigned char *body = packed + p; size_t body_n = sz - p;
        if (compressed) {
            if (body_n < 4) { e = EINVAL; break; }
            uint32_t original = le32(body);
            if (original > (1u << 30)) { e = EINVAL; break; }
            free(plain); plain = malloc(original ? original : 1);
            if (!plain || skyrim_lz4_frame_decode(body + 4, body_n - 4, plain, original) != (long)original) { e = EINVAL; break; }
            body = plain; body_n = original;
        }
        if (skyrim_make_parents(data_dir, safe)) { e = EIO; break; }
        int out_fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (out_fd < 0) { e = errno; break; }
        if (write(out_fd, body, body_n) != (ssize_t)body_n) e = EIO;
        if (close(out_fd) && !e) e = EIO;
        if (!e) ++*files_out;
    }
    free(packed); free(plain); free(names); free(recs);
    close(fd);
    return e;
}
