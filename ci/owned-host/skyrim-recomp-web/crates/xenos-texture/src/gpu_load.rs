//! Shared, backend-neutral GPU texture conversion plan and canonical shader.
//! Both native HAL and the existing WebGPU executor consume these exact bytes.
use crate::host::{Conversion, HostMapping};
use crate::layout::packed_mip_offset;
use crate::{DataDimension, Endian, TextureFetchConstant, TextureGuestInfo};

/// Row alignment of `copy_buffer_to_texture`.
const ROW_ALIGNMENT: u32 = 256;

/// One level's dispatch.
pub struct LevelPlan {
    pub level: u32,
    pub params: [u32; 32],
    pub blocks: (u32, u32, u32),
    pub out_offset: u64,
    pub bytes_per_row: u32,
    pub rows_per_image: u32,
    pub copy: (u32, u32, u32),
    /// Physical address range of the level's guest data.
    pub source: (u32, u32),
}

pub struct Plan {
    pub levels: Vec<LevelPlan>,
    /// Size of the converted rows (padded for the copy).
    pub out_bytes: u64,
    /// Host bytes of all levels without padding.
    pub host_bytes: u64,
}

impl Plan {
    /// Whether the dispatches write every level below `count` (all of each level's texels).
    pub fn covers_levels(&self, count: u32) -> bool {
        (0..count).all(|m| self.levels.iter().any(|l| l.level == m))
    }

    /// Host bytes of all levels, for the cache's memory budget.
    pub fn host_bytes(&self) -> u64 {
        self.host_bytes
    }
}

fn conversion_code(mapping: &HostMapping) -> Option<(u32, [u32; 2], u32)> {
    Some(match mapping.conversion {
        Conversion::Copy => (0, [0, 0], 0),
        Conversion::PackedToRgba8 { fields, count, signed: false } => {
            let pack = |f: &[(u8, u8)]| {
                f.iter()
                    .enumerate()
                    .fold(0u32, |acc, (i, (s, b))| acc | (u32::from(*s) | u32::from(*b) << 8) << (16 * i))
            };
            (1, [pack(&fields[0..2]), pack(&fields[2..4])], u32::from(count))
        }
        Conversion::Norm16ToFloat16 { components, signed: false } => (2, [0, 0], u32::from(components)),
        Conversion::Edram16ToFloat16 { components } => (3, [0, 0], u32::from(components)),
        Conversion::Float7e3ToRgba16Float => (4, [0, 0], 4),
        Conversion::Depth24UnormToFloat32 => (5, [0, 0], 1),
        Conversion::Depth20e4ToFloat32 => (6, [0, 0], 1),
        _ => return None,
    })
}

