use super::{DEADLINE, U, finish, open, setup};
use skyrim_native_gpu::{
    Gpu,
    backend::Platform,
    bindings::{Entry, Resource},
    compute::{ComputeDispatch, SharedComputePipeline},
    render::BoundGroup,
    types,
};
use std::num::NonZeroU64;

fn slot(binding: u32, ty: types::BufferBindingType, dynamic: bool) -> types::BindGroupLayoutEntry {
    types::BindGroupLayoutEntry {
        binding,
        visibility: types::ShaderStages::COMPUTE,
        ty: types::BindingType::Buffer {
            ty,
            has_dynamic_offset: dynamic,
            min_binding_size: NonZeroU64::new(4),
        },
        count: None,
    }
}
fn add(
    gpu: &Gpu<Platform>,
) -> (
    skyrim_native_gpu::bindings::GroupLayout<Platform>,
    SharedComputePipeline<Platform>,
) {
    let gl = gpu
        .group_layout(&[
            slot(0, types::BufferBindingType::Uniform, true),
            slot(
                1,
                types::BufferBindingType::Storage { read_only: false },
                false,
            ),
        ])
        .unwrap();
    let pl = gpu.pipeline_layout(&[gl.clone()]).unwrap();
    let shader=gpu.shader("@group(0) @binding(0) var<uniform> p:u32; @group(0) @binding(1) var<storage,read_write> a:array<u32>; @compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id:vec3u){a[id.x]=a[id.x]+p;}").unwrap();
    let p = gpu.shared_compute_pipeline(&pl, &shader, "main").unwrap();
    (gl, p)
}
fn entry(
    binding: u32,
    buffer: &skyrim_native_gpu::Buffer<Platform>,
    size: u64,
) -> Entry<'_, Platform> {
    Entry {
        binding,
        resource: Resource::Buffer {
            buffer,
            offset: 0,
            size,
        },
    }
}
fn words(bytes: Vec<u8>) -> Vec<u32> {
    bytes
        .chunks_exact(4)
        .map(|b| u32::from_le_bytes(b.try_into().unwrap()))
        .collect()
}

