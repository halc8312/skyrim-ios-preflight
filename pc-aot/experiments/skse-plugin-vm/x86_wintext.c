/* SPDX-License-Identifier: GPL-3.0-only */
#include "x86_wintext.h"

#include <limits.h>
#include <string.h>

enum {
    MB_PRECOMPOSED = 0x01, MB_COMPOSITE = 0x02, MB_USEGLYPHCHARS = 0x04,
    MB_ERR_INVALID_CHARS = 0x08,
    WC_DISCARDNS = 0x10, WC_SEPCHARS = 0x20, WC_DEFAULTCHAR = 0x40,
    WC_ERR_INVALID_CHARS = 0x80, WC_COMPOSITECHECK = 0x200,
    WC_NO_BEST_FIT_CHARS = 0x400,
    REPLACEMENT_CHARACTER = 0xfffd
};

static const uint16_t cp1252_controls[] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
    0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
    0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
};

/* Windows reference tests check these tables against the native converters. */
static const struct { uint16_t code; uint8_t byte; } cp1252_best_fit[] = {
    {0x0100, 0x41}, {0x0101, 0x61}, {0x0102, 0x41}, {0x0103, 0x61}, {0x0104, 0x41},
    {0x0105, 0x61}, {0x0106, 0x43}, {0x0107, 0x63}, {0x0108, 0x43}, {0x0109, 0x63},
    {0x010a, 0x43}, {0x010b, 0x63}, {0x010c, 0x43}, {0x010d, 0x63}, {0x010e, 0x44},
    {0x010f, 0x64}, {0x0110, 0xd0}, {0x0111, 0x64}, {0x0112, 0x45}, {0x0113, 0x65},
    {0x0114, 0x45}, {0x0115, 0x65}, {0x0116, 0x45}, {0x0117, 0x65}, {0x0118, 0x45},
    {0x0119, 0x65}, {0x011a, 0x45}, {0x011b, 0x65}, {0x011c, 0x47}, {0x011d, 0x67},
    {0x011e, 0x47}, {0x011f, 0x67}, {0x0120, 0x47}, {0x0121, 0x67}, {0x0122, 0x47},
    {0x0123, 0x67}, {0x0124, 0x48}, {0x0125, 0x68}, {0x0126, 0x48}, {0x0127, 0x68},
    {0x0128, 0x49}, {0x0129, 0x69}, {0x012a, 0x49}, {0x012b, 0x69}, {0x012c, 0x49},
    {0x012d, 0x69}, {0x012e, 0x49}, {0x012f, 0x69}, {0x0130, 0x49}, {0x0131, 0x69},
    {0x0134, 0x4a}, {0x0135, 0x6a}, {0x0136, 0x4b}, {0x0137, 0x6b}, {0x0139, 0x4c},
    {0x013a, 0x6c}, {0x013b, 0x4c}, {0x013c, 0x6c}, {0x013d, 0x4c}, {0x013e, 0x6c},
    {0x0141, 0x4c}, {0x0142, 0x6c}, {0x0143, 0x4e}, {0x0144, 0x6e}, {0x0145, 0x4e},
    {0x0146, 0x6e}, {0x0147, 0x4e}, {0x0148, 0x6e}, {0x014c, 0x4f}, {0x014d, 0x6f},
    {0x014e, 0x4f}, {0x014f, 0x6f}, {0x0150, 0x4f}, {0x0151, 0x6f}, {0x0154, 0x52},
    {0x0155, 0x72}, {0x0156, 0x52}, {0x0157, 0x72}, {0x0158, 0x52}, {0x0159, 0x72},
    {0x015a, 0x53}, {0x015b, 0x73}, {0x015c, 0x53}, {0x015d, 0x73}, {0x015e, 0x53},
    {0x015f, 0x73}, {0x0162, 0x54}, {0x0163, 0x74}, {0x0164, 0x54}, {0x0165, 0x74},
    {0x0166, 0x54}, {0x0167, 0x74}, {0x0168, 0x55}, {0x0169, 0x75}, {0x016a, 0x55},
    {0x016b, 0x75}, {0x016c, 0x55}, {0x016d, 0x75}, {0x016e, 0x55}, {0x016f, 0x75},
    {0x0170, 0x55}, {0x0171, 0x75}, {0x0172, 0x55}, {0x0173, 0x75}, {0x0174, 0x57},
    {0x0175, 0x77}, {0x0176, 0x59}, {0x0177, 0x79}, {0x0179, 0x5a}, {0x017a, 0x7a},
    {0x017b, 0x5a}, {0x017c, 0x7a}, {0x0180, 0x62}, {0x0189, 0xd0}, {0x0191, 0x83},
    {0x0197, 0x49}, {0x019a, 0x6c}, {0x019f, 0x4f}, {0x01a0, 0x4f}, {0x01a1, 0x6f},
    {0x01ab, 0x74}, {0x01ae, 0x54}, {0x01af, 0x55}, {0x01b0, 0x75}, {0x01b6, 0x7a},
    {0x01c0, 0x7c}, {0x01c3, 0x21}, {0x01cd, 0x41}, {0x01ce, 0x61}, {0x01cf, 0x49},
    {0x01d0, 0x69}, {0x01d1, 0x4f}, {0x01d2, 0x6f}, {0x01d3, 0x55}, {0x01d4, 0x75},
    {0x01d5, 0x55}, {0x01d6, 0x75}, {0x01d7, 0x55}, {0x01d8, 0x75}, {0x01d9, 0x55},
    {0x01da, 0x75}, {0x01db, 0x55}, {0x01dc, 0x75}, {0x01de, 0x41}, {0x01df, 0x61},
    {0x01e4, 0x47}, {0x01e5, 0x67}, {0x01e6, 0x47}, {0x01e7, 0x67}, {0x01e8, 0x4b},
    {0x01e9, 0x6b}, {0x01ea, 0x4f}, {0x01eb, 0x6f}, {0x01ec, 0x4f}, {0x01ed, 0x6f},
    {0x01f0, 0x6a}, {0x0261, 0x67}, {0x02b9, 0x27}, {0x02ba, 0x22}, {0x02bc, 0x27},
    {0x02c4, 0x5e}, {0x02c8, 0x27}, {0x02c9, 0xaf}, {0x02ca, 0xb4}, {0x02cb, 0x60},
    {0x02cd, 0x5f}, {0x02da, 0xb0}, {0x0300, 0x60}, {0x0301, 0xb4}, {0x0302, 0x5e},
    {0x0303, 0x7e}, {0x0304, 0xaf}, {0x0305, 0xaf}, {0x0308, 0xa8}, {0x030a, 0xb0},
    {0x030e, 0x22}, {0x0327, 0xb8}, {0x0331, 0x5f}, {0x0332, 0x5f}, {0x037e, 0x3b},
    {0x0393, 0x47}, {0x0398, 0x54}, {0x03a3, 0x53}, {0x03a6, 0x46}, {0x03a9, 0x4f},
    {0x03b1, 0x61}, {0x03b2, 0xdf}, {0x03b4, 0x64}, {0x03b5, 0x65}, {0x03bc, 0xb5},
    {0x03c0, 0x70}, {0x03c3, 0x73}, {0x03c4, 0x74}, {0x03c6, 0x66}, {0x04bb, 0x68},
    {0x0589, 0x3a}, {0x066a, 0x25}, {0x2000, 0x20}, {0x2001, 0x20}, {0x2002, 0x20},
    {0x2003, 0x20}, {0x2004, 0x20}, {0x2005, 0x20}, {0x2006, 0x20}, {0x2007, 0xa0},
    {0x2008, 0x20}, {0x2009, 0x20}, {0x200a, 0x20}, {0x2010, 0x2d}, {0x2011, 0x2d},
    {0x2012, 0x2d}, {0x2015, 0x2d}, {0x2017, 0x3d}, {0x201b, 0x27}, {0x201f, 0x22},
    {0x2024, 0xb7}, {0x202f, 0xa0}, {0x2032, 0x27}, {0x2033, 0x22}, {0x2035, 0x60},
    {0x2036, 0x22}, {0x2044, 0x2f}, {0x2070, 0xb0}, {0x2074, 0x34}, {0x2075, 0x35},
    {0x2076, 0x36}, {0x2077, 0x37}, {0x2078, 0x38}, {0x207f, 0x6e}, {0x2080, 0x30},
    {0x2081, 0x31}, {0x2082, 0x32}, {0x2083, 0x33}, {0x2084, 0x34}, {0x2085, 0x35},
    {0x2086, 0x36}, {0x2087, 0x37}, {0x2088, 0x38}, {0x2089, 0x39}, {0x20a1, 0xa2},
    {0x20a4, 0xa3}, {0x20a7, 0x50}, {0x2102, 0x43}, {0x2107, 0x45}, {0x210a, 0x67},
    {0x210b, 0x48}, {0x210c, 0x48}, {0x210d, 0x48}, {0x210e, 0x68}, {0x2110, 0x49},
    {0x2111, 0x49}, {0x2112, 0x4c}, {0x2113, 0x6c}, {0x2115, 0x4e}, {0x2118, 0x50},
    {0x2119, 0x50}, {0x211a, 0x51}, {0x211b, 0x52}, {0x211c, 0x52}, {0x211d, 0x52},
    {0x2124, 0x5a}, {0x2128, 0x5a}, {0x212a, 0x4b}, {0x212b, 0xc5}, {0x212c, 0x42},
    {0x212d, 0x43}, {0x212e, 0x65}, {0x212f, 0x65}, {0x2130, 0x45}, {0x2131, 0x46},
    {0x2133, 0x4d}, {0x2134, 0x6f}, {0x2205, 0xd8}, {0x2212, 0x2d}, {0x2213, 0xb1},
    {0x2215, 0x2f}, {0x2216, 0x5c}, {0x2217, 0x2a}, {0x2218, 0xb0}, {0x2219, 0xb7},
    {0x221a, 0x76}, {0x221e, 0x38}, {0x2223, 0x7c}, {0x2229, 0x6e}, {0x2236, 0x3a},
    {0x223c, 0x7e}, {0x2248, 0x98}, {0x2261, 0x3d}, {0x2264, 0x3d}, {0x2265, 0x3d},
    {0x226a, 0xab}, {0x226b, 0xbb}, {0x22c5, 0xb7}, {0x2302, 0xa6}, {0x2303, 0x5e},
    {0x2310, 0xac}, {0x2320, 0x28}, {0x2321, 0x29}, {0x2329, 0x3c}, {0x232a, 0x3e},
    {0x2500, 0x2d}, {0x2502, 0xa6}, {0x250c, 0x2b}, {0x2510, 0x2b}, {0x2514, 0x2b},
    {0x2518, 0x2b}, {0x251c, 0x2b}, {0x2524, 0xa6}, {0x252c, 0x2d}, {0x2534, 0x2d},
    {0x253c, 0x2b}, {0x2550, 0x2d}, {0x2551, 0xa6}, {0x2552, 0x2b}, {0x2553, 0x2b},
    {0x2554, 0x2b}, {0x2555, 0x2b}, {0x2556, 0x2b}, {0x2557, 0x2b}, {0x2558, 0x2b},
    {0x2559, 0x2b}, {0x255a, 0x2b}, {0x255b, 0x2b}, {0x255c, 0x2b}, {0x255d, 0x2b},
    {0x255e, 0xa6}, {0x255f, 0xa6}, {0x2560, 0xa6}, {0x2561, 0xa6}, {0x2562, 0xa6},
    {0x2563, 0xa6}, {0x2564, 0x2d}, {0x2565, 0x2d}, {0x2566, 0x2d}, {0x2567, 0x2d},
    {0x2568, 0x2d}, {0x2569, 0x2d}, {0x256a, 0x2b}, {0x256b, 0x2b}, {0x256c, 0x2b},
    {0x2580, 0xaf}, {0x2584, 0x5f}, {0x2588, 0xa6}, {0x258c, 0xa6}, {0x2590, 0xa6},
    {0x2591, 0xa6}, {0x2592, 0xa6}, {0x2593, 0xa6}, {0x25a0, 0xa6}, {0x263c, 0xa4},
    {0x2758, 0x7c}, {0x3000, 0x20}, {0x3008, 0x3c}, {0x3009, 0x3e}, {0x300a, 0xab},
    {0x300b, 0xbb}, {0x301a, 0x5b}, {0x301b, 0x5d}, {0x30fb, 0xb7},
};

