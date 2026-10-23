//! Authored Xenos alpha-to-mask draws on a native GPU. No game shaders or data.
#![cfg(not(target_os = "emscripten"))]

#[path = "../../xenos-shader/tests/common/mod.rs"]
mod asm;
mod common;

use std::sync::{Arc, Mutex};

use asm::*;
use gpu_webgpu::*;
use xenos_render::executor::Executor;
use xenos_render::targets::{DrawTargets, RenderTargetCache, ResolveStatus, SharedMemoryTarget};
use xenos_render::{PhysicalMemory, ShaderStage};
use xenos_state::edram::RenderTargetSetup;
use xenos_state::registers as reg;
use xenos_state::resolve::ResolveInfo;

const SIZE: u32 = 8;
const VERTICES: u32 = 0x10000;

struct Target {
    view: TextureView,
    samples: u32,
    mask: u32,
}

impl RenderTargetCache for Target {
    fn targets(&mut self, _: &Device, _: &mut CommandEncoder, _: &RenderTargetSetup, _: u32) -> Option<DrawTargets> {
        Some(DrawTargets {
            colors: [Some((self.view.clone(), TextureFormat::RGBA8Unorm)), None, None, None],
            depth: None,
            width: SIZE,
            height: SIZE,
            sample_count: self.samples,
            sample_mask: self.mask,
            blendable: [true; 4],
            clears: Default::default(),
        })
    }
    fn resolve(
        &mut self,
        _: &Device,
        _: &ResolveInfo,
        _: &dyn PhysicalMemory,
        _: Option<&SharedMemoryTarget<'_>>,
        _: Option<&mut CommandEncoder>,
    ) -> ResolveStatus {
        ResolveStatus::Done { written: None, in_gpu_memory: false }
    }
    fn poll_resolve(&mut self, _: &Device, _: &dyn PhysicalMemory) -> ResolveStatus {
        ResolveStatus::Done { written: None, in_gpu_memory: false }
    }
    fn end_frame(&mut self) {}
    fn flush_uploads(&mut self, _: &Device) {}
}

fn f(value: f32) -> u32 {
    value.to_bits()
}

fn read_samples(device: &Device, view: &TextureView) -> Vec<f32> {
    let module = device.create_shader_module_wgsl(
        "authored sample readback",
        &format!(
            r#"
@group(0) @binding(0) var source: texture_multisampled_2d<f32>;
@group(0) @binding(1) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {{
    if (id.x >= {size}u || id.y >= {size}u) {{ return; }}
    for (var sample = 0u; sample < 4u; sample++) {{
        result[(id.y * {size}u + id.x) * 4u + sample] = textureLoad(source, vec2<i32>(id.xy), sample);
    }}
}}
"#,
            size = SIZE
        ),
    );
    let pipeline = device.create_compute_pipeline(&ComputePipelineDescriptor {
        label: "authored sample readback",
        layout: None,
        stage: ProgrammableStage::new(&module, "main"),
    });
    let bytes = u64::from(SIZE * SIZE * 4 * 16);
    let storage = device.create_buffer(&BufferDescriptor {
        label: "authored sample values",
        size: bytes,
        usage: BufferUsage::Storage | BufferUsage::CopySrc,
        mapped_at_creation: false,
    });
    let readback = device.create_buffer(&BufferDescriptor {
        label: "authored sample readback",
        size: bytes,
        usage: BufferUsage::MapRead | BufferUsage::CopyDst,
        mapped_at_creation: false,
    });
    let group = device.create_bind_group(
        "authored samples",
        &pipeline.get_bind_group_layout(0),
        &[
            BindGroupEntry { binding: 0, resource: BindingResource::TextureView(view) },
            BindGroupEntry {
                binding: 1,
                resource: BindingResource::Buffer { buffer: &storage, offset: 0, size: None },
            },
        ],
    );
    let mut encoder = device.create_command_encoder("authored samples");
    {
        let mut pass = encoder.begin_compute_pass("authored samples");
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&storage, 0, &readback, 0, bytes);
    device.queue().submit([encoder.finish("authored samples")]);
    device.block_on(readback.map_async(MapMode::Read, 0, None)).unwrap();
    let mut data = vec![0; bytes as usize];
    readback.read_mapped(0, &mut data).unwrap();
    data.chunks_exact(4).map(|v| f32::from_le_bytes(v.try_into().unwrap())).collect()
}

