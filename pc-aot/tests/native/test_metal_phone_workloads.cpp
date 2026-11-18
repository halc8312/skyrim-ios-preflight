#include "phone_workloads.h"
#include <cstdio>
#include <set>
#include <string>

using namespace SkyrimPhoneWorkloads;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template<size_t N>
static uint64_t fingerprint(const std::array<uint32_t, N>& values) {
    uint64_t hash = 14695981039346656037ull;
    for (uint8_t byte : littleEndianBytes(values)) hash = (hash ^ byte) * 1099511628211ull;
    return hash;
}

int main() {
    try {
        // Frozen vectors from an independent arbitrary-precision reference with
        // explicit modulo-2^32 operations; not calculated from the header here.
        constexpr uint64_t inputGold[] = {0x716c4bbd5153ca7eull, 0xb622ad2ca2d4948eull, 0xe330fb38c0872766ull};
        constexpr uint64_t outputGold[3][2] = {
            {0x16ba50279f85293aull, 0xb9caacf328321a69ull},
            {0xfcb3b08592c41b8full, 0xa0315a0097ca8db8ull},
            {0xb544e1109ae5619aull, 0xccd016b4ed056c20ull}
        };
        constexpr uint32_t firstWords[3][2][4] = {
            {{0xd404b0cdu, 0xea056cf1u, 0x955cf68eu, 0x58954477u},
             {0xf947b5f1u, 0xbf3bbf6au, 0x83b81a6cu, 0xf0aff7ebu}},
            {{0xd5d23a62u, 0x457bf1c6u, 0x97730ea6u, 0x846ade31u},
             {0x907a1deeu, 0xe11bf1c7u, 0x215a7d15u, 0xe776c749u}},
            {{0x5fa5599au, 0x565fc39bu, 0x140226feu, 0x4fd2426fu},
             {0x15f97e48u, 0xc3708804u, 0x81ccb709u, 0x9d54a73cu}}
        };
        std::set<std::string> names;
        for (unsigned c = 0; c < CaseCount; ++c) {
            names.insert(caseID(c));
            const Input input = makeInput(c);
            check(input == makeInput(c), "input generation is nondeterministic");
            check(fingerprint(input) == inputGold[c], "authored input vector changed");
            for (unsigned stage = 0; stage < 2; ++stage) {
                const uint32_t tag = stage ? ComputeTag : RenderTag;
                const Output output = oracle(c, input, tag);
                check(fingerprint(output) == outputGold[c][stage], "full oracle golden vector changed");
                for (unsigned j = 0; j < 4; ++j)
                    check(output[j] == firstWords[c][stage][j], "oracle first lane changed");
                std::set<std::array<uint32_t, 4>> lanes;
                for (unsigned id = 0; id < LaneCount; ++id) lanes.insert(lane(c, input, id, tag));
                check(lanes.size() == LaneCount, "lane-dependent inputs collapsed");
                Input changed = input;
                for (auto& word : changed) word ^= 0x01010101u;
                check(oracle(c, changed, tag) != output, "oracle ignores runtime buffer inputs");
                check(oracle(c, input, tag ^ 1u) != output, "oracle ignores stage tag");
            }
        }
        check(names.size() == CaseCount, "case identities collide");
        unsigned positions[RouteCount][RouteCount]{};
        for (unsigned iteration = 0; iteration < RepeatCount; ++iteration) {
            std::set<unsigned> routes;
            for (unsigned slot = 0; slot < RouteCount; ++slot) {
                unsigned route = routeIndex(iteration, slot);
                ++positions[route][slot];
                routes.insert(route);
            }
            check(routes.size() == RouteCount, "iteration omitted or duplicated a route");
        }
        for (const auto& route : positions)
            for (unsigned count : route) check(count == 1, "route position imbalance");
        check(rotate(0x81234567u, 0) == 0x81234567u && rotate(0x81234567u, 32) == 0x81234567u,
              "rotation boundary mismatch");
        const auto serialized = littleEndianBytes(std::array<uint32_t, 2>{0x12345678u, 0xaabbccddu});
        check(serialized == std::array<uint8_t, 8>{0x78, 0x56, 0x34, 0x12, 0xdd, 0xcc, 0xbb, 0xaa},
              "input fingerprint is not canonical little endian");
        unsigned rejected = 0;
        try { (void)caseID(3); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)rounds(3); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)routeIndex(4, 0); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)routeIndex(0, 4); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)lane(0, makeInput(0), LaneCount, RenderTag); } catch (const std::out_of_range&) { ++rejected; }
        check(rejected == 5, "invalid authored bounds accepted");
        std::puts("PHONE_WORKLOAD_CPU cases=3 stages=2 balanced_samples=48 golden_vectors=6 gpu_execution_verified=0");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
