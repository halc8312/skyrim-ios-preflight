#ifndef OH_METAL_CONSTANT_VALIDATION_H
#define OH_METAL_CONSTANT_VALIDATION_H
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <utility>
#include "ConstantReplayKernel.h"
namespace ohcp {
constexpr std::size_t max_records = 16384, max_values = 1 << 17, block_bytes = 4 << 20;
static_assert(record_words == 4 + lanes/32);
static_assert(bin_header_words == 1 + 3*stages);
// Canonical binned representation; all GPU indices and buffer extents are proven here.
inline bool validate(const Word *r, std::size_t words, std::size_t values, std::size_t out) {
    if (!r || words < bin_header_words || words > bin_header_words + (record_words+1) * max_records || !out || out > block_bytes
        || out % 16 || values > max_values * 16 || values % 16) return false;
    std::size_t count = r[0];
    if (count > max_records) return false;
    std::size_t base = bin_header_words + count * record_words;
    if (words != base + count) return false;
    std::size_t source = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> spans;
    for (std::size_t index = 0; index < count; ++index) {
        const Word *record = r + bin_header_words + index * record_words;
        if (record[0] >= stages || record[2] > lanes || record[3] != source
            || std::uint64_t(record[1]) * 16 + std::uint64_t(record[2]) * 16 > out) return false;
        if (record[2]) spans.emplace_back(std::uint64_t(record[1])*16, std::uint64_t(record[1]+std::uint64_t(record[2]))*16);
        for (std::size_t w = 0; w < lanes/32; ++w) {
            // This public corpus uses only in-range changes. No out-of-range GPU reads.
            Word valid = record[2] >= (w + 1) * 32 ? ~Word(0)
                : record[2] <= w * 32 ? 0 : (Word(1) << (record[2] - w * 32)) - 1;
            if (record[4+w] & ~valid) return false;
            source += bits(record[4+w]);
        }
    }
    if (source * 16 != values) return false;
    std::sort(spans.begin(), spans.end());
    for (std::size_t i=1; i<spans.size(); ++i) if (spans[i-1].second > spans[i].first) return false;
    std::vector<bool> seen(count, false);
    std::size_t next = base;
    for (std::size_t stage = 0; stage < stages; ++stage) {
        const Word *header = r + 1 + stage * 3;
        if (header[0] != next || header[1] > count || next + header[1] > words) return false;
        Word maximum = 0, previous = 0;
        for (Word i = 0; i < header[1]; ++i) {
            Word index = r[next+i];
            if (index >= count || seen[index] || (i && index <= previous) || r[bin_header_words+index*record_words] != stage) return false;
            seen[index] = true; previous = index;
            maximum = std::max(maximum, r[bin_header_words+index*record_words+2]);
        }
        if (header[2] != maximum) return false;
        next += header[1];
    }
    return next == words && std::all_of(seen.begin(), seen.end(), [](bool b) { return b; });
}
} // namespace ohcp
#endif
