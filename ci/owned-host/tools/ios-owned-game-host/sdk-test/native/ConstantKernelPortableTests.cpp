#include "../../native/MetalConstantValidation.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "check failed at line " << __LINE__ << "\n"; std::abort(); } } while (0)

static std::uint32_t le(const std::uint8_t *p) {
    return std::uint32_t(p[0]) | std::uint32_t(p[1])<<8 | std::uint32_t(p[2])<<16 | std::uint32_t(p[3])<<24;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); std::size_t compared = 0, cases = 0;
    for (auto count : {6,27,37,68,136,1024}) {
        std::ifstream file(std::string(argv[1])+"/case-"+std::to_string(count)+".bin", std::ios::binary);
        CHECK(file); std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        CHECK(bytes.size() >= 20 && memcmp(bytes.data(),"OHCP0001",8)==0);
        std::size_t words = le(bytes.data()+8), values = le(bytes.data()+12), output = le(bytes.data()+16);
        CHECK(words <= 16+13*ohcp::max_records && values <= ohcp::max_values*16 && output <= ohcp::block_bytes);
        CHECK(bytes.size() == 20 + words*4 + values + output);
        std::vector<ohcp::Word> records(words);
        for (std::size_t i=0; i<words; ++i) records[i] = le(bytes.data()+20+i*4);
        CHECK(records[0] == unsigned(count) && ohcp::validate(records.data(), words, values, output));
        std::vector<ohcp::Vec4> data(values/16), blocks(output/16);
        memcpy(data.data(), bytes.data()+20+words*4, values); memset(blocks.data(), 0xa5, output);
        for (unsigned stage=0; stage<5; ++stage) for (unsigned lane=0; lane<256; ++lane)
            ohcp::replay_lane(records.data(), data.data(), blocks.data(), stage, lane);
        CHECK(memcmp(blocks.data(), bytes.data()+20+words*4+values, output)==0);
        auto bad = records; bad[1] = 0; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[16] = 5; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[17] = 0xffffffff; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[18] = 257; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[19] = 0xffffffff; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[words-1] = 0xffffffff; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[3]++; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[17+12] = 0; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[16+3*12+4] |= 8; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[1+4*3] = 0xffffffff; CHECK(!ohcp::validate(bad.data(), words, values, output));
        bad = records; bad[0] = ohcp::max_records+1; CHECK(!ohcp::validate(bad.data(), words, values, output));
        CHECK(!ohcp::validate(records.data(), words-1, values, output));
        CHECK(!ohcp::validate(records.data(), words, values+1, output));
        CHECK(!ohcp::validate(records.data(), words, values, output+1));
        compared += output; ++cases;
    }
    std::vector<ohcp::Word> edge(29,0); edge[0]=1;
    for (int stage=0; stage<5; ++stage) edge[1+stage*3] = stage ? 29 : 28;
    edge[2]=1; edge[3]=1; edge[18]=1; edge[20]=1; edge[28]=0;
    CHECK(ohcp::validate(edge.data(), edge.size(), 16, 16));
    edge[17]=1; CHECK(!ohcp::validate(edge.data(), edge.size(), 16, 16));
    std::cout << "constant_kernel_cases=" << cases << " exact_bytes=" << compared << " malformed_cases=" << cases*14+1 << '\n';
}
