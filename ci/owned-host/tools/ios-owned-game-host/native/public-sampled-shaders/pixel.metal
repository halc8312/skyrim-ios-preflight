// language: metal2.4
#include <metal_stdlib>
#include <simd/simd.h>

using metal::uint;
struct DefaultConstructible {
    template<typename T>
    operator T() && {
        return T {};
    }
};

struct _mslBufferSizes {
    uint size4;
    uint size5;
    uint size6;
    uint size7;
    uint size8;
};

struct XeSystemConstants {
    uint flags;
    uint vertex_index_endian;
    int vertex_base_index;
    uint vertex_index_load_address;
    uint vertex_index_min;
    uint vertex_index_max;
    uint alpha_test_function;
    float alpha_test_reference;
    metal::packed_float3 ndc_scale;
    float point_vertex_diameter_min;
    metal::packed_float3 ndc_offset;
    float point_vertex_diameter_max;
    metal::float2 point_constant_diameter;
    metal::float2 point_screen_diameter_to_ndc_radius;
    metal::float2 ps_param_gen_offset;
    uint ps_param_gen_register;
    float resolution_scale;
    metal::float4 color_exp_bias;
};
struct type_7 {
    metal::uint4 inner[2];
};
struct type_8 {
    metal::uint4 inner[8];
};
struct XeBoolLoopConstants {
    type_7 bools;
    type_8 loops;
};
struct type_9 {
    metal::uint4 inner[48];
};
struct XeFetchConstants {
    type_9 words;
};
struct type_10 {
    metal::float4 inner[256];
};
typedef uint type_11[1];
struct type_13 {
    uint inner[64];
};
struct type_16 {
    metal::float4 inner[1];
};
struct type_18 {
    uint inner[4];
};
struct type_19 {
    metal::float4 inner[5];
};
struct type_20 {
    metal::float4 inner[4];
};
struct XePixelInput {
    metal::float4 frag_coord;
    bool front_facing;
    char _pad2[15];
    metal::float4 i0_;
};
struct XePixelOutput {
    uint coverage;
    char _pad1[12];
    metal::float4 color0_;
};
constant float XE_FLT_MAX = 340282350000000000000000000000000000000.0;
constant type_13 xe_texture_formats = type_13 {{9u, 9u, 65u, 2263596u, 166955u, 199211u, 17043524u, 4527188u, 65u, 65u, 4162u, 17043524u, 17043524u, 8322u, 17043524u, 8521764u, 333403u, 366163u, 4u, 4u, 4u, 34087044u, 1u, 1u, 129u, 8322u, 34087044u, 1u, 2u, 4u, 1u, 2u, 4u, 1u, 2u, 4u, 1u, 2u, 4u, 65u, 4162u, 129u, 8322u, 65u, 65u, 4162u, 129u, 129u, 8322u, 4162u, 17043524u, 4u, 4u, 4u, 4527188u, 333403u, 366163u, 3u, 65u, 65u, 4162u, 4u, 17043524u, 4u}};

uint xe_shared_load(
    uint i,
    device type_11 const& xe_shared_memory_0_,
    device type_11 const& xe_shared_memory_1_,
    device type_11 const& xe_shared_memory_2_,
    device type_11 const& xe_shared_memory_3_,
    device type_11 const& xe_shared_table,
    constant _mslBufferSizes& _buffer_sizes
) {
    uint block = i >> 14u;
    if (block >= 8192u) {
        return 0u;
    }
    uint slot = uint(block) < 1 + (_buffer_sizes.size8 - 0 - 4) / 4 ? xe_shared_table[block] : DefaultConstructible();
    if (slot == 0u) {
        return 0u;
    }
    uint o = (((slot - 1u) << 14u) & 8388607u) | (i & 16383u);
    switch((slot - 1u) >> 9u) {
        case 0u: {
            if (o < (1 + (_buffer_sizes.size4 - 0 - 4) / 4)) {
                uint _e30 = uint(o) < 1 + (_buffer_sizes.size4 - 0 - 4) / 4 ? xe_shared_memory_0_[o] : DefaultConstructible();
                return _e30;
            }
            break;
        }
        case 1u: {
            if (o < (1 + (_buffer_sizes.size5 - 0 - 4) / 4)) {
                uint _e36 = uint(o) < 1 + (_buffer_sizes.size5 - 0 - 4) / 4 ? xe_shared_memory_1_[o] : DefaultConstructible();
                return _e36;
            }
            break;
        }
        case 2u: {
            if (o < (1 + (_buffer_sizes.size6 - 0 - 4) / 4)) {
                uint _e42 = uint(o) < 1 + (_buffer_sizes.size6 - 0 - 4) / 4 ? xe_shared_memory_2_[o] : DefaultConstructible();
                return _e42;
            }
            break;
        }
        case 3u: {
            if (o < (1 + (_buffer_sizes.size7 - 0 - 4) / 4)) {
                uint _e48 = uint(o) < 1 + (_buffer_sizes.size7 - 0 - 4) / 4 ? xe_shared_memory_3_[o] : DefaultConstructible();
                return _e48;
            }
            break;
        }
        default: {
            break;
        }
    }
    return 0u;
}