/// The dispatches loading `fetch` (whose host mapping is `mapping`) for levels up to
/// `level_count`, or `None` if the GPU loader can't produce its host data.
pub fn plan(
    fetch: &TextureFetchConstant,
    info: &TextureGuestInfo,
    mapping: &HostMapping,
    level_count: u32,
) -> Option<Plan> {
    let (code, fields, count) = conversion_code(mapping)?;
    let format_info = info.format.info();
    let (bw, bh, bpb) = (format_info.block_width, format_info.block_height, format_info.bytes_per_block());
    let host_block = mapping.host_format.block_size();
    let host_unit = mapping.host_format.bytes_per_block();
    // Per-texel conversions of multi-texel blocks (BC decompression etc.) stay on the CPU.
    if !(host_block > 1 || (bw == 1 && bh == 1)) || bpb > 16 || host_unit > 16 {
        return None;
    }
    if code != 0 && bpb != 2 && bpb != 4 && bpb != 8 {
        return None;
    }
    let sub = &info.subresources;
    let is_3d = sub.dimension == DataDimension::D3;
    let tiled = fetch.tiled && sub.dimension != DataDimension::D1;
    let first_level = if sub.has_base() { 0 } else { 1 };
    let mut levels = Vec::new();
    let mut out_bytes = 0u64;
    let mut host_bytes = 0u64;
    for level in first_level..=info.layout.max_level.min(level_count.saturating_sub(1)) {
        let Some((address, layout, _)) = info.level_storage(level) else { continue };
        let width = (sub.width >> level).max(1);
        let height = (sub.height >> level).max(1);
        let depth = (sub.depth() >> level).max(1);
        let slices = if is_3d { depth } else { sub.array_size() };
        let (ox, oy, oz) = if info.packed_mips && level >= info.layout.packed_level {
            packed_mip_offset(sub.width, sub.height, sub.depth(), info.format, level).unwrap_or((0, 0, 0))
        } else {
            (0, 0, 0)
        };
        let (blocks_x, blocks_y) = (width.div_ceil(bw), height.div_ceil(bh));
        let bytes_per_row = (blocks_x * host_unit).next_multiple_of(ROW_ALIGNMENT);
        let image_bytes = bytes_per_row * blocks_y;
        let copy = if host_block > 1 { (blocks_x * bw, blocks_y * bh, slices) } else { (width, height, slices) };
        let mut params = [0u32; 32];
        params[..24].copy_from_slice(&[
            address,
            tiled as u32,
            is_3d as u32,
            layout.row_pitch_bytes / bpb,
            layout.z_slice_stride_block_rows,
            layout.row_pitch_bytes,
            bpb,
            bpb.ilog2(),
            blocks_x,
            blocks_y,
            slices,
            layout.array_slice_stride_bytes,
            ox,
            oy,
            oz,
            match fetch.endianness {
                Endian::None => 0,
                Endian::K8in16 => 1,
                Endian::K8in32 => 2,
                Endian::K16in32 => 3,
            },
            code,
            bytes_per_row,
            image_bytes,
            host_unit,
            layout.level_data_extent_bytes,
            0, // buffer base, filled in by `encode`
            fields[0],
            fields[1],
        ]);
        params[24] = count;
        levels.push(LevelPlan {
            level,
            params,
            blocks: (blocks_x, blocks_y, slices),
            out_offset: out_bytes,
            bytes_per_row,
            rows_per_image: blocks_y,
            copy,
            source: (address, layout.level_data_extent_bytes),
        });
        out_bytes += u64::from(image_bytes) * u64::from(slices);
        host_bytes += u64::from(blocks_x * host_unit) * u64::from(blocks_y) * u64::from(slices);
    }
    (!levels.is_empty()).then_some(Plan { levels, out_bytes, host_bytes })
}

/// Where each level's raw bytes go in the upload buffer (256-byte aligned offsets, with room for
/// the level rounded out to whole dwords at both ends), and the bytes needed in all.
pub fn upload_offsets(plan: &Plan) -> (Vec<u64>, u64) {
    let mut offsets = Vec::with_capacity(plan.levels.len());
    let mut total = 0u64;
    for level in &plan.levels {
        offsets.push(total);
        total = (total + u64::from(level.source.1).next_multiple_of(4).max(4) + 4).next_multiple_of(256);
    }
    (offsets, total)
}

/// Parameters (32 u32): 0 level address, 1 tiled, 2 is_3d, 3 pitch in blocks, 4 z stride in block
/// rows, 5 linear row pitch bytes, 6 bytes per block, 7 log2, 8/9 blocks x/y, 10 slices, 11 array
/// slice stride, 12-14 packed mip offset, 15 endian, 16 conversion, 17 output bytes per row,
/// 18 output bytes per slice, 19 host bytes per block, 20 level data extent, 21 guest address of
/// `src[0]`, 22/23 packed fields (shift | bits << 8, two per word), 24 component count,
/// 25 output offset in words.
pub const SHADER: &str = r#"
struct Params { v: array<vec4<u32>, 8> }
@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> src: array<u32>;
@group(0) @binding(2) var<storage, read_write> dst: DST_TYPE;

fn store_word(i: u32, v: u32) {
    STORE_WORD
}

fn prm(i: u32) -> u32 { return params.v[i >> 2u][i & 3u]; }

fn tiled_combine(oib: i32, bank: u32, pipe: u32, y_lsb: u32) -> i32 {
    return i32((y_lsb << 4u) | (pipe << 6u) | (bank << 11u)) | (oib & 15) | (((oib >> 4u) & 1) << 5u)
        | (((oib >> 5u) & 7) << 8u) | ((oib >> 8u) << 12u);
}

fn tiled_2d(x: i32, y: i32, pitch: u32, bpb_log2: u32) -> i32 {
    let outer = ((y >> 5u) * i32(pitch >> 5u) + (x >> 5u)) << 6u;
    let inner = (((y >> 1u) & 7) << 3u) | (x & 7);
    let oib = (outer | inner) << bpb_log2;
    let bank = u32((y >> 4u) & 1);
    let pipe = u32(((x >> 3u) & 3) ^ (((y >> 3u) & 1) << 1u));
    return tiled_combine(oib, bank, pipe, u32(y & 1));
}

