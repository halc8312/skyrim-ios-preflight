//! Authored offscreen Xenos coverage conformance, opt-in public-probes only.
//! No guest image, layer, external bytes, global renderer settings changes or display.
//! GPU driver calls may block; callers must enforce a process deadline.
use std::sync::{Arc, Mutex};

use crate::executor::Executor;
use crate::targets::{DrawTargets, RenderTargetCache, ResolveStatus, SharedMemoryTarget};
use crate::{PhysicalMemory, ShaderStage};
use gpu_webgpu::*;
use xenos_state::edram::RenderTargetSetup;
use xenos_state::registers as reg;
use xenos_state::resolve::ResolveInfo;

const SIZE: u32 = 8;
const VERTICES: u32 = 0x10000;

// Hand-authored shader words: EXEC_END(fetch r1.xyz1, MAX oPos=r1,
// MAX o0=c0) and EXEC_END(MAX oC0=r0). These encode exactly the authored
// assembler fixture in tests/alpha_coverage_native.rs; no asset or test-source
// dependency is used by the compiled helper.
const VERTEX: [u32; 12] =
    [0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100, 0xC80F8000, 0, 0x22000000];
const PIXEL: [u32; 6] = [0x00001001, 0x00002200, 0, 0xC80F8000, 0, 0xE2000000];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AlphaCoverageProbe {
    pub backend: u32,
    pub checks: u64,
    pub rendered_cases: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum AlphaCoverageProbeError {
    Instance = 1,
    Adapter = 2,
    Device = 3,
    Readback = 4,
    Pixels = 5,
    Validation = 6,
    DeviceLost = 7,
    CorePolicy = 8,
    Draw = 9,
}
type Check<T> = std::result::Result<T, AlphaCoverageProbeError>;

struct Memory(Mutex<Vec<u8>>);
impl PhysicalMemory for Memory {
    fn read(&self, address: u32, out: &mut [u8]) {
        let memory = self.0.lock().unwrap();
        for (index, value) in out.iter_mut().enumerate() {
            *value = memory.get(address as usize + index).copied().unwrap_or(0);
        }
    }
    fn write(&self, address: u32, data: &[u8]) {
        let mut memory = self.0.lock().unwrap();
        for (index, value) in data.iter().enumerate() {
            if let Some(out) = memory.get_mut(address as usize + index) {
                *out = *value;
            }
        }
    }
}

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

fn read_buffer(device: &Device, readback: &Buffer, bytes: usize) -> Check<Vec<u8>> {
    device.block_on(readback.map_async(MapMode::Read, 0, None)).map_err(|_| AlphaCoverageProbeError::Readback)?;
    let mut out = vec![0; bytes];
    readback.read_mapped(0, &mut out).map_err(|_| AlphaCoverageProbeError::Readback)?;
    readback.unmap();
    Ok(out)
}

fn read_samples(device: &Device, view: &TextureView) -> Check<Vec<f32>> {
    let module = device.create_shader_module_wgsl(
        "authored coverage sample readback",
        r#"
@group(0) @binding(0) var source: texture_multisampled_2d<f32>;
@group(0) @binding(1) var<storage, read_write> result: array<vec4<f32>>;
@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x >= 8u || id.y >= 8u) { return; }
    for (var sample = 0u; sample < 4u; sample++) {
        result[(id.y * 8u + id.x) * 4u + sample] = textureLoad(source, vec2<i32>(id.xy), sample);
    }
}
"#,
    );
    let pipeline = device.create_compute_pipeline(&ComputePipelineDescriptor {
        label: "authored coverage sample readback",
        layout: None,
        stage: ProgrammableStage::new(&module, "main"),
    });
    let bytes = u64::from(SIZE * SIZE * 4 * 16);
    let storage = device.create_buffer(&BufferDescriptor {
        label: "authored coverage samples",
        size: bytes,
        usage: BufferUsage::Storage | BufferUsage::CopySrc,
        mapped_at_creation: false,
    });
    let readback = device.create_buffer(&BufferDescriptor {
        label: "authored coverage sample readback",
        size: bytes,
        usage: BufferUsage::MapRead | BufferUsage::CopyDst,
        mapped_at_creation: false,
    });
    let group = device.create_bind_group(
        "authored coverage samples",
        &pipeline.get_bind_group_layout(0),
        &[
            BindGroupEntry { binding: 0, resource: BindingResource::TextureView(view) },
            BindGroupEntry {
                binding: 1,
                resource: BindingResource::Buffer { buffer: &storage, offset: 0, size: None },
            },
        ],
    );
    let mut encoder = device.create_command_encoder("authored coverage samples");
    {
        let mut pass = encoder.begin_compute_pass("authored coverage samples");
        pass.set_pipeline(&pipeline);
        pass.set_bind_group(0, &group, &[]);
        pass.dispatch_workgroups(1, 1, 1);
    }
    encoder.copy_buffer_to_buffer(&storage, 0, &readback, 0, bytes);
    device.queue().submit([encoder.finish("authored coverage samples")]);
    Ok(read_buffer(device, &readback, bytes as usize)?
        .chunks_exact(4)
        .map(|v| f32::from_le_bytes(v.try_into().unwrap()))
        .collect())
}