metal::float4 xe_mul(
    metal::float4 a,
    metal::float4 b
) {
    return metal::select(a * b, metal::float4(0.0), (a == metal::float4(0.0)) | (b == metal::float4(0.0)));
}

float xe_mul1_(
    float a_1,
    float b_1
) {
    bool local_2 = {};
    if (!((a_1 == 0.0))) {
        local_2 = b_1 == 0.0;
    } else {
        local_2 = true;
    }
    bool _e11 = local_2;
    return _e11 ? 0.0 : (a_1 * b_1);
}

metal::float4 xe_max(
    metal::float4 a_2,
    metal::float4 b_2
) {
    return metal::select(b_2, a_2, a_2 >= b_2);
}

metal::float4 xe_min(
    metal::float4 a_3,
    metal::float4 b_3
) {
    return metal::select(b_3, a_3, a_3 < b_3);
}

float xe_max1_(
    float a_4,
    float b_4
) {
    return (a_4 >= b_4) ? a_4 : b_4;
}

float xe_min1_(
    float a_5,
    float b_5
) {
    return (a_5 < b_5) ? a_5 : b_5;
}

bool xe_isfinite(
    float x_1
) {
    return (as_type<uint>(x_1) & 2139095040u) != 2139095040u;
}

float xe_rfp(
    float v
) {
    uint r = {};
    uint bits = as_type<uint>(v);
    uint truncated = bits & 4294967292u;
    uint rounded = truncated + 4u;
    bool overflow = (rounded & 2139095040u) == 2139095040u;
    r = ((bits & 3u) >= 2u) ? (overflow ? truncated : rounded) : truncated;
    uint _e17 = r;
    r = ((bits & 2139095040u) == 2139095040u) ? bits : _e17;
    uint _e23 = r;
    return as_type<float>(_e23);
}

float xe_inf_to_max(
    float v_1
) {
    uint bits_1 = as_type<uint>(v_1);
    if (bits_1 == 2139095040u) {
        return XE_FLT_MAX;
    }
    if (bits_1 == 4286578688u) {
        return -340282350000000000000000000000000000000.0;
    }
    return v_1;
}

float xe_inf_to_zero(
    float v_2
) {
    uint bits_2 = as_type<uint>(v_2);
    if (bits_2 == 2139095040u) {
        return 0.0;
    }
    if (bits_2 == 4286578688u) {
        return -0.0;
    }
    return v_2;
}

float xe_dp4_(
    metal::float4 a_6,
    metal::float4 b_6
) {
    metal::float4 _e2 = xe_mul(a_6, b_6);
    return (((0.0 + _e2.x) + _e2.y) + _e2.z) + _e2.w;
}

float xe_dp3_(
    metal::float4 a_7,
    metal::float4 b_7
) {
    metal::float4 _e2 = xe_mul(a_7, b_7);
    return ((0.0 + _e2.x) + _e2.y) + _e2.z;
}

float xe_dp2add(
    metal::float4 a_8,
    metal::float4 b_8,
    float c
) {
    metal::float4 _e3 = xe_mul(a_8, b_8);
    return ((0.0 + _e3.x) + _e3.y) + c;
}

