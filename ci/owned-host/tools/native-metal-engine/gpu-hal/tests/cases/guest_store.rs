//! Authored native draw -> actual resolve/store kernel -> tiled guest bytes.
//! CPU visibility uses a synthetic page lock/CAS, never real game memory.
use super::resolve_cache_cases::{info, selection};
use super::{DEADLINE, finish, open, setup};
use skyrim_native_gpu::{
    Error, Gpu,
    backend::Platform,
    edram::targets::{
        ConstantDraw, ConstantSlot, DrawAdmission, DrawArena, GuestMemory, GuestPage, TargetCache,
        TargetOptions,
    },
    render::{Pipeline, PipelineDescriptor},
    types as t,
};
use std::{collections::BTreeMap, sync::Arc, time::Duration};
use xenos_state::{edram::RenderTargetKey as Key, resolve::ResolveInfo, xenos::MsaaSamples};

pub(super) fn key() -> Key {
    Key {
        base_tiles: 0,
        pitch_tiles_at_32bpp: 1,
        msaa_samples: MsaaSamples::X1,
        is_depth: false,
        format: 0,
    }
}
pub(super) fn pages() -> Vec<GuestPage> {
    (1..33)
        .map(|n| GuestPage {
            address: n * 4096,
            bytes: Arc::new([0xcd; 4096]),
            generation: Some(10),
        })
        .collect()
}
pub(super) fn environment(
    gpu: &mut Gpu<Platform>,
) -> (TargetCache, DrawArena, GuestMemory, Pipeline<Platform>) {
    let arena = gpu
        .draw_arena(
            16 << 10,
            [0, 1, 2, 3, 4].map(|binding| ConstantSlot {
                binding,
                visibility: t::ShaderStages::FRAGMENT,
                storage: false,
                bytes: 16,
            }),
        )
        .unwrap();
    let layout = gpu.pipeline_layout(&[arena.layout().clone()]).unwrap();
    let shader = gpu
        .shader(
            r#"
@group(0) @binding(0) var<uniform> color:vec4u;
@vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f {
let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);}
@fragment fn fs()->@location(0) vec4f {return vec4f(color)/255.;}
"#,
        )
        .unwrap();
    let pipeline = gpu
        .render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &[Some(t::ColorTargetState {
                format: t::TextureFormat::Rgba8Unorm,
                blend: None,
                write_mask: t::ColorWrites::ALL,
            })],
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
        })
        .unwrap();
    (
        gpu.edram_target_cache(TargetOptions::default()).unwrap(),
        arena,
        gpu.guest_memory(1).unwrap(),
        pipeline,
    )
}
pub(super) fn encode(
    gpu: &mut Gpu<Platform>,
    cache: &mut TargetCache,
    arena: &mut DrawArena,
    memory: &mut GuestMemory,
    pipeline: &Pipeline<Platform>,
    pages: &[GuestPage],
    info: &ResolveInfo,
    submit: bool,
) {
    let mut f = cache
        .begin(gpu)
        .unwrap()
        .with_constants(arena)
        .unwrap()
        .with_guest_memory(memory)
        .unwrap();
    f.targets(&selection(key()), 16).unwrap();
    assert_eq!(f.sync_guest_pages(pages).unwrap(), DrawAdmission::Accepted);
    let color: Vec<u8> = [17u32, 53, 101, 255]
        .into_iter()
        .flat_map(u32::to_le_bytes)
        .collect();
    assert_eq!(
        f.draw(&ConstantDraw {
            pipeline,
            groups: &[],
            constants: [&color; 5],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: 0,
        })
        .unwrap(),
        DrawAdmission::Accepted
    );
    assert_eq!(f.resolve_guest(info).unwrap(), Some(0x1000..0x21000));
    if submit {
        f.submit().unwrap();
    }
}
fn expected(info: &ResolveInfo) -> Vec<u8> {
    let mut out = vec![0xcd; 128 << 10];
    let mut color = [17u8, 53, 101, 255];
    match info.copy_dest_info.copy_dest_endian.raw() {
        1 => {
            color.swap(0, 1);
            color.swap(2, 3);
        }
        2 => color.reverse(),
        3 => color.rotate_left(2),
        _ => {}
    }
    for y in 0..info.rect.height {
        for x in 0..info.rect.width {
            let offset =
                xenos_texture::address::tiled_2d(x as i32, y as i32, info.dest_pitch_aligned, 2)
                    as usize;
            out[offset..offset + 4].copy_from_slice(&color);
        }
    }
    out
}
fn model_writeback(
    memory: &mut GuestMemory,
    gpu: &mut Gpu<Platform>,
    cpu: &mut BTreeMap<u32, GuestPage>,
) {
    let pending = memory.gpu_newer_pages();
    let stats = memory
        .writeback(gpu, &pending, DEADLINE, |old, new| {
            let p = cpu.get_mut(&old.address).unwrap();
            if p.generation != old.generation || p.bytes != old.bytes {
                return false;
            }
            *p = new.clone();
            true
        })
        .unwrap();
    assert_eq!(stats.written as usize, pending.len());
    assert_eq!(stats.conflicted, 0);
}