fn tiled_3d(x: i32, y: i32, z: i32, pitch: u32, height: u32, bpb_log2: u32) -> i32 {
    let outer = ((((z >> 2u) * i32(height >> 4u) + (y >> 4u)) * i32(pitch >> 5u)) + (x >> 5u)) << 7u;
    let inner = ((z & 3) << 5u) | (((y >> 1u) & 3) << 3u) | (x & 7);
    let oib = (outer | inner) << bpb_log2;
    let bank = u32(((y >> 3u) ^ (z >> 2u)) & 1);
    let pipe = u32((x >> 3u) & 3) ^ (bank << 1u);
    return tiled_combine(oib, bank, pipe, u32(y & 1));
}

fn swap32(w: u32, endian: u32) -> u32 {
    switch endian {
        case 1u: { return ((w & 0x00FF00FFu) << 8u) | ((w >> 8u) & 0x00FF00FFu); }
        case 2u: { return (w << 24u) | ((w & 0xFF00u) << 8u) | ((w >> 8u) & 0xFF00u) | (w >> 24u); }
        case 3u: { return (w << 16u) | (w >> 16u); }
        default: { return w; }
    }
}

// Exact port of xenos_texture::convert::f32_to_f16 (round to nearest even).
fn f16(v: f32) -> u32 {
    let x = bitcast<u32>(v);
    let sign = (x >> 16u) & 0x8000u;
    let exp = i32((x >> 23u) & 0xFFu);
    let man = x & 0x7FFFFFu;
    if (exp == 0xFF) {
        return sign | 0x7C00u | select(0u, 0x200u, man != 0u);
    }
    let e = exp - 127 + 15;
    if (e >= 31) { return sign | 0x7C00u; }
    if (e <= 0) {
        if (e < -10) { return sign; }
        let m = man | 0x800000u;
        let shift = u32(14 - e);
        var h = m >> shift;
        let rem = m & ((1u << shift) - 1u);
        let halfway = 1u << (shift - 1u);
        if (rem > halfway || (rem == halfway && (h & 1u) == 1u)) { h += 1u; }
        return sign | h;
    }
    var h = (u32(e) << 10u) | (man >> 13u);
    let rem = man & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u) == 1u)) { h += 1u; }
    return sign | h;
}

fn f7e3(bits: u32) -> f32 {
    let v = bits & 0x3FFu;
    if (v == 0u) { return 0.0; }
    var mantissa = v & 0x7Fu;
    var exponent = v >> 7u;
    if (exponent == 0u) {
        let lz = 7u - (31u - countLeadingZeros(mantissa));
        exponent = 1u - lz;
        mantissa = (mantissa << lz) & 0x7Fu;
    }
    return bitcast<f32>(((exponent + 124u) << 23u) | (mantissa << 16u));
}

// Xenos 20e4 float depth (24 bits) to f32.
fn f20e4(bits: u32) -> f32 {
    let v = bits & 0xFFFFFFu;
    if (v == 0u) { return 0.0; }
    var mantissa = v & 0xFFFFFu;
    var exponent = v >> 20u;
    if (exponent == 0u) {
        let lz = 20u - (31u - countLeadingZeros(mantissa));
        exponent = 1u - lz;
        mantissa = (mantissa << lz) & 0xFFFFFu;
    }
    return bitcast<f32>(((exponent + 112u) << 23u) | (mantissa << 3u));
}

fn put_bits(byte_addr: u32, value: u32, mask: u32) {
    let i = byte_addr >> 2u;
    let shift = (byte_addr & 3u) * 8u;
    PUT_BITS
}