metal::float4 xe_cube(
    metal::float4 s
) {
    metal::float4 r_1 = {};
    bool local_3 = {};
    float x_2 = s.z;
    float y_1 = s.w;
    float z = s.x;
    if (metal::abs(z) >= metal::abs(x_2)) {
        local_3 = metal::abs(z) >= metal::abs(y_1);
    } else {
        local_3 = false;
    }
    bool _e14 = local_3;
    if (_e14) {
        bool n_1 = z < 0.0;
        r_1 = metal::float4(-(y_1), n_1 ? -(x_2) : x_2, z, n_1 ? 5.0 : 4.0);
    } else {
        if (metal::abs(y_1) >= metal::abs(x_2)) {
            bool n_2 = y_1 < 0.0;
            r_1 = metal::float4(n_2 ? -(z) : z, x_2, y_1, n_2 ? 3.0 : 2.0);
        } else {
            bool n_3 = x_2 < 0.0;
            r_1 = metal::float4(-(y_1), n_3 ? z : -(z), x_2, n_3 ? 1.0 : 0.0);
        }
    }
    float _e46 = r_1.z;
    r_1.z = _e46 * 2.0;
    metal::float4 _e49 = r_1;
    return _e49;
}

float xe_max4_(
    metal::float4 v_3
) {
    bool local_4 = {};
    bool local_5 = {};
    bool local_6 = {};
    if (v_3.x >= v_3.y) {
        local_4 = v_3.x >= v_3.z;
    } else {
        local_4 = false;
    }
    bool _e10 = local_4;
    if (_e10) {
        local_5 = v_3.x >= v_3.w;
    } else {
        local_5 = false;
    }
    bool _e17 = local_5;
    if (_e17) {
        return v_3.x;
    }
    if (v_3.y >= v_3.z) {
        local_6 = v_3.y >= v_3.w;
    } else {
        local_6 = false;
    }
    bool _e28 = local_6;
    if (_e28) {
        return v_3.y;
    }
    if (v_3.z >= v_3.w) {
        return v_3.z;
    }
    return v_3.w;
}

bool xe_bool(
    uint b_9,
    constant XeBoolLoopConstants& xe_bool_loop
) {
    uint _e4 = b_9 >> 7u;
    uint _e9 = (b_9 >> 5u) & 3u;
    uint _e11 = uint(_e9) < 4 && uint(_e4) < 2 ? xe_bool_loop.bools.inner[_e4][_e9] : DefaultConstructible();
    return ((_e11 >> (b_9 & 31u)) & 1u) != 0u;
}

uint xe_loop_constant(
    uint i_1,
    constant XeBoolLoopConstants& xe_bool_loop
) {
    uint _e4 = i_1 >> 2u;
    uint _e7 = i_1 & 3u;
    uint _e9 = uint(_e7) < 4 && uint(_e4) < 8 ? xe_bool_loop.loops.inner[_e4][_e7] : DefaultConstructible();
    return _e9;
}

uint xe_fetch_word(
    uint i_2,
    constant XeFetchConstants& xe_fetch
) {
    uint _e4 = i_2 >> 2u;
    uint _e7 = i_2 & 3u;
    uint _e9 = uint(_e7) < 4 && uint(_e4) < 48 ? xe_fetch.words.inner[_e4][_e7] : DefaultConstructible();
    return _e9;
}

metal::float4 xe_float_constant(
    int index,
    device type_10 const& xe_float_constants
) {
    bool local_7 = {};
    if (!((index < 0))) {
        local_7 = index > 255;
    } else {
        local_7 = true;
    }
    bool _e9 = local_7;
    if (_e9) {
        return metal::float4(0.0);
    }
    uint _e13 = static_cast<uint>(index);
    metal::float4 _e15 = uint(_e13) < 256 ? xe_float_constants.inner[_e13] : DefaultConstructible();
    return _e15;
}

