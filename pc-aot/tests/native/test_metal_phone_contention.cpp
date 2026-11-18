#include "phone_contention.h"
#include <cstdio>
#include <set>
#include <stdexcept>

using namespace SkyrimContentionWorkload;
static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> static uint64_t fingerprint(const T& values) {
    uint64_t result = 14695981039346656037ull;
    for (uint32_t value : values)
        for (unsigned byte = 0; byte < 4; ++byte) {
            result ^= (value >> (byte * 8)) & 255u;
            result *= 1099511628211ull;
        }
    return result;
}
int main() {
    try {
        // Independently frozen, explicitly little-endian vectors, not values
        // generated from the oracle under test at test execution time.
        constexpr uint64_t expected[] = {
            0x2f0bd3f103188840ull, 0xf4558196ee1638f7ull, 0x18d990c1a9efda20ull,
            0x522034f2bee45996ull, 0x3a08af7d7ebd9de4ull, 0xd0dc839a44ff3abdull,
            0x67a29c733e56c329ull, 0x6b38f889d49d7f9cull, 0xd4dd264d8d49edd3ull,
            0xd560bab857fff6a0ull, 0x2a9384546ab3c98aull, 0x557b67d2b6ae9d05ull,
            0x6860ec505770fbf7ull, 0xc191d4dfae5607eeull, 0x0150b5b7bc20d7c8ull,
            0x3d268769a7eff791ull, 0xe6d45cdf7246b08aull, 0x4056e59849174129ull,
            0x7ae6bf8fd1c26868ull, 0x729e01956cc1f3abull, 0x2f3e7465b79f6f3full,
            0x9b4541ea826dc379ull, 0x9469f98669234b24ull, 0x4727a06fae70b780ull,
        };
        const auto input = makeInput();
        check(fingerprint(input) == 0x5986ad95adb403d8ull, "fixed input changed");
        auto changed = input;
        for (auto& value : changed) value ^= 0x10203041u;
        std::set<uint32_t> salts;
        std::set<uint64_t> results;
        unsigned index = 0;
        for (unsigned profile = 0; profile < 2; ++profile)
            for (unsigned bank = 0; bank < 4; ++bank)
                for (unsigned role = 0; role < 3; ++role, ++index) {
                    const auto key = salt(profile, bank, role);
                    check(salts.insert(key).second, "salt bank contains duplicate source identity");
                    const auto output = oracle(input, key);
                    check(fingerprint(output) == expected[index], "frozen output mismatch");
                    check(results.insert(fingerprint(output)).second, "salt failed to change semantic output");
                    check(output != oracle(changed, key), "output does not depend on runtime input");
                }
        for (bool flip : {false, true}) {
            unsigned first[2][2][2] = {};
            unsigned combinations[2][4][2][2] = {};
            unsigned profileFirst[2] = {}, requests = 0;
            for (unsigned bank = 0; bank < 4; ++bank)
                for (unsigned ps = 0; ps < 2; ++ps)
                    for (unsigned as = 0; as < 2; ++as)
                        for (unsigned ms = 0; ms < 2; ++ms) {
                            const auto item = plan(bank, ps, as, ms, flip);
                            ++combinations[item.profile][bank][item.metal4][item.contended];
                            if (as == 0 && ms == 0) ++first[item.profile][item.metal4][item.contended];
                            if (ps == 0 && as == 0 && ms == 0) ++profileFirst[item.profile];
                            requests += 3;
                        }
            check(requests == 96 && profileFirst[0] == 2 && profileFirst[1] == 2, "work/profile order unbalanced");
            for (unsigned p = 0; p < 2; ++p)
                for (unsigned a = 0; a < 2; ++a)
                    for (unsigned m = 0; m < 2; ++m) {
                        check(first[p][a][m] == 1, "API/mode first encounter unbalanced");
                        for (unsigned b = 0; b < 4; ++b)
                            check(combinations[p][b][a][m] == 1, "matched bank missing or repeated");
                    }
        }
        bool refused = false;
        try { (void)salt(2, 0, 0); } catch (const std::out_of_range&) { refused = true; }
        check(refused, "out-of-bank salt accepted");
        refused = false;
        try { (void)plan(4, 0, 0, 0, false); } catch (const std::out_of_range&) { refused = true; }
        check(refused, "out-of-bank scheduling plan accepted");
        std::puts("PHONE_CONTENTION_CPU golden_vectors=24 balanced_trials=32 requests=96 base_orders=2 gpu_execution_verified=0");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
