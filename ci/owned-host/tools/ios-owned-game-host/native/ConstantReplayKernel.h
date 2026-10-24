#ifndef OH_CONSTANT_REPLAY_KERNEL_H
#define OH_CONSTANT_REPLAY_KERNEL_H
// Authored constant replay core. Also compiled as ordinary C++ for bit-exact tests.
// This preserves integer bits (including NaNs); it performs no floating-point work.
#ifdef __METAL_VERSION__
#include <metal_stdlib>
#define OHCP_DEVICE device
namespace ohcp { using Word = uint; using Vec4 = uint4;
inline Word bits(Word n) { return metal::popcount(n); }
inline Vec4 zero4() { return Vec4(0); }
constant Word stages = 5, lanes = 256, record_words = 12, bin_header_words = 16;
#else
#include <cstdint>
#define OHCP_DEVICE
namespace ohcp { using Word = std::uint32_t;
struct Vec4 { Word x, y, z, w; };
inline Word bits(Word n) { return static_cast<Word>(__builtin_popcount(n)); }
inline Vec4 zero4() { return Vec4{0,0,0,0}; }
constexpr Word stages = 5, lanes = 256, record_words = 12, bin_header_words = 16;
#endif
inline void replay_lane(const OHCP_DEVICE Word *records, const OHCP_DEVICE Vec4 *values,
                        OHCP_DEVICE Vec4 *blocks, Word stage, Word lane) {
    Word header = 1 + stage * 3;
    if (lane >= records[header + 2]) return;
    Word end = records[header] + records[header + 1];
    Word word = lane / 32, bit = Word(1) << (lane % 32);
    Vec4 current = zero4();
    for (Word index = records[header]; index < end; ++index) {
        Word base = bin_header_words + records[index] * record_words;
        Word mask = records[base + 4 + word];
        if ((mask & bit) != 0) {
            Word n = bits(mask & (bit - 1));
            for (Word w = 0; w < word; ++w) n += bits(records[base + 4 + w]);
            current = values[records[base + 3] + n];
        }
        if (lane < records[base + 2]) blocks[records[base + 1] + lane] = current;
    }
}
} // namespace ohcp
#undef OHCP_DEVICE
#endif