uint xe_swap32_(
    uint v_4,
    uint endian
) {
    uint r_2 = {};
    bool local_8 = {};
    bool local_9 = {};
    r_2 = v_4;
    if (!((endian == 1u))) {
        local_8 = endian == 2u;
    } else {
        local_8 = true;
    }
    bool _e11 = local_8;
    if (_e11) {
        uint _e12 = r_2;
        uint _e17 = r_2;
        r_2 = ((_e12 >> 8u) & 16711935u) | ((_e17 & 16711935u) << 8u);
    }
    if (!((endian == 2u))) {
        local_9 = endian == 3u;
    } else {
        local_9 = true;
    }
    bool _e31 = local_9;
    if (_e31) {
        uint _e32 = r_2;
        uint _e35 = r_2;
        r_2 = (_e32 >> 16u) | (_e35 << 16u);
    }
    uint _e39 = r_2;
    return _e39;
}

uint xe_vfetch_word(
    int address,
    int end,
    uint endian_1,
    device type_11 const& xe_shared_memory_0_,
    device type_11 const& xe_shared_memory_1_,
    device type_11 const& xe_shared_memory_2_,
    device type_11 const& xe_shared_memory_3_,
    device type_11 const& xe_shared_table,
    constant _mslBufferSizes& _buffer_sizes
) {
    if (static_cast<uint>(address) >= static_cast<uint>(end)) {
        return 0u;
    }
    uint _e8 = xe_shared_load(static_cast<uint>(address), xe_shared_memory_0_, xe_shared_memory_1_, xe_shared_memory_2_, xe_shared_memory_3_, xe_shared_table, _buffer_sizes);
    uint _e9 = xe_swap32_(_e8, endian_1);
    return _e9;
}

float xe_half_to_f32_(
    uint h
) {
    float s_2 = ((h & 32768u) != 0u) ? -1.0 : 1.0;
    uint e = (h >> 10u) & 31u;
    float m = static_cast<float>(h & 1023u);
    if (e == 0u) {
        return s_2 * metal::ldexp(m, -24);
    }
    return s_2 * metal::ldexp(1.0 + (m * 0.0009765625), as_type<int>(as_type<uint>(static_cast<int>(e)) - as_type<uint>(15)));
}

float xe_clamp_point_size(
    float v_5,
    constant XeSystemConstants& xe_system
) {
    float _e3 = xe_system.point_vertex_diameter_min;
    int lo = as_type<int>(_e3);
    float _e7 = xe_system.point_vertex_diameter_max;
    int hi = as_type<int>(_e7);
    return as_type<float>(metal::min(metal::max(as_type<int>(v_5), lo), hi));
}

float xe_pwl_gamma_to_linear(
    float gamma
) {
    float l = {};
    float g = metal::clamp(gamma, 0.0, 1.0);
    bool p1_ = g >= 0.2509804;
    bool p2_ = g >= 0.3764706;
    bool p3_ = g >= 0.7529412;
    float scale = p2_ ? (p3_ ? 0.0078125 : 0.00390625) : (p1_ ? 0.001953125 : 0.0009765625);
    float offset = p2_ ? (p3_ ? -1024.0 : -256.0) : (p1_ ? -64.0 : 0.0);
    l = ((g * 261120.0) * scale) + offset;
    float _e29 = l;
    float _e30 = l;
    l = _e29 + metal::trunc(_e30 * scale);
    float _e34 = l;
    return _e34 * 0.0009775171;
}

uint xe_tex_data_dimension(
    uint fc,
    constant XeFetchConstants& xe_fetch
) {
    uint _e5 = xe_fetch_word((fc * 6u) + 5u, xe_fetch);
    return (_e5 >> 9u) & 3u;
}

bool xe_tex_is_3d(
    uint fc_1,
    constant XeFetchConstants& xe_fetch
) {
    uint _e1 = xe_tex_data_dimension(fc_1, xe_fetch);
    return _e1 == 2u;
}

metal::float2 xe_tex_size_2d(
    uint fc_2,
    constant XeFetchConstants& xe_fetch
) {
    uint _e5 = xe_fetch_word((fc_2 * 6u) + 2u, xe_fetch);
    uint _e6 = xe_tex_data_dimension(fc_2, xe_fetch);
    if (_e6 == 0u) {
        return metal::float2(static_cast<float>((_e5 & 16777215u) + 1u), 1.0);
    }
    return metal::float2(static_cast<float>((_e5 & 8191u) + 1u), static_cast<float>(((_e5 >> 13u) & 8191u) + 1u));
}

