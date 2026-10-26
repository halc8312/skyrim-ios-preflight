//! Real mirror/resolve -> canonical conversion -> native sampled image. Data is
//! authored locally; the independent CPU decoder is the per-texel oracle.
use super::{DEADLINE, finish, open, open_config, setup};
use super::{guest_store_cases as store, resolve_cache_cases};
use skyrim_native_gpu::{
    Config, Error,
    edram::targets::{GuestPage, TextureAdmission},
};
use std::sync::Arc;
use xenos_texture::{
    DataDimension as D, Endian, TextureFetchConstant as Fetch, TextureFormat as F,
};
#[path = "../../../../../skyrim-recomp-web/crates/xenos-shader/tests/common/mod.rs"]
mod assembler;

#[test]
fn guest_texture_translated_pixel_shader_samples_owned_generation() {
    use assembler::{Alu, TFetch, assemble, exece, r};
    use skyrim_native_gpu::{
        edram::targets::{ConstantDraw, DrawAdmission, GuestTextureBinding, TargetOptions},
        pipeline::PipelineCache,
        types as t,
    };
    use xenos_shader::{ShaderStage, TranslationOptions, bindings};
    let (_lock, before) = setup();
    let mut gpu = open();
    let options = TranslationOptions {
        vs_output_interpolators: 1,
        ps_interpolator_count: Some(1),
        ..Default::default()
    };
    let vertex = gpu
        .guest_shader(
            ShaderStage::Vertex,
            &[
                0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100,
                0xC80F8000, 0, 0x22000000,
            ],
            &options,
        )
        .unwrap();
    let code = assemble(&[exece(vec![
        TFetch {
            fetch_constant: 1,
            dest: 1,
            src: 0,
            ..Default::default()
        }
        .into(),
        Alu::vector(2, 0, 15, &[r(1), r(1)]).export().into(),
    ])]);
    let pixel = gpu
        .guest_shader(
            ShaderStage::Pixel,
            &code,
            &TranslationOptions {
                texture_binding_base: 96,
                ..options
            },
        )
        .unwrap();
    let mut arena = gpu.xenos_draw_arena(16 << 10).unwrap();
    let mut memory = gpu.guest_memory(1).unwrap();
    let mut entries = Vec::new();
    for binding in &pixel.metadata().texture_bindings {
        entries.push(t::BindGroupLayoutEntry {
            binding: binding.texture_binding,
            visibility: t::ShaderStages::FRAGMENT,
            ty: t::BindingType::Texture {
                sample_type: t::TextureSampleType::Float { filterable: true },
                view_dimension: t::TextureViewDimension::D2,
                multisampled: false,
            },
            count: None,
        });
        entries.push(t::BindGroupLayoutEntry {
            binding: binding.sampler_binding,
            visibility: t::ShaderStages::FRAGMENT,
            ty: t::BindingType::Sampler(t::SamplerBindingType::Filtering),
            count: None,
        });
    }
    let textures = gpu.group_layout(&entries).unwrap();
    let layout = gpu
        .pipeline_layout(&[arena.layout().clone(), memory.layout().clone(), textures])
        .unwrap();
    let mut pipelines = PipelineCache::new(8).unwrap();
    let program = arena
        .guest_pipeline(
            &gpu,
            &mut pipelines,
            &super::pipeline_cases::normalized_key(),
            &layout,
            &vertex,
            Some(&pixel),
        )
        .unwrap();
    let mut cache = gpu.edram_target_cache(TargetOptions::default()).unwrap();
    let mut vertex_bytes = [0u8; 4096];
    let data: Vec<u8> = [[-1.0f32, -1., 0.5], [3., -1., 0.5], [-1., 3., 0.5]]
        .into_iter()
        .flatten()
        .flat_map(f32::to_be_bytes)
        .collect();
    vertex_bytes[..data.len()].copy_from_slice(&data);
    let mut tex_bytes = [0u8; 4096];
    for pixel in tex_bytes.chunks_exact_mut(4) {
        pixel.copy_from_slice(&[17, 53, 101, 255]);
    }
    let pages = vec![
        GuestPage {
            address: 4096,
            bytes: Arc::new(tex_bytes),
            generation: Some(1),
        },
        GuestPage {
            address: 65536,
            bytes: Arc::new(vertex_bytes),
            generation: None,
        },
    ];
    let mut f = fetch(F::k_8_8_8_8, false);
    f.width = 4;
    f.height = 4;
    f.pitch_div_32 = 1;
    f.swizzle = [
        xenos_texture::SwizzleSource::X,
        xenos_texture::SwizzleSource::Y,
        xenos_texture::SwizzleSource::Z,
        xenos_texture::SwizzleSource::W,
    ];
    let constants: Vec<u8> = [0.5f32, 0.5, 0., 1.]
        .into_iter()
        .flat_map(f32::to_le_bytes)
        .collect();
    let bools = vec![0u8; 160];
    let mut fetch_bytes = vec![0u8; 768];
    for (i, v) in [0x10003u32, 2 | 9 * 4 << 2].into_iter().enumerate() {
        fetch_bytes[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
    }
    for (i, v) in f.encode().iter().enumerate() {
        fetch_bytes[24 + i * 4..28 + i * 4].copy_from_slice(&v.to_le_bytes());
    }
    let system = bindings::SystemConstants::default();
    for negative in [1, 2, 3, 4, 0] {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        frame
            .targets(&resolve_cache_cases::selection(store::key()), 16)
            .unwrap();
        frame.sync_guest_pages(&pages).unwrap();
        let texture = loaded(frame.load_guest_texture(&f, false).unwrap());
        let mut actual_fetch = fetch_bytes.clone();
        if negative == 2 {
            actual_fetch[28..32].copy_from_slice(&(f.encode()[1] + 4096).to_le_bytes());
        }
        if negative == 3 {
            frame.sync_guest_pages(&pages[1..]).unwrap();
        }
        if negative == 4 {
            let mut changed = pages.clone();
            changed[0].generation = Some(2);
            changed[0].bytes = Arc::new([99; 4096]);
            frame.sync_guest_pages(&changed).unwrap();
        }
        let draw = ConstantDraw {
            pipeline: program.pipeline(),
            groups: &[],
            constants: [&constants, &[], &bools, &actual_fetch, system.as_bytes()],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: 0,
        };
        let result = if negative == 1 {
            frame.draw_guest(&program, &draw)
        } else {
            frame.draw_guest_textured(
                &program,
                &draw,
                &[GuestTextureBinding {
                    fetch_constant: 1,
                    texture: &texture,
                }],
            )
        };
        if negative != 0 {
            assert!(result.is_err(), "negative{negative}");
            assert!(frame.submit().is_err());
            continue;
        }
        assert_eq!(result.unwrap(), DrawAdmission::Accepted);
        let pixels = frame
            .resolve_pixels(&resolve_cache_cases::info(store::key()))
            .unwrap()
            .unwrap();
        frame.submit().unwrap();
        assert!(
            pixels
                .read(&mut gpu, DEADLINE)
                .unwrap()
                .iter()
                .all(|&p| p == u32::from_le_bytes([17, 53, 101, 255]))
        );
    }
    drop(gpu);
    finish(before);
}

fn fetch(format: F, tiled: bool) -> Fetch {
    let mut f = Fetch::decode([2, 0, 0, 0, 0, 0]);
    f.format = format;
    f.tiled = tiled;
    f.dimension = D::D2OrStacked;
    f.base_page = 1;
    f.width = 33;
    f.height = 17;
    f.pitch_div_32 = 2;
    f
}
fn cpu_pages() -> (Vec<u8>, Vec<GuestPage>) {
    let bytes: Vec<u8> = (0..(1 << 20))
        .map(|i| ((i * 13 + i / 31) % 251) as u8)
        .collect();
    let pages = bytes
        .chunks_exact(4096)
        .enumerate()
        .map(|(i, b)| GuestPage {
            address: i as u32 * 4096,
            bytes: Arc::new(b.try_into().unwrap()),
            generation: Some(1),
        })
        .collect();
    (bytes, pages)
}
fn loaded(a: TextureAdmission) -> skyrim_native_gpu::edram::targets::GuestTexture {
    match a {
        TextureAdmission::Loaded(t) => t,
        TextureAdmission::NeedsSubmit => panic!("unexpected pressure"),
    }
}

#[test]
fn guest_texture_cpu_bc_and_gpu_mips_arrays_volume_match_decoder() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, _) = store::environment(&mut gpu);
    let (cpu, pages) = cpu_pages();
    for kind in 0..10 {
        let mut f = fetch(
            match kind {
                0 => F::k_DXT1,
                1 => F::k_DXT2_3,
                2 => F::k_DXT4_5,
                3 => F::k_DXN,
                _ => F::k_8_8_8_8,
            },
            true,
        );
        f.width = 32;
        f.height = 32;
        f.pitch_div_32 = 1;
        match kind {
            4 => {
                f.dimension = D::D3;
                f.depth = 4;
            }
            5 => {
                f.stacked = true;
                f.depth = 3;
            }
            6 => {
                f.dimension = D::Cube;
                f.depth = 6;
            }
            7 | 8 | 9 => {
                f.width = 64;
                f.height = 64;
                f.pitch_div_32 = 2;
                f.mip_page = 64;
                f.mip_max_level = 6;
                f.packed_mips = kind != 7;
                if kind == 9 {
                    f.base_page = 0;
                }
            }
            _ => {}
        }
        let expected = xenos_texture::load_texture(
            &f,
            &cpu,
            &xenos_texture::LoadOptions {
                row_alignment: 256,
                ..Default::default()
            },
        )
        .unwrap();
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        frame.sync_guest_pages(&pages).unwrap();
        let texture = loaded(frame.load_guest_texture(&f, false).unwrap());
        frame.submit().unwrap();
        for l in &expected.levels {
            assert_eq!(
                texture.test_read(&mut gpu, l.level, DEADLINE).unwrap(),
                l.data,
                "kind{kind} mip{}",
                l.level
            );
        }
        if kind == 9 {
            assert!(
                texture
                    .test_read(&mut gpu, 0, DEADLINE)
                    .unwrap()
                    .iter()
                    .all(|&b| b == 0),
                "absent mip0 must be initialized"
            );
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_texture_cpu_fallback_refuses_gpu_newer_memory() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = store::environment(&mut gpu);
    let pages = store::pages();
    store::encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &pages,
        &resolve_cache_cases::info(store::key()),
        true,
    );
    let mut f = fetch(F::k_DXT1, true);
    f.width = 32;
    f.height = 32;
    f.pitch_div_32 = 1;
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&pages).unwrap();
    assert!(matches!(
        frame.load_guest_texture(&f, false),
        Err(Error::Rejected(
            "guest CPU texture fallback would read stale bytes"
        ))
    ));
    assert!(frame.submit().is_err());
    drop(gpu);
    finish(before);
}