static const struct { uint16_t letter, mark; } cp1252_composite[256] = {
    [0x8a] = {0x0053, 0x030c}, [0x8e] = {0x005a, 0x030c}, [0x9a] = {0x0073, 0x030c},
    [0x9e] = {0x007a, 0x030c}, [0x9f] = {0x0059, 0x0308}, [0xc0] = {0x0041, 0x0300},
    [0xc1] = {0x0041, 0x0301}, [0xc2] = {0x0041, 0x0302}, [0xc3] = {0x0041, 0x0303},
    [0xc4] = {0x0041, 0x0308}, [0xc5] = {0x0041, 0x030a}, [0xc7] = {0x0043, 0x0327},
    [0xc8] = {0x0045, 0x0300}, [0xc9] = {0x0045, 0x0301}, [0xca] = {0x0045, 0x0302},
    [0xcb] = {0x0045, 0x0308}, [0xcc] = {0x0049, 0x0300}, [0xcd] = {0x0049, 0x0301},
    [0xce] = {0x0049, 0x0302}, [0xcf] = {0x0049, 0x0308}, [0xd1] = {0x004e, 0x0303},
    [0xd2] = {0x004f, 0x0300}, [0xd3] = {0x004f, 0x0301}, [0xd4] = {0x004f, 0x0302},
    [0xd5] = {0x004f, 0x0303}, [0xd6] = {0x004f, 0x0308}, [0xd8] = {0x004f, 0x0338},
    [0xd9] = {0x0055, 0x0300}, [0xda] = {0x0055, 0x0301}, [0xdb] = {0x0055, 0x0302},
    [0xdc] = {0x0055, 0x0308}, [0xdd] = {0x0059, 0x0301}, [0xe0] = {0x0061, 0x0300},
    [0xe1] = {0x0061, 0x0301}, [0xe2] = {0x0061, 0x0302}, [0xe3] = {0x0061, 0x0303},
    [0xe4] = {0x0061, 0x0308}, [0xe5] = {0x0061, 0x030a}, [0xe7] = {0x0063, 0x0327},
    [0xe8] = {0x0065, 0x0300}, [0xe9] = {0x0065, 0x0301}, [0xea] = {0x0065, 0x0302},
    [0xeb] = {0x0065, 0x0308}, [0xec] = {0x0069, 0x0300}, [0xed] = {0x0069, 0x0301},
    [0xee] = {0x0069, 0x0302}, [0xef] = {0x0069, 0x0308}, [0xf1] = {0x006e, 0x0303},
    [0xf2] = {0x006f, 0x0300}, [0xf3] = {0x006f, 0x0301}, [0xf4] = {0x006f, 0x0302},
    [0xf5] = {0x006f, 0x0303}, [0xf6] = {0x006f, 0x0308}, [0xf8] = {0x006f, 0x0338},
    [0xf9] = {0x0075, 0x0300}, [0xfa] = {0x0075, 0x0301}, [0xfb] = {0x0075, 0x0302},
    [0xfc] = {0x0075, 0x0308}, [0xfd] = {0x0079, 0x0301}, [0xff] = {0x0079, 0x0308},
};