fn draw(
    device: &Device,
    samples: u32,
    alpha: f32,
    offsets: [u32; 4],
    bias: i32,
    enabled: bool,
    alpha_test: bool,
) -> (Vec<u8>, Option<Vec<f32>>) {
    let host_samples = if samples == 1 { 1 } else { 4 };
    let host_mask = if samples == 2 { 0b1001 } else { !0 };
    let texture = device.create_texture(&TextureDescriptor {
        label: "authored coverage samples",
        size: extent(SIZE, SIZE, 1),
        sample_count: host_samples,
        format: TextureFormat::RGBA8Unorm,
        usage: TextureUsage::RenderAttachment
            | TextureUsage::TextureBinding
            | if host_samples == 1 { TextureUsage::CopySrc } else { TextureUsage::None },
        ..Default::default()
    });
    let view = texture.create_view(None);
    let memory = Arc::new(common::Memory(Mutex::new(vec![0; 0x20000])));
    let points = [[-1.0f32, 1.0, 0.5], [1.0, 1.0, 0.5], [-1.0, -1.0, 0.5]];
    memory.write(VERTICES, &points.iter().flatten().flat_map(|v| v.to_be_bytes()).collect::<Vec<_>>());
    let mut executor =
        Executor::new(device, memory, Box::new(Target { view: view.clone(), samples: host_samples, mask: host_mask }));
    executor.write_registers(&[
        (reg::RB_MODECONTROL, 4),
        (reg::RB_SURFACE_INFO, SIZE | samples.trailing_zeros() << 16),
        (reg::RB_COLOR_INFO, ((bias as u32) & 63) << 20),
        (reg::RB_COLOR_MASK, 15),
        (reg::RB_BLENDCONTROL0, 0x10001),
        (reg::RB_DEPTHCONTROL, 0),
        (
            reg::RB_COLORCONTROL,
            if enabled { 1 << 4 } else { 0 }
                | if alpha_test { 1 << 3 | 4 } else { 7 }
                | offsets.iter().enumerate().map(|(i, v)| v << (24 + i * 2)).fold(0, |a, b| a | b),
        ),
        (reg::RB_ALPHA_REF, f(0.75)),
        (reg::PA_CL_VTE_CNTL, 0x3F | 1 << 10),
        (reg::PA_CL_VPORT_XSCALE, f(SIZE as f32 / 2.0)),
        (reg::PA_CL_VPORT_XOFFSET, f(SIZE as f32 / 2.0)),
        (reg::PA_CL_VPORT_YSCALE, f(-(SIZE as f32) / 2.0)),
        (reg::PA_CL_VPORT_YOFFSET, f(SIZE as f32 / 2.0)),
        (reg::PA_CL_VPORT_ZSCALE, f(1.0)),
        (reg::PA_CL_VPORT_ZOFFSET, 0),
        (reg::PA_SC_WINDOW_SCISSOR_TL, 1 << 31),
        (reg::PA_SC_WINDOW_SCISSOR_BR, SIZE | SIZE << 16),
        (reg::PA_SC_SCREEN_SCISSOR_TL, 0),
        (reg::PA_SC_SCREEN_SCISSOR_BR, 8192 | 8192 << 16),
        (reg::SQ_PROGRAM_CNTL, 1 | 1 << 8 | 1 << 31),
        (reg::SQ_VS_CONST, 255 << 12),
        (reg::SQ_PS_CONST, 256 | 255 << 12),
        (reg::VGT_MAX_VTX_INDX, 0xFFFFFF),
        (reg::SHADER_CONSTANT_FETCH_00_0, VERTICES | 3),
        (reg::SHADER_CONSTANT_FETCH_00_0 + 1, 2 | 36 << 2),
        (reg::SHADER_CONSTANT_000_X, f(1.0)),
        (reg::SHADER_CONSTANT_000_X + 1, 0),
        (reg::SHADER_CONSTANT_000_X + 2, 0),
        (reg::SHADER_CONSTANT_000_X + 3, f(alpha)),
        (reg::VGT_DRAW_INITIATOR, 8 | 2 << 6 | 3 << 16),
    ]);
    executor.load_shader(
        ShaderStage::Vertex,
        assemble(&[exece(vec![
            VFetch { dest: 1, dest_swizzle: "xyz1", format: 57, stride: 3, fetch_constant: 0, ..Default::default() }
                .into(),
            Alu::vector(2, 62, 15, &[r(1), r(1)]).export().into(),
            Alu::vector(2, 0, 15, &[c(0), c(0)]).export().into(),
        ])])
        .into(),
    );
    executor.load_shader(
        ShaderStage::Pixel,
        assemble(&[exece(vec![Alu::vector(2, 0, 15, &[r(0), r(0)]).export().into()])]).into(),
    );
    executor.draw();
    executor.flush();
    assert_eq!(executor.stats.draws, 1, "{:?}", executor.stats);
    let sample_values = (host_samples == 4).then(|| read_samples(device, &view));
    let resolved = if host_samples == 1 {
        texture
    } else {
        let resolved = device.create_texture(&TextureDescriptor {
            label: "authored coverage readback",
            size: extent(SIZE, SIZE, 1),
            format: TextureFormat::RGBA8Unorm,
            usage: TextureUsage::RenderAttachment | TextureUsage::CopySrc,
            ..Default::default()
        });
        let resolved_view = resolved.create_view(None);
        let mut encoder = device.create_command_encoder("authored resolve");
        {
            let mut attachment = RenderPassColorAttachment::load(&view);
            attachment.resolve_target = Some(&resolved_view);
            let _pass = encoder.begin_render_pass("authored resolve", &[Some(attachment)], None);
        }
        device.queue().submit([encoder.finish("authored resolve")]);
        resolved
    };
    let pixels = common::OneTarget::read(device, &resolved, SIZE);
    assert!(device.take_errors().is_empty());
    (pixels, sample_values)
}

