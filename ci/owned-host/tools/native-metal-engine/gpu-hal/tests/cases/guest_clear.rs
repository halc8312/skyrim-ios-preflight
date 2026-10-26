//! Exact guest clear values, copy-before-clear, ownership and abandon behavior.
//! All inputs are authored; literal payloads are independent of GPU codecs.
use super::resolve_cache_cases::info;
use super::{DEADLINE, finish, open_config, setup};
use skyrim_native_gpu::{Config, edram::targets::TargetOptions, types};
use xenos_state::{
    edram::RenderTargetKey as Key,
    resolve::ResolveInfo,
    xenos::{MsaaSamples as M, TextureFormat as F},
};

fn request(k: Key) -> ResolveInfo {
    let mut r = info(k);
    r.rect.height = 32;
    if k.is_depth {
        let mut d = r.color_edram.take().unwrap();
        d.is_depth = true;
        d.format_is_64bpp = false;
        r.depth_edram = Some(d);
        r.depth_original_base = k.base_tiles;
        r.copy_control.copy_src_select = 4;
        r.dest_format = if k.format == 0 { F::K248 } else { F::K248Float };
        r.copy_dest_info.copy_dest_format = r.dest_format;
    }
    r
}
fn enable(r: &mut ResolveInfo, words: [u32; 2]) {
    if r.is_copying_depth() {
        r.copy_control.depth_clear_enable = true;
        r.depth_clear = words[0];
    } else {
        r.copy_control.color_clear_enable = true;
        r.color_clear = if r.color_edram.unwrap().format_is_64bpp {
            words[1]
        } else {
            words[0]
        };
        r.color_clear_lo = words[0];
    }
}

#[test]
fn guest_clear_preserves_literal_color_depth_stencil_at_all_samples_and_scales() {
    let (_lock, before) = setup();
    let mut cfg = Config::default();
    cfg.optional_texture_features = types::Features::DEPTH32FLOAT_STENCIL8;
    let mut gpu = open_config(cfg);
    for scale in [1, 2, 3] {
        for msaa in [M::X1, M::X2, M::X4] {
            for variant in 0..4 {
                let k = Key {
                    base_tiles: 2047,
                    pitch_tiles_at_32bpp: 2,
                    msaa_samples: msaa,
                    is_depth: variant >= 2,
                    format: match variant {
                        0 => 0,
                        1 => 7,
                        2 => 0,
                        _ => 1,
                    },
                };
                let a = match variant {
                    0 => [0xe7995317, 0],
                    1 => [0x38003400, 0x40003c00],
                    2 => [0x7fffffa5, 0],
                    _ => [0x6000017b, 0],
                };
                let b = match variant {
                    0 => [0x82734421, 0],
                    1 => [0x3c003800, 0x34004000],
                    2 => [0x8000013e, 0],
                    _ => [0x700001df, 0],
                };
                let mut cache = gpu
                    .edram_target_cache(TargetOptions {
                        scale,
                        ..Default::default()
                    })
                    .unwrap();
                let full = request(k);
                let mut initial = full.clone();
                initial.copy_dest_extent_length = 0;
                enable(&mut initial, a);
                let (copied, after) = {
                    let mut frame = cache.begin(&mut gpu).unwrap();
                    assert!(frame.resolve_pixels(&initial).unwrap().is_none());
                    let mut part = full.clone();
                    part.rect.width = 16;
                    part.rect.height = 8;
                    part.edram_offset_x_div_8 = 1;
                    part.edram_offset_y_div_8 = 1;
                    enable(&mut part, b);
                    let copied = frame.resolve_pixels(&part).unwrap().unwrap();
                    let after = frame.resolve_pixels(&full).unwrap().unwrap();
                    let (_, stats) = frame.submit().unwrap();
                    assert!(stats.clear_draws >= 2);
                    (copied, after)
                };
                let words = if variant == 1 { 2 } else { 1 };
                for p in copied.read(&mut gpu, DEADLINE).unwrap().chunks_exact(words) {
                    assert_eq!(
                        p,
                        &a[..words],
                        "copy before clear v{variant} {msaa:?} scale{scale}"
                    );
                }
                let output = after.read(&mut gpu, DEADLINE).unwrap();
                for y in 0..full.rect.height {
                    for x in 0..full.rect.width {
                        let expected = if (8..24).contains(&x) && (8..16).contains(&y) {
                            b
                        } else {
                            a
                        };
                        let n = (y * full.rect.width + x) as usize * words;
                        assert_eq!(
                            &output[n..n + words],
                            &expected[..words],
                            "partial v{variant} {msaa:?} s{scale} x{x} y{y}"
                        );
                    }
                }
                // A new clear and target growth in a dropped frame must publish neither.
                let old_owners = cache.ownership().to_vec();
                let old_count = cache.target_count();
                {
                    let mut f = cache.begin(&mut gpu).unwrap();
                    let mut dropped = initial.clone();
                    dropped.rect.height = 256;
                    enable(&mut dropped, b);
                    f.resolve_pixels(&dropped).unwrap();
                }
                assert_eq!(cache.ownership(), old_owners);
                assert_eq!(cache.target_count(), old_count);
                let unchanged = {
                    let mut f = cache.begin(&mut gpu).unwrap();
                    let p = f.resolve_pixels(&full).unwrap().unwrap();
                    f.submit().unwrap();
                    p
                };
                assert_eq!(unchanged.read(&mut gpu, DEADLINE).unwrap(), output);
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_clear_transfers_aliases_and_preserves_tiles_outside_simultaneous_depth_color_claim() {
    let (_lock, before) = setup();
    let mut cfg = Config::default();
    cfg.optional_texture_features = types::Features::DEPTH32FLOAT_STENCIL8;
    let mut gpu = open_config(cfg);
    let color = Key {
        base_tiles: 2047,
        pitch_tiles_at_32bpp: 2,
        msaa_samples: M::X1,
        is_depth: false,
        format: 0,
    };
    let depth = Key {
        base_tiles: 0,
        is_depth: true,
        format: 0,
        ..color
    };
    let mut cache = gpu.edram_target_cache(Default::default()).unwrap();
    let base = request(color);
    let mut initial = base.clone();
    initial.copy_dest_extent_length = 0;
    enable(&mut initial, [0xff332211, 0]);
    let mut both = initial.clone();
    both.depth_edram = request(depth).depth_edram;
    both.depth_original_base = 0;
    both.copy_control.depth_clear_enable = true;
    both.depth_clear = 0x800001c7;
    enable(&mut both, [0xff776655, 0]);
    let (color_copy, depth_copy) = {
        let mut f = cache.begin(&mut gpu).unwrap();
        f.resolve_pixels(&initial).unwrap();
        f.resolve_pixels(&both).unwrap();
        let c = f.resolve_pixels(&base).unwrap().unwrap();
        let d = f.resolve_pixels(&request(depth)).unwrap().unwrap();
        f.submit().unwrap();
        (c, d)
    };
    let c = color_copy.read(&mut gpu, DEADLINE).unwrap();
    let d = depth_copy.read(&mut gpu, DEADLINE).unwrap();
    // Color owns only tile2047; depth claims0..3. Both rectangular views see
    // the current owner of each physical tile, not stale private target bytes.
    for y in 0..32 {
        for x in 0..160 {
            let n = y * 160 + x;
            assert_eq!(
                c[n],
                if y < 16 && x < 80 {
                    0xff776655
                } else {
                    0x800001c7
                }
            );
            assert_eq!(d[n], 0x800001c7);
        }
    }
    drop(gpu);
    finish(before);
}
