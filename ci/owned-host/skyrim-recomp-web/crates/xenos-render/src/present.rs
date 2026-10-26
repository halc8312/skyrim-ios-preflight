//! Shows the guest's front buffer (VdSwap's fetch constant) in the presentation target,
//! letterboxed to keep its aspect. The front buffer comes from the texture cache, which loads it
//! from the GPU copy of guest memory when a resolve stored it there.

use crate::settings::PresentationSettings;
use gpu_webgpu::*;
use xenos_texture::{SwizzleSource, TextureFetchConstant};

use xenos_gfx::presentation::{BLIT,filtered_shader,letterbox};

const BACKGROUND: Color = Color { r: 0.0, g: 0.0, b: 0.0, a: 1.0 };

/// A front buffer ready to show: the texture cache's view of it and how its channels map.
pub(crate) struct FrontBuffer {
    pub view: TextureView,
    pub filterable: bool,
    pub width: u32,
    pub height: u32,
    pub swizzle: [u32; 4],
}

impl FrontBuffer {
    /// The fetch constant a front buffer is loaded with (base level only, plain 2D), and the
    /// output channel mapping for `component_map` (guest component → host channel).
    pub fn fetch_and_swizzle(
        fetch: &TextureFetchConstant,
        component_map: [xenos_texture::HostChannel; 4],
    ) -> (TextureFetchConstant, [u32; 4]) {
        let mut fb = fetch.clone();
        fb.mip_page = 0;
        fb.mip_max_level = 0;
        fb.mip_min_level = 0;
        fb.dimension = xenos_texture::DataDimension::D2OrStacked;
        fb.stacked = false;
        let channel = |c: xenos_texture::HostChannel| match c {
            xenos_texture::HostChannel::R => 0,
            xenos_texture::HostChannel::G => 1,
            xenos_texture::HostChannel::B => 2,
            xenos_texture::HostChannel::A => 3,
        };
        let swizzle = fetch.swizzle.map(|s| match s {
            SwizzleSource::Zero => 4,
            SwizzleSource::One => 5,
            s => channel(component_map[s.component().unwrap_or(0)]),
        });
        (fb, swizzle)
    }
}

struct Shown {
    width: u32,
    height: u32,
    // Keep the compared handle alive; a retired view cannot reuse its identity.
    // This is only the current front buffer, never a history of old textures.
    view: TextureView,
    group: BindGroup,
}

pub(crate) struct Presenter {
    pipeline: RenderPipeline,
    filtered_pipeline: Option<RenderPipeline>,
    layout: BindGroupLayout,
    sampler: Sampler,
    /// The front buffer's channel mapping (rewritten per swap: buffers are costly to create).
    swizzle: Buffer,
    options: Buffer,
    options_words: [u32; 4],
    shown: Option<Shown>,
    /// Swaps shown so far.
    pub frames: u64,
}

impl Presenter {
    pub(crate) fn new(device: &Device, target_format: TextureFormat) -> Self {
        let module = device.create_shader_module_wgsl("present", BLIT);
        let layout = device.create_bind_group_layout(
            "present",
            &[
                BindGroupLayoutEntry {
                    binding: 0,
                    visibility: ShaderStage::Fragment,
                    ty: BindingType::Texture {
                        sample_type: TextureSampleType::Float,
                        view_dimension: TextureViewDimension::D2,
                        multisampled: false,
                    },
                },
                BindGroupLayoutEntry {
                    binding: 1,
                    visibility: ShaderStage::Fragment,
                    ty: BindingType::Sampler(SamplerBindingType::Filtering),
                },
                BindGroupLayoutEntry {
                    binding: 2,
                    visibility: ShaderStage::Fragment,
                    ty: BindingType::Buffer {
                        ty: BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: 16,
                    },
                },
                BindGroupLayoutEntry {
                    binding: 3,
                    visibility: ShaderStage::Fragment,
                    ty: BindingType::Buffer {
                        ty: BufferBindingType::Uniform,
                        has_dynamic_offset: false,
                        min_binding_size: 16,
                    },
                },
            ],
        );
        let pipeline_layout = device.create_pipeline_layout("present", &[&layout]);
        let pipeline = device.create_render_pipeline(&RenderPipelineDescriptor {
            label: "present",
            layout: Some(&pipeline_layout),
            vertex: ProgrammableStage::new(&module, "vs"),
            vertex_buffers: &[],
            primitive: PrimitiveState::default(),
            depth_stencil: None,
            multisample: MultisampleState::default(),
            fragment: Some(FragmentState {
                stage: ProgrammableStage::new(&module, "fs"),
                targets: &[ColorTargetState::new(target_format)],
            }),
        });
        let sampler = device.create_sampler(&SamplerDescriptor {
            label: "present",
            mag_filter: FilterMode::Linear,
            min_filter: FilterMode::Linear,
            ..Default::default()
        });
        let swizzle = device.create_buffer(&BufferDescriptor {
            label: "present swizzle",
            size: 16,
            usage: BufferUsage::Uniform | BufferUsage::CopyDst,
            mapped_at_creation: false,
        });
        let options = device.create_buffer(&BufferDescriptor {
            label: "present options",
            size: 16,
            usage: BufferUsage::Uniform | BufferUsage::CopyDst,
            mapped_at_creation: false,
        });
        Presenter {
            pipeline,
            filtered_pipeline: None,
            layout,
            sampler,
            swizzle,
            options,
            options_words: [0; 4],
            shown: None,
            frames: 0,
        }
    }