fn read_rgba(device: &Device, texture: &Texture) -> Check<Vec<u8>> {
    let readback = device.create_buffer(&BufferDescriptor {
        label: "authored coverage resolved readback",
        size: 256 * u64::from(SIZE),
        usage: BufferUsage::MapRead | BufferUsage::CopyDst,
        mapped_at_creation: false,
    });
    let mut encoder = device.create_command_encoder("authored coverage resolved readback");
    encoder.copy_texture_to_buffer(
        &TexelCopyTextureInfo::whole(texture),
        &TexelCopyBufferInfo { buffer: &readback, offset: 0, bytes_per_row: 256, rows_per_image: None },
        extent(SIZE, SIZE, 1),
    );
    device.queue().submit([encoder.finish("authored coverage resolved readback")]);
    Ok(read_buffer(device, &readback, 256 * SIZE as usize)?
        .chunks(256)
        .flat_map(|row| row[..SIZE as usize * 4].iter().copied())
        .collect())
}

fn draw(
    device: &Device,
    samples: u32,
    alpha: f32,
    offsets: [u32; 4],
    bias: i32,
    enabled: bool,
    alpha_test: bool,
) -> Check<(Vec<u8>, Option<Vec<f32>>)> {
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
    let memory = Arc::new(Memory(Mutex::new(vec![0; 0x20000])));
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
        (reg::RB_ALPHA_REF, 0.75f32.to_bits()),
        (reg::PA_CL_VTE_CNTL, 0x3F | 1 << 10),
        (reg::PA_CL_VPORT_XSCALE, (SIZE as f32 / 2.0).to_bits()),
        (reg::PA_CL_VPORT_XOFFSET, (SIZE as f32 / 2.0).to_bits()),
        (reg::PA_CL_VPORT_YSCALE, (-(SIZE as f32) / 2.0).to_bits()),
        (reg::PA_CL_VPORT_YOFFSET, (SIZE as f32 / 2.0).to_bits()),
        (reg::PA_CL_VPORT_ZSCALE, 1.0f32.to_bits()),
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
        (reg::SHADER_CONSTANT_000_X, 1.0f32.to_bits()),
        (reg::SHADER_CONSTANT_000_X + 1, 0),
        (reg::SHADER_CONSTANT_000_X + 2, 0),
        (reg::SHADER_CONSTANT_000_X + 3, alpha.to_bits()),
        (reg::VGT_DRAW_INITIATOR, 8 | 2 << 6 | 3 << 16),
    ]);
    executor.load_shader(ShaderStage::Vertex, VERTEX.as_slice().into());
    executor.load_shader(ShaderStage::Pixel, PIXEL.as_slice().into());
    executor.draw();
    executor.flush();
    if executor.stats.draws != 1 || executor.stats.skipped_draws != 0 {
        return Err(AlphaCoverageProbeError::Draw);
    }
    let samples = if host_samples == 4 { Some(read_samples(device, &view)?) } else { None };
    let resolved = if host_samples == 1 {
        texture
    } else {
        let resolved = device.create_texture(&TextureDescriptor {
            label: "authored coverage resolved target",
            size: extent(SIZE, SIZE, 1),
            format: TextureFormat::RGBA8Unorm,
            usage: TextureUsage::RenderAttachment | TextureUsage::CopySrc,
            ..Default::default()
        });
        let resolved_view = resolved.create_view(None);
        let mut encoder = device.create_command_encoder("authored coverage resolve");
        {
            let mut color = RenderPassColorAttachment::load(&view);
            color.resolve_target = Some(&resolved_view);
            let _pass = encoder.begin_render_pass("authored coverage resolve", &[Some(color)], None);
        }
        device.queue().submit([encoder.finish("authored coverage resolve")]);
        resolved
    };
    Ok((read_rgba(device, &resolved)?, samples))
}