metal::float3 xe_tex_size_3d(
    uint fc_3,
    constant XeFetchConstants& xe_fetch
) {
    uint _e5 = xe_fetch_word((fc_3 * 6u) + 2u, xe_fetch);
    bool _e6 = xe_tex_is_3d(fc_3, xe_fetch);
    if (_e6) {
        return metal::float3(static_cast<float>((_e5 & 2047u) + 1u), static_cast<float>(((_e5 >> 11u) & 2047u) + 1u), static_cast<float>((_e5 >> 22u) + 1u));
    }
    return metal::float3(static_cast<float>((_e5 & 8191u) + 1u), static_cast<float>(((_e5 >> 13u) & 8191u) + 1u), static_cast<float>((_e5 >> 26u) + 1u));
}

float xe_tex_lod_bias(
    uint fc_4,
    constant XeFetchConstants& xe_fetch
) {
    uint _e5 = xe_fetch_word((fc_4 * 6u) + 4u, xe_fetch);
    return static_cast<float>(metal::extract_bits(as_type<int>(_e5), metal::min(12u, 32u), metal::min(10u, 32u - metal::min(12u, 32u)))) * 0.03125;
}

metal::float2 xe_tex_grad_exp_adjust(
    uint fc_5,
    constant XeFetchConstants& xe_fetch
) {
    uint _e5 = xe_fetch_word((fc_5 * 6u) + 4u, xe_fetch);
    int w4_ = as_type<int>(_e5);
    return metal::float2(static_cast<float>(metal::extract_bits(w4_, metal::min(22u, 32u), metal::min(5u, 32u - metal::min(22u, 32u)))), static_cast<float>(metal::extract_bits(w4_, metal::min(27u, 32u), metal::min(5u, 32u - metal::min(27u, 32u)))));
}

bool xe_tex_vol_linear(
    uint fc_6,
    constant XeFetchConstants& xe_fetch
) {
    uint _e5 = xe_fetch_word((fc_6 * 6u) + 4u, xe_fetch);
    return (_e5 & 3u) != 0u;
}

uint naga_f2u32(float value) {
    return static_cast<uint>(metal::clamp(value, 0.0, 4294967000.0));
}

metal::float3 xe_cube_direction(
    metal::float3 c_1
) {
    float sc = (c_1.x * 2.0) - 3.0;
    float tc = (c_1.y * 2.0) - 3.0;
    uint face = naga_f2u32(metal::clamp(c_1.z, 0.0, 5.0));
    bool n_4 = (face & 1u) != 0u;
    float sign_1 = n_4 ? -1.0 : 1.0;
    switch(face >> 1u) {
        case 0u: {
            return metal::float3(sign_1, -(tc), n_4 ? sc : -(sc));
        }
        case 1u: {
            return metal::float3(sc, sign_1, n_4 ? -(tc) : tc);
        }
        default: {
            return metal::float3(n_4 ? -(sc) : sc, -(tc), sign_1);
        }
    }
}

