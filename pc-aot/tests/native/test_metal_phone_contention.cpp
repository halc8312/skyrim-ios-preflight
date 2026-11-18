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
        constexpr uint64_t legacyExpected[] = {
            0x2f0bd3f103188840ull, 0xf4558196ee1638f7ull, 0x18d990c1a9efda20ull,
            0x522034f2bee45996ull, 0x3a08af7d7ebd9de4ull, 0xd0dc839a44ff3abdull,
            0x67a29c733e56c329ull, 0x6b38f889d49d7f9cull, 0xd4dd264d8d49edd3ull,
            0xd560bab857fff6a0ull, 0x2a9384546ab3c98aull, 0x557b67d2b6ae9d05ull,
            0x6860ec505770fbf7ull, 0xc191d4dfae5607eeull, 0x0150b5b7bc20d7c8ull,
            0x3d268769a7eff791ull, 0xe6d45cdf7246b08aull, 0x4056e59849174129ull,
            0x7ae6bf8fd1c26868ull, 0x729e01956cc1f3abull, 0x2f3e7465b79f6f3full,
            0x9b4541ea826dc379ull, 0x9469f98669234b24ull, 0x4727a06fae70b780ull,
        };
        constexpr uint64_t panelExpected[] = {
            0x52838a4f3a3ff147ull, 0x74fe5a542523ed51ull, 0xaff648a487c136b0ull,
            0xb44be371bd0eb53bull, 0x1ce781369fc55b89ull, 0x405459a5d2704483ull,
            0x94dfb54eeaac6540ull, 0x4459911df901a3ccull, 0x4233bcf90353188bull,
            0xb92418241eb3bce8ull, 0x803b76881320e67eull, 0x70cf7614250bf4c9ull,
            0x0b44d7928876c4aaull, 0x1c98d38177adc643ull, 0x4f546fef9f78214bull,
            0x05e9796c414d8107ull, 0x1bb3506546718bafull, 0x1629c231b70d070dull,
            0xde63103a86742334ull, 0xf6ba01a7925f9a82ull, 0x9229af5acbce7da3ull,
            0x1c657e3d9bb1c1faull, 0x3fc505cfbdcf23c3ull, 0x84729555da945798ull,
            0x4f10787763abe36full, 0x043c5865eabbd738ull, 0x0789caa63a0613ebull,
            0xfb51d4f280a27a91ull, 0x101ac793c6884f7eull, 0x33a4269a1c8c1590ull,
            0xe5d9203aa1ee2274ull, 0xe84d582708d04862ull, 0xd7d80cbea70726bdull,
            0xca819e759802ac22ull, 0x26b76de0f1770050ull, 0x49cc3c109b454780ull,
            0xca990aafead25a7eull, 0x186868c5376d0c58ull, 0x1878df75f13f343aull,
            0x3bcf321e54b21aadull, 0x1cf4f31a5d6da63aull, 0xdbc5282a195a756eull,
            0x3f3eec8befd47ed1ull, 0x17d2fefda734b2cfull, 0x677095de32e59c23ull,
            0xa4f944660ac83e1aull, 0xb302794d42c9b246ull, 0x37beef458d4018f0ull,
            0x4c15fcb8bfbc1a3eull, 0xefa943b7a904d3b6ull, 0x7e3d87b37cdb81b6ull,
            0x5dd8812d37fa2ff6ull, 0x2ff6fb892d21d576ull, 0x7412cab2a286b14dull,
            0x69005df9de44c1daull, 0xed795ac244e29961ull, 0x96bd66794d697859ull,
            0xeee157a946603ea9ull, 0xdde17401be629c10ull, 0x2223a2fe0815f412ull,
            0xc5e882e6a56c9cb4ull, 0x527cbc45ddd2fff7ull, 0xabb104b0f940fc4eull,
            0x734e18d7993dc86full, 0xd542009d61974b96ull, 0xeb1232e78cf788a6ull,
            0xf7ddaf6408d70bb4ull, 0xba9806f88ac81b4eull, 0x12e32ed00ba26220ull,
            0xb9a784d910335f07ull, 0xd9fd3d3b547828b2ull, 0x0428d3196abd189aull,
            0x4e7d05d9ff282a63ull, 0x624029b4dce2835aull, 0xb2a41a2e5d22f25bull,
            0x5cb5baf4e8f7b68bull, 0xc1f7558eb0cf0915ull, 0xf8705f47838ad110ull,
            0x471a07e630cc7308ull, 0x9d4e2ca8939c9ca1ull, 0xc8f5b96193a900cbull,
            0x17b94eb243441324ull, 0xbdd6fc4e2f09d237ull, 0x1712586c419c7684ull,
            0xfa5dd1e87e7c5df3ull, 0x12373e76592d768full, 0xb2ccd4276f0c95a9ull,
            0xf471b22083d7773aull, 0xd2192b0d74391c68ull, 0x96eba995acd4d238ull,
            0x6a597985aed96626ull, 0xe29c3b44ff3a49a3ull, 0x9fa388aeef426ea2ull,
            0x522802ecbee78f84ull, 0x45d21847f9ba567cull, 0x23e07e6d6d5fe956ull,
            0xe061e42f4a1fb1b9ull, 0x78a08ae11919de04ull, 0xf92f55c49f499ef3ull,
            0x32ecc2700da575a5ull, 0x19d3c8e009fd4587ull, 0x83e33740b34e348eull,
            0x82f39612a1013a32ull, 0x2536adcd647fc816ull, 0x99f749c0202ca780ull,
            0x42f5046a8a91f78eull, 0x94929a4c8834262aull, 0xea9cefe133faf6d0ull,
            0x82578364f601b315ull, 0x8626690b0c601bdbull, 0xeb55638394f417dcull,
            0x1f7d218045814e0aull, 0xa0b03279d337c938ull, 0x99b66cd67e717e38ull,
            0x247551b4940ed07aull, 0xe78ea12a30edbe31ull, 0x6d7898d6bf7b20c1ull,
            0x0b24cabd02a47cd2ull, 0x3d3d5597c4cbfa14ull, 0x8701a40907c244b6ull,
            0xb24a9b576fea78d2ull, 0x5ecfbc623e9f3fedull, 0x0ffc4721c68f1410ull,
            0xff7b4e2bfc6f3679ull, 0x03cc6abdd4d37291ull, 0x31b0a6cdbfa84adaull,
            0xaf6264bf0f3cec5cull, 0x58155e2381de4fd0ull, 0x210bcd6df4945a48ull,
            0x49b64a346a8ea0afull, 0xc342d38d115d136aull, 0x90f4847ef62bb31bull,
            0xab071f640abb6a5bull, 0x928891baac4dd288ull, 0x347e292bf52fa367ull,
            0xa19cc271ecf75bb7ull, 0xcb1b00ba24f909a9ull, 0x39d876610f84fe28ull,
            0xd41ffeeb6ae543b5ull, 0x09d66124dd5d56ddull, 0x7ee654378716683dull,
            0x64ea988b657b2c50ull, 0xa1fb049534730192ull, 0xa800a13ad7deb3a6ull,
            0xa13f357f21d926eeull, 0x5ca6aa8d83cc5ca8ull, 0xa2001bd135b61977ull,
            0x1467f7dcb6b0d15dull, 0xf1566886dcaca529ull, 0x6a9ee464b7550193ull,
            0xd4313f3079e995bdull, 0xd2d6a54e753cd811ull, 0xaf4f3ff3321fb00eull,
            0xeea2df17c4205874ull, 0xeb3f119c2208a840ull, 0xb638329a3f471504ull,
            0xf8dba20a3cfd2969ull, 0x25daeed9d25f8c2cull, 0x4ccebd5e5538bdc5ull,
            0x39b75cabb5e3aac4ull, 0x0a7d77e42b5934faull, 0x842d74742e8701a5ull,
            0x15160f7a4557ce2bull, 0x9ad88da193c24ee8ull, 0xd8b283b62bdb2ee3ull,
            0xb5335639d5cef5f2ull, 0x82e2e01ae63d3119ull, 0xb85185ff5e8242a7ull,
            0xe3a49b95463a2794ull, 0x0903a42b43b59710ull, 0xeca6a1e1a44ebb08ull,
            0x5b3a71a725ab381cull, 0xb76e7612075759adull, 0xb3cdc8119baf746bull,
            0xd4ea807cf807cc46ull, 0x6cc405f542d8c3d6ull, 0x64fa5a9904ddfc3eull,
            0x935be2a1db45f868ull, 0x8295dfdea3555c8dull, 0x52d0f8e55cd14dd8ull,
            0x25cb4c3869e43c4eull, 0xb0028e176b2bb66bull, 0x6238de72be2da1a6ull,
            0x524123fc6920af00ull, 0x414d60be83574936ull, 0x2aacbad8b8aa9f07ull,
            0xbdac9635daac999dull, 0xa98538f8919158a4ull, 0x84bc2f6292556504ull,
            0x49d89fb3f0bf87fbull, 0x7ab7f1c0823fcae6ull, 0xee59841043b1920bull,
        };
        const auto input = makeInput();
        check(fingerprint(input) == 0x5986ad95adb403d8ull, "fixed input changed");
        auto changed = input;
        for (auto& value : changed) value ^= 0x10203041u;
        std::set<uint32_t> salts;
        std::set<uint64_t> results;
        for (unsigned group = 1; group <= 72; ++group)
                for (unsigned role = 0; role < 3; ++role) {
                    const auto key = groupSalt(group, role);
                    check(salts.insert(key).second, "salt bank contains duplicate source identity");
                    const auto output = oracle(input, key);
                    const auto expected = group <= 8 ? legacyExpected[(group - 1) * 3 + role]
                                                      : panelExpected[(group - 9) * 3 + role];
                    check(fingerprint(output) == expected, "frozen output mismatch");
                    check(results.insert(fingerprint(output)).second, "salt failed to change semantic output");
                    check(output != oracle(changed, key), "output does not depend on runtime input");
                }
        for (bool flip : {false, true}) {
            constexpr unsigned expectedGroups[2][32] = {
                {9,10,12,11,14,15,13,16,20,17,19,18,21,22,24,23,27,28,26,25,32,29,31,30,34,35,33,36,39,40,38,37},
                {43,44,42,41,48,45,47,46,50,51,49,52,55,56,54,53,57,58,60,59,62,63,61,64,68,65,67,66,69,70,72,71}
            };
            unsigned totals[4] = {}, positions[4][4] = {}, lanes[4][4] = {}, withinBlockPairs[4][4] = {};
            std::set<unsigned> groups;
            std::set<uint32_t> normalSalts;
            for (unsigned block = 0; block < BlockCount; ++block) {
                std::set<unsigned> conditions;
                for (unsigned position = 0; position < Conditions; ++position) {
                    const auto item = plan(block, position, flip);
                    check(item.block == block && item.position == position, "plan location mismatch");
                    check(item.sourceGroup == expectedGroups[unsigned(flip)][block * 4 + position], "frozen source-group assignment mismatch");
                    check(conditions.insert(item.condition).second, "condition repeated inside block");
                    check(groups.insert(item.sourceGroup).second, "source group repeated inside panel");
                    ++totals[item.condition]; ++positions[item.condition][position]; ++lanes[item.condition][item.lane];
                    if (position) ++withinBlockPairs[plan(block, position - 1, flip).condition][item.condition];
                    for (unsigned role = 0; role < 3; ++role)
                        check(normalSalts.insert(groupSalt(item.sourceGroup, role)).second, "normal source identity repeated");
                    const auto other = plan(block, position, !flip);
                    check(item.metal4 != other.metal4 && item.contended == other.contended,
                          "alternate selector changed more than backend assignment");
                }
            }
            check(groups.size() == 32 && normalSalts.size() == 96 && *groups.begin() == panelFirstGroup(flip) &&
                  *groups.rbegin() == panelFirstGroup(flip) + 31, "panel bounds/unique identities mismatch");
            check(plan(0, 0, flip).metal4 == flip, "selector did not change initial backend");
            for (unsigned c = 0; c < 4; ++c) {
                check(totals[c] == 8, "condition count differs from eight");
                for (unsigned p = 0; p < 4; ++p) {
                    check(positions[c][p] == 2 && lanes[c][p] == 2, "condition position/lane unbalanced");
                    check(withinBlockPairs[c][p] == (c == p ? 0u : 2u), "within-block Williams carryover unbalanced");
                }
            }
        }
        bool refused = false;
        try { (void)groupSalt(0, 0); } catch (const std::out_of_range&) { refused = true; }
        check(refused, "out-of-bank salt accepted");
        refused = false;
        try { (void)groupSalt(73, 0); } catch (const std::out_of_range&) { refused = true; }
        check(refused, "out-of-panel salt accepted");
        refused = false;
        try { (void)plan(8, 0, false); } catch (const std::out_of_range&) { refused = true; }
        check(refused, "out-of-bank scheduling plan accepted");
        std::puts("PHONE_CONTENTION_CPU golden_vectors=216 panels=2 balanced_trials=32 requests=96 unique_per_panel=96 gpu_execution_verified=0");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