// Independent scalar thresholds and guest2x -> host4x bit mapping. This is
// intentionally not the implementation in the generated WGSL helper.
fn reference_mask(alpha: f32, samples: u32, offset: u32) -> u32 {
    let thresholds: &[(u32, f32)] = match samples {
        1 => &[(0, 1.0)],
        2 => &[(0, 0.5), (3, 1.0)],
        4 => &[(0, 0.75), (1, 0.25), (2, 0.5), (3, 1.0)],
        _ => unreachable!(),
    };
    thresholds.iter().fold(0, |mask, &(bit, threshold)| {
        mask | if alpha >= threshold - offset as f32 / (4 * samples) as f32 { 1 << bit } else { 0 }
    })
}

fn check(
    device: &Device,
    samples: u32,
    alpha: f32,
    offsets: [u32; 4],
    bias: i32,
    enabled: bool,
    alpha_test: bool,
) -> Check<()> {
    let (data, sample_values) = draw(device, samples, alpha, offsets, bias, enabled, alpha_test)?;
    if data.len() != (SIZE * SIZE * 4) as usize
        || sample_values.as_ref().is_some_and(|v| v.len() != (SIZE * SIZE * 4 * 4) as usize)
    {
        return Err(AlphaCoverageProbeError::Readback);
    }
    // D3D9 half-pixel boundary is excluded: fully raster-covered interior
    // pixels isolate alpha-to-mask from geometric edge sample coverage.
    for y in 1..SIZE - 1 {
        for x in 1..SIZE - 1 {
            let mask = if alpha_test && alpha <= 0.75 {
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
            if (0..4).any(|i| (data[at + i] as f32 - expected[i]).abs() > 1.0) {
                return Err(AlphaCoverageProbeError::Pixels);
            }
            if let Some(values) = &sample_values {
                for sample in 0..4 {
                    let expected =
                        if mask & (1 << sample) != 0 { [value(1.0), 0.0, 0.0, value(alpha)] } else { [0.0; 4] };
                    let at = (((y * SIZE + x) * 4 + sample) * 4) as usize;
                    if (0..4).any(|i| !values[at + i].is_finite() || (values[at + i] * 255.0 - expected[i]).abs() > 1.0)
                    {
                        return Err(AlphaCoverageProbeError::Pixels);
                    }
                }
            }
        }
    }
    Ok(())
}

/// Seventeen real Executor draws through the production translator/pipeline,
/// then individual multisample and resolved RGBA checks. No disabled/skipped
/// success path. Results expose numeric categories only, never pixels.
pub fn run_public_alpha_coverage_probe() -> Check<AlphaCoverageProbe> {
    #[cfg(target_os = "linux")]
    unsafe {
        unsafe extern "C" {
            fn prctl(option: i32, ...) -> i32;
        }
        if prctl(4, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long) != 0
            || prctl(3, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long, 0 as std::ffi::c_long) != 0
        {
            return Err(AlphaCoverageProbeError::CorePolicy);
        }
    }
    let instance = Instance::new().map_err(|_| AlphaCoverageProbeError::Instance)?;
    let adapter = instance
        .block_on(instance.request_adapter(&RequestAdapterOptions::default()))
        .map_err(|_| AlphaCoverageProbeError::Adapter)?;
    let backend = adapter.info().backend.0;
    let features = crate::wanted_features(&adapter.features());
    let device = instance
        .block_on(adapter.request_device(&DeviceDescriptor {
            label: "public alpha coverage",
            required_features: &features,
            required_limits: Some(adapter.limits()),
        }))
        .map_err(|_| AlphaCoverageProbeError::Device)?;
    device.push_error_scope(ErrorFilter::Validation);
    check(&device, 4, 0.25, [3; 4], -2, false, false)?;
    check(&device, 1, 0.25, [3; 4], 0, false, false)?;
    check(&device, 4, 1.0, [2; 4], -2, true, false)?;
    for samples in [1, 2, 4] {
        for (alpha, offsets, bias) in [(0.5, [0; 4], 0), (0.875, [3, 1, 0, 2], 0), (0.25, [2; 4], 2), (1.0, [2; 4], -2)]
        {
            check(&device, samples, alpha, offsets, bias, true, false)?;
        }
    }
    check(&device, 4, 0.5, [3; 4], 0, true, true)?;
    check(&device, 4, 1.0, [3; 4], 0, true, true)?;
    if device.block_on(device.pop_error_scope()).map_err(|_| AlphaCoverageProbeError::Validation)?.is_some()
        || !device.take_errors().is_empty()
    {
        return Err(AlphaCoverageProbeError::Validation);
    }
    if device.lost_reason().is_some() {
        return Err(AlphaCoverageProbeError::DeviceLost);
    }
    // disabled/raw-alpha/1x-2x-4x/quad-offsets/negative-positive-bias/alpha-test/
    // per-sample/resolved/validation/device-health, respectively.
    Ok(AlphaCoverageProbe { backend, checks: 1023, rendered_cases: 17 })
}
