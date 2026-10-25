// Generated only from self-authored Xenos shader fixtures.
#pragma once
#include <array>
#include <cstdint>
namespace ohframe {
struct ShaderBinding {uint32_t group,binding,slot,minimum_bytes,used;};
struct SizeWord {uint32_t group,binding,word;};
inline constexpr char vertex_entry[]="xe_vs_main";
inline constexpr std::array<ShaderBinding,9> vertex_bindings={{
{0,0,0,4096,1},
{0,1,1,160,0},
{0,2,2,768,1},
{0,3,3,112,1},
{1,0,4,4,1},
{1,1,5,4,1},
{1,2,6,4,1},
{1,3,7,4,1},
{1,4,8,4,1},
}};
inline constexpr std::array<SizeWord,5> vertex_sizes={{
{1,0,0},
{1,1,1},
{1,2,2},
{1,3,3},
{1,4,4},
}};
inline constexpr int vertex_sizes_slot=30;
inline constexpr char pixel_entry[]="xe_ps_main";
inline constexpr std::array<ShaderBinding,9> pixel_bindings={{
{0,1,0,160,0},
{0,2,1,768,0},
{0,3,2,112,1},
{0,4,3,4096,0},
{1,0,4,4,0},
{1,1,5,4,0},
{1,2,6,4,0},
{1,3,7,4,0},
{1,4,8,4,0},
}};
inline constexpr std::array<SizeWord,5> pixel_sizes={{
{1,0,0},
{1,1,1},
{1,2,2},
{1,3,3},
{1,4,4},
}};
inline constexpr int pixel_sizes_slot=-1;
}
