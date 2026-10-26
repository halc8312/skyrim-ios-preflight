//! Native execution of the actual public GPU resolve-store kernel. These are
//! authored bytes and CPU address oracles, NOT a native guest-memory manager.
use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    bindings::{Entry, Resource},
    render::BoundGroup,
    types as t,
};
use std::num::NonZeroU64;
use xenos_texture::address::{tiled_2d, tiled_3d};
const BLOCK: usize = 4096;
const CHUNK: usize = 131072;
const TOTAL: usize = CHUNK * 4;
fn buf<'a>(
    binding: u32,
    buffer: &'a skyrim_native_gpu::Buffer<skyrim_native_gpu::backend::Platform>,
) -> Entry<'a, skyrim_native_gpu::backend::Platform> {
    Entry {
        binding,
        resource: Resource::Buffer {
            buffer,
            offset: 0,
            size: buffer.size(),
        },
    }
}
fn swapped(mut b: Vec<u8>, mode: u32) -> Vec<u8> {
    let size = b.len();
    match (size, mode) {
        (2, 1) => b.reverse(),
        (4 | 8 | 16, 1) => {
            for p in b.chunks_mut(2) {
                p.reverse()
            }
        }
        (4 | 8 | 16, 2) => {
            for p in b.chunks_mut(4) {
                p.reverse()
            }
        }
        (4 | 8 | 16, 3) => {
            for p in b.chunks_mut(4) {
                p.rotate_left(2)
            }
        }
        (8 | 16, 4) => {
            for p in b.chunks_mut(8) {
                p.reverse()
            }
        }
        (16, 5) => b.reverse(),
        _ => {}
    };
    b
}
#[test]
fn actual_resolve_store_covers_atomic_subwords_all_endians_3d_four_chunks_and_preserved_holes() {
    run_store(false);
}
#[test]
fn masked_resolve_store_records_only_actual_bytes_across_endians_subwords_volume_and_unmapped_holes()
 {
    run_store(true);
}
fn run_store(masked: bool) {
    let (_lock, before) = setup();
    let mut gpu = open();
    let slot = |binding: u32, uniform: bool, write: bool| t::BindGroupLayoutEntry {
        binding,
        visibility: t::ShaderStages::COMPUTE,
        ty: t::BindingType::Buffer {
            ty: if uniform {
                t::BufferBindingType::Uniform
            } else {
                t::BufferBindingType::Storage { read_only: !write }
            },
            has_dynamic_offset: false,
            min_binding_size: NonZeroU64::new(if uniform { 64 } else { 4 }),
        },
        count: None,
    };
    let mut entries = vec![
        slot(0, true, false),
        slot(1, false, false),
        slot(2, false, false),
        slot(3, false, true),
        slot(4, false, true),
        slot(5, false, true),
        slot(6, false, true),
    ];
    if masked {
        entries.push(slot(7, false, true));
    }
    let gl = gpu.group_layout(&entries).unwrap();
    let pl = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let pipelines: Vec<_> = [false, true]
        .iter()
        .map(|&atomic| {
            let shader = gpu
                .shader(&if masked {
                    xenos_gfx::edram_shaders::store_masked_module(atomic)
                } else {
                    xenos_gfx::edram_shaders::store_module(atomic)
                })
                .unwrap();
            gpu.shared_compute_pipeline(&pl, &shader, "main").unwrap()
        })
        .collect();
    let chunks: Vec<_> = (0..4)
        .map(|_| {
            gpu.create_buffer(
                "store chunk",
                CHUNK as u64,
                U::STORAGE_READ_WRITE | U::COPY_DST | U::COPY_SRC,
            )
            .unwrap()
        })
        .collect();
    let out = gpu
        .create_buffer(
            "store exact snapshot",
            TOTAL as u64,
            U::MAP_READ | U::COPY_DST,
        )
        .unwrap();
    let mapping: Vec<_> = (0..TOTAL / BLOCK)
        .map(|i| {
            if masked && i % 7 == 0 {
                0
            } else {
                ((i * 17) % (TOTAL / BLOCK) + 1) as u32
            }
        })
        .collect();
    let table = gpu
        .create_buffer_init(
            "immutable shuffled physical table",
            &mapping
                .iter()
                .flat_map(|v| v.to_le_bytes())
                .collect::<Vec<_>>(),
            U::STORAGE_READ_ONLY | U::COPY_DST,
        )
        .unwrap();
    let initial = vec![0xCD; CHUNK];
    let width = 16u32;
    let height = 8u32;
    let base = 4096u32;
    let origin = [8u32, 8];
    let pitch = 64u32;
    let height_aligned = 64u32;
    let mut chunks_covered = std::collections::BTreeSet::new();
    for bpe in [1usize, 2, 4, 8, 16, 0] {
        for endian in 0..6 {
            for volume in [false, true] {
                if bpe == 0 && volume {
                    continue;
                } // 1111 conversion is a 2D format contract.
                let packed = bpe == 0;
                let element_bytes = if packed { 8 } else { bpe };
                let wpp = if packed { 1 } else { element_bytes.div_ceil(4) };
                let mut words = Vec::new();
                for y in 0..height {
                    for x in 0..width {
                        for w in 0..wpp {
                            words.push(if packed {
                                (x + 3 * y) & 15
                            } else {
                                0x71422391u32
                                    .wrapping_add((y * width + x) * 0x01030709)
                                    .rotate_left(w as u32 * 7)
                            });
                        }
                    }
                }
                let source = gpu
                    .create_buffer_init(
                        "store linear source",
                        &words
                            .iter()
                            .flat_map(|v| v.to_le_bytes())
                            .collect::<Vec<_>>(),
                        U::STORAGE_READ_ONLY | U::COPY_DST,
                    )
                    .unwrap();
                let p = [
                    base,
                    pitch,
                    height_aligned,
                    u32::from(volume),
                    2,
                    origin[0],
                    origin[1],
                    element_bytes as u32,
                    endian,
                    width,
                    height,
                    wpp as u32,
                    u32::from(packed),
                    ((BLOCK / 4).ilog2()) | ((CHUNK / 4).ilog2() << 8),
                    if masked { 1024 } else { 0 },
                    if masked {
                        TOTAL as u32 - 1024
                    } else {
                        TOTAL as u32
                    },
                ];
                let params = gpu
                    .create_buffer_init(
                        "immutable store parameters",
                        &p.iter().flat_map(|v| v.to_le_bytes()).collect::<Vec<_>>(),
                        U::UNIFORM | U::COPY_DST,
                    )
                    .unwrap();
                let mask_bytes = ((TOTAL - 1024).div_ceil(32) * 4) as u64;
                let mask = masked.then(|| {
                    gpu.create_buffer(
                        "exact written-byte mask",
                        mask_bytes,
                        U::STORAGE_READ_WRITE | U::COPY_SRC,
                    )
                    .unwrap()
                });
                let mask_read = masked.then(|| {
                    gpu.create_buffer(
                        "written mask readback",
                        mask_bytes,
                        U::COPY_DST | U::MAP_READ,
                    )
                    .unwrap()
                });
                let mut entries = vec![
                    buf(0, &params),
                    buf(1, &source),
                    buf(2, &table),
                    buf(3, &chunks[0]),
                    buf(4, &chunks[1]),
                    buf(5, &chunks[2]),
                    buf(6, &chunks[3]),
                ];
                if let Some(mask) = &mask {
                    entries.push(buf(7, mask));
                }
                let group = gpu.bind_group(&gl, &entries).unwrap();
                for chunk in &chunks {
                    gpu.write_buffer(chunk, 0, &initial).unwrap();
                }
                let mut e = gpu.encoder().unwrap();
                let divisor = if packed { 4 } else { 1 };
                e.dispatch_shared(
                    &pipelines[usize::from(element_bytes < 4)],
                    &[BoundGroup {
                        group: &group,
                        dynamic_offsets: &[],
                    }],
                    [
                        (width / divisor).div_ceil(8),
                        (height / divisor).div_ceil(8),
                        1,
                    ],
                )
                .unwrap();
                for (n, chunk) in chunks.iter().enumerate() {
                    e.copy_buffer(chunk, 0, &out, (n * CHUNK) as u64, CHUNK as u64)
                        .unwrap();
                }
                if let (Some(mask), Some(rb)) = (&mask, &mask_read) {
                    e.copy_buffer(mask, 0, rb, 0, mask_bytes).unwrap();
                }
                gpu.submit(vec![e.finish().unwrap()]).unwrap();
                let actual = gpu.read_buffer(&out, 0..TOTAL as u64, DEADLINE).unwrap();
                let mut expected = vec![0xCD; TOTAL];
                let mut expected_mask = vec![0u8; mask_bytes as usize];
                let mut visited = std::collections::BTreeSet::new();
                for y in 0..height / divisor {
                    for x in 0..width / divisor {
                        let data = if packed {
                            let mut out = vec![0u8; 8];
                            for py in 0..4 {
                                for px in 0..4 {
                                    let nibble = words[((y * 4 + py) * width + x * 4 + px) as usize]
                                        as u8
                                        & 15;
                                    let i = (py * 4 + px) as usize;
                                    out[i / 2] |= nibble << ((i % 2) * 4);
                                }
                            }
                            out
                        } else {
                            let i = (y * width + x) as usize * wpp;
                            words[i..i + wpp]
                                .iter()
                                .flat_map(|v| v.to_le_bytes())
                                .take(element_bytes)
                                .collect()
                        };
                        let xx = (x + origin[0] / divisor) as i32;
                        let yy = (y + origin[1] / divisor) as i32;
                        let rel = if volume {
                            tiled_3d(xx, yy, 2, pitch, height_aligned, element_bytes.ilog2())
                        } else {
                            i64::from(tiled_2d(xx, yy, pitch, element_bytes.ilog2()))
                        };
                        assert!(rel >= 0);
                        let physical = base as usize + rel as usize;
                        assert!(physical + element_bytes <= TOTAL);
                        if mapping[physical / BLOCK] == 0 {
                            continue;
                        }
                        if masked {
                            for byte in physical..physical + element_bytes {
                                let bit = byte - 1024;
                                expected_mask[bit / 8] |= 1 << (bit % 8);
                            }
                        }
                        let slot = (mapping[physical / BLOCK] - 1) as usize;
                        let offset = slot * BLOCK + physical % BLOCK;
                        chunks_covered.insert(offset / CHUNK);
                        for i in offset..offset + element_bytes {
                            assert!(visited.insert(i), "fixture must not alias destinations");
                        }
                        expected[offset..offset + element_bytes]
                            .copy_from_slice(&swapped(data, endian));
                    }
                }
                if let Some(rb) = &mask_read {
                    let actual_mask = gpu.read_buffer(rb, 0..mask_bytes, DEADLINE).unwrap();
                    assert_eq!(
                        actual_mask, expected_mask,
                        "mask bpe{bpe} endian{endian} volume{volume}"
                    );
                }
                assert!(
                    actual == expected,
                    "store bpe{bpe} endian{endian} volume{volume}; first={:?}",
                    actual.iter().zip(&expected).position(|(a, b)| a != b)
                );
            }
        }
    }
    assert_eq!(chunks_covered, (0..4).collect());
    drop(gpu);
    finish(before);
}