#[test]
fn guest_store_exact_tiling_endians_preserved_holes_and_versioned_visibility() {
    let (_lock, before) = setup();
    let mut gpu = open();
    for endian in 0..4 {
        let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
        let pages = pages();
        let mut info = info(key());
        info.copy_dest_info.copy_dest_endian = xenos_state::xenos::Endian128::from_raw(endian);
        encode(
            &mut gpu,
            &mut cache,
            &mut arena,
            &mut memory,
            &pipeline,
            &pages,
            &info,
            true,
        );
        assert_eq!(memory.gpu_newer_pages().len(), 32);
        let mut cpu: BTreeMap<_, _> = pages.into_iter().map(|p| (p.address, p)).collect();
        model_writeback(&mut memory, &mut gpu, &mut cpu);
        assert!(memory.gpu_newer_pages().is_empty());
        let bytes: Vec<_> = cpu.values().flat_map(|p| p.bytes.iter().copied()).collect();
        assert_eq!(bytes, expected(&info), "endian{endian}");
        assert!(cpu.values().all(|p| p.generation == Some(11)));
        // The GPU bytes are already current after CPU visibility: no reupload.
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&cpu.values().cloned().collect::<Vec<_>>())
            .unwrap();
        assert_eq!(f.stats().guest_changed_pages, 0);
        f.submit().unwrap();
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_unchanged_baseline_preserves_gpu_and_cpu_aba_is_not_cpu_wins() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    let initial = pages();
    encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &initial,
        &info(key()),
        true,
    );
    {
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&initial).unwrap();
        assert_eq!(f.stats().guest_upload_bytes, 0);
        f.submit().unwrap();
    }
    for (version, modify) in [(10, true), (11, false), (11, true)] {
        let mut next = initial.clone();
        next[0].generation = Some(version);
        if modify {
            Arc::make_mut(&mut next[0].bytes)[0] ^= 1;
        }
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        assert!(f.sync_guest_pages(&next).is_err());
        assert!(f.submit().is_err());
        assert_eq!(memory.gpu_newer_pages().len(), 32);
    }
    let mut cpu: BTreeMap<_, _> = initial.into_iter().map(|p| (p.address, p)).collect();
    model_writeback(&mut memory, &mut gpu, &mut cpu);
    assert_eq!(
        cpu.values()
            .flat_map(|p| p.bytes.iter().copied())
            .collect::<Vec<_>>(),
        expected(&info(key()))
    );
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_abandoned_frame_preserves_cpu_metadata_and_original_gpu_bytes() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    let initial = pages();
    {
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&initial).unwrap();
        f.submit().unwrap();
    }
    encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &initial,
        &info(key()),
        false,
    );
    assert!(memory.gpu_newer_pages().is_empty());
    assert_eq!(
        memory.test_read_page(&mut gpu, 0x1000, DEADLINE).unwrap(),
        vec![0xcd; 4096]
    );
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_partial_compare_exchange_keeps_conflicting_pages_pinned() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    let initial = pages();
    encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &initial,
        &info(key()),
        true,
    );
    let pending = memory.gpu_newer_pages();
    let mut cpu: BTreeMap<_, _> = initial.into_iter().map(|p| (p.address, p)).collect();
    cpu.get_mut(&0x1000).unwrap().generation = Some(12); // ABA bytes, distinct write history.
    let stats = memory
        .writeback(&mut gpu, &pending, DEADLINE, |old, new| {
            let p = cpu.get_mut(&old.address).unwrap();
            if p.generation != old.generation || p.bytes != old.bytes {
                return false;
            }
            *p = new.clone();
            true
        })
        .unwrap();
    assert_eq!(stats.written, 31);
    assert_eq!(stats.conflicted, 1);
    assert_eq!(memory.gpu_newer_pages(), vec![0x1000]);
    assert_eq!(cpu[&0x1000].generation, Some(12));
    assert_eq!(&*cpu[&0x1000].bytes, &[0xcd; 4096]);
    // Fill every other slot, then protect them in one capture. The only possible
    // victim is the unresolved GPU page's block; no eviction is allowed.
    let others: Vec<_> = (1..512)
        .map(|block| GuestPage {
            address: block * 65536,
            bytes: Arc::new([0; 4096]),
            generation: None,
        })
        .collect();
    {
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&others).unwrap();
        f.submit().unwrap();
    }
    let mut request = others;
    request.push(GuestPage {
        address: 512 * 65536,
        bytes: Arc::new([0; 4096]),
        generation: None,
    });
    let mut f = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    assert!(matches!(
        f.sync_guest_pages(&request),
        Err(Error::Rejected("guest mirror slots unavailable"))
    ));
    assert!(f.submit().is_err());
    assert_eq!(memory.gpu_newer_pages(), vec![0x1000]);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_requires_tracked_complete_fresh_destination_and_rejects_malformed_clears() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, _) = environment(&mut gpu);
    for mode in 0..5 {
        let mut p = pages();
        let mut resolve = info(key());
        match mode {
            0 => p[0].generation = None,
            1 => {
                p.remove(0);
            }
            2 => p[0].generation = Some(u64::MAX),
            3 => {
                resolve.copy_control.color_clear_enable = true;
                resolve.color_original_base = u32::MAX;
            }
            _ => p.clear(),
        }
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&p).unwrap();
        assert!(f.resolve_guest(&resolve).is_err());
        assert!(f.submit().is_err());
        assert!(memory.gpu_newer_pages().is_empty());
        assert_eq!(memory.mapped_blocks(), 0);
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_writeback_timeout_cannot_invoke_cpu_commit_or_clear_gpu_newer() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    let initial = pages();
    encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &initial,
        &info(key()),
        true,
    );
    let pending = memory.gpu_newer_pages();
    let mut calls = 0;
    let verified_before = gpu.verified_serial();
    // A fence cap only delays retirement on Metal. Hold execution evidence
    // explicitly so a fast host cannot turn this timeout test into success.
    gpu.delay_completion_observation(Some(verified_before));
    assert!(matches!(
        memory.writeback(&mut gpu, &pending, Duration::from_millis(50), |_, _| {
            calls += 1;
            true
        }),
        Err(Error::Timeout(_))
    ));
    assert_eq!(calls, 0);
    assert_eq!(memory.gpu_newer_pages(), pending);
    assert_eq!(gpu.verified_serial(), verified_before);
    gpu.delay_completion_observation(None);
    let mut cpu: BTreeMap<_, _> = initial.into_iter().map(|p| (p.address, p)).collect();
    model_writeback(&mut memory, &mut gpu, &mut cpu);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_failed_readback_submission_never_commits_cpu_and_poison_cannot_be_reset() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &pages(),
        &info(key()),
        true,
    );
    let pending = memory.gpu_newer_pages();
    let mut calls = 0;
    gpu.fail_next_submit();
    assert!(
        memory
            .writeback(&mut gpu, &pending, DEADLINE, |_, _| {
                calls += 1;
                true
            })
            .is_err()
    );
    assert_eq!(calls, 0);
    assert_eq!(memory.gpu_newer_pages(), pending);
    assert!(
        memory
            .writeback(&mut gpu, &pending, DEADLINE, |_, _| true)
            .is_err()
    );
    // Unknown backend submission uses the existing quarantine rather than
    // pretending buffers or CPU visibility have completed.
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_two_versions_stay_ordered_in_one_submit_without_cpu_roundtrip() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    let initial = pages();
    let resolve = info(key());
    let mut f = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    f.targets(&selection(key()), 16).unwrap();
    let mut tickets = Vec::new();
    for values in [[17u32, 53, 101, 255], [41, 83, 167, 255]] {
        assert_eq!(
            f.sync_guest_pages(&initial).unwrap(),
            DrawAdmission::Accepted
        );
        let color: Vec<_> = values.into_iter().flat_map(u32::to_le_bytes).collect();
        f.draw(&ConstantDraw {
            pipeline: &pipeline,
            groups: &[],
            constants: [&color; 5],
            vertices: 0..3,
            instances: 0..1,
            indices: None,
            viewport: None,
            scissor: None,
            blend: t::Color::TRANSPARENT,
            stencil_reference: 0,
        })
        .unwrap();
        f.resolve_guest(&resolve).unwrap();
        // An independent linear snapshot must retain its earlier version even
        // after the second draw rewrites the same native render target.
        tickets.push(f.resolve_pixels(&resolve).unwrap().unwrap());
    }
    assert_eq!(f.stats().guest_changed_pages, 32);
    let (serial, stats, _) = f.submit().unwrap();
    assert_eq!(serial, 1);
    assert_eq!(stats.resolves, 4);
    for (i, ticket) in tickets.iter().enumerate() {
        let literal = if i == 0 { 0xff653511 } else { 0xffa75329 };
        assert!(
            ticket
                .read(&mut gpu, DEADLINE)
                .unwrap()
                .iter()
                .all(|&v| v == literal)
        );
    }
    let mut cpu: BTreeMap<_, _> = initial.into_iter().map(|p| (p.address, p)).collect();
    model_writeback(&mut memory, &mut gpu, &mut cpu);
    let mut expected = expected(&resolve);
    for y in 0..resolve.rect.height {
        for x in 0..resolve.rect.width {
            let offset =
                xenos_texture::address::tiled_2d(x as i32, y as i32, resolve.dest_pitch_aligned, 2)
                    as usize;
            expected[offset..offset + 4].copy_from_slice(&[41, 83, 167, 255]);
        }
    }
    assert_eq!(
        cpu.values()
            .flat_map(|p| p.bytes.iter().copied())
            .collect::<Vec<_>>(),
        expected
    );
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_callback_unwind_keeps_owner_failed_instead_of_guessing_cpu_visibility() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &pages(),
        &info(key()),
        true,
    );
    let pending = memory.gpu_newer_pages();
    let panicked = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _ = memory.writeback(&mut gpu, &pending, DEADLINE, |_, _| {
            panic!("authored CPU adapter interruption after unknown commit");
        });
    }));
    assert!(panicked.is_err());
    assert!(
        memory
            .writeback(&mut gpu, &pending, DEADLINE, |_, _| true)
            .is_err()
    );
    assert_eq!(memory.gpu_newer_pages(), pending);
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_atomic_subwords_3d_and_packed_nibbles_use_exact_native_parameters() {
    use xenos_state::xenos::{CopyCommand, Endian128, TextureFormat as F};
    let (_lock, before) = setup();
    let mut gpu = open();
    for (format, bpe) in [(F::K8, 1usize), (F::K88, 2), (F::Dxt3aAs1111, 8)] {
        for endian in 0..2 {
            for volume in [false, true] {
                let packed = format == F::Dxt3aAs1111;
                if packed && volume {
                    continue;
                }
                let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
                let initial = pages();
                let mut resolve = info(key());
                resolve.dest_format = format;
                resolve.copy_dest_info.copy_dest_format = format;
                resolve.copy_dest_info.copy_dest_endian = Endian128::from_raw(endian);
                resolve.copy_dest_info.copy_dest_array = volume;
                resolve.copy_dest_info.copy_dest_slice = 2;
                resolve.dest_offset_x_div_8 = 1;
                resolve.dest_offset_y_div_8 = 1;
                if packed {
                    resolve.copy_control.copy_command = CopyCommand::ConvertTo1111;
                    resolve.convert_to_1111_supported = true;
                    resolve.copy_func = 0x7777; // All four comparisons are always true.
                }
                gpu.test_resolve_initialization(true);
                encode(
                    &mut gpu,
                    &mut cache,
                    &mut arena,
                    &mut memory,
                    &pipeline,
                    &initial,
                    &resolve,
                    true,
                );
                let mut cpu: BTreeMap<_, _> = initial.into_iter().map(|p| (p.address, p)).collect();
                model_writeback(&mut memory, &mut gpu, &mut cpu);
                let mut expected = vec![0xcd; 128 << 10];
                let divisor = if packed { 4 } else { 1 };
                let mut pixel = match bpe {
                    1 => vec![17],
                    2 => vec![17, 53],
                    _ => vec![255; 8],
                };
                if endian == 1 && bpe == 2 {
                    pixel.reverse();
                }
                for y in 0..resolve.rect.height / divisor {
                    for x in 0..resolve.rect.width / divisor {
                        let x = (x + 8 / divisor) as i32;
                        let y = (y + 8 / divisor) as i32;
                        let at = if volume {
                            xenos_texture::address::tiled_3d(x, y, 2, 160, 32, bpe.ilog2()) as usize
                        } else {
                            xenos_texture::address::tiled_2d(x, y, 160, bpe.ilog2()) as usize
                        };
                        expected[at..at + bpe].copy_from_slice(&pixel);
                    }
                }
                assert_eq!(
                    cpu.values()
                        .flat_map(|p| p.bytes.iter().copied())
                        .collect::<Vec<_>>(),
                    expected,
                    "format{format:?} endian{endian} volume{volume}"
                );
            }
        }
    }
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_rejects_wrapped_clipped_or_unaligned_destinations_before_publication() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, _) = environment(&mut gpu);
    for bad in 0..9 {
        let mut resolve = info(key());
        match bad {
            0 => resolve.dest_pitch_aligned = 31,
            1 => resolve.copy_dest_base += 1,
            2 => resolve.dest_offset_x_div_8 = u32::MAX,
            3 => resolve.copy_dest_extent_length = 1024,
            4 => resolve.copy_dest_base = 0x1fff_f000,
            5 => resolve.copy_dest_info.copy_dest_slice = 8,
            6 => resolve.dest_pitch_aligned = 32,
            7 => resolve.color_edram = None,
            _ => {
                resolve.copy_dest_info.copy_dest_array = true;
                resolve.dest_offset_y_div_8 = 4;
            }
        }
        let mut f = cache
            .begin(&mut gpu)
            .unwrap()
            .with_constants(&mut arena)
            .unwrap()
            .with_guest_memory(&mut memory)
            .unwrap();
        f.sync_guest_pages(&pages()).unwrap();
        assert!(f.resolve_guest(&resolve).is_err(), "case{bad}");
        assert!(f.submit().is_err());
        assert!(memory.gpu_newer_pages().is_empty());
        assert_eq!(memory.mapped_blocks(), 0);
    }
    // A no-op also closes its capture, so it cannot authorize a later store.
    let mut f = cache
        .begin(&mut gpu)
        .unwrap()
        .with_constants(&mut arena)
        .unwrap()
        .with_guest_memory(&mut memory)
        .unwrap();
    f.sync_guest_pages(&pages()).unwrap();
    let mut empty = info(key());
    empty.copy_dest_extent_length = 0;
    assert!(f.resolve_guest(&empty).unwrap().is_none());
    assert!(f.resolve_guest(&info(key())).is_err());
    assert!(f.submit().is_err());
    drop(gpu);
    finish(before);
}

#[test]
fn guest_store_failed_earlier_execution_prevents_later_copy_from_publishing_cpu_bytes() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (mut cache, mut arena, mut memory, pipeline) = environment(&mut gpu);
    encode(
        &mut gpu,
        &mut cache,
        &mut arena,
        &mut memory,
        &pipeline,
        &pages(),
        &info(key()),
        true,
    );
    let serial = memory.last_submission();
    let pending = memory.gpu_newer_pages();
    let mut calls = 0;
    gpu.fail_completion(serial);
    assert!(
        memory
            .writeback(&mut gpu, &pending, DEADLINE, |_, _| {
                calls += 1;
                true
            })
            .is_err()
    );
    assert_eq!(calls, 0);
    assert_eq!(memory.gpu_newer_pages(), pending);
    drop(gpu);
    finish(before);
}
