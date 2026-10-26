//! Explicit, fixed MSL path for one operation WebGPU cannot express. Never
//! masquerades as a reflected Naga shader or accepts arbitrary MSL/raw handles.
//! Same HAL device/queue, scope tracking and terminal lifetimes as other passes.
use super::*;
use crate::bindings::{Entry, Resource};
type Metal = hal::api::Metal;

pub(super) struct RetainedShader {
    owner: Rc<Owner<Metal>>,
    raw: Option<<Metal as hal::Api>::ShaderModule>,
}
impl Drop for RetainedShader {
    fn drop(&mut self) {
        unsafe {
            self.owner
                .device
                .destroy_shader_module(self.raw.take().unwrap());
        }
    }
}

/// Opt-in internal stencil restore, one draw instead of eight bitwise draws.
/// Does not fuse the preceding depth/scratch pass. No game-default selection.
#[derive(Clone)]
pub struct StencilRestore {
    pipeline: Pipeline<Metal>,
    samples: u32,
}

impl Gpu<Metal> {
    pub fn metal_stencil_restore(&self, samples: u32) -> Result<StencilRestore> {
        self.owner.healthy()?;
        if !matches!(samples, 1 | 4) {
            return Err(Error::Rejected("stencil restore sample count"));
        }
        for (format, required) in [
            (Format::R32Uint, hal::TextureFormatCapabilities::SAMPLED),
            (
                Format::Depth24PlusStencil8,
                hal::TextureFormatCapabilities::DEPTH_STENCIL_ATTACHMENT,
            ),
        ] {
            let caps = unsafe { self.owner._adapter.texture_format_capabilities(format) };
            if !caps.contains(required)
                || samples == 4 && !caps.contains(hal::TextureFormatCapabilities::MULTISAMPLE_X4)
            {
                return Err(Error::Rejected("stencil restore format/sample capability"));
            }
        }
        // With one fragment-only texture and no arrays/immediates, HAL29 maps
        // group0/binding0 to fragment texture(0). No caller-controlled layout.
        let group = self.group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Texture {
                sample_type: Sample::Uint,
                view_dimension: types::TextureViewDimension::D2,
                multisampled: samples > 1,
            },
            count: None,
        }])?;
        let layout = self.pipeline_layout(&[group])?;
        let identity = id()?;
        // HAL passthrough does not inject Naga runtime bounds checks. This fixed
        // kernel checks every texel/sample access itself, and has no buffers.
        let raw = unsafe {
            self.owner.device.create_shader_module(
                &hal::ShaderModuleDescriptor {
                    label: Some("fixed stencil export kernel"),
                    runtime_checks: types::ShaderRuntimeChecks::checked(),
                },
                hal::ShaderInput::Msl {
                    shader: include_str!("stencil_restore.metal"),
                    num_workgroups: (0, 0, 0),
                },
            )
        }
        .map_err(|e| self.owner.shader_error(e))?;
        let shader = RetainedShader {
            owner: self.owner.clone(),
            raw: Some(raw),
        };
        let face = types::StencilFaceState {
            compare: types::CompareFunction::Always,
            fail_op: types::StencilOperation::Keep,
            depth_fail_op: types::StencilOperation::Keep,
            pass_op: types::StencilOperation::Replace,
        };
        let depth = types::DepthStencilState {
            format: Format::Depth24PlusStencil8,
            depth_write_enabled: Some(false),
            depth_compare: Some(types::CompareFunction::Always),
            stencil: types::StencilState {
                front: face,
                back: face,
                read_mask: 255,
                write_mask: 255,
            },
            bias: Default::default(),
        };
        let constants = Default::default();
        let primitive = types::PrimitiveState::default();
        let raw = unsafe {
            self.owner
                .device
                .create_render_pipeline(&hal::RenderPipelineDescriptor {
                    label: Some("single-draw stencil restore"),
                    layout: layout.raw(),
                    vertex_processor: hal::VertexProcessor::Standard {
                        vertex_buffers: &[],
                        vertex_stage: hal::ProgrammableStage {
                            module: shader.raw.as_ref().unwrap(),
                            entry_point: "stencil_vs",
                            constants: &constants,
                            zero_initialize_workgroup_memory: true,
                        },
                    },
                    primitive,
                    depth_stencil: Some(depth),
                    multisample: types::MultisampleState {
                        count: samples,
                        ..Default::default()
                    },
                    fragment_stage: Some(hal::ProgrammableStage {
                        module: shader.raw.as_ref().unwrap(),
                        entry_point: if samples > 1 {
                            "stencil_ms_fs"
                        } else {
                            "stencil_fs"
                        },
                        constants: &constants,
                        zero_initialize_workgroup_memory: true,
                    }),
                    color_targets: &[],
                    multiview_mask: None,
                    cache: None,
                })
        }
        .map_err(|e| self.owner.pipeline_error(e))?;
        let pipeline = Pipeline(Rc::new(PipelineInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            id: identity,
            layout,
            required_sizes: Vec::new(),
            _programs: PipelinePrograms::MetalStencil { _shader: shader },
            colors: Vec::new(),
            depth: Some(Format::Depth24PlusStencil8),
            samples,
            primitive,
            plan_epoch: Cell::new(0),
        }));
        Ok(StencilRestore { pipeline, samples })
    }
}

impl StencilRestore {
    /// The existing destination depth is loaded and never written. Invalid
    /// scratch samples discard, preserving their existing stencil exactly.
    /// Group creation is cold; a future cache adapter may cache it by view ID.
    pub fn encode(
        &self,
        gpu: &Gpu<Metal>,
        encoder: &mut Encoder<Metal>,
        scratch: &TextureView<Metal>,
        destination: &TextureView<Metal>,
        scissor: Option<[u32; 4]>,
    ) -> Result<PassStats> {
        let src = scratch.texture();
        let dst = destination.texture();
        let sr = scratch.range();
        let dr = destination.range();
        if src.descriptor().format != Format::R32Uint
            || dst.descriptor().format != Format::Depth24PlusStencil8
            || src.descriptor().sample_count != self.samples
            || dst.descriptor().sample_count != self.samples
            || scratch.dimension() != types::TextureViewDimension::D2
            || destination.dimension() != types::TextureViewDimension::D2
            || sr.mip_level_count != Some(1)
            || sr.array_layer_count != Some(1)
            || dr.mip_level_count != Some(1)
            || dr.array_layer_count != Some(1)
            || src.mip_size(sr.base_mip_level)? != dst.mip_size(dr.base_mip_level)?
        {
            return Err(Error::Rejected(
                "stencil restore source/destination contract",
            ));
        }
        let group = gpu.bind_group(
            &self.pipeline.0.layout.0.groups[0],
            &[Entry {
                binding: 0,
                resource: Resource::Texture(scratch),
            }],
        )?;
        let groups = [BoundGroup {
            group: &group,
            dynamic_offsets: &[],
        }];
        encoder.render_full(
            &[],
            Some(DepthAttachment {
                view: destination,
                clear_depth: None,
                clear_stencil: None,
            }),
            &[],
            &[Draw {
                pipeline: &self.pipeline,
                groups: &groups,
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor,
                blend: types::Color::TRANSPARENT,
                stencil_reference: 0,
            }],
        )
    }
}
