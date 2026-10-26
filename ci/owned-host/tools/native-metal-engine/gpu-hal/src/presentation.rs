//! The game's existing presentation filters on the direct backend. This draws
//! to an owned offscreen/surface view; drawable acquisition is a separate layer.
//! Parameters are immutable generations, never queued writes to a live uniform.
use crate::*;
use crate::{
    bindings::{Entry, Group, GroupLayout, Resource, Sampler},
    render::{BoundGroup, ColorAttachment, Draw, Pipeline, PipelineDescriptor, Viewport},
    texture::TextureView,
};
use types::{TextureFormat as F, TextureViewDimension as V};
use xenos_gfx::presentation::{BLIT, filtered_shader, letterbox};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Options {
    filter: u32,
    sharpen: u32,
}
impl Options {
    /// 0=automatic, 1=bilinear, 2=nearest, 3=bicubic; sharpening0..100.
    pub fn new(filter: u32, sharpen: u32) -> Result<Self> {
        if filter > 3 || sharpen > 100 {
            return Err(Error::Rejected("presentation settings"));
        }
        Ok(Self { filter, sharpen })
    }
    fn words(self) -> [u32; 4] {
        [self.filter, self.sharpen, 0, 0]
    }
}
pub struct Front<B: Backend> {
    pub view: TextureView<B>,
    pub display_size: [u32; 2],
    pub swizzle: [u32; 4],
}
impl<B: Backend> Clone for Front<B> {
    fn clone(&self) -> Self {
        Self {
            view: self.view.clone(),
            display_size: self.display_size,
            swizzle: self.swizzle,
        }
    }
}
struct Shown<B: Backend> {
    front: Front<B>,
    options: Options,
    params: Buffer<B>,
    group: Group<B>,
}
pub struct Presenter<B: Backend> {
    owner: Rc<Owner<B>>,
    format: F,
    layout: GroupLayout<B>,
    sampler: Sampler<B>,
    pipeline: Pipeline<B>,
    filtered: Option<Pipeline<B>>,
    shown: Option<Shown<B>>,
}
impl<B: Backend> Gpu<B> {
    pub fn presenter(&self, format: F) -> Result<Presenter<B>> {
        self.owner.healthy()?;
        if !matches!(
            format,
            F::Rgba8Unorm | F::Rgba8UnormSrgb | F::Bgra8Unorm | F::Bgra8UnormSrgb
        ) {
            return Err(Error::Rejected("presentation destination format"));
        }
        let uniform = |binding| types::BindGroupLayoutEntry {
            binding,
            visibility: types::ShaderStages::FRAGMENT,
            ty: types::BindingType::Buffer {
                ty: types::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: NonZeroU64::new(16),
            },
            count: None,
        };
        let layout = self.group_layout(&[
            types::BindGroupLayoutEntry {
                binding: 0,
                visibility: types::ShaderStages::FRAGMENT,
                ty: types::BindingType::Texture {
                    sample_type: types::TextureSampleType::Float { filterable: true },
                    view_dimension: V::D2,
                    multisampled: false,
                },
                count: None,
            },
            types::BindGroupLayoutEntry {
                binding: 1,
                visibility: types::ShaderStages::FRAGMENT,
                ty: types::BindingType::Sampler(types::SamplerBindingType::Filtering),
                count: None,
            },
            uniform(2),
            uniform(3),
        ])?;
        let shader = self.shader(BLIT)?;
        let pl = self.pipeline_layout(&[layout.clone()])?;
        let pipeline = self.render_pipeline(&PipelineDescriptor {
            layout: &pl,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, "fs")),
            colors: &[Some(types::ColorTargetState {
                format,
                blend: None,
                write_mask: types::ColorWrites::ALL,
            })],
            primitive: Default::default(),
            depth_stencil: None,
            multisample: Default::default(),
        })?;
        let sampler = self.sampler(&types::SamplerDescriptor {
            mag_filter: types::FilterMode::Linear,
            min_filter: types::FilterMode::Linear,
            ..Default::default()
        })?;
        Ok(Presenter {
            owner: self.owner.clone(),
            format,
            layout,
            sampler,
            pipeline,
            filtered: None,
            shown: None,
        })
    }
}
impl<B: Backend> Presenter<B> {
    /// `None` retains the previous front buffer. Unchanged bindings allocate
    /// nothing; changed options allocate a new small, immutable parameter version.
    pub fn swap(
        &mut self,
        gpu: &mut Gpu<B>,
        front: Option<Front<B>>,
        options: Options,
    ) -> Result<Option<[u32; 2]>> {
        gpu.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &gpu.owner) {
            return Err(Error::Rejected("presenter device"));
        }
        let Some(front) = front.or_else(|| self.shown.as_ref().map(|s| s.front.clone())) else {
            return Ok(None);
        };
        let r = front.view.range();
        let d = front.view.texture().descriptor();
        if !front.view.belongs_to(&self.owner)
            || front.view.dimension() != V::D2
            || r.mip_level_count != Some(1)
            || r.array_layer_count != Some(1)
            || r.aspect != types::TextureAspect::All
            || d.sample_count != 1
            || front.swizzle.iter().any(|&v| v > 5)
            || front
                .display_size
                .iter()
                .any(|&v| v == 0 || v > self.owner.limits.max_texture_dimension_2d)
        {
            return Err(Error::Rejected("presentation source contract"));
        }
        let same_params = self
            .shown
            .as_ref()
            .is_some_and(|s| s.options == options && s.front.swizzle == front.swizzle);
        if same_params
            && self
                .shown
                .as_ref()
                .is_some_and(|s| s.front.view.id() == front.view.id())
        {
            self.shown.as_mut().unwrap().front.display_size = front.display_size;
            return Ok(Some(front.display_size));
        }
        if options != Options::default() && self.filtered.is_none() {
            let shader = gpu.shader(&filtered_shader())?;
            let pl = gpu.pipeline_layout(&[self.layout.clone()])?;
            self.filtered = Some(gpu.render_pipeline(&PipelineDescriptor {
                layout: &pl,
                vertex: &shader,
                vertex_entry: "vs",
                fragment: Some((&shader, "fs")),
                colors: &[Some(types::ColorTargetState {
                    format: self.format,
                    blend: None,
                    write_mask: types::ColorWrites::ALL,
                })],
                primitive: Default::default(),
                depth_stencil: None,
                multisample: Default::default(),
            })?);
        }
        let alignment = self.owner.limits.min_uniform_buffer_offset_alignment.max(1);
        let offset = 16u32.div_ceil(alignment) * alignment;
        let params = if same_params {
            self.shown.as_ref().unwrap().params.clone()
        } else {
            let mut bytes = vec![0; offset as usize + 16];
            for (start, words) in [(0, front.swizzle), (offset as usize, options.words())] {
                for (i, w) in words.iter().enumerate() {
                    bytes[start + i * 4..start + i * 4 + 4].copy_from_slice(&w.to_le_bytes());
                }
            }
            let params = gpu.create_buffer_init(
                "immutable present parameters",
                &bytes,
                U::UNIFORM | U::COPY_DST,
            )?;
            params
        };
        let group = gpu.bind_group(
            &self.layout,
            &[
                Entry {
                    binding: 0,
                    resource: Resource::Texture(&front.view),
                },
                Entry {
                    binding: 1,
                    resource: Resource::Sampler(&self.sampler),
                },
                Entry {
                    binding: 2,
                    resource: Resource::Buffer {
                        buffer: &params,
                        offset: 0,
                        size: 16,
                    },
                },
                Entry {
                    binding: 3,
                    resource: Resource::Buffer {
                        buffer: &params,
                        offset: u64::from(offset),
                        size: 16,
                    },
                },
            ],
        )?;
        let size = front.display_size;
        self.shown = Some(Shown {
            front,
            options,
            params,
            group,
        });
        Ok(Some(size))
    }
    pub fn encode(
        &self,
        encoder: &mut Encoder<B>,
        target: &TextureView<B>,
    ) -> Result<crate::render::PassStats> {
        self.owner.healthy()?;
        let r = target.range();
        let desc = target.texture().descriptor();
        if !Rc::ptr_eq(&self.owner, &encoder.owner)
            || !target.belongs_to(&self.owner)
            || target.dimension() != V::D2
            || r.mip_level_count != Some(1)
            || r.array_layer_count != Some(1)
            || desc.format != self.format
            || desc.sample_count != 1
        {
            return Err(Error::Rejected("presentation target contract"));
        }
        let size = target.texture().mip_size(r.base_mip_level)?;
        let attachments = [Some(ColorAttachment {
            view: target,
            clear: Some(types::Color::BLACK),
        })];
        if let Some(shown) = &self.shown {
            let (x, y, width, height) = letterbox(
                (shown.front.display_size[0], shown.front.display_size[1]),
                (size.width, size.height),
            );
            let groups = [BoundGroup {
                group: &shown.group,
                dynamic_offsets: &[],
            }];
            encoder.render(
                &attachments,
                &[Draw {
                    pipeline: if shown.options == Options::default() {
                        &self.pipeline
                    } else {
                        self.filtered.as_ref().unwrap()
                    },
                    groups: &groups,
                    vertices: 0..3,
                    instances: 0..1,
                    indices: None,
                    viewport: Some(Viewport {
                        x,
                        y,
                        width,
                        height,
                        min_depth: 0.,
                        max_depth: 1.,
                    }),
                    scissor: None,
                    blend: types::Color::TRANSPARENT,
                    stencil_reference: 0,
                }],
            )
        } else {
            encoder.render(&attachments, &[])
        }
    }
}