#[test]
fn guest_texture_gpu_conversions_match_independent_cpu_decoder() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, _) = store::environment(&mut gpu);
    let (cpu, pages) = cpu_pages();
    let mut count = 0;
    for format in [
        F::k_8,
        F::k_8_8,
        F::k_8_8_8_8,
        F::k_4_4_4_4,
        F::k_16,
        F::k_16_16,
        F::k_16_16_16_16_FLOAT,
        F::k_32_FLOAT,
    ] {
        for tiled in [false, true] {
            for endian in [
                Endian::None,
                Endian::K8in16,
                Endian::K8in32,
                Endian::K16in32,
            ] {
                let mut f = fetch(format, tiled);
                f.endianness = endian;
                let expected = xenos_texture::load_texture(
                    &f,
                    &cpu,
                    &xenos_texture::LoadOptions {
                        row_alignment: 256,
                        ..Default::default()
                    },
                )
                .unwrap();
                let mut frame = cache
                    .begin(&mut gpu)
                    .unwrap()
                    .with_constants(&mut arena)
                    .unwrap()
                    .with_guest_memory(&mut memory)
                    .unwrap();
                frame.sync_guest_pages(&pages).unwrap();
                let texture = loaded(frame.load_guest_texture(&f, false).unwrap());
                frame.submit().unwrap();
                for level in &expected.levels {
                    let actual = texture.test_read(&mut gpu, level.level, DEADLINE).unwrap();
                    assert_eq!(
                        actual, level.data,
                        "{format:?} tiled={tiled} endian={endian:?}"
                    );
                }
                count += 1;
            }
        }
    }
    assert_eq!(count, 64);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_texture_cache_uses_gpu_content_not_stale_cpu_baseline() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = store::environment(&mut gpu);
    let pages = store::pages();
    let info = resolve_cache_cases::info(store::key());
    let mut f = fetch(F::k_8_8_8_8, true);
    f.width = 80;
    f.height = 16;
    f.pitch_div_32 = 5;
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&pages).unwrap();
    let old = loaded(frame.load_guest_texture(&f, false).unwrap());
    let hit = loaded(frame.load_guest_texture(&f, false).unwrap());
    assert_eq!(old.id(), hit.id());
    frame.submit().unwrap();
    store::encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &pages,
        &info,
        true,
    );
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&pages).unwrap();
    let new = loaded(frame.load_guest_texture(&f, false).unwrap());
    assert_ne!(new.id(), old.id());
    frame.submit().unwrap();
    let bytes = new.test_read(&mut gpu, 0, DEADLINE).unwrap();
    for row in bytes.chunks_exact(512) {
        for pixel in row[..320].chunks_exact(4) {
            assert_eq!(pixel, [17, 53, 101, 255]);
        }
    }
    let bytes = old.test_read(&mut gpu, 0, DEADLINE).unwrap();
    for row in bytes.chunks_exact(512) {
        assert!(row[..320].iter().all(|&b| b == 0xcd));
    }
    // GPU-only ownership was not consumed by a texture conversion.
    assert_eq!(memory.gpu_newer_pages().len(), 32);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_texture_abandoned_generation_is_never_published() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, _) = store::environment(&mut gpu);
    let (_, pages) = cpu_pages();
    let f = fetch(F::k_8_8_8_8, true);
    let baseline = gpu.test_texture_allocations();
    for _ in 0..6 {
        {
            let mut frame = cache
                .begin(&mut gpu)
                .unwrap()
                .with_constants(&mut arena)
                .unwrap()
                .with_guest_memory(&mut memory)
                .unwrap();
            frame.sync_guest_pages(&pages).unwrap();
            let texture = loaded(frame.load_guest_texture(&f, false).unwrap());
            drop(texture);
        }
        assert_eq!(
            gpu.test_texture_allocations(),
            baseline,
            "abandoned texture retained by queue-prefix initialization"
        );
    }
    let abandoned = {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        frame.sync_guest_pages(&pages).unwrap();
        loaded(frame.load_guest_texture(&f, false).unwrap())
    };
    assert!(matches!(
        abandoned.test_read(&mut gpu, 0, DEADLINE),
        Err(Error::Rejected("guest texture transaction not accepted"))
    ));
    assert_eq!(memory.mapped_blocks(), 0);
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&pages).unwrap();
    let current = loaded(frame.load_guest_texture(&f, false).unwrap());
    assert_ne!(abandoned.id(), current.id());
    frame.submit().unwrap();
    current.test_read(&mut gpu, 0, DEADLINE).unwrap();
    drop(gpu);
    finish(before);
}