float xe_tex_component(
    float v_6,
    uint sign,
    uint width,
    bool integer
) {
    float r_3 = {};
    float n = {};
    bool local_10 = {};
    bool local_11 = {};
    r_3 = v_6;
    switch(sign) {
        case 1u: {
            if (width > 1u) {
                float max_u = static_cast<float>((1u << width) - 1u);
                n = metal::rint(v_6 * max_u);
                float _e15 = n;
                if (_e15 >= static_cast<float>(1u << (width - 1u))) {
                    float _e22 = n;
                    n = _e22 - (max_u + 1.0);
                }
                float _e26 = n;
                r_3 = metal::max(_e26 / static_cast<float>((1u << (width - 1u)) - 1u), -1.0);
            }
            break;
        }
        case 2u: {
            if (width > 1u) {
                float half_range = static_cast<float>(1u << (width - 1u));
                r_3 = ((v_6 * ((2.0 * half_range) - 1.0)) - half_range) / (half_range - 1.0);
            } else {
                r_3 = (v_6 * 2.0) - 1.0;
            }
            float _e57 = r_3;
            r_3 = metal::max(_e57, -1.0);
            break;
        }
        case 3u: {
            float _e60 = xe_pwl_gamma_to_linear(v_6);
            r_3 = _e60;
            break;
        }
        default: {
            break;
        }
    }
    if (integer) {
        local_10 = width > 0u;
    } else {
        local_10 = false;
    }
    bool _e66 = local_10;
    if (_e66) {
        if (!((sign == 1u))) {
            local_11 = sign == 2u;
        } else {
            local_11 = true;
        }
        bool _e75 = local_11;
        if (_e75) {
            float _e76 = r_3;
            r_3 = _e76 * static_cast<float>((1u << (width - 1u)) - 1u);
        } else {
            float _e85 = r_3;
            r_3 = _e85 * static_cast<float>((1u << width) - 1u);
        }
    }
    float _e92 = r_3;
    return _e92;
}

metal::float4 xe_color_range(
    metal::float4 c_2,
    uint range
) {
    if (range == 0u) {
        return c_2;
    }
    metal::float4 finite = metal::select(metal::float4(0.0), c_2, c_2 == c_2);
    if (range == 1u) {
        return metal::clamp(finite, metal::float4(0.0), metal::float4(31.875, 31.875, 31.875, 1.0));
    }
    return metal::clamp(finite, metal::float4(-32.0), metal::float4(32.0));
}

float xe_tex_swizzle_component(
    metal::float4 v_7,
    uint s_1
) {
    if (s_1 < 4u) {
        return uint(s_1) < 4 ? v_7[s_1] : DefaultConstructible();
    }
    return (s_1 == 5u) ? 1.0 : 0.0;
}

metal::float4 xe_tex_post(
    metal::float4 sampled,
    uint fc_7,
    constant XeFetchConstants& xe_fetch
) {
    metal::float4 data = {};
    uint i_3 = 0u;
    uint _e4 = xe_fetch_word(fc_7 * 6u, xe_fetch);
    uint _e9 = xe_fetch_word((fc_7 * 6u) + 1u, xe_fetch);
    uint _e14 = xe_fetch_word((fc_7 * 6u) + 3u, xe_fetch);
    uint _e17 = _e9 & 63u;
    uint info = uint(_e17) < 64 ? xe_texture_formats.inner[_e17] : DefaultConstructible();
    uint count = metal::max(info & 7u, 1u);
    bool integer_1 = (_e14 & 1u) != 0u;
    uint2 loop_bound = uint2(4294967295u);
    bool loop_init = true;
    while(true) {
        if (metal::all(loop_bound == uint2(0u))) { break; }
        loop_bound -= uint2(loop_bound.y == 0u, 1u);
        if (!loop_init) {
            uint _e55 = i_3;
            i_3 = _e55 + 1u;
        }
        loop_init = false;
        uint _e30 = i_3;
        if (_e30 < 4u) {
        } else {
            break;
        }
        {
            uint _e33 = i_3;
            uint c_3 = metal::min(_e33, count - 1u);
            uint width_1 = (info >> (3u + (6u * c_3))) & 63u;
            uint sign_2 = (_e4 >> (2u + (2u * c_3))) & 3u;
            uint _e51 = i_3;
            float _e54 = xe_tex_component(uint(c_3) < 4 ? sampled[c_3] : DefaultConstructible(), sign_2, width_1, integer_1);
            if (uint(_e51) < 4) {
                data[_e51] = _e54;
            }
        }
    }
    uint swizzle = (_e14 >> 1u) & 4095u;
    metal::float4 _e62 = data;
    float _e65 = xe_tex_swizzle_component(_e62, swizzle & 7u);
    metal::float4 _e66 = data;
    float _e71 = xe_tex_swizzle_component(_e66, (swizzle >> 3u) & 7u);
    metal::float4 _e72 = data;
    float _e77 = xe_tex_swizzle_component(_e72, (swizzle >> 6u) & 7u);
    metal::float4 _e78 = data;
    float _e83 = xe_tex_swizzle_component(_e78, (swizzle >> 9u) & 7u);
    metal::float4 result = metal::float4(_e65, _e71, _e77, _e83);
    return result * metal::ldexp(1.0, metal::extract_bits(as_type<int>(_e14), metal::min(13u, 32u), metal::min(6u, 32u - metal::min(13u, 32u))));
}

