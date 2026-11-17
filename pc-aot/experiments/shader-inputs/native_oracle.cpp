// Authored synthetic fixtures only. No input path or bytecode argument exists.
// The generated header contains only literals defined in native_check.py.
#include "dxso/dxso_module.h"
#include "dxvk/dxvk_shader_key.h"
#include "util/sha1/sha1_util.h"
#include <iostream>
#include <vector>
#include "synthetic_cases.h"

namespace dxvk { Logger Logger::s_instance("authored-dxso-oracle.log"); }
namespace dxvk::env { std::string getExePath() { return "authored-dxso-oracle"; } }

int main(int argc, char**) {
  if (argc != 1) return 2;
  for (const auto& fixture : fixtures) {
    // Exact-sized owned input permits ASan to observe a past-end decoder read.
    std::vector<uint32_t> tokens(fixture.words, fixture.words + fixture.count);
    dxvk::DxsoReader reader(reinterpret_cast<const char*>(tokens.data()));
    dxvk::DxsoModule module(reader);
    const auto analysis = module.analyze();
    if (analysis.bytecodeByteLength > tokens.size() * sizeof(uint32_t)) return 3;
    const auto stage = module.info().shaderStage();
    const dxvk::DxvkShaderKey key(stage,
      dxvk::Sha1Hash::compute(tokens.data(), analysis.bytecodeByteLength));
    std::cout << "{\"case\":\"" << fixture.name << "\",\"stage_vk\":" << stage
      << ",\"analyzed_bytes\":" << analysis.bytecodeByteLength
      << ",\"dxvk_key\":\"" << key.toString() << "\"}\n";
  }
}