#[test]
fn guest_texture_rejects_uncaptured_and_aliased_physical_reads() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, _) = store::environment(&mut gpu);
    let (_, pages) = cpu_pages();
    {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        frame.sync_guest_pages(&pages).unwrap();
        loaded(
            frame
                .load_guest_texture(&fetch(F::k_8_8_8_8, true), false)
                .unwrap(),
        );
        frame.submit().unwrap();
    }
    for kind in 0..4 {
        let mut frame = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        let mut f = fetch(F::k_8_8_8_8, true);
        frame
            .sync_guest_pages(if kind == 0 { &pages[0..1] } else { &pages })
            .unwrap();
        if kind == 1 {
            f.base_page = 0x80001;
        } // would wrap to physical page1 in old code
        if kind == 2 {
            f.width = 0;
        }
        if kind == 3 {
            f.width += 8192;
            assert_eq!(f.encode(), fetch(F::k_8_8_8_8, true).encode());
        } // cache key aliases after truncation
        assert!(frame.load_guest_texture(&f, false).is_err());
        assert!(frame.submit().is_err());
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_texture_staging_pressure_retries_same_owned_pages() {
    let (_lock, before) = setup();
    let mut gpu = open_config(Config {
        max_pending_upload_bytes: 4100,
        ..Default::default()
    });
    let (mut cache, mut arena, mut memory, _) = store::environment(&mut gpu);
    let f = Fetch {
        width: 1,
        height: 1,
        pitch_div_32: 1,
        ..fetch(F::k_8_8_8_8, false)
    };
    let pages = vec![GuestPage {
        address: 4096,
        bytes: Arc::new([73; 4096]),
        generation: Some(1),
    }];
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&pages).unwrap();
    assert!(matches!(
        frame.load_guest_texture(&f, false).unwrap(),
        TextureAdmission::NeedsSubmit
    ));
    frame.submit().unwrap();
    let mut frame = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    frame.sync_guest_pages(&pages).unwrap();
    let texture = loaded(frame.load_guest_texture(&f, false).unwrap());
    frame.submit().unwrap();
    assert_eq!(
        &texture.test_read(&mut gpu, 0, DEADLINE).unwrap()[..4],
        &[73; 4]
    );
    drop(gpu);
    finish(before);
}