bool xe_compare(
    uint function,
    float a_9,
    float b_10
) {
    switch(function) {
        case 0u: {
            return false;
        }
        case 1u: {
            return a_9 < b_10;
        }
        case 2u: {
            return a_9 == b_10;
        }
        case 3u: {
            return a_9 <= b_10;
        }
        case 4u: {
            return a_9 > b_10;
        }
        case 5u: {
            return a_9 != b_10;
        }
        case 6u: {
            return a_9 >= b_10;
        }
        default: {
            return true;
        }
    }
}

metal::uint2 naga_f2u32(metal::float2 value) {
    return static_cast<metal::uint2>(metal::clamp(value, 0.0, 4294967000.0));
}

uint xe_alpha_to_mask(
    float alpha,
    metal::float2 position,
    uint flags
) {
    uint mask = 0u;
    if ((flags & 262144u) == 0u) {
        return 4294967295u;
    }
    metal::uint2 pixel = naga_f2u32(position);
    uint index_1 = ((pixel.x & 1u) * 2u) + (pixel.y & 1u);
    float offset_1 = static_cast<float>((flags >> (21u + (2u * index_1))) & 3u);
    uint samples = (flags >> 19u) & 3u;
    if (samples == 0u) {
        if (alpha >= (1.0 - (offset_1 * 0.25))) {
            mask = 1u;
        }
    } else {
        if (samples == 1u) {
            if (alpha >= (0.5 - (offset_1 * 0.125))) {
                uint _e47 = mask;
                mask = _e47 | 1u;
            }
            if (alpha >= (1.0 - (offset_1 * 0.125))) {
                uint _e55 = mask;
                mask = _e55 | 8u;
            }
        } else {
            if (samples == 2u) {
                if (alpha >= (0.75 - (offset_1 * 0.0625))) {
                    uint _e65 = mask;
                    mask = _e65 | 1u;
                }
                if (alpha >= (0.25 - (offset_1 * 0.0625))) {
                    uint _e73 = mask;
                    mask = _e73 | 2u;
                }
                if (alpha >= (0.5 - (offset_1 * 0.0625))) {
                    uint _e81 = mask;
                    mask = _e81 | 4u;
                }
                if (alpha >= (1.0 - (offset_1 * 0.0625))) {
                    uint _e89 = mask;
                    mask = _e89 | 8u;
                }
            }
        }
    }
    uint _e92 = mask;
    return _e92;
}

void xe_ucode(
    constant XeFetchConstants& xe_fetch,
    metal::texture2d<float, metal::access::sample> xe_texture_0_,
    metal::sampler xe_sampler_1_,
    thread type_16& xe_r,
    thread type_20& xe_out_color
) {
    {
        float _e1 = xe_tex_lod_bias(1u, xe_fetch);
        metal::float2 _e3 = xe_tex_size_2d(1u, xe_fetch);
        float _e7 = xe_r.inner[0].x;
        float _e15 = xe_r.inner[0].y;
        metal::float2 xe_uv = metal::float2(_e7 + (0.0014648438 / _e3.x), _e15 + (0.0014648438 / _e3.y));
        metal::float4 xe_ts = xe_texture_0_.sample(xe_sampler_1_, xe_uv, metal::level(_e1));
        metal::float4 _e25 = xe_tex_post(xe_ts, 1u, xe_fetch);
        xe_r.inner[0].x = _e25.x;
        xe_r.inner[0].y = _e25.y;
        xe_r.inner[0].z = _e25.z;
        xe_r.inner[0].w = _e25.w;
    }
    {
        metal::float4 xe_v0_ = xe_r.inner[0];
        metal::float4 xe_v1_ = xe_r.inner[0];
        metal::float4 _e48 = xe_max(xe_v0_, xe_v1_);
        xe_out_color.inner[0].x = _e48.x;
        xe_out_color.inner[0].y = _e48.y;
        xe_out_color.inner[0].z = _e48.z;
        xe_out_color.inner[0].w = _e48.w;
    }
    return;
}

