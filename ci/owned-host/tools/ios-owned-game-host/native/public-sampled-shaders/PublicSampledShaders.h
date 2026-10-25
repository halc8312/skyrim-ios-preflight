// Generated exclusively from self-authored Xenos shader instructions.
#pragma once
#include <array>
#include <cstdint>
#include "../public-shaders/PublicFrameShaders.h"
namespace ohsample {
struct SampledBinding {uint32_t binding,slot,kind,used;};
inline constexpr char pixel_entry[]="xe_ps_main";
inline constexpr std::array<ohframe::ShaderBinding,9> pixel_buffers={{
{0,1,0,160,0},
{0,2,1,768,1},
{0,3,2,112,1},
{0,4,3,4096,0},
{1,0,4,4,0},
{1,1,5,4,0},
{1,2,6,4,0},
{1,3,7,4,0},
{1,4,8,4,0},
}};
inline constexpr std::array<SampledBinding,2> pixel_samples={{
{0,0,1,1},
{1,0,2,1},
}};
inline constexpr std::array<ohframe::SizeWord,5> pixel_sizes={{
{1,0,0},
{1,1,1},
{1,2,2},
{1,3,3},
{1,4,4},
}};
inline constexpr int pixel_sizes_slot=-1;
}