fn field(i: u32) -> vec2<u32> {
    let packed = prm(22u + (i >> 1u)) >> (16u * (i & 1u));
    return vec2<u32>(packed & 0xFFu, (packed >> 8u) & 0xFFu);
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
    let bx = gid.x;
    let by = gid.y;
    let slice = gid.z;
    if (bx >= prm(8u) || by >= prm(9u) || slice >= prm(10u)) { return; }
    let bpb = prm(6u);
    let is_3d = prm(2u) != 0u;
    let layer = select(slice, 0u, is_3d);
    let z = select(0u, slice, is_3d);
    let x = bx + prm(12u);
    let y = by + prm(13u);
    let zz = z + prm(14u);
    var offset: i32;
    if (prm(1u) != 0u) {
        if (is_3d) {
            offset = tiled_3d(i32(x), i32(y), i32(zz), prm(3u), prm(4u), prm(7u));
        } else {
            offset = tiled_2d(i32(x), i32(y), prm(3u), prm(7u));
        }
    } else {
        offset = i32((zz * prm(4u) + y) * prm(5u) + x * bpb);
    }
    offset += i32(layer * prm(11u));
    let endian = prm(15u);
    var w = array<u32, 4>(0u, 0u, 0u, 0u);
    if (offset >= 0 && u32(offset) + bpb <= prm(20u)) {
        let a = prm(0u) + u32(offset) - prm(21u);
        if (bpb >= 4u) {
            for (var k = 0u; k < bpb / 4u; k++) {
                w[k] = swap32(src[(a >> 2u) + k], endian);
            }
        } else {
            let word = swap32(src[a >> 2u], endian);
            w[0] = (word >> ((a & 3u) * 8u)) & select(0xFFu, 0xFFFFu, bpb == 2u);
        }
    }
    let unit = prm(19u);
    let out = prm(25u) * 4u + slice * prm(18u) + by * prm(17u) + bx * unit;
    let conversion = prm(16u);
    var o = array<u32, 4>(w[0], w[1], w[2], w[3]);
    switch conversion {
        case 1u: {
            // Packed fixed point to RGBA8 (missing components 255).
            var rgba = 0u;
            for (var c = 0u; c < 4u; c++) {
                var byte = 255u;
                if (c < prm(24u)) {
                    let f = field(c);
                    let max = (1u << f.y) - 1u;
                    let raw = (w[0] >> f.x) & max;
                    byte = (raw * 255u + max / 2u) / max;
                }
                rgba |= byte << (8u * c);
            }
            o[0] = rgba;
        }
        case 2u, 3u: {
            // 16-bit unorm (2) or EDRAM -32..32 fixed point (3) components to halves.
            var halves = array<u32, 4>(0u, 0u, 0u, 0u);
            for (var c = 0u; c < prm(24u); c++) {
                let raw = (w[c >> 1u] >> (16u * (c & 1u))) & 0xFFFFu;
                var f: f32;
                if (conversion == 2u) {
                    f = f32(raw) / 65535.0;
                } else {
                    let s = i32(raw << 16u) >> 16u;
                    f = max(f32(s) * (32.0 / 32767.0), -32.0);
                }
                halves[c] = f16(f);
            }
            o[0] = halves[0] | (halves[1] << 16u);
            o[1] = halves[2] | (halves[3] << 16u);
        }
        case 4u: {
            let v = w[0];
            o[0] = f16(f7e3(v)) | (f16(f7e3(v >> 10u)) << 16u);
            o[1] = f16(f7e3(v >> 20u)) | (f16(f32(v >> 30u) / 3.0) << 16u);
        }
        case 5u: {
            // 24-bit unorm depth in the high bits.
            let n = w[0] >> 8u;
            o[0] = bitcast<u32>(f32(n + (n >> 23u)) * (1.0 / 16777216.0));
        }
        case 6u: {
            o[0] = bitcast<u32>(f20e4(w[0] >> 8u));
        }
        default: {}
    }
    if (unit >= 4u) {
        for (var k = 0u; k < unit / 4u; k++) {
            store_word((out >> 2u) + k, o[k]);
        }
    } else {
        put_bits(out, o[0], select(0xFFu, 0xFFFFu, unit == 2u));
    }
}
"#;

/// The shader with plain stores (`atomic == false`: every write is a whole aligned dword) or
/// with atomics (8/16-bit elements share dwords with neighbours written by other invocations).
pub fn shader_variant(source: &str, atomic: bool) -> String {
    let (ty, store, put) = if atomic {
        (
            "array<atomic<u32>>",
            "atomicStore(&dst[i], v);",
            "atomicAnd(&dst[i], ~(mask << shift));\n    atomicOr(&dst[i], (value & mask) << shift);",
        )
    } else {
        ("array<u32>", "dst[i] = v;", "dst[i] = (dst[i] & ~(mask << shift)) | ((value & mask) << shift);")
    };
    source.replace("DST_TYPE", ty).replace("STORE_WORD", store).replace("PUT_BITS", put)
}