struct xe_ps_mainInput {
    metal::float4 i0_ [[user(loc0), center_perspective]];
};
struct xe_ps_mainOutput {
    uint coverage [[sample_mask]];
    metal::float4 color0_ [[color(0)]];
};
fragment xe_ps_mainOutput xe_ps_main(
  xe_ps_mainInput varyings [[stage_in]]
, metal::float4 frag_coord [[position]]
, bool front_facing [[front_facing]]
, constant XeFetchConstants& xe_fetch [[buffer(1)]]
, constant XeSystemConstants& xe_system [[buffer(2)]]
, metal::texture2d<float, metal::access::sample> xe_texture_0_ [[texture(0)]]
, metal::sampler xe_sampler_1_ [[sampler(0)]]
) {
    type_16 xe_r = {};
    type_20 xe_out_color = {};
    const XePixelInput input = { frag_coord, front_facing, {}, varyings.i0_ };
    float x = {};
    bool local = {};
    float y = {};
    bool local_1 = {};
    XePixelOutput out = {};
    xe_r.inner[0] = input.i0_;
    uint _e6 = xe_system.flags;
    if ((_e6 & 256u) != 0u) {
        uint flags_1 = xe_system.flags;
        float _e18 = xe_system.resolution_scale;
        metal::float2 _e25 = xe_system.ps_param_gen_offset;
        metal::float2 pixel_1 = metal::abs(metal::floor(input.frag_coord.xy / metal::float2(_e18))) + _e25;
        x = pixel_1.x;
        if ((flags_1 & 32u) != 0u) {
            local = !(input.front_facing);
        } else {
            local = false;
        }
        bool _e38 = local;
        if (_e38) {
            float _e39 = x;
            x = as_type<float>(as_type<uint>(_e39) ^ 2147483648u);
        }
        y = pixel_1.y;
        if ((flags_1 & 128u) != 0u) {
            float _e50 = y;
            y = as_type<float>(as_type<uint>(_e50) ^ 2147483648u);
        }
        uint line_bit = ((flags_1 & 64u) != 0u) ? 2147483648u : 0u;
        uint gen_register = xe_system.ps_param_gen_register;
        if (gen_register < 1u) {
            float _e69 = x;
            float _e70 = y;
            if (uint(gen_register) < 1) {
                xe_r.inner[gen_register] = metal::float4(_e69, _e70, as_type<float>(line_bit), 0.0);
            }
        }
    }
    xe_ucode(xe_fetch, xe_texture_0_, xe_sampler_1_, xe_r, xe_out_color);
    uint _e76 = xe_system.flags;
    if ((_e76 & 512u) != 0u) {
        uint _e85 = xe_system.alpha_test_function;
        float _e89 = xe_out_color.inner[0].w;
        float _e92 = xe_system.alpha_test_reference;
        bool _e93 = xe_compare(_e85, _e89, _e92);
        local_1 = !(_e93);
    } else {
        local_1 = false;
    }
    bool _e96 = local_1;
    if (_e96) {
        metal::discard_fragment();
    }
    float _e102 = xe_out_color.inner[0].w;
    uint _e107 = xe_system.flags;
    uint _e108 = xe_alpha_to_mask(_e102, input.frag_coord.xy, _e107);
    out.coverage = _e108;
    uint _e110 = out.coverage;
    if (_e110 == 0u) {
        metal::discard_fragment();
    }
    metal::float4 _e116 = xe_out_color.inner[0];
    float _e120 = xe_system.color_exp_bias.x;
    uint _e124 = xe_system.flags;
    metal::float4 _e129 = xe_color_range(_e116 * _e120, (_e124 >> 10u) & 3u);
    out.color0_ = _e129;
    XePixelOutput _e130 = out;
    const auto _tmp = _e130;
    return xe_ps_mainOutput { _tmp.coverage, _tmp.color0_ };
}