#[test]
fn compute_batch_preflights_all_and_keeps_order_hazards_offsets_and_lifetimes() {
    let (_lock, before) = setup();
    let mut gpu = open();
    let (gl, p) = add(&gpu);
    let uniforms = gpu
        .create_buffer("batch uniform", 768, U::UNIFORM | U::COPY_DST)
        .unwrap();
    for (off, n) in [(0, 3u32), (256, 11), (512, 5)] {
        gpu.write_buffer(&uniforms, off, &n.to_le_bytes()).unwrap();
    }
    let a = gpu
        .create_buffer(
            "batch A",
            64,
            U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::COPY_SRC | U::COPY_DST,
        )
        .unwrap();
    let b = gpu
        .create_buffer(
            "batch B",
            64,
            U::STORAGE_READ_WRITE | U::STORAGE_READ_ONLY | U::COPY_SRC,
        )
        .unwrap();
    gpu.write_buffer(
        &a,
        0,
        &(0..16u32).flat_map(u32::to_le_bytes).collect::<Vec<_>>(),
    )
    .unwrap();
    let ga = gpu
        .bind_group(&gl, &[entry(0, &uniforms, 4), entry(1, &a, 64)])
        .unwrap();
    let gb = gpu
        .bind_group(&gl, &[entry(0, &uniforms, 4), entry(1, &b, 64)])
        .unwrap();
    let out = gpu
        .create_buffer("batch snapshots", 256, U::COPY_DST | U::MAP_READ)
        .unwrap();
    let read_gl = gpu
        .group_layout(&[
            slot(
                0,
                types::BufferBindingType::Storage { read_only: true },
                false,
            ),
            slot(
                1,
                types::BufferBindingType::Storage { read_only: false },
                false,
            ),
        ])
        .unwrap();
    let read_pl = gpu.pipeline_layout(&[read_gl.clone()]).unwrap();
    let shader=gpu.shader("@group(0) @binding(0) var<storage,read> a:array<u32>; @group(0) @binding(1) var<storage,read_write> b:array<u32>; @compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id:vec3u){b[id.x]=a[id.x]*2u;}").unwrap();
    let read_p = gpu
        .shared_compute_pipeline(&read_pl, &shader, "main")
        .unwrap();
    let gab = gpu
        .bind_group(&read_gl, &[entry(0, &a, 64), entry(1, &b, 64)])
        .unwrap();
    let gba = gpu
        .bind_group(&read_gl, &[entry(0, &b, 64), entry(1, &a, 64)])
        .unwrap();
    let a3 = [BoundGroup {
        group: &ga,
        dynamic_offsets: &[0],
    }];
    let a11 = [BoundGroup {
        group: &ga,
        dynamic_offsets: &[256],
    }];
    let b5 = [BoundGroup {
        group: &gb,
        dynamic_offsets: &[512],
    }];
    let ab = [BoundGroup {
        group: &gab,
        dynamic_offsets: &[],
    }];
    let ba = [BoundGroup {
        group: &gba,
        dynamic_offsets: &[],
    }];
    let bad = [BoundGroup {
        group: &ga,
        dynamic_offsets: &[1],
    }];
    let mut e = gpu.encoder().unwrap();
    assert!(
        e.dispatch_shared_batch(&[
            ComputeDispatch {
                pipeline: &p,
                groups: &a3,
                workgroups: [16, 1, 1]
            },
            ComputeDispatch {
                pipeline: &p,
                groups: &bad,
                workgroups: [0, 1, 1]
            },
        ])
        .is_err()
    );
    let other = open();
    let (_foreign_gl, foreign_p) = add(&other);
    assert!(
        e.dispatch_shared_batch(&[
            ComputeDispatch {
                pipeline: &p,
                groups: &a3,
                workgroups: [16, 1, 1]
            },
            ComputeDispatch {
                pipeline: &foreign_p,
                groups: &a3,
                workgroups: [16, 1, 1]
            },
        ])
        .is_err()
    );
    assert!(
        e.dispatch_shared_batch(&[
            ComputeDispatch {
                pipeline: &p,
                groups: &a3,
                workgroups: [16, 1, 1]
            },
            ComputeDispatch {
                pipeline: &p,
                groups: &a3,
                workgroups: [65536, 1, 1]
            },
        ])
        .is_err()
    );
    e.copy_buffer(&a, 0, &out, 0, 64).unwrap();
    let s = e
        .dispatch_shared_batch(&[
            ComputeDispatch {
                pipeline: &p,
                groups: &a3,
                workgroups: [16, 1, 1],
            },
            ComputeDispatch {
                pipeline: &p,
                groups: &a3,
                workgroups: [16, 1, 1],
            },
            ComputeDispatch {
                pipeline: &p,
                groups: &a3,
                workgroups: [16, 1, 1],
            },
            ComputeDispatch {
                pipeline: &p,
                groups: &a11,
                workgroups: [0, 1, 1],
            },
            ComputeDispatch {
                pipeline: &p,
                groups: &a11,
                workgroups: [16, 1, 1],
            },
            ComputeDispatch {
                pipeline: &read_p,
                groups: &ab,
                workgroups: [16, 1, 1],
            },
            ComputeDispatch {
                pipeline: &p,
                groups: &b5,
                workgroups: [16, 1, 1],
            },
            ComputeDispatch {
                pipeline: &read_p,
                groups: &ba,
                workgroups: [16, 1, 1],
            },
        ])
        .unwrap();
    assert_eq!(
        (
            s.dispatches,
            s.skipped_empty,
            s.passes,
            s.pipeline_binds,
            s.group_binds
        ),
        (7, 1, 1, 4, 5)
    );
    assert_eq!(e.dispatch_shared_batch(&[]).unwrap().passes, 0);
    assert_eq!(
        e.dispatch_shared_batch(&[ComputeDispatch {
            pipeline: &p,
            groups: &a3,
            workgroups: [16, 0, 1]
        }])
        .unwrap()
        .passes,
        0
    );
    e.copy_buffer(&a, 0, &out, 64, 64).unwrap();
    e.copy_buffer(&b, 0, &out, 128, 64).unwrap();
    // A following separate pass must inherit the same hazard history.
    e.dispatch_shared(&p, &a3, [16, 1, 1]).unwrap();
    e.copy_buffer(&a, 0, &out, 192, 64).unwrap();
    drop(ga);
    drop(gb);
    drop(gab);
    drop(gba);
    drop(a);
    drop(b);
    drop(p);
    drop(read_p);
    drop(uniforms);
    gpu.submit(vec![e.finish().unwrap()]).unwrap();
    let got = words(gpu.read_buffer(&out, 0..256, DEADLINE).unwrap());
    assert_eq!(&got[0..16], &(0..16u32).collect::<Vec<_>>());
    assert_eq!(
        &got[16..32],
        &(0..16u32).map(|x| 4 * x + 90).collect::<Vec<_>>()
    );
    assert_eq!(
        &got[32..48],
        &(0..16u32).map(|x| 2 * x + 45).collect::<Vec<_>>()
    );
    assert_eq!(
        &got[48..64],
        &(0..16u32).map(|x| 4 * x + 93).collect::<Vec<_>>()
    );
    super::retire_all(&mut gpu);
    assert_eq!(gpu.native_counters().compute_pipelines.read(), 0);
    drop(foreign_p);
    drop(other);
    drop(gpu);
    finish(before);
}