static uint16_t read_u16(const uint8_t *src) {
    return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static void write_u16(uint8_t *dst, uint16_t unit) {
    dst[0] = (uint8_t)unit;
    dst[1] = (uint8_t)(unit >> 8);
}

static bool high_surrogate(uint16_t unit) {
    return unit >= 0xd800 && unit <= 0xdbff;
}

static bool low_surrogate(uint16_t unit) {
    return unit >= 0xdc00 && unit <= 0xdfff;
}

/* Windows 10 build 19045 GetStringTypeW reference ranges for the
 * supported UTF-16 units. The test compares every covered value. */
static const struct { uint16_t first, last; uint16_t flags[3]; } ctype_ranges[] = {
    {0x0000, 0x0008, {0x0220, 0x0000, 0x0000}},
    {0x0009, 0x0009, {0x0268, 0x0009, 0x0008}},
    {0x000a, 0x000a, {0x0228, 0x0008, 0x0008}},
    {0x000b, 0x000b, {0x0228, 0x0009, 0x0008}},
    {0x000c, 0x000c, {0x0228, 0x000a, 0x0008}},
    {0x000d, 0x000d, {0x0228, 0x0008, 0x0008}},
    {0x000e, 0x001b, {0x0220, 0x0000, 0x0000}},
    {0x001c, 0x001e, {0x0220, 0x0008, 0x0000}},
    {0x001f, 0x001f, {0x0220, 0x0009, 0x0000}},
    {0x0020, 0x0020, {0x0248, 0x000a, 0x0048}},
    {0x0021, 0x0021, {0x0210, 0x000b, 0x0048}},
    {0x0022, 0x0022, {0x0210, 0x000b, 0x0448}},
    {0x0023, 0x0023, {0x0210, 0x0005, 0x0048}},
    {0x0024, 0x0024, {0x0210, 0x0005, 0x0448}},
    {0x0025, 0x0025, {0x0210, 0x0005, 0x0048}},
    {0x0026, 0x0026, {0x0210, 0x000b, 0x0048}},
    {0x0027, 0x0027, {0x0210, 0x000b, 0x0440}},
    {0x0028, 0x002a, {0x0210, 0x000b, 0x0048}},
    {0x002b, 0x002b, {0x0210, 0x0004, 0x0048}},
    {0x002c, 0x002c, {0x0210, 0x0007, 0x0048}},
    {0x002d, 0x002d, {0x0210, 0x0004, 0x0440}},
    {0x002e, 0x002e, {0x0210, 0x0007, 0x0048}},
    {0x002f, 0x002f, {0x0210, 0x0007, 0x0448}},
    {0x0030, 0x0039, {0x0284, 0x0003, 0x0040}},
    {0x003a, 0x003a, {0x0210, 0x0007, 0x0048}},
    {0x003b, 0x003c, {0x0210, 0x000b, 0x0048}},
    {0x003d, 0x003d, {0x0210, 0x000b, 0x0448}},
    {0x003e, 0x003f, {0x0210, 0x000b, 0x0048}},
    {0x0040, 0x0040, {0x0210, 0x000b, 0x0448}},
    {0x0041, 0x0046, {0x0381, 0x0001, 0x8040}},
    {0x0047, 0x005a, {0x0301, 0x0001, 0x8040}},
    {0x005b, 0x005b, {0x0210, 0x000b, 0x0048}},
    {0x005c, 0x005c, {0x0210, 0x000b, 0x0448}},
    {0x005d, 0x005d, {0x0210, 0x000b, 0x0048}},
    {0x005e, 0x005e, {0x0210, 0x000b, 0x044a}},
    {0x005f, 0x005f, {0x0210, 0x000b, 0x0448}},
    {0x0060, 0x0060, {0x0210, 0x000b, 0x044a}},
    {0x0061, 0x0066, {0x0382, 0x0001, 0x8040}},
    {0x0067, 0x007a, {0x0302, 0x0001, 0x8040}},
    {0x007b, 0x007d, {0x0210, 0x000b, 0x0048}},
    {0x007e, 0x007e, {0x0210, 0x000b, 0x0448}},
    {0x007f, 0x0084, {0x0220, 0x0000, 0x0000}},
    {0x0085, 0x0085, {0x0228, 0x0008, 0x0000}},
    {0x0086, 0x009f, {0x0220, 0x0000, 0x0000}},
    {0x00a0, 0x00a0, {0x0248, 0x0007, 0x0008}},
    {0x00a1, 0x00a1, {0x0210, 0x000b, 0x0008}},
    {0x00a2, 0x00a3, {0x0210, 0x0005, 0x0048}},
    {0x00a4, 0x00a4, {0x0210, 0x0005, 0x0008}},
    {0x00a5, 0x00a5, {0x0210, 0x0005, 0x0048}},
    {0x00a6, 0x00a6, {0x0210, 0x000b, 0x0048}},
    {0x00a7, 0x00a7, {0x0210, 0x000b, 0x0008}},
    {0x00a8, 0x00a8, {0x0210, 0x000b, 0x040a}},
    {0x00a9, 0x00a9, {0x0210, 0x000b, 0x0008}},
    {0x00aa, 0x00aa, {0x0312, 0x0001, 0x8400}},
    {0x00ab, 0x00ab, {0x0210, 0x000b, 0x0008}},
    {0x00ac, 0x00ac, {0x0210, 0x000b, 0x0048}},
    {0x00ad, 0x00ad, {0x0230, 0x0000, 0x0408}},
    {0x00ae, 0x00ae, {0x0210, 0x000b, 0x0008}},
    {0x00af, 0x00af, {0x0210, 0x000b, 0x044a}},
    {0x00b0, 0x00b1, {0x0210, 0x0005, 0x0008}},
    {0x00b2, 0x00b3, {0x0214, 0x0003, 0x0000}},
    {0x00b4, 0x00b4, {0x0210, 0x000b, 0x040a}},
    {0x00b5, 0x00b5, {0x0312, 0x0001, 0x8008}},
    {0x00b6, 0x00b6, {0x0210, 0x000b, 0x0008}},
    {0x00b7, 0x00b7, {0x0210, 0x000b, 0x000a}},
    {0x00b8, 0x00b8, {0x0210, 0x000b, 0x040a}},
    {0x00b9, 0x00b9, {0x0214, 0x0003, 0x0000}},
    {0x00ba, 0x00ba, {0x0312, 0x0001, 0x8400}},
    {0x00bb, 0x00bb, {0x0210, 0x000b, 0x0008}},
    {0x00bc, 0x00be, {0x0210, 0x000b, 0x0000}},
    {0x00bf, 0x00bf, {0x0210, 0x000b, 0x0008}},
    {0x00c0, 0x00c5, {0x0301, 0x0001, 0x8003}},
    {0x00c6, 0x00c6, {0x0301, 0x0001, 0x8000}},
    {0x00c7, 0x00cf, {0x0301, 0x0001, 0x8003}},
    {0x00d0, 0x00d0, {0x0301, 0x0001, 0x8000}},
    {0x00d1, 0x00d6, {0x0301, 0x0001, 0x8003}},
    {0x00d7, 0x00d7, {0x0210, 0x000b, 0x0008}},
    {0x00d8, 0x00dd, {0x0301, 0x0001, 0x8003}},
    {0x00de, 0x00de, {0x0301, 0x0001, 0x8000}},
    {0x00df, 0x00df, {0x0302, 0x0001, 0x8000}},
    {0x00e0, 0x00e5, {0x0302, 0x0001, 0x8003}},
    {0x00e6, 0x00e6, {0x0302, 0x0001, 0x8000}},
    {0x00e7, 0x00ef, {0x0302, 0x0001, 0x8003}},
    {0x00f0, 0x00f0, {0x0302, 0x0001, 0x8000}},
    {0x00f1, 0x00f6, {0x0302, 0x0001, 0x8003}},
    {0x00f7, 0x00f7, {0x0210, 0x000b, 0x0008}},
    {0x00f8, 0x00fd, {0x0302, 0x0001, 0x8003}},
    {0x00fe, 0x00fe, {0x0302, 0x0001, 0x8000}},
    {0x00ff, 0x00ff, {0x0302, 0x0001, 0x8003}},
    {0x0152, 0x0152, {0x0301, 0x0001, 0x8000}},
    {0x0153, 0x0153, {0x0302, 0x0001, 0x8000}},
    {0x0160, 0x0160, {0x0301, 0x0001, 0x8002}},
    {0x0161, 0x0161, {0x0302, 0x0001, 0x8002}},
    {0x0178, 0x0178, {0x0301, 0x0001, 0x8002}},
    {0x017d, 0x017d, {0x0301, 0x0001, 0x8002}},
    {0x017e, 0x017e, {0x0302, 0x0001, 0x8002}},
    {0x0192, 0x0192, {0x0302, 0x0001, 0x8000}},
    {0x02c6, 0x02c6, {0x0200, 0x000b, 0x000a}},
    {0x02dc, 0x02dc, {0x0200, 0x000b, 0x040a}},
    {0x2013, 0x2014, {0x0210, 0x000b, 0x0400}},
    {0x2018, 0x2019, {0x0210, 0x000b, 0x0080}},
    {0x201a, 0x201a, {0x0210, 0x000b, 0x0000}},
    {0x201c, 0x201d, {0x0210, 0x000b, 0x0080}},
    {0x201e, 0x201e, {0x0210, 0x000b, 0x0000}},
    {0x2020, 0x2022, {0x0210, 0x000b, 0x0000}},
    {0x2026, 0x2026, {0x0210, 0x000b, 0x0000}},
    {0x2030, 0x2030, {0x0210, 0x0005, 0x0000}},
    {0x2039, 0x203a, {0x0210, 0x000b, 0x0000}},
    {0x20ac, 0x20ac, {0x0200, 0x0005, 0x0008}},
    {0x2122, 0x2122, {0x0200, 0x000b, 0x0008}},
    {0xd800, 0xdbff, {0x0200, 0x0001, 0x0800}},
    {0xdc00, 0xdfff, {0x0200, 0x0001, 0x1000}},
};

static const uint16_t *string_type_flags(uint16_t unit) {
    size_t first = 0, end = sizeof ctype_ranges / sizeof ctype_ranges[0];
    while (first < end) {
        size_t mid = first + (end - first) / 2;
        if (unit < ctype_ranges[mid].first) end = mid;
        else if (unit > ctype_ranges[mid].last) first = mid + 1;
        else return ctype_ranges[mid].flags;
    }
    return NULL;
}

uint32_t skse_win_string_type(uint32_t info_type, const uint8_t *utf16le, size_t source_units,
                              uint8_t *types_le) {
    if (!utf16le || !types_le || types_le == utf16le || !source_units || source_units > INT_MAX)
        return SKSE_WIN_TEXT_INVALID_PARAMETER;
    unsigned column;
    switch (info_type) {
    case 1: column = 0; break;
    case 2: column = 1; break;
    case 4: column = 2; break;
    default: return SKSE_WIN_TEXT_INVALID_FLAGS;
    }

    for (size_t i = 0; i < source_units; ++i)
        if (!string_type_flags(read_u16(utf16le + 2 * i))) return SKSE_WIN_TEXT_NOT_SUPPORTED;
    for (size_t i = 0; i < source_units; ++i) {
        const uint16_t *flags = string_type_flags(read_u16(utf16le + 2 * i));
        write_u16(types_le + 2 * i, flags[column]);
    }
    return 0;
}

static bool code_page_supported(uint32_t page) {
    return page == SKSE_WIN_CP_UTF8 || page == SKSE_WIN_CP_ACP || page == SKSE_WIN_CP_1252;
}

uint16_t skse_win_decode_cp1252(uint8_t byte) {
    if (byte >= 0x80 && byte < 0xa0) return cp1252_controls[byte - 0x80];
    return byte;
}

static int encode_cp1252(uint16_t code, bool allow_best_fit) {
    if (code < 0x80 || (code >= 0xa0 && code <= 0xff)) return code;
    for (size_t i = 0; i < sizeof cp1252_controls / sizeof cp1252_controls[0]; ++i)
        if (code == cp1252_controls[i]) return (int)(0x80 + i);
    if (!allow_best_fit) return -1;

    /* The fullwidth ASCII block is consecutive apart from U+FF1F,
     * which Windows sends through the default-character path. */
    if (code >= 0xff01 && code <= 0xff5e && code != 0xff1f) return code - 0xfee0;

    size_t first = 0, end = sizeof cp1252_best_fit / sizeof cp1252_best_fit[0];
    while (first < end) {
        size_t mid = first + (end - first) / 2;
        if (code == cp1252_best_fit[mid].code) return cp1252_best_fit[mid].byte;
        if (code < cp1252_best_fit[mid].code) end = mid;
        else first = mid + 1;
    }
    return -1;
}

static size_t write_utf8_scalar(uint8_t out[4], uint32_t code) {
    if (code < 0x80) {
        out[0] = (uint8_t)code;
        return 1;
    }
    size_t width;
    uint8_t prefix;
    if (code < 0x800) {
        width = 2;
        prefix = 0xc0;
    } else if (code < 0x10000) {
        width = 3;
        prefix = 0xe0;
    } else {
        width = 4;
        prefix = 0xf0;
    }
    for (size_t i = width - 1; i > 0; --i) {
        out[i] = (uint8_t)(0x80 | (code & 0x3f));
        code >>= 6;
    }
    out[0] = (uint8_t)(prefix | code);
    return width;
}

static uint32_t read_utf8_scalar(const uint8_t *bytes, size_t length, size_t *offset,
                                bool *invalid) {
    uint8_t lead = bytes[(*offset)++];
    if (lead < 0x80) return lead;

    unsigned trail_count = 0;
    uint32_t code = 0;
    if (lead >= 0xc2 && lead <= 0xdf) {
        trail_count = 1;
        code = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        trail_count = 2;
        code = lead & 0x0f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        trail_count = 3;
        code = lead & 0x07;
    }
    if (trail_count) {
        for (unsigned i = 0; i < trail_count; ++i) {
            if (*offset == length || (bytes[*offset] & 0xc0) != 0x80) goto invalid;
            uint8_t trail = bytes[(*offset)++];
            /* Windows consumes the first continuation even when the pair
             * indicates an overlong, surrogate, or out-of-range sequence. */
            if (i == 0 && ((lead == 0xe0 && trail < 0xa0) || (lead == 0xed && trail >= 0xa0) ||
                           (lead == 0xf0 && trail < 0x90) || (lead == 0xf4 && trail >= 0x90))) goto invalid;
            code = (code << 6) | (trail & 0x3f);
        }
        return code;
    }
invalid:
    *invalid = true;
    return REPLACEMENT_CHARACTER;
}

static uint32_t wide_to_utf8(uint32_t flags, const uint8_t *source, size_t source_units,
                             uint8_t *destination, size_t capacity, size_t *written) {
    size_t used = 0;
    bool invalid = false;
    size_t limit = capacity ? capacity : INT_MAX;
    for (size_t i = 0; i < source_units; ++i) {
        uint16_t unit = read_u16(source + 2 * i);
        uint32_t code = unit;
        if (high_surrogate(unit) && i + 1 < source_units && low_surrogate(read_u16(source + 2 * (i + 1)))) {
            uint16_t next = read_u16(source + 2 * ++i);
            code = 0x10000u + ((uint32_t)(unit - 0xd800) << 10) + (uint32_t)(next - 0xdc00);
        } else if (high_surrogate(unit) || low_surrogate(unit)) {
            invalid = true;
            code = REPLACEMENT_CHARACTER;
        }
        uint8_t bytes[4];
        size_t count = write_utf8_scalar(bytes, code);
        if (count > limit - used) return SKSE_WIN_TEXT_INSUFFICIENT_BUFFER;
        if (capacity) memcpy(destination + used, bytes, count);
        used += count;
    }
    if (invalid && (flags & WC_ERR_INVALID_CHARS)) return SKSE_WIN_TEXT_NO_UNICODE_TRANSLATION;
    *written = used;
    return 0;
}

uint32_t skse_win_wide_to_multi(uint32_t code_page, uint32_t flags,
                              const uint8_t *utf16le, size_t source_units,
                              uint8_t *bytes, size_t capacity_bytes,
                              const uint8_t *default_byte, bool *used_default,
                              size_t *written) {
    if (written) *written = 0;
    if (!written || !utf16le || !source_units || source_units > INT_MAX || capacity_bytes > INT_MAX ||
        (capacity_bytes && (!bytes || bytes == utf16le))) return SKSE_WIN_TEXT_INVALID_PARAMETER;
    if (!code_page_supported(code_page)) return SKSE_WIN_TEXT_NOT_SUPPORTED;

    if (code_page == SKSE_WIN_CP_UTF8) {
        if (flags & ~(uint32_t)WC_ERR_INVALID_CHARS) return SKSE_WIN_TEXT_INVALID_FLAGS;
        if (default_byte || used_default) return SKSE_WIN_TEXT_INVALID_PARAMETER;
        return wide_to_utf8(flags, utf16le, source_units, bytes, capacity_bytes, written);
    }
    const uint32_t composite_options = WC_DISCARDNS | WC_SEPCHARS | WC_DEFAULTCHAR;
    const uint32_t valid_flags = WC_NO_BEST_FIT_CHARS | WC_COMPOSITECHECK | composite_options;
    if ((flags & ~valid_flags) || ((flags & composite_options) && !(flags & WC_COMPOSITECHECK)))
        return SKSE_WIN_TEXT_INVALID_FLAGS;
    if (flags & WC_COMPOSITECHECK) return SKSE_WIN_TEXT_NOT_SUPPORTED;

    bool allow_best_fit = !(flags & WC_NO_BEST_FIT_CHARS);
    uint8_t fallback = default_byte ? *default_byte : '?';
    size_t used = capacity_bytes && capacity_bytes < source_units ? capacity_bytes : source_units;
    if (used_default) *used_default = false;
    if (capacity_bytes || used_default) {
        for (size_t i = 0; i < used; ++i) {
            int byte = encode_cp1252(read_u16(utf16le + 2 * i), allow_best_fit);
            if (byte < 0) {
                byte = fallback;
                if (used_default) *used_default = true;
            }
            if (capacity_bytes) bytes[i] = (uint8_t)byte;
        }
    }
    if (used < source_units) return SKSE_WIN_TEXT_INSUFFICIENT_BUFFER;
    *written = used;
    return 0;
}

uint32_t skse_win_multi_to_wide(uint32_t code_page, uint32_t flags,
                              const uint8_t *bytes, size_t source_bytes,
                              uint8_t *utf16le, size_t capacity_units,
                              size_t *written) {
    if (written) *written = 0;
    if (!written || !bytes || !source_bytes || source_bytes > INT_MAX || capacity_units > INT_MAX ||
        (capacity_units && (!utf16le || utf16le == bytes))) return SKSE_WIN_TEXT_INVALID_PARAMETER;
    if (!code_page_supported(code_page)) return SKSE_WIN_TEXT_NOT_SUPPORTED;

    const bool utf8 = code_page == SKSE_WIN_CP_UTF8;
    const uint32_t valid_flags = utf8 ? MB_ERR_INVALID_CHARS :
                                 MB_PRECOMPOSED | MB_COMPOSITE | MB_USEGLYPHCHARS | MB_ERR_INVALID_CHARS;
    if ((flags & ~valid_flags) || ((flags & MB_COMPOSITE) && (flags & MB_PRECOMPOSED)))
        return SKSE_WIN_TEXT_INVALID_FLAGS;

    size_t used = 0;
    bool invalid = false;
    const size_t limit = capacity_units ? capacity_units : INT_MAX;
    for (size_t offset = 0; offset < source_bytes;) {
        uint16_t units[2];
        size_t count = 1;
        if (utf8) {
            uint32_t code = read_utf8_scalar(bytes, source_bytes, &offset, &invalid);
            if (code < 0x10000) {
                units[0] = (uint16_t)code;
            } else {
                uint32_t pair = code - 0x10000;
                units[0] = (uint16_t)(0xd800 | (pair >> 10));
                units[1] = (uint16_t)(0xdc00 | (pair & 0x3ff));
                count = 2;
            }
        } else {
            uint8_t byte = bytes[offset++];
            units[0] = skse_win_decode_cp1252(byte);
            if ((flags & MB_COMPOSITE) && cp1252_composite[byte].mark) {
                units[0] = cp1252_composite[byte].letter;
                units[1] = cp1252_composite[byte].mark;
                count = 2;
            }
        }

        /* Win32 retains a partial prefix, including the first UTF-16 unit
         * of a pair, when the destination runs out of space. */
        const size_t room = limit - used;
        if (capacity_units) {
            size_t emit = count < room ? count : room;
            for (size_t i = 0; i < emit; ++i) write_u16(utf16le + 2 * (used + i), units[i]);
        }
        if (count > room) {
            // Native MB_COMPOSITE accepts the base alone for the final input byte.
            if (utf8 || !capacity_units || !room || offset < source_bytes)
                return SKSE_WIN_TEXT_INSUFFICIENT_BUFFER;
            count = room;
        }
        used += count;
    }
    if (invalid && (flags & MB_ERR_INVALID_CHARS)) return SKSE_WIN_TEXT_NO_UNICODE_TRANSLATION;
    *written = used;
    return 0;
}

typedef struct {
    uint16_t field;
    const char *english;
    const char *invariant; /* NULL means the same value in both locales. */
    uint8_t radix; /* Nonzero only for fields that support LOCALE_RETURN_NUMBER. */
} LocaleField;

/* Values are Latin-1 strings, widened to UTF-16LE at the boundary. The
 * Windows reference uses explicit LCIDs and LOCALE_NOUSEROVERRIDE.
 * Fields that require UI-language localization or binary NLS data are
 * outside this catalog. */
static const LocaleField locale_fields[] = {
    {0x0001, "0409", "007f", 16}, /* LOCALE_ILANGUAGE */
    {0x0003, "ENU", "IVL", 0}, /* LOCALE_SABBREVLANGNAME */
    {0x0004, "English", "Invariant Language", 0}, /* LOCALE_SNATIVELANGUAGENAME */
    {0x0005, "1", NULL, 10}, /* LOCALE_IDIALINGCODE */
    {0x0007, "USA", "", 0}, /* LOCALE_SABBREVCTRYNAME */
    {0x0008, "United States", "Invariant Country", 0}, /* LOCALE_SNATIVECOUNTRYNAME */
    {0x0009, "0409", NULL, 16}, /* LOCALE_IDEFAULTLANGUAGE */
    {0x000a, "1", NULL, 10}, /* LOCALE_IDEFAULTCOUNTRY */
    {0x000b, "437", NULL, 10}, /* LOCALE_IDEFAULTCODEPAGE */
    {0x000c, ",", NULL, 0}, /* LOCALE_SLIST */
    {0x000d, "1", "0", 10}, /* LOCALE_IMEASURE */
    {0x000e, ".", NULL, 0}, /* LOCALE_SDECIMAL */
    {0x000f, ",", NULL, 0}, /* LOCALE_STHOUSAND */
    {0x0010, "3;0", NULL, 0}, /* LOCALE_SGROUPING */
    {0x0011, "2", NULL, 10}, /* LOCALE_IDIGITS */
    {0x0012, "1", NULL, 10}, /* LOCALE_ILZERO */
    {0x0013, "0123456789", NULL, 0}, /* LOCALE_SNATIVEDIGITS */
    {0x0014, "$", "\xa4", 0}, /* LOCALE_SCURRENCY */
    {0x0015, "USD", "XDR", 0}, /* LOCALE_SINTLSYMBOL */
    {0x0016, ".", NULL, 0}, /* LOCALE_SMONDECIMALSEP */
    {0x0017, ",", NULL, 0}, /* LOCALE_SMONTHOUSANDSEP */
    {0x0018, "3;0", NULL, 0}, /* LOCALE_SMONGROUPING */
    {0x0019, "2", NULL, 10}, /* LOCALE_ICURRDIGITS */
    {0x001a, "2", NULL, 10}, /* LOCALE_IINTLCURRDIGITS */
    {0x001b, "0", NULL, 10}, /* LOCALE_ICURRENCY */
    {0x001c, "0", NULL, 10}, /* LOCALE_INEGCURR */
    {0x001d, "/", NULL, 0}, /* LOCALE_SDATE */
    {0x001e, ":", NULL, 0}, /* LOCALE_STIME */
    {0x001f, "M/d/yyyy", "MM/dd/yyyy", 0}, /* LOCALE_SSHORTDATE */
    {0x0020, "dddd, MMMM d, yyyy", "dddd, dd MMMM yyyy", 0}, /* LOCALE_SLONGDATE */
    {0x0021, "0", NULL, 10}, /* LOCALE_IDATE */
    {0x0022, "0", "1", 10}, /* LOCALE_ILDATE */
    {0x0023, "0", "1", 10}, /* LOCALE_ITIME */
    {0x0024, "1", NULL, 10}, /* LOCALE_ICENTURY */
    {0x0025, "0", "1", 10}, /* LOCALE_ITLZERO */
    {0x0026, "0", "1", 10}, /* LOCALE_IDAYLZERO */
    {0x0027, "0", "1", 10}, /* LOCALE_IMONLZERO */
    {0x0028, "AM", NULL, 0}, /* LOCALE_SAM */
    {0x0029, "PM", NULL, 0}, /* LOCALE_SPM */
    {0x002a, "Monday", NULL, 0}, /* LOCALE_SDAYNAME1 */
    {0x002b, "Tuesday", NULL, 0}, /* LOCALE_SDAYNAME2 */
    {0x002c, "Wednesday", NULL, 0}, /* LOCALE_SDAYNAME3 */
    {0x002d, "Thursday", NULL, 0}, /* LOCALE_SDAYNAME4 */
    {0x002e, "Friday", NULL, 0}, /* LOCALE_SDAYNAME5 */
    {0x002f, "Saturday", NULL, 0}, /* LOCALE_SDAYNAME6 */
    {0x0030, "Sunday", NULL, 0}, /* LOCALE_SDAYNAME7 */
    {0x0031, "Mon", NULL, 0}, /* LOCALE_SABBREVDAYNAME1 */
    {0x0032, "Tue", NULL, 0}, /* LOCALE_SABBREVDAYNAME2 */
    {0x0033, "Wed", NULL, 0}, /* LOCALE_SABBREVDAYNAME3 */
    {0x0034, "Thu", NULL, 0}, /* LOCALE_SABBREVDAYNAME4 */
    {0x0035, "Fri", NULL, 0}, /* LOCALE_SABBREVDAYNAME5 */
    {0x0036, "Sat", NULL, 0}, /* LOCALE_SABBREVDAYNAME6 */
    {0x0037, "Sun", NULL, 0}, /* LOCALE_SABBREVDAYNAME7 */
    {0x0038, "January", NULL, 0}, /* LOCALE_SMONTHNAME1 */
    {0x0039, "February", NULL, 0}, /* LOCALE_SMONTHNAME2 */
    {0x003a, "March", NULL, 0}, /* LOCALE_SMONTHNAME3 */
    {0x003b, "April", NULL, 0}, /* LOCALE_SMONTHNAME4 */
    {0x003c, "May", NULL, 0}, /* LOCALE_SMONTHNAME5 */
    {0x003d, "June", NULL, 0}, /* LOCALE_SMONTHNAME6 */
    {0x003e, "July", NULL, 0}, /* LOCALE_SMONTHNAME7 */
    {0x003f, "August", NULL, 0}, /* LOCALE_SMONTHNAME8 */
    {0x0040, "September", NULL, 0}, /* LOCALE_SMONTHNAME9 */
    {0x0041, "October", NULL, 0}, /* LOCALE_SMONTHNAME10 */
    {0x0042, "November", NULL, 0}, /* LOCALE_SMONTHNAME11 */
    {0x0043, "December", NULL, 0}, /* LOCALE_SMONTHNAME12 */
    {0x0044, "Jan", NULL, 0}, /* LOCALE_SABBREVMONTHNAME1 */
    {0x0045, "Feb", NULL, 0}, /* LOCALE_SABBREVMONTHNAME2 */
    {0x0046, "Mar", NULL, 0}, /* LOCALE_SABBREVMONTHNAME3 */
    {0x0047, "Apr", NULL, 0}, /* LOCALE_SABBREVMONTHNAME4 */
    {0x0048, "May", NULL, 0}, /* LOCALE_SABBREVMONTHNAME5 */
    {0x0049, "Jun", NULL, 0}, /* LOCALE_SABBREVMONTHNAME6 */
    {0x004a, "Jul", NULL, 0}, /* LOCALE_SABBREVMONTHNAME7 */
    {0x004b, "Aug", NULL, 0}, /* LOCALE_SABBREVMONTHNAME8 */
    {0x004c, "Sep", NULL, 0}, /* LOCALE_SABBREVMONTHNAME9 */
    {0x004d, "Oct", NULL, 0}, /* LOCALE_SABBREVMONTHNAME10 */
    {0x004e, "Nov", NULL, 0}, /* LOCALE_SABBREVMONTHNAME11 */
    {0x004f, "Dec", NULL, 0}, /* LOCALE_SABBREVMONTHNAME12 */
    {0x0050, "", "+", 0}, /* LOCALE_SPOSITIVESIGN */
    {0x0051, "-", NULL, 0}, /* LOCALE_SNEGATIVESIGN */
    {0x0052, "3", NULL, 10}, /* LOCALE_IPOSSIGNPOSN */
    {0x0053, "0", NULL, 10}, /* LOCALE_INEGSIGNPOSN */
    {0x0054, "1", NULL, 10}, /* LOCALE_IPOSSYMPRECEDES */
    {0x0055, "0", NULL, 10}, /* LOCALE_IPOSSEPBYSPACE */
    {0x0056, "1", NULL, 10}, /* LOCALE_INEGSYMPRECEDES */
    {0x0057, "0", NULL, 10}, /* LOCALE_INEGSEPBYSPACE */
    {0x0059, "en", "iv", 0}, /* LOCALE_SISO639LANGNAME */
    {0x005a, "US", "IV", 0}, /* LOCALE_SISO3166CTRYNAME */
    {0x005c, "en-US", "", 0}, /* LOCALE_SNAME */
    {0x0060, "Mo", NULL, 0}, /* LOCALE_SSHORTESTDAYNAME1 */
    {0x0061, "Tu", NULL, 0}, /* LOCALE_SSHORTESTDAYNAME2 */
    {0x0062, "We", NULL, 0}, /* LOCALE_SSHORTESTDAYNAME3 */
    {0x0063, "Th", NULL, 0}, /* LOCALE_SSHORTESTDAYNAME4 */
    {0x0064, "Fr", NULL, 0}, /* LOCALE_SSHORTESTDAYNAME5 */
    {0x0065, "Sa", NULL, 0}, /* LOCALE_SSHORTESTDAYNAME6 */
    {0x0066, "Su", NULL, 0}, /* LOCALE_SSHORTESTDAYNAME7 */
    {0x0067, "eng", "ivl", 0}, /* LOCALE_SISO639LANGNAME2 */
    {0x0068, "USA", "", 0}, /* LOCALE_SISO3166CTRYNAME2 */
    {0x006c, "Latn;", NULL, 0}, /* LOCALE_SSCRIPTS */
    {0x006d, "en", "", 0}, /* LOCALE_SPARENT */
    {0x0070, "0", NULL, 10}, /* LOCALE_IREADINGLAYOUT */
    {0x0071, "0", NULL, 10}, /* LOCALE_INEUTRAL */
    {0x0072, "English (United States)", "Invariant Language (Invariant Country)", 0}, /* LOCALE_SENGLISHDISPLAYNAME */
    {0x0073, "English (United States)", "Invariant Language (Invariant Region)", 0}, /* LOCALE_SNATIVEDISPLAYNAME */
    {0x0074, "1", "0", 10}, /* LOCALE_INEGATIVEPERCENT */
    {0x0075, "1", "0", 10}, /* LOCALE_IPOSITIVEPERCENT */
    {0x0076, "%", NULL, 0}, /* LOCALE_SPERCENT */
    {0x0078, "MMMM d", "MMMM dd", 0}, /* LOCALE_SMONTHDAY */
    {0x0079, "h:mm tt", "HH:mm", 0}, /* LOCALE_SSHORTTIME */
    {0x007e, "a", NULL, 0}, /* LOCALE_SSHORTESTAM */
    {0x007f, "p", NULL, 0}, /* LOCALE_SSHORTESTPM */
    {0x1001, "English", "Invariant Language", 0}, /* LOCALE_SENGLISHLANGUAGENAME */
    {0x1002, "United States", "Invariant Country", 0}, /* LOCALE_SENGLISHCOUNTRYNAME */
    {0x1003, "h:mm:ss tt", "HH:mm:ss", 0}, /* LOCALE_STIMEFORMAT */
    {0x1004, "1252", NULL, 10}, /* LOCALE_IDEFAULTANSICODEPAGE */
    {0x1005, "0", NULL, 10}, /* LOCALE_ITIMEMARKPOSN */
    {0x1006, "MMMM yyyy", "yyyy MMMM", 0}, /* LOCALE_SYEARMONTH */
    {0x1007, "US Dollar", "International Monetary Fund", 0}, /* LOCALE_SENGCURRNAME */
    {0x1008, "US Dollar", "International Monetary Fund", 0}, /* LOCALE_SNATIVECURRNAME */
    {0x1009, "1", NULL, 10}, /* LOCALE_ICALENDARTYPE */
    {0x100a, "1", "9", 10}, /* LOCALE_IPAPERSIZE */
    {0x100b, "2", "0", 10}, /* LOCALE_IOPTIONALCALENDAR */
    {0x100c, "6", NULL, 10}, /* LOCALE_IFIRSTDAYOFWEEK */
    {0x100d, "0", NULL, 10}, /* LOCALE_IFIRSTWEEKOFYEAR */
    {0x100e, "", NULL, 0}, /* LOCALE_SMONTHNAME13 */
    {0x100f, "", NULL, 0}, /* LOCALE_SABBREVMONTHNAME13 */
    {0x1010, "1", NULL, 10}, /* LOCALE_INEGNUMBER */
    {0x1011, "10000", NULL, 10}, /* LOCALE_IDEFAULTMACCODEPAGE */
    {0x1012, "037", NULL, 10}, /* LOCALE_IDEFAULTEBCDICCODEPAGE */
    {0x1014, "1", NULL, 10}, /* LOCALE_IDIGITSUBSTITUTION */
};

static const LocaleField *find_locale_field(uint16_t field) {
    size_t first = 0, end = sizeof locale_fields / sizeof locale_fields[0];
    while (first < end) {
        size_t mid = first + (end - first) / 2;
        if (field < locale_fields[mid].field) end = mid;
        else if (field > locale_fields[mid].field) first = mid + 1;
        else return &locale_fields[mid];
    }
    return NULL;
}

bool skse_win_is_valid_locale(uint32_t lcid, uint32_t flags) {
    return flags <= 2 && (lcid == SKSE_WIN_GUEST_DEFAULT_LCID || lcid == SKSE_WIN_LOCALE_INVARIANT);
}

static uint32_t normalized_guest_lcid(uint32_t lcid) {
    if (lcid == 0 || lcid == 0x0400 || lcid == 0x0800) return SKSE_WIN_GUEST_DEFAULT_LCID;
    return lcid;
}

uint32_t skse_win_locale_info(uint32_t lcid, uint32_t locale_type,
                              uint8_t *output_le, size_t capacity_units, size_t *written) {
    if (written) *written = 0;
    if (!written || capacity_units > INT_MAX || (capacity_units && !output_le) || lcid > 0x000fffffu)
        return SKSE_WIN_TEXT_INVALID_PARAMETER;
    lcid = normalized_guest_lcid(lcid);
    if (!skse_win_is_valid_locale(lcid, 0)) return SKSE_WIN_TEXT_NOT_SUPPORTED;

    const uint16_t field = (uint16_t)locale_type;
    /* The W API ignores LOCALE_USE_CP_ACP. The two guest locales use
     * fixed defaults and have identical base and genitive month names. */
    if (!field || (locale_type & UINT32_C(0x0fff0000))) return SKSE_WIN_TEXT_INVALID_FLAGS;
    const LocaleField *value = find_locale_field(field);
    if (!value) return SKSE_WIN_TEXT_NOT_SUPPORTED;
    const bool return_number = (locale_type & UINT32_C(0x20000000)) != 0;
    if (return_number && !value->radix) return SKSE_WIN_TEXT_INVALID_FLAGS;

    const char *text = value->english;
    if (lcid == SKSE_WIN_LOCALE_INVARIANT && value->invariant) text = value->invariant;
    size_t needed = return_number ? 2 : strlen(text) + 1;
    if (capacity_units && capacity_units < needed) return SKSE_WIN_TEXT_INSUFFICIENT_BUFFER;
    if (capacity_units) {
        if (return_number) {
            uint32_t number = 0;
            for (const unsigned char *digit = (const unsigned char *)text; *digit; ++digit) {
                unsigned unit = *digit >= 'a' ? (unsigned)(*digit - 'a' + 10) : (unsigned)(*digit - '0');
                number = number * value->radix + unit;
            }
            write_u16(output_le, (uint16_t)number);
            write_u16(output_le + 2, (uint16_t)(number >> 16));
        } else {
            for (size_t i = 0; i < needed; ++i) write_u16(output_le + 2 * i, (uint8_t)text[i]);
        }
    }
    *written = needed;
    return 0;
}

enum {
    NORM_IGNORECASE = 0x1,
    SORT_STRINGSORT = 0x1000,
    LCMAP_LOWERCASE = 0x100,
    LCMAP_UPPERCASE = 0x200,
    LCMAP_LINGUISTIC_CASING = 0x01000000,
    CSTR_LESS_THAN = 1,
    CSTR_EQUAL = 2,
    CSTR_GREATER_THAN = 3
};

static bool case_map_supports(uint16_t unit) {
    if (unit <= 0xff) return true;
    for (size_t i = 0; i < sizeof cp1252_controls / sizeof cp1252_controls[0]; ++i)
        if (unit == cp1252_controls[i]) return true;
    return false;
}

static uint16_t map_case(uint16_t unit, bool uppercase, bool linguistic) {
    /* Windows' LCMapStringW mappings for Latin-1 and the Windows-1252
     * extension. The native test compares the full source repertoire. */
    if (uppercase) {
        if ((unit >= 'a' && unit <= 'z') || (unit >= 0xe0 && unit <= 0xfe && unit != 0xf7))
            return unit - 0x20;
        switch (unit) {
        case 0x00b5: return linguistic ? 0x039c : unit;
        case 0x00ff: return 0x0178;
        case 0x0153: return 0x0152;
        case 0x0161: return 0x0160;
        case 0x017e: return 0x017d;
        case 0x0192: return 0x0191;
        default: return unit;
        }
    }
    if ((unit >= 'A' && unit <= 'Z') || (unit >= 0xc0 && unit <= 0xde && unit != 0xd7))
        return unit + 0x20;
    switch (unit) {
    case 0x0152: return 0x0153;
    case 0x0160: return 0x0161;
    case 0x0178: return 0x00ff;
    case 0x017d: return 0x017e;
    default: return unit;
    }
}

uint32_t skse_win_map_string(uint32_t lcid, uint32_t flags,
                              const uint8_t *source_le, size_t source_units,
                              uint8_t *output_le, size_t capacity_units, size_t *written) {
    if (written) *written = 0;
    if (!written || !source_le || !source_units || source_units > INT_MAX ||
        capacity_units > INT_MAX || (capacity_units && !output_le) || lcid > 0x000fffffu)
        return SKSE_WIN_TEXT_INVALID_PARAMETER;
    if (!skse_win_is_valid_locale(normalized_guest_lcid(lcid), 0)) return SKSE_WIN_TEXT_NOT_SUPPORTED;

    const uint32_t casing = flags & ~((uint32_t)LCMAP_LINGUISTIC_CASING);
    if (!casing || (flags & UINT32_C(0x80000000))) return SKSE_WIN_TEXT_INVALID_FLAGS;
    if (casing != LCMAP_LOWERCASE && casing != LCMAP_UPPERCASE) return SKSE_WIN_TEXT_NOT_SUPPORTED;

    for (size_t i = 0; i < source_units; ++i)
        if (!case_map_supports(read_u16(source_le + 2 * i))) return SKSE_WIN_TEXT_NOT_SUPPORTED;
    if (capacity_units && capacity_units < source_units) return SKSE_WIN_TEXT_INSUFFICIENT_BUFFER;
    if (capacity_units) {
        const bool uppercase = casing == LCMAP_UPPERCASE;
        const bool linguistic = (flags & LCMAP_LINGUISTIC_CASING) != 0;
        for (size_t i = 0; i < source_units; ++i) {
            uint16_t unit = read_u16(source_le + 2 * i);
            write_u16(output_le + 2 * i, map_case(unit, uppercase, linguistic));
        }
    }
    *written = source_units;
    return 0;
}

static bool collation_supports(const uint8_t *text_le, size_t units, bool simple_symbol_weights) {
    for (size_t i = 0; i < units; ++i) {
        uint16_t unit = read_u16(text_le + 2 * i);
        if (unit && (unit < ' ' || unit > '~')) return false;
        if (!simple_symbol_weights && (unit == '\'' || unit == '-'))
            return false;
    }
    return true;
}

static uint16_t next_collation_unit(const uint8_t **text_le, size_t *units) {
    while (*units) {
        uint16_t unit = read_u16(*text_le);
        *text_le += 2;
        --*units;
        if (unit) return unit;
    }
    return 0;
}

static unsigned collation_weight(uint16_t unit) {
    /* Windows' primary order on this ASCII repertoire. Case is a separate
     * weight. The initial two symbols use this order for case-insensitive
     * string sort only. */
    static const char primary_order[] =
        "'- !\"#$%&()*,./:;?@[\\]^_`{|}~+<=>0123456789abcdefghijklmnopqrstuvwxyz";
    if (!unit) return 0;
    if (unit >= 'A' && unit <= 'Z') unit += 'a' - 'A';
    return (unsigned)(strchr(primary_order, unit) - primary_order) + 1;
}

uint32_t skse_win_compare_string(uint32_t lcid, uint32_t flags,
                                  const uint8_t *left_le, size_t left_units,
                                  const uint8_t *right_le, size_t right_units, uint32_t *relation) {
    if (relation) *relation = 0;
    if (!relation || !left_le || !right_le || left_units > INT_MAX || right_units > INT_MAX ||
        lcid > 0x000fffffu)
        return SKSE_WIN_TEXT_INVALID_PARAMETER;
    if (!skse_win_is_valid_locale(normalized_guest_lcid(lcid), 0)) return SKSE_WIN_TEXT_NOT_SUPPORTED;

    /* Include documented Windows flags in validation while leaving the
     * corresponding unimplemented sort modes explicit. */
    if (flags & ~UINT32_C(0x0803103f)) return SKSE_WIN_TEXT_INVALID_FLAGS;
    const uint32_t supported_flags = NORM_IGNORECASE | SORT_STRINGSORT;
    if (flags & ~supported_flags) return SKSE_WIN_TEXT_NOT_SUPPORTED;
    const bool simple_symbol_weights = flags == supported_flags;
    if (!collation_supports(left_le, left_units, simple_symbol_weights) ||
        !collation_supports(right_le, right_units, simple_symbol_weights))
        return SKSE_WIN_TEXT_NOT_SUPPORTED;

    uint32_t case_relation = CSTR_EQUAL;
    for (;;) {
        uint16_t left = next_collation_unit(&left_le, &left_units);
        uint16_t right = next_collation_unit(&right_le, &right_units);
        unsigned left_weight = collation_weight(left), right_weight = collation_weight(right);
        if (left_weight != right_weight) {
            *relation = left_weight < right_weight ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
            return 0;
        }
        if (!left) {
            *relation = (flags & NORM_IGNORECASE) ? CSTR_EQUAL : case_relation;
            return 0;
        }
        /* Lowercase precedes uppercase when all primary weights match. */
        if (case_relation == CSTR_EQUAL && left != right) case_relation = left < right ? CSTR_GREATER_THAN : CSTR_LESS_THAN;
    }
}
