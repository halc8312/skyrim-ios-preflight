//! Authored Xenos shader + immutable pass-plan conformance. No game, globals or
//! surface. Callers retain an independent process deadline for GPU driver calls.
use crate::{
    frame_plan as fp,
    prepared::{DrawState, prepare_geometry},
};
use gpu_webgpu::*;
use std::result::Result;
use std::{collections::BTreeSet, sync::Arc};
use xenos_shader::{Shader, ShaderStage as GuestStage, Translation, TranslationOptions, bindings, msl};
use xenos_state::{
    bitfields::VgtDrawInitiator,
    primitive::{DrawDescription, HostIndices, HostTopology},
    registers as reg,
    xenos::{IndexFormat as GuestIndexFormat, MajorMode, PrimitiveType, SourceSelect},
};

pub const SIZE: u32 = 32;
// Same public, hand-authored Xenos operations as debug_alpha_coverage. No game
// microcode is embedded: fetch float3 position, export position/color, copy color.
const VERTEX: [u32; 12] =
    [0x00013001, 0x00002200, 0, 0x00081000, 0x00390A88, 3, 0xC80F803E, 0, 0xE2010100, 0xC80F8000, 0, 0x22000000];
const PIXEL: [u32; 6] = [0x00001001, 0x00002200, 0, 0xC80F8000, 0, 0xE2000000];

pub fn shaders() -> Result<[Translation; 2], String> {
    let options =
        TranslationOptions { vs_output_interpolators: 1, ps_interpolator_count: Some(1), ..Default::default() };
    Ok([
        Shader::new(GuestStage::Vertex, &VERTEX).map_err(|_| "vertex decode")?.translate(&options),
        Shader::new(GuestStage::Pixel, &PIXEL).map_err(|_| "pixel decode")?.translate(&options),
    ])
}

