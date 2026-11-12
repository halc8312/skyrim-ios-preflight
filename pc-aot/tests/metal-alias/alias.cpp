// Authored binding fixture, matching the combined-image binding API MoltenVK uses.
#include "spirv_msl.hpp"
#include <fstream>
#include <iostream>
#include <vector>
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
        if (!in) return 2;
        const auto bytes = in.tellg();
        if (bytes <= 0 || bytes % 4) return 2;
        std::vector<uint32_t> words(static_cast<size_t>(bytes) / 4);
        in.seekg(0);
        if (!in.read(reinterpret_cast<char *>(words.data()), bytes)) return 2;
        spirv_cross::CompilerMSL compiler(std::move(words));
        auto options = compiler.get_msl_options();
        options.platform = spirv_cross::CompilerMSL::Options::iOS;
        options.set_msl_version(3, 1);
        options.argument_buffers = true;
        options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
        compiler.set_msl_options(options);
        for (uint32_t n = 0; n != 2; ++n) {
            spirv_cross::MSLResourceBinding binding;
            binding.stage = spv::ExecutionModelFragment;
            binding.desc_set = 0;
            binding.binding = n;
            binding.count = 1;
            binding.msl_texture = n * 2;
            binding.msl_sampler = n * 2 + 1;
            binding.basetype = spirv_cross::SPIRType::SampledImage;
            compiler.add_msl_resource_binding(binding);
        }
        auto msl = compiler.compile();
        std::ofstream out(argv[2], std::ios::binary);
        out << msl;
        return out ? 0 : 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
