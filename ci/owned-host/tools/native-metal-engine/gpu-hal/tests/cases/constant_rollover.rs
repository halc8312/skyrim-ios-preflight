//! Actual production constant patch kernel, repeatedly overwriting ONE bounded
//! block between graphics consumers in ONE submission. No game-derived input.
use super::{DEADLINE, U, constant_fixture, finish, open, setup};
use skyrim_native_gpu::{
    bindings::{Entry, Resource},
    compute::StorageBinding,
    render::{BoundGroup, ColorAttachment, Draw, PipelineDescriptor},
    texture::TextureDescriptor,
    types as t,
};
use std::num::NonZeroU64;

#[test]
fn ordered_constant_block_rollovers_keep_all_raw_snapshots_and_graphics_consumers_in_one_submit() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let kernel = xenos_gfx::constants::BINNED_SHADER;
    let patch = gpu.create_compute(kernel, "main").unwrap();
    let phases = 12u32;
    let count = 27;
    let (r, v, o) = constant_fixture::batch(count, 3);
    let records = gpu
        .create_buffer(
            "reused patch records",
            r.len() as u64,
            U::STORAGE_READ_ONLY | U::COPY_DST,
        )
        .unwrap();
    let values = gpu
        .create_buffer(
            "reused patch values",
            v.len() as u64,
            U::STORAGE_READ_ONLY | U::COPY_DST,
        )
        .unwrap();
    let block = gpu
        .create_buffer_init(
            "single reused constant block",
            &vec![0xA5; o.len()],
            U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::COPY_SRC | U::COPY_DST,
        )
        .unwrap();
    let compute = gpu
        .bind_compute(
            &patch,
            &[
                StorageBinding {
                    binding: 0,
                    buffer: &records,
                    offset: 0,
                    size: records.size(),
                },
                StorageBinding {
                    binding: 1,
                    buffer: &values,
                    offset: 0,
                    size: values.size(),
                },
                StorageBinding {
                    binding: 2,
                    buffer: &block,
                    offset: 0,
                    size: block.size(),
                },
            ],
        )
        .unwrap();
    let snapshots = gpu
        .create_buffer(
            "raw generation snapshots",
            block.size() * u64::from(phases),
            U::MAP_READ | U::COPY_DST,
        )
        .unwrap();
    let size = t::Extent3d {
        width: phases,
        height: 1,
        depth_or_array_layers: 1,
    };
    let target = gpu
        .create_texture(
            "graphics generation oracle",
            TextureDescriptor {
                size,
                mip_levels: 1,
                sample_count: 1,
                dimension: t::TextureDimension::D2,
                format: t::TextureFormat::Rgba8Unorm,
                usage: t::TextureUses::COLOR_TARGET | t::TextureUses::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu
        .texture_view(&target, t::TextureViewDimension::D2, 0..1, 0..1)
        .unwrap();
    let gl = gpu
        .group_layout(&[t::BindGroupLayoutEntry {
            binding: 0,
            visibility: t::ShaderStages::FRAGMENT,
            ty: t::BindingType::Buffer {
                ty: t::BufferBindingType::Storage { read_only: true },
                has_dynamic_offset: false,
                min_binding_size: NonZeroU64::new(4),
            },
            count: None,
        }])
        .unwrap();
    let layout = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let group = gpu
        .bind_group(
            &gl,
            &[Entry {
                binding: 0,
                resource: Resource::Buffer {
                    buffer: &block,
                    offset: 0,
                    size: block.size(),
                },
            }],
        )
        .unwrap();
    let shader=gpu.shader("@group(0) @binding(0) var<storage,read> c:array<u32>; @vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);} @fragment fn fs()->@location(0) vec4f{var h=2166136261u;for(var i=0u;i<arrayLength(&c);i++){h=(h^c[i])*16777619u;}return vec4f(f32(h&255u),f32((h>>8u)&255u),f32((h>>16u)&255u),f32(h>>24u))/255.;}").unwrap();
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
    let pixels = gpu
        .create_buffer("graphics result", 256, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    let mut expected = Vec::new();
    let mut hashes = Vec::new();
    for phase in 0..phases {
        let seed = phase * 19 + 3;
        let (r, v, oracle) = constant_fixture::batch(count, seed);
        assert_eq!(oracle.len() as u64, block.size());
        gpu.write_buffer_ordered(&mut e, &records, 0, &r).unwrap();
        gpu.write_buffer_ordered(&mut e, &values, 0, &v).unwrap();
        e.dispatch(&patch, &compute, [5, 1, 1]).unwrap();
        e.copy_buffer(
            &block,
            0,
            &snapshots,
            u64::from(phase) * block.size(),
            block.size(),
        )
        .unwrap();
        e.render(
            &[Some(ColorAttachment {
                view: &view,
                clear: (phase == 0).then_some(t::Color::BLACK),
            })],
            &[Draw {
                pipeline: &pipeline,
                groups: &[BoundGroup {
                    group: &group,
                    dynamic_offsets: &[],
                }],
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor: Some([phase, 0, 1, 1]),
                blend: t::Color::TRANSPARENT,
                stencil_reference: 0,
            }],
        )
        .unwrap();
        let hash = oracle.chunks_exact(4).fold(2166136261u32, |h, b| {
            (h ^ u32::from_le_bytes(b.try_into().unwrap())).wrapping_mul(16777619)
        });
        hashes.push(hash.to_le_bytes());
        expected.extend(oracle);
    }
    e.copy_texture_to_buffer(
        &target,
        0,
        t::Origin3d::ZERO,
        size,
        &pixels,
        t::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(1),
        },
    )
    .unwrap();
    drop(records);
    drop(values);
    drop(compute);
    drop(patch);
    drop(group);
    drop(pipeline);
    drop(block);
    let serial = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(serial, 1, "rollovers must not submit implicitly");
    assert_eq!(
        gpu.read_buffer(&snapshots, 0..snapshots.size(), DEADLINE)
            .unwrap(),
        expected
    );
    let actual = gpu.read_buffer(&pixels, 0..256, DEADLINE).unwrap();
    for phase in 0..phases as usize {
        assert_eq!(&actual[phase * 4..phase * 4 + 4], &hashes[phase]);
    }
    assert!(actual[phases as usize * 4..].iter().all(|&v| v == 0));
    drop(gpu);
    finish(before);
}

#[test]
fn shared_segment_producer_drives_real_gpu_and_uniform_consumers_without_capacity_submissions() {
    use xenos_gfx::constant_segments::{PatchSegment, PushError};
    let (_lock, before) = setup();
    let mut gpu = open();
    let phases = 12usize;
    let capacity = 8192usize;
    let mut producer = PatchSegment::new(capacity as u64, 256).unwrap();
    let mut stream = gpu.constant_stream(capacity as u64).unwrap();
    let snapshots = gpu
        .create_buffer_init(
            "segment raw snapshots",
            &vec![0xa5; capacity * phases],
            U::MAP_READ | U::COPY_DST,
        )
        .unwrap();
    let target = gpu
        .create_texture(
            "segment graphics oracle",
            TextureDescriptor {
                size: t::Extent3d {
                    width: 256,
                    height: 1,
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 1,
                dimension: t::TextureDimension::D2,
                format: t::TextureFormat::Rgba8Unorm,
                usage: t::TextureUses::COLOR_TARGET | t::TextureUses::COPY_SRC,
            },
        )
        .unwrap();
    let view = gpu
        .texture_view(&target, t::TextureViewDimension::D2, 0..1, 0..1)
        .unwrap();
    let gl = gpu
        .group_layout(&[t::BindGroupLayoutEntry {
            binding: 0,
            visibility: t::ShaderStages::FRAGMENT,
            ty: t::BindingType::Buffer {
                ty: t::BufferBindingType::Uniform,
                has_dynamic_offset: true,
                min_binding_size: NonZeroU64::new(16),
            },
            count: None,
        }])
        .unwrap();
    let layout = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let shader=gpu.shader("@group(0) @binding(0) var<uniform> c:vec4u; @vertex fn vs(@builtin(vertex_index) i:u32)->@builtin(position) vec4f{let p=array<vec2f,3>(vec2f(-1.,-1.),vec2f(3.,-1.),vec2f(-1.,3.));return vec4f(p[i],0.,1.);} @fragment fn fs()->@location(0)vec4f{return vec4f(f32(c.x&255u)/255.,0.,0.,1.);}").unwrap();
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
    let pixels = gpu
        .create_buffer("segment graphics readback", 1024, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let mut e = gpu.encoder().unwrap();
    let mut expected = vec![0xa5; capacity * phases];
    let mut expected_pixels = Vec::new();
    let mut old = None;
    for phase in 0..phases {
        let mut blocks = Vec::new();
        for n in 0..100 {
            let bytes = [16, 256, 2048, 4096][n % 4];
            let seed = (phase * 37 + n * 11 + 1) as u32;
            let data: Vec<u8> = (0..bytes / 4)
                .flat_map(|i| [seed, 0x7fc00001, 0x80000000, i as u32][i % 4].to_le_bytes())
                .collect();
            match producer.push(n % 5, &data, bytes as u32) {
                Ok(block) => {
                    assert_eq!(producer.push(n % 5, &data, bytes as u32), Ok(block));
                    blocks.push((block, data));
                }
                Err(PushError::Full(_)) => break,
                e => panic!("{e:?}"),
            }
        }
        assert!(blocks.len() >= 4);
        let sealed = producer.seal().unwrap();
        let prepared = stream.encode(&mut gpu, &mut e, &sealed).unwrap();
        if let Some(stale) = old {
            assert!(prepared.window(stale).is_err());
        }
        let (buffer, _, _) = prepared.window(blocks[0].0).unwrap();
        let group = gpu
            .bind_group(
                &gl,
                &[Entry {
                    binding: 0,
                    resource: Resource::Buffer {
                        buffer,
                        offset: 0,
                        size: 16,
                    },
                }],
            )
            .unwrap();
        for (block, data) in &blocks {
            let (buffer, offset, _) = prepared.window(*block).unwrap();
            let at = phase * capacity + offset as usize;
            e.copy_buffer(buffer, offset, &snapshots, at as u64, data.len() as u64)
                .unwrap();
            expected[at..at + data.len()].copy_from_slice(data);
            let pixel = expected_pixels.len() as u32;
            assert!(pixel < 256);
            let offsets = [offset as u32];
            e.render(
                &[Some(ColorAttachment {
                    view: &view,
                    clear: None,
                })],
                &[Draw {
                    pipeline: &pipeline,
                    groups: &[BoundGroup {
                        group: &group,
                        dynamic_offsets: &offsets,
                    }],
                    vertices: 0..3,
                    instances: 0..1,
                    indices: None,
                    viewport: None,
                    scissor: Some([pixel, 0, 1, 1]),
                    blend: t::Color::TRANSPARENT,
                    stencil_reference: 0,
                }],
            )
            .unwrap();
            expected_pixels.push(data[0]);
        }
        old = Some(blocks[0].0);
        drop(prepared);
        drop(sealed);
        producer.reset().unwrap();
    }
    e.copy_texture_to_buffer(
        &target,
        0,
        t::Origin3d::ZERO,
        t::Extent3d {
            width: 256,
            height: 1,
            depth_or_array_layers: 1,
        },
        &pixels,
        t::TexelCopyBufferLayout {
            offset: 0,
            bytes_per_row: Some(1024),
            rows_per_image: Some(1),
        },
    )
    .unwrap();
    drop(stream);
    drop(producer);
    let serial = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(serial, 1);
    assert_eq!(
        gpu.read_buffer(&snapshots, 0..snapshots.size(), DEADLINE)
            .unwrap(),
        expected
    );
    let actual = gpu.read_buffer(&pixels, 0..1024, DEADLINE).unwrap();
    for (i, &red) in expected_pixels.iter().enumerate() {
        assert_eq!(&actual[i * 4..i * 4 + 4], &[red, 0, 0, 255]);
    }
    assert!(actual[expected_pixels.len() * 4..].iter().all(|&v| v == 0));
    drop(gpu);
    finish(before);
}

#[test]
fn native_constant_seals_preserve_prior_windows_and_require_epoch_reset_after_encoder_change() {
    use xenos_gfx::constant_segments::{PatchSegment, StageConstants};
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut stream = gpu.constant_stream(2048).unwrap();
    let mut b = PatchSegment::new(2048, 256).unwrap();
    let old_data = [0x71; 256];
    let mut new_data = old_data;
    new_data[240..].fill(0x29);
    let mut e = gpu.encoder().unwrap();
    let out = gpu
        .create_buffer("seal snapshots", 544, U::MAP_READ | U::COPY_DST)
        .unwrap();
    let blocks = b
        .push_draw(&[
            StageConstants {
                stage: 0,
                data: &[0x13; 16],
                window: 16,
            },
            StageConstants {
                stage: 1,
                data: &old_data,
                window: 256,
            },
        ])
        .unwrap();
    {
        let s = b.seal().unwrap();
        let p = stream.encode(&mut gpu, &mut e, &s).unwrap();
        let (buffer, at, _) = p.window(blocks[1].unwrap()).unwrap();
        e.copy_buffer(buffer, at, &out, 0, 256).unwrap();
    }
    b.continue_after_seal().unwrap();
    assert_eq!(b.push(0, &[0x13; 16], 16).unwrap(), blocks[0].unwrap());
    let next = b.push(1, &new_data, 256).unwrap();
    {
        let s = b.seal().unwrap();
        let p = stream.encode(&mut gpu, &mut e, &s).unwrap();
        let (buffer, at, _) = p.window(next).unwrap();
        e.copy_buffer(buffer, at, &out, 256, 256).unwrap();
        let (buffer, at, _) = p.window(blocks[0].unwrap()).unwrap();
        e.copy_buffer(buffer, at, &out, 512, 16).unwrap();
    }
    b.continue_after_seal().unwrap();
    b.push(0, &[0x13; 16], 16).unwrap();
    assert_eq!(b.record_count(), 0);
    // A carry-only dispatch cannot silently migrate to a new encoder; it may
    // otherwise precede or outlive the encoder that actually produced its data.
    {
        let s = b.seal().unwrap();
        let mut other = gpu.encoder().unwrap();
        assert!(stream.encode(&mut gpu, &mut other, &s).is_err());
        assert_eq!(other.ordered_upload_bytes(), 0);
        let prepared = stream.encode(&mut gpu, &mut e, &s).unwrap();
        assert!(prepared.window(blocks[0].unwrap()).is_ok());
    }
    let serial = gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(serial, 1);
    let bytes = gpu.read_buffer(&out, 0..544, DEADLINE).unwrap();
    assert_eq!(&bytes[..256], &old_data);
    assert_eq!(&bytes[256..512], &new_data);
    assert_eq!(&bytes[512..528], &[0x13; 16]);
    assert_eq!(&bytes[528..], &[0; 16]);
    // Discard a real newly encoded patch, then reset and regenerate the SAME
    // CPU bytes in another encoder. The second attempt must not return a stale
    // cached offset or depend on the abandoned dispatch.
    b.reset().unwrap();
    let abandoned = b.push(0, &[0xe8; 16], 16).unwrap();
    {
        let s = b.seal().unwrap();
        let mut discard = gpu.encoder().unwrap();
        let p = stream.encode(&mut gpu, &mut discard, &s).unwrap();
        assert!(p.window(abandoned).is_ok());
    }
    b.reset().unwrap();
    let last = b.push(0, &[0xe8; 16], 16).unwrap();
    let mut e = gpu.encoder().unwrap();
    {
        let s = b.seal().unwrap();
        let p = stream.encode(&mut gpu, &mut e, &s).unwrap();
        assert!(p.window(abandoned).is_err());
        let (buffer, at, _) = p.window(last).unwrap();
        e.copy_buffer(buffer, at, &out, 528, 16).unwrap();
    }
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    assert_eq!(
        gpu.read_buffer(&out, 528..544, DEADLINE).unwrap(),
        [0xe8; 16]
    );
    drop(gpu);
    finish(before);
}

#[test]
fn continued_short_constant_blocks_reseed_high_lanes_and_stream_rejects_missing_dispatches() {
    use xenos_gfx::constant_segments::PatchSegment;
    let (_lock, before) = setup();
    let mut gpu = open();
    for (rewind, same, short) in [false, true].into_iter().flat_map(|rewind| {
        [false, true]
            .into_iter()
            .flat_map(move |same| [0, 16].map(|short| (rewind, same, short)))
    }) {
        let mut stream = gpu.constant_stream(16384).unwrap();
        let mut b = PatchSegment::new(16384, stream.alignment()).unwrap();
        let mut e = gpu.encoder().unwrap();
        let output = gpu
            .create_buffer(
                "continued high-lane oracle",
                4096,
                U::COPY_DST | U::MAP_READ,
            )
            .unwrap();
        let full = [0x37; 4096];
        b.push(0, &full, 4096).unwrap();
        {
            let s = b.seal().unwrap();
            stream.encode(&mut gpu, &mut e, &s).unwrap();
        }
        if rewind {
            b.reset().unwrap();
        } else {
            b.continue_after_seal().unwrap();
        }
        let value = if same { 0x37 } else { 0x18 };
        b.push(0, &vec![value; short], short.max(16) as u32)
            .unwrap();
        let mut wanted = full;
        wanted[..short.max(16)].fill(value);
        let block = b.push(0, &wanted, 4096).unwrap();
        {
            let s = b.seal().unwrap();
            let prepared = stream.encode(&mut gpu, &mut e, &s).unwrap();
            let (buffer, at, _) = prepared.window(block).unwrap();
            e.copy_buffer(buffer, at, &output, 0, 4096).unwrap();
        }
        gpu.submit(vec![e.finish().unwrap()]).unwrap();
        assert_eq!(
            gpu.read_buffer(&output, 0..4096, DEADLINE).unwrap(),
            wanted,
            "rewind={rewind} same={same} first length {short}"
        );
    }
    let mut stream = gpu.constant_stream(16384).unwrap();
    let mut x = PatchSegment::new(16384, stream.alignment()).unwrap();
    let mut y = PatchSegment::new(16384, stream.alignment()).unwrap();
    let mut e = gpu.encoder().unwrap();
    x.push(0, &[1; 16], 16).unwrap();
    {
        let s = x.seal().unwrap();
        stream.encode(&mut gpu, &mut e, &s).unwrap();
    }
    y.push(0, &[2; 16], 16).unwrap();
    {
        let s = y.seal().unwrap();
        stream.encode(&mut gpu, &mut e, &s).unwrap();
    }
    x.continue_after_seal().unwrap();
    x.push(0, &[1; 16], 16).unwrap();
    {
        let s = x.seal().unwrap();
        let bytes = e.ordered_upload_bytes();
        assert!(stream.encode(&mut gpu, &mut e, &s).is_err());
        assert_eq!(e.ordered_upload_bytes(), bytes);
    }
    drop(e);
    x.reset().unwrap();
    x.push(0, &[3; 16], 16).unwrap();
    {
        let s = x.seal().unwrap();
        let mut discarded = gpu.encoder().unwrap();
        stream.encode(&mut gpu, &mut discarded, &s).unwrap();
    }
    x.continue_after_seal().unwrap();
    x.push(0, &[3; 16], 16).unwrap();
    {
        let s = x.seal().unwrap();
        let mut recycled = gpu.encoder().unwrap();
        assert!(stream.encode(&mut gpu, &mut recycled, &s).is_err());
        assert_eq!(recycled.ordered_upload_bytes(), 0);
    }
    drop(gpu);
    let mut c = skyrim_native_gpu::Config::default();
    c.max_pending_upload_bytes = 64;
    let mut gpu = super::open_config(c);
    let mut stream = gpu.constant_stream(4096).unwrap();
    let mut b = PatchSegment::new(4096, stream.alignment()).unwrap();
    let mut e = gpu.encoder().unwrap();
    b.push(0, &[5; 16], 16).unwrap();
    {
        let s = b.seal().unwrap();
        assert!(stream.encode(&mut gpu, &mut e, &s).is_err());
        assert_eq!(e.ordered_upload_bytes(), 0);
    }
    b.continue_after_seal().unwrap();
    b.push(0, &[5; 16], 16).unwrap();
    assert!(stream.encode(&mut gpu, &mut e, &b.seal().unwrap()).is_err());
    assert_eq!(e.ordered_upload_bytes(), 0);
    drop(gpu);
    finish(before);
}

#[test]
fn interleaved_constant_recording_and_reversed_submissions_preserve_exact_snapshots() {
    use xenos_gfx::constant_segments::PatchSegment;
    let (_lock, before) = setup();
    let mut gpu = open();
    let mut stream = gpu.constant_stream(4096).unwrap();
    let mut x = PatchSegment::new(4096, stream.alignment()).unwrap();
    let mut y = PatchSegment::new(4096, stream.alignment()).unwrap();
    let a: Vec<u8> = (0..1024u32)
        .flat_map(|n| (n ^ 0x7fc00001).to_le_bytes())
        .collect();
    let b: Vec<u8> = (0..1024u32)
        .flat_map(|n| (n ^ 0x80000000).to_le_bytes())
        .collect();
    let out = gpu
        .create_buffer(
            "reverse constant submissions",
            8192,
            U::MAP_READ | U::COPY_DST,
        )
        .unwrap();
    let mut first = gpu.encoder().unwrap();
    let block_a = x.push(0, &a, 4096).unwrap();
    let (buffer_a, offset_a) = {
        let sealed = x.seal().unwrap();
        let prepared = stream.encode(&mut gpu, &mut first, &sealed).unwrap();
        let (buffer, offset, _) = prepared.window(block_a).unwrap();
        (buffer.clone(), offset)
    };
    let mut second = gpu.encoder().unwrap();
    let block_b = y.push(0, &b, 4096).unwrap();
    {
        let sealed = y.seal().unwrap();
        let prepared = stream.encode(&mut gpu, &mut second, &sealed).unwrap();
        let (buffer, offset, _) = prepared.window(block_b).unwrap();
        second
            .copy_buffer(buffer, offset, &out, 4096, 4096)
            .unwrap();
    }
    // Deliberately retain a lowlevel buffer handle for this adversarial test.
    // The future ordinary executor API must hide such escape/interleaving.
    first
        .copy_buffer(&buffer_a, offset_a, &out, 0, 4096)
        .unwrap();
    // Actual independent submissions in reverse recording order: initial
    // resource transitions must come from accepted submission state, not the
    // other unsubmitted encoder's speculative state.
    gpu.submit(vec![second.finish().unwrap()]).unwrap();
    gpu.submit(vec![first.finish().unwrap()]).unwrap();
    let actual = gpu.read_buffer(&out, 0..8192, DEADLINE).unwrap();
    assert_eq!(&actual[..4096], a);
    assert_eq!(&actual[4096..], b);
    drop(gpu);
    finish(before);
}