// Independent scalar reference of Xenia's Xenos thresholds/sample remap. Host
// resolve averages four samples for guest2x, so only bits0/3 contribute here.
fn reference_mask(alpha: f32, samples: u32, offset: u32) -> u32 {
    let thresholds: &[(u32, f32)] = match samples {
        1 => &[(0, 1.0)],
        2 => &[(0, 0.5), (3, 1.0)],
        4 => &[(0, 0.75), (1, 0.25), (2, 0.5), (3, 1.0)],
        _ => panic!(),
    };
    thresholds.iter().fold(0, |mask, &(bit, threshold)| {
        mask | if alpha >= threshold - offset as f32 / (4 * samples) as f32 { 1 << bit } else { 0 }
    })
}

fn check(device: &Device, samples: u32, alpha: f32, offsets: [u32; 4], bias: i32, enabled: bool, test: bool) {
    let (data, sample_values) = draw(device, samples, alpha, offsets, bias, enabled, test);
    // Exclude the rectangle's D3D9 half-pixel boundary; only fully raster-covered
    // interior pixels isolate alpha coverage from geometric sample coverage.
    for y in 1..SIZE - 1 {
        for x in 1..SIZE - 1 {
            let mask = if test && alpha <= 0.75 {
                0
            } else if enabled {
                reference_mask(alpha, samples, offsets[((x & 1) * 2 + (y & 1)) as usize])
            } else if samples == 2 {
                0b1001
            } else {
                (1 << samples) - 1
            };
            let coverage = mask.count_ones() as f32 / if samples == 1 { 1.0 } else { 4.0 };
            let value = |v: f32| ((v * (bias as f32).exp2()).clamp(0.0, 1.0) * 255.0).round();
            let expected = [value(1.0) * coverage, 0.0, 0.0, value(alpha) * coverage];
            let at = ((y * SIZE + x) * 4) as usize;
            for i in 0..4 {
                assert!(
                    (data[at + i] as f32 - expected[i]).abs() <= 1.0,
                    "samples{samples} alpha{alpha} offsets{offsets:?} bias{bias} enabled{enabled} test{test} pixel({x},{y}) channel{i}: got{} expected{} mask{mask:x}",
                    data[at + i],
                    expected[i]
                );
            }
            if let Some(values) = &sample_values {
                for sample in 0..4 {
                    let present = mask & (1 << sample) != 0;
                    let expected = if present { [value(1.0), 0.0, 0.0, value(alpha)] } else { [0.0; 4] };
                    let at = (((y * SIZE + x) * 4 + sample) * 4) as usize;
                    for i in 0..4 {
                        assert!(
                            (values[at + i] * 255.0 - expected[i]).abs() <= 1.0,
                            "samples{samples} alpha{alpha} offsets{offsets:?} pixel({x},{y}) sample{sample} channel{i} mask{mask:x}"
                        );
                    }
                }
            }
        }
    }
}

#[test]
fn alpha_coverage_uses_unbiased_alpha_guest_offsets_and_sample_mapping() {
    let device = common::device();
    // Establish geometry/readback and disabled paths before the regression oracle.
    check(&device, 4, 0.25, [3; 4], -2, false, false);
    check(&device, 1, 0.25, [3; 4], 0, false, false);
    // Fully opaque shader alpha must stay covered despite negative exponent bias.
    check(&device, 4, 1.0, [2; 4], -2, true, false);
    for samples in [1, 2, 4] {
        for (alpha, offsets, bias) in [(0.5, [0; 4], 0), (0.875, [3, 1, 0, 2], 0), (0.25, [2; 4], 2), (1.0, [2; 4], -2)]
        {
            check(&device, samples, alpha, offsets, bias, true, false);
        }
    }
    // Unaffected disabled and alpha-test branches retain their actual pixels.
    check(&device, 4, 0.5, [3; 4], 0, true, true);
    check(&device, 4, 1.0, [3; 4], 0, true, true);
}

#[cfg(feature = "public-probes")]
#[test]
fn sdk_helper_uses_actual_translator_and_all_seventeen_cases() {
    let result = xenos_render::debug_alpha_coverage::run_public_alpha_coverage_probe().unwrap();
    assert_eq!(result.checks, 1023);
    assert_eq!(result.rendered_cases, 17);
    assert_ne!(result.backend, 0);
}
