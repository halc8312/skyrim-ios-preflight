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
        constexpr uint64_t inputGold[] = {
            0x716c4bbd5153ca7eull, 0x9fcbbcca067eeee2ull, 0x4f66669b80cd7df9ull, 0x91678a26e2f4d124ull,
            0xb622ad2ca2d4948eull, 0xe6e35015ed320412ull, 0x09380a646767daceull, 0xb71b76f0c9d11361ull,
            0xe330fb38c0872766ull, 0x58b243d1536e3595ull, 0x67db777479839bb0ull, 0x45e6be80b63216c5ull
        };
        constexpr uint64_t outputGold[CaseCount][2] = {
            {0x28b7feb7f053a132ull, 0x8e0248c22a25e552ull},
            {0x2e973a0a59b6759bull, 0x105d2d2d68d097cbull},
            {0xdd38cf78624c78f1ull, 0x8a4d6a56150a68f5ull},
            {0xe831f1e01c63ff2aull, 0x1b024286447fe8d5ull},
            {0x2c327bc7622487e9ull, 0x8e574c5624356e35ull},
            {0xc418aef570ce9149ull, 0x21332f464d407322ull},
            {0x6ebf77117acbcea7ull, 0xe3f5f79f305d2bdbull},
            {0x9549fe5cf42968c9ull, 0xaa8ffc96fe26e767ull},
            {0x26df6a67401f4165ull, 0xce3d0b6277ea176cull},
            {0xe286425ff8f6d317ull, 0x108c24a2fda9ca86ull},
            {0xeb822e2134100295ull, 0x22fe28e9293d6719ull},
            {0x655695d672ca66b0ull, 0x399784209a958edaull}
        };
        constexpr uint32_t firstWordGold[CaseCount][2] = {
            {0x1c9ca7feu, 0x2abcb902u}, {0xa42be146u, 0xd0c230c9u},
            {0x44111d06u, 0x861b63feu}, {0x80788fefu, 0x49039f6eu},
            {0xcb83997fu, 0xd5c11a53u}, {0x8379a685u, 0x4aaa4833u},
            {0x4262c11au, 0x95d242f5u}, {0x3a48ad31u, 0x7c1db6eeu},
            {0x7fd3891au, 0x34ab8dc6u}, {0xa3fd58c1u, 0x47941634u},
            {0x1f415e13u, 0xcc354137u}, {0x9674d8d6u, 0x26a5dc97u}
        };
        std::set<std::string> names;
        std::set<uint64_t> inputHashes;
        for (unsigned c = 0; c < CaseCount; ++c) {
            names.insert(caseID(c));
            const Input input = makeInput(c);
            check(input == makeInput(c), "input generation is nondeterministic");
            check(fingerprint(input) == inputGold[c], "authored input vector changed");
            inputHashes.insert(fingerprint(input));
            for (unsigned stage = 0; stage < 2; ++stage) {
                const uint32_t tag = stage ? ComputeTag : RenderTag;
                const Output output = oracle(c, input, tag);
                check(fingerprint(output) == outputGold[c][stage], "full oracle golden vector changed");
                check(output[0] == firstWordGold[c][stage], "oracle first word changed");
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
        check(inputHashes.size() == CaseCount, "case input sets collide");
        for (unsigned family = 0; family < FamilyCount; ++family) {
            // Holding input bytes fixed must still change the output: variants
            // cannot be merely renamed cases or different runtime buffers.
            const Input sharedInput = makeInput(family * VariantCount);
            for (uint32_t stage : {RenderTag, ComputeTag}) {
                std::set<Output> semanticOutputs;
                for (unsigned variant = 0; variant < VariantCount; ++variant) {
                    const unsigned c = family * VariantCount + variant;
                    check(rounds(c) == rounds(family * VariantCount), "family rounds differ across variants");
                    semanticOutputs.insert(oracle(c, sharedInput, stage));
                }
                check(semanticOutputs.size() == VariantCount, "variant has no independent semantic effect");
            }
        }
        // API IDs are legacy-sync, Metal4-sync, legacy-async, Metal4-async.
        for (const auto& base : {std::array<unsigned, 4>{0, 1, 2, 3}, std::array<unsigned, 4>{1, 0, 3, 2}}) {
            for (unsigned family = 0; family < FamilyCount; ++family) {
                unsigned firstCounts[RouteCount]{};
                for (unsigned variant = 0; variant < VariantCount; ++variant) {
                    const unsigned c = family * VariantCount + variant;
                    ++firstCounts[base[routeIndex(0, 0, c)]];
                    unsigned positions[RouteCount][RouteCount]{};
                    for (unsigned iteration = 0; iteration < RepeatCount; ++iteration) {
                        std::set<unsigned> routes;
                        for (unsigned slot = 0; slot < RouteCount; ++slot) {
                            unsigned route = base[routeIndex(iteration, slot, c)];
                            ++positions[route][slot];
                            routes.insert(route);
                        }
                        check(routes.size() == RouteCount, "iteration omitted or duplicated a route");
                    }
                    for (const auto& route : positions)
                        for (unsigned count : route) check(count == 1, "case route position imbalance");
                }
                for (unsigned count : firstCounts) check(count == 1, "family first API imbalance");
            }
        }
        check(rotate(0x81234567u, 0) == 0x81234567u && rotate(0x81234567u, 32) == 0x81234567u,
              "rotation boundary mismatch");
        const auto serialized = littleEndianBytes(std::array<uint32_t, 2>{0x12345678u, 0xaabbccddu});
        check(serialized == std::array<uint8_t, 8>{0x78, 0x56, 0x34, 0x12, 0xdd, 0xcc, 0xbb, 0xaa},
              "input fingerprint is not canonical little endian");
        unsigned rejected = 0;
        try { (void)caseID(CaseCount); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)rounds(CaseCount); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)routeIndex(4, 0, 0); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)routeIndex(0, 4, 0); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)routeIndex(0, 0, CaseCount); } catch (const std::out_of_range&) { ++rejected; }
        try { (void)lane(0, makeInput(0), LaneCount, RenderTag); } catch (const std::out_of_range&) { ++rejected; }
        check(rejected == 6, "invalid authored bounds accepted");
        std::puts("PHONE_WORKLOAD_CPU families=3 cases=12 stages=2 balanced_samples=192 golden_vectors=24 first_api_balanced=1 gpu_execution_verified=0");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