    pub(crate) fn set_output_options(
        &mut self,
        device: &Device,
        settings: PresentationSettings,
        format: TextureFormat,
    ) {
        let words = settings.shader_words();
        if words == self.options_words {
            return;
        }
        if words != [0; 4] && self.filtered_pipeline.is_none() {
            let module = device.create_shader_module_wgsl("present filtered", &filtered_shader());
            let layout = device.create_pipeline_layout("present filtered", &[&self.layout]);
            self.filtered_pipeline = Some(device.create_render_pipeline(&RenderPipelineDescriptor {
                label: "present filtered",
                layout: Some(&layout),
                vertex: ProgrammableStage::new(&module, "vs"),
                vertex_buffers: &[],
                primitive: PrimitiveState::default(),
                depth_stencil: None,
                multisample: MultisampleState::default(),
                fragment: Some(FragmentState {
                    stage: ProgrammableStage::new(&module, "fs"),
                    targets: &[ColorTargetState::new(format)],
                }),
            }));
        }
        let bytes: Vec<_> = words.iter().flat_map(|v| v.to_le_bytes()).collect();
        device.queue().write_buffer(&self.options, 0, &bytes);
        self.options_words = words;
    }

    /// Shows `front` from now on (or keeps the previous frame if there is none).
    pub(crate) fn swap(&mut self, device: &Device, front: Option<FrontBuffer>) -> Option<(u32, u32)> {
        let Some(front) = front else {
            return self.shown.as_ref().map(|s| (s.width, s.height));
        };
        if !front.filterable {
            log::warn!("present: front buffer format is not filterable");
            return None;
        }
        let bytes: Vec<u8> = front.swizzle.iter().flat_map(|v| v.to_le_bytes()).collect();
        device.queue().write_buffer(&self.swizzle, 0, &bytes);
        if let Some(shown) = self.shown.as_mut().filter(|shown| shown.view == front.view) {
            // Layout, sampler and uniform-buffer objects are immutable for this
            // presenter. Buffer contents and viewport dimensions still update.
            shown.width = front.width;
            shown.height = front.height;
        } else {
            let swizzle = &self.swizzle;
            let group = device.create_bind_group(
                "present",
                &self.layout,
                &[
                    BindGroupEntry { binding: 0, resource: BindingResource::TextureView(&front.view) },
                    BindGroupEntry { binding: 1, resource: BindingResource::Sampler(&self.sampler) },
                    BindGroupEntry {
                        binding: 2,
                        resource: BindingResource::Buffer { buffer: swizzle, offset: 0, size: None },
                    },
                    BindGroupEntry {
                        binding: 3,
                        resource: BindingResource::Buffer { buffer: &self.options, offset: 0, size: None },
                    },
                ],
            );
            self.shown = Some(Shown { width: front.width, height: front.height, view: front.view, group });
        }
        self.frames += 1;
        Some((front.width, front.height))
    }

    /// Draws the latest front buffer into `target` (black if there is none yet).
    pub(crate) fn encode(&self, encoder: &mut CommandEncoder, target: &TextureView, width: u32, height: u32) {
        let mut pass =
            encoder.begin_render_pass("present", &[Some(RenderPassColorAttachment::clear(target, BACKGROUND))], None);
        if let Some(shown) = &self.shown {
            let (x, y, w, h) = letterbox((shown.width, shown.height), (width, height));
            let pipeline = if self.options_words == [0; 4] {
                &self.pipeline
            } else {
                self.filtered_pipeline.as_ref().expect("filtered options compile their pipeline")
            };
            pass.set_pipeline(pipeline);
            pass.set_bind_group(0, &shown.group, &[]);
            pass.set_viewport(x, y, w, h, 0.0, 1.0);
            pass.draw(3, 1, 0, 0);
        }
        pass.end();
    }
}

#[cfg(all(test, not(target_os = "emscripten")))]
#[path = "present_cache_tests.rs"]
mod cache_tests;

#[cfg(test)]
mod tests {
    use super::letterbox;

    #[test]
    fn letterbox_keeps_aspect() {
        assert_eq!(letterbox((1280, 720), (1280, 720)), (0.0, 0.0, 1280.0, 720.0));
        let (x, y, w, h) = letterbox((640, 480), (1280, 720));
        assert_eq!((w, h), (960.0, 720.0));
        assert_eq!((x, y), (160.0, 0.0));
    }
    #[test]
    fn optional_filtered_shader_validates_and_original_entry_stays_separate() {
        for source in [super::BLIT.to_owned(), super::filtered_shader()] {
            let module = naga::front::wgsl::parse_str(&source).expect("authored presentation WGSL");
            naga::valid::Validator::new(naga::valid::ValidationFlags::all(), naga::valid::Capabilities::all())
                .validate(&module)
                .expect("valid presentation WGSL");
        }
        assert!(!super::BLIT.contains("options"));
        assert!(super::filtered_shader().contains("textureLoad(frame"));
    }

    #[cfg(not(target_os = "emscripten"))]
    #[test]
    fn authored_filters_and_sharpen_execute_with_original_default_pixels() {
        let result = crate::native::run_public_presentation_probe().expect("public authored GPU presentation");
        eprintln!("public presentation backend: {}", result.backend);
        assert_eq!(result.checks, 255);
        assert_eq!(result.rendered_cases, 6);
    }
}