pub struct Fixture {
    pub id: u32,
    pub frame: fp::FramePlan,
    pub expected: Vec<u8>,
    pub tolerance: u8,
}
struct Builder {
    resources: Vec<fp::BufferVersion>,
    bindings: Vec<fp::Binding>,
}
impl Builder {
    fn resource(&mut self, id: u32, generation: u64, bytes: Vec<u8>) -> fp::BufferSlice {
        let version = fp::Version { id, generation };
        let len = bytes.len() as u32;
        if let Some(r) = self.resources.iter().find(|r| r.version == version) {
            assert_eq!(&*r.bytes, &bytes);
        } else {
            self.resources.push(fp::BufferVersion { version, bytes: Arc::from(bytes) });
        }
        fp::BufferSlice { version, offset: 0, bytes: len }
    }
    fn bind(&mut self, group: u32, binding: u32, slice: fp::BufferSlice) {
        if let Some(b) = self.bindings.iter_mut().find(|b| b.group == group && b.binding == binding) {
            b.slice = slice;
        } else {
            self.bindings.push(fp::Binding { group, binding, slice });
        }
    }
    fn new() -> Self {
        let mut b = Self { resources: vec![], bindings: vec![] };
        for (binding, len) in [(0, 4096), (1, 160), (2, 768), (3, 112), (4, 4096)] {
            let bytes = match binding {
                2 => {
                    let mut fetch = vec![0u8; len];
                    let words = [0x10000u32 | 3, 2 | 9 * 4 << 2];
                    for (i, w) in words.into_iter().enumerate() {
                        fetch[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
                    }
                    fetch
                }
                3 => bindings::SystemConstants::default().as_bytes().to_vec(),
                _ => vec![0; len],
            };
            let slice = b.resource(10 + binding, 1, bytes);
            b.bind(0, binding, slice);
        }
        for binding in 0..4 {
            let slice = b.resource(20 + binding, 1, vec![0; 4]);
            b.bind(1, binding, slice);
        }
        let mut table = vec![0; bindings::SHARED_MEMORY_BLOCKS as usize * 4];
        table[4..8].copy_from_slice(&1u32.to_le_bytes());
        let slice = b.resource(24, 1, table);
        b.bind(1, 4, slice);
        b
    }
    fn draw(
        &mut self,
        color: [f32; 4],
        z: f32,
        scissor: [u32; 4],
        quad: bool,
        blend: bool,
        depth: bool,
        version: u64,
    ) -> fp::Draw {
        let mut c = vec![0; 4096];
        for (i, v) in color.into_iter().enumerate() {
            c[i * 4..i * 4 + 4].copy_from_slice(&v.to_le_bytes());
        }
        let slice = self.resource(10, version + 1, c);
        self.bind(0, 0, slice);
        let vertices: Vec<[f32; 3]> = if quad {
            vec![[-1., -1., z], [1., -1., z], [1., 1., z], [-1., 1., z]]
        } else {
            vec![[-1., -1., z], [3., -1., z], [-1., 3., z]]
        };
        let mut bytes = vec![0; 65536];
        let raw: Vec<_> = vertices.iter().flatten().flat_map(|v| v.to_be_bytes()).collect();
        bytes[..raw.len()].copy_from_slice(&raw);
        let slice = self.resource(20, version + 1, bytes);
        self.bind(1, 0, slice);
        let mut regs = vec![0u32; 0x5003];
        regs[reg::VGT_DRAW_INITIATOR as usize] = VgtDrawInitiator {
            prim_type: if quad { PrimitiveType::QuadList } else { PrimitiveType::TriangleList },
            source_select: SourceSelect::AutoIndex,
            major_mode: MajorMode::Implicit,
            index_size: GuestIndexFormat::Int16,
            not_eop: false,
            num_indices: vertices.len() as u32,
        }
        .encode();
        regs[reg::VGT_MAX_VTX_INDX as usize] = 0xff_ffff;
        let plan = prepare_geometry(&DrawDescription::from_registers(&regs), &vec![0u8; 0]).unwrap();
        assert!(plan.direct_indices.is_none());
        let indices = match &plan.draw.indices {
            HostIndices::None => None,
            HostIndices::U16(v) => {
                let bytes = v.iter().flat_map(|v| v.to_le_bytes()).collect();
                Some(fp::IndexSlice { slice: self.resource(30, version, bytes), stride: 2 })
            }
            HostIndices::U32(v) => {
                let bytes = v.iter().flat_map(|v| v.to_le_bytes()).collect();
                Some(fp::IndexSlice { slice: self.resource(30, version, bytes), stride: 4 })
            }
        };
        let topology = match plan.draw.topology {
            HostTopology::TriangleList => fp::Topology::Triangles,
            HostTopology::TriangleStrip => fp::Topology::TriangleStrip,
            _ => unreachable!(),
        };
        fp::Draw {
            state: DrawState::new([0., 0., SIZE as f32, SIZE as f32, 0., 1.], scissor, [0.; 4], 0),
            topology,
            count: plan.draw.vertex_count,
            indices,
            bindings: self.bindings.clone(),
            blend,
            depth_less_write: depth,
        }
    }
}

pub fn fixtures() -> Vec<Fixture> {
    const FULL: [u32; 4] = [0, 0, SIZE, SIZE];
    const BLACK: [f64; 4] = [0., 0., 0., 1.];
    let red = [1., 0., 0., 1.];
    let green = [0., 1., 0., 1.];
    let blue = [0., 0., 1., 1.];
    (1..=9)
        .map(|id| {
            let mut b = Builder::new();
            let mut passes = vec![];
            let mut expected = vec![0; SIZE as usize * SIZE as usize * 4];
            let mut tolerance = 0;
            let initial = |draws| fp::Pass { clear_color: Some(BLACK), clear_depth: Some(1.), draws };
            match id {
                1 => passes.push(initial(vec![b.draw(red, 0.5, FULL, false, false, false, 1)])),
                2 => {
                    let a = b.draw(red, 0.5, FULL, false, false, false, 1);
                    let c = b.draw(blue, 0.5, [16, 0, 16, SIZE], false, false, false, 2);
                    passes.push(initial(vec![a, c]));
                }
                3 => {
                    let a = b.draw(red, 0.5, FULL, false, false, false, 1);
                    let c = b.draw(green, 0.5, [0, 0, 8, SIZE], false, false, false, 2);
                    passes.push(initial(vec![a, c]));
                    let d = b.draw(blue, 0.5, [16, 0, 16, SIZE], false, false, false, 3);
                    passes.push(fp::Pass { clear_color: None, clear_depth: None, draws: vec![d] });
                }
                4 => {
                    let a = b.draw(red, 0.5, FULL, false, false, true, 1);
                    let c = b.draw(blue, 0.75, FULL, false, false, true, 2);
                    let d = b.draw(green, 0.25, [0, 0, 16, SIZE], false, false, true, 3);
                    passes.push(initial(vec![a, c, d]));
                }
                5 => {
                    let a = b.draw(red, 0.5, FULL, false, false, false, 1);
                    let c = b.draw([0., 0., 1., 0.5], 0.5, FULL, false, true, false, 2);
                    passes.push(initial(vec![a, c]));
                    tolerance = 1;
                }
                6 => passes.push(initial(vec![b.draw(green, 0.5, FULL, true, false, false, 1)])),
                7 => {
                    let mut d = b.draw([1., 1., 0., 1.], 0.5, [10, 6, 12, 20], false, false, false, 1);
                    d.state.viewport_bits = [8f32, 4., 16., 24., 0., 1.].map(f32::to_bits);
                    passes.push(initial(vec![d]));
                }
                8 => {
                    let mut d = b.draw([0., 1., 1., 1.], 0.5, FULL, false, false, false, 1);
                    let original =
                        b.resources.iter().find(|r| r.version == fp::Version { id: 20, generation: 2 }).unwrap();
                    let mut shifted = vec![0; 256];
                    shifted.extend_from_slice(&original.bytes);
                    let mut chunk2 = b.resource(22, 2, shifted);
                    chunk2.offset = 256;
                    chunk2.bytes -= 256;
                    let empty = b.resource(20, 1, vec![0; 4]);
                    let mut table = vec![0; bindings::SHARED_MEMORY_BLOCKS as usize * 4];
                    // Physical block 1 lives in chunk 2, at its block 1. This
                    // catches slot and runtime-byte-length declaration ordering.
                    table[4..8].copy_from_slice(&1025u32.to_le_bytes());
                    let table = b.resource(24, 2, table);
                    for binding in &mut d.bindings {
                        binding.slice = match (binding.group, binding.binding) {
                            (1, 0) => empty,
                            (1, 2) => chunk2,
                            (1, 4) => table,
                            _ => binding.slice,
                        };
                    }
                    passes.push(initial(vec![d]));
                }
                9 => {
                    let mut d = b.draw([1., 0., 1., 1.], 0.5, FULL, false, false, false, 1);
                    let vertices = [
                        [-1f32, -1., 0.5],
                        [-1., 1., 0.5],
                        [-0.375, -1., 0.5],
                        [-0.375, 1., 0.5],
                        [0.375, 0., 0.5],
                        [0.375, 1., 0.5],
                        [1., 0., 0.5],
                        [1., 1., 0.5],
                    ];
                    let mut bytes = vec![0; 65536];
                    let raw: Vec<_> = vertices.iter().flatten().flat_map(|v| v.to_be_bytes()).collect();
                    bytes[..raw.len()].copy_from_slice(&raw);
                    let vertex = b.resource(20, 3, bytes);
                    let color =
                        b.resources.iter().find(|r| r.version == fp::Version { id: 10, generation: 2 }).unwrap();
                    let mut shifted = vec![0; 256];
                    shifted.extend_from_slice(&color.bytes);
                    let mut color = b.resource(10, 3, shifted);
                    color.offset = 256;
                    color.bytes -= 256;
                    let mut indices = vec![0; 4];
                    indices.extend([0u16, 1, 2, 3, 0xffff, 4, 5, 6, 7].into_iter().flat_map(u16::to_le_bytes));
                    indices.extend([0; 2]);
                    let mut index = b.resource(30, 1, indices);
                    index.offset = 4;
                    index.bytes = 18;
                    d.indices = Some(fp::IndexSlice { slice: index, stride: 2 });
                    d.topology = fp::Topology::TriangleStrip;
                    d.count = 9;
                    for binding in &mut d.bindings {
                        binding.slice = match (binding.group, binding.binding) {
                            (0, 0) => color,
                            (1, 0) => vertex,
                            _ => binding.slice,
                        };
                    }
                    passes.push(initial(vec![d]));
                }
                _ => unreachable!(),
            }
            for y in 0..SIZE {
                for x in 0..SIZE {
                    let color = match id {
                        1 => [255, 0, 0, 255],
                        2 => {
                            if x < 16 {
                                [255, 0, 0, 255]
                            } else {
                                [0, 0, 255, 255]
                            }
                        }
                        3 => {
                            if x < 8 {
                                [0, 255, 0, 255]
                            } else if x < 16 {
                                [255, 0, 0, 255]
                            } else {
                                [0, 0, 255, 255]
                            }
                        }
                        4 => {
                            if x < 16 {
                                [0, 255, 0, 255]
                            } else {
                                [255, 0, 0, 255]
                            }
                        }
                        5 => [128, 0, 128, 255],
                        6 => [0, 255, 0, 255],
                        7 => {
                            if (10..22).contains(&x) && (6..26).contains(&y) {
                                [255, 255, 0, 255]
                            } else {
                                [0, 0, 0, 255]
                            }
                        }
                        8 => [0, 255, 255, 255],
                        9 => {
                            if x < 10 || (x >= 22 && y < 16) {
                                [255, 0, 255, 255]
                            } else {
                                [0, 0, 0, 255]
                            }
                        }
                        _ => unreachable!(),
                    };
                    let at = ((y * SIZE + x) * 4) as usize;
                    expected[at..at + 4].copy_from_slice(&color);
                }
            }
            Fixture { id, frame: fp::FramePlan::new(SIZE, SIZE, b.resources, passes).unwrap(), expected, tolerance }
        })
        .collect()
}

pub fn verify(fixture: &Fixture, bytes: &[u8]) -> bool {
    bytes.len() == fixture.expected.len()
        && bytes.iter().zip(&fixture.expected).all(|(a, b)| a.abs_diff(*b) <= fixture.tolerance)
}

pub struct WebGpuFrames {
    device: Device,
    vertex: ShaderModule,
    pixel: ShaderModule,
    vertex_entry: &'static str,
    pixel_entry: &'static str,
    used: BTreeSet<(u32, u32)>,
}
impl WebGpuFrames {
    pub fn new(device: Device) -> Result<Self, String> {
        let [vertex, pixel] = shaders()?;
        let mut used = BTreeSet::new();
        for shader in [&vertex, &pixel] {
            for b in msl::lower(shader).map_err(|e| format!("lower {e:?}"))?.bindings {
                if b.used {
                    used.insert((b.group, b.binding));
                }
            }
        }
        let vertex_entry = vertex.entry_point;
        let pixel_entry = pixel.entry_point;
        device.push_error_scope(ErrorFilter::Validation);
        let vertex = device.create_shader_module_wgsl("public planned Xenos vertex", &vertex.wgsl);
        let pixel = device.create_shader_module_wgsl("public planned Xenos pixel", &pixel.wgsl);
        let error = device.block_on(device.pop_error_scope()).map_err(|e| format!("shader scope {e:?}"))?;
        if error.is_some() {
            return Err("shader validation".into());
        }
        Ok(Self { device, vertex, pixel, vertex_entry, pixel_entry, used })
    }
    pub fn render(&self, frame: &fp::FramePlan) -> Result<Vec<u8>, String> {
        let device = &self.device;
        device.push_error_scope(ErrorFilter::Validation);
        let result = self.render_inner(frame);
        let validation = device.block_on(device.pop_error_scope()).map_err(|e| format!("scope {e:?}"))?;
        if validation.is_some() || !device.take_errors().is_empty() {
            return Err("GPU validation".into());
        }
        result
    }
    fn render_inner(&self, frame: &fp::FramePlan) -> Result<Vec<u8>, String> {
        let device = &self.device;
        let resources: Vec<_> = frame
            .resources()
            .iter()
            .map(|r| {
                let b = device.create_buffer(&BufferDescriptor {
                    label: "immutable planned version",
                    size: r.bytes.len() as u64,
                    usage: BufferUsage::Uniform | BufferUsage::Storage | BufferUsage::Index,
                    mapped_at_creation: true,
                });
                let result = b.write_mapped(0, &r.bytes);
                b.unmap();
                result.map_err(|e| format!("map {e:?}"))?;
                Ok(b)
            })
            .collect::<Result<_, String>>()?;
        let find = |v: fp::Version| frame.resources().iter().position(|r| r.version == v).expect("sealed frame");
        let target = device.create_texture(&TextureDescriptor {
            label: "public planned color",
            size: extent(frame.width(), frame.height(), 1),
            format: TextureFormat::RGBA8Unorm,
            usage: TextureUsage::RenderAttachment | TextureUsage::CopySrc,
            ..Default::default()
        });
        let view = target.create_view(None);
        let depth = device.create_texture(&TextureDescriptor {
            label: "public planned depth",
            size: extent(frame.width(), frame.height(), 1),
            format: TextureFormat::Depth32Float,
            usage: TextureUsage::RenderAttachment,
            ..Default::default()
        });
        let depth_view = depth.create_view(None);
        // Retain every immutable version and binding through completion. Build
        // and validate pipelines before recording, so an invalid object is never
        // submitted to wgpu-native (which may otherwise terminate the process).
        let mut held = Vec::new();
        for d in frame.passes().iter().flat_map(|p| &p.draws) {
            let blend = d.blend.then_some(BlendState {
                color: BlendComponent {
                    operation: BlendOperation::Add,
                    src_factor: BlendFactor::SrcAlpha,
                    dst_factor: BlendFactor::OneMinusSrcAlpha,
                },
                alpha: BlendComponent {
                    operation: BlendOperation::Add,
                    src_factor: BlendFactor::One,
                    dst_factor: BlendFactor::OneMinusSrcAlpha,
                },
            });
            let color_targets =
                [ColorTargetState { format: TextureFormat::RGBA8Unorm, blend, write_mask: ColorWriteMask::All }];
            device.push_error_scope(ErrorFilter::Validation);
            let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
                label: "planned Xenos shader pair",
                layout: None,
                vertex: ProgrammableStage::new(&self.vertex, self.vertex_entry),
                vertex_buffers: &[],
                primitive: PrimitiveState {
                    topology: match d.topology {
                        fp::Topology::Triangles => PrimitiveTopology::TriangleList,
                        fp::Topology::TriangleStrip => PrimitiveTopology::TriangleStrip,
                    },
                    strip_index_format: if d.topology == fp::Topology::TriangleStrip {
                        d.indices.map_or(IndexFormat::Undefined, |i| {
                            if i.stride == 2 { IndexFormat::Uint16 } else { IndexFormat::Uint32 }
                        })
                    } else {
                        IndexFormat::Undefined
                    },
                    ..Default::default()
                },
                depth_stencil: Some(DepthStencilState {
                    format: TextureFormat::Depth32Float,
                    depth_write_enabled: d.depth_less_write,
                    depth_compare: if d.depth_less_write { CompareFunction::Less } else { CompareFunction::Always },
                    stencil_front: StencilFaceState {
                        compare: CompareFunction::Always,
                        fail_op: StencilOperation::Keep,
                        depth_fail_op: StencilOperation::Keep,
                        pass_op: StencilOperation::Keep,
                    },
                    stencil_back: StencilFaceState {
                        compare: CompareFunction::Always,
                        fail_op: StencilOperation::Keep,
                        depth_fail_op: StencilOperation::Keep,
                        pass_op: StencilOperation::Keep,
                    },
                    stencil_read_mask: 0,
                    stencil_write_mask: 0,
                    depth_bias: 0,
                    depth_bias_slope_scale: 0.,
                    depth_bias_clamp: 0.,
                }),
                multisample: MultisampleState::default(),
                fragment: Some(FragmentState {
                    stage: ProgrammableStage::new(&self.pixel, self.pixel_entry),
                    targets: &color_targets,
                }),
            });
            let error = device.block_on(device.pop_error_scope()).map_err(|e| format!("pipeline scope {e:?}"))?;
            if error.is_some() {
                return Err("pipeline validation".into());
            }
            device.push_error_scope(ErrorFilter::Validation);
            let mut groups = Vec::new();
            for group in [0, 1] {
                let entries: Vec<_> = d
                    .bindings
                    .iter()
                    .filter(|b| b.group == group && self.used.contains(&(b.group, b.binding)))
                    .map(|b| BindGroupEntry {
                        binding: b.binding,
                        resource: BindingResource::Buffer {
                            buffer: &resources[find(b.slice.version)],
                            offset: u64::from(b.slice.offset),
                            size: Some(u64::from(b.slice.bytes)),
                        },
                    })
                    .collect();
                let bg = device.create_bind_group("planned bindings", &pipeline.get_bind_group_layout(group), &entries);
                groups.push(bg);
            }
            let error = device.block_on(device.pop_error_scope()).map_err(|e| format!("binding scope {e:?}"))?;
            if error.is_some() {
                return Err("binding validation".into());
            }
            held.push((pipeline, groups));
        }
        let mut draw_index = 0;
        let mut encoder = device.create_command_encoder("public planned passes");
        for pass in frame.passes() {
            let colors = [Some(match pass.clear_color {
                Some([r, g, b, a]) => RenderPassColorAttachment::clear(&view, Color { r, g, b, a }),
                None => RenderPassColorAttachment::load(&view),
            })];
            let depth_attachment = RenderPassDepthStencilAttachment {
                view: &depth_view,
                depth_load_op: if pass.clear_depth.is_some() { LoadOp::Clear } else { LoadOp::Load },
                depth_store_op: StoreOp::Store,
                depth_clear_value: pass.clear_depth.unwrap_or(0.) as f32,
                depth_read_only: false,
                stencil_load_op: LoadOp::Undefined,
                stencil_store_op: StoreOp::Undefined,
                stencil_clear_value: 0,
                stencil_read_only: false,
            };
            let mut rp = encoder.begin_render_pass("ordered planned pass", &colors, Some(&depth_attachment));
            for d in &pass.draws {
                let (pipeline, groups) = &held[draw_index];
                draw_index += 1;
                rp.set_pipeline(pipeline);
                for (group, bg) in groups.iter().enumerate() {
                    rp.set_bind_group(group as u32, bg, &[]);
                }
                let [x, y, w, h, z0, z1] = d.state.viewport();
                rp.set_viewport(x, y, w, h, z0, z1);
                let [x, y, w, h] = d.state.scissor;
                rp.set_scissor_rect(x, y, w, h);
                let [r, g, b, a] = d.state.blend();
                rp.set_blend_constant(Color { r, g, b, a });
                rp.set_stencil_reference(d.state.stencil_reference);
                if let Some(i) = d.indices {
                    rp.set_index_buffer(
                        &resources[find(i.slice.version)],
                        if i.stride == 2 { IndexFormat::Uint16 } else { IndexFormat::Uint32 },
                        u64::from(i.slice.offset),
                        Some(u64::from(i.slice.bytes)),
                    );
                    rp.draw_indexed(d.count, 1, 0, 0, 0);
                } else {
                    rp.draw(d.count, 1, 0, 0);
                }
            }
            rp.end();
        }
        let row = (frame.width() * 4).next_multiple_of(256);
        let read = device.create_buffer(&BufferDescriptor {
            label: "planned result",
            size: u64::from(row * frame.height()),
            usage: BufferUsage::MapRead | BufferUsage::CopyDst,
            mapped_at_creation: false,
        });
        encoder.copy_texture_to_buffer(
            &TexelCopyTextureInfo::whole(&target),
            &TexelCopyBufferInfo { buffer: &read, offset: 0, bytes_per_row: row, rows_per_image: None },
            extent(frame.width(), frame.height(), 1),
        );
        device.queue().submit([encoder.finish("public planned result")]);
        if let Err(e) = device.block_on(read.map_async(MapMode::Read, 0, None)) {
            read.unmap();
            return Err(format!("readback {e:?}"));
        }
        let mut padded = vec![0; row as usize * frame.height() as usize];
        let result = read.read_mapped(0, &mut padded);
        read.unmap();
        result.map_err(|e| format!("mapped {e:?}"))?;
        Ok(padded.chunks(row as usize).flat_map(|r| r[..frame.width() as usize * 4].iter().copied()).collect())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn authored_planned_passes_match_independent_pixels() {
        let instance = Instance::new().unwrap();
        let adapter = instance.block_on(instance.request_adapter(&RequestAdapterOptions::default())).unwrap();
        let device = instance.block_on(adapter.request_device(&DeviceDescriptor::default())).unwrap();
        let backend = WebGpuFrames::new(device).unwrap();
        let fixtures = fixtures();
        for f in fixtures.iter().chain(fixtures.iter().rev()) {
            let result = backend.render(&f.frame).unwrap();
            assert!(verify(f, &result), "authored case {}", f.id);
        }
        let f = &fixtures[0];
        let mut corrupted = f.expected.clone();
        corrupted[0] ^= 0xff;
        assert!(!verify(f, &corrupted));
    }
}
