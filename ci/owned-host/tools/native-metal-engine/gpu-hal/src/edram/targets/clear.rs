//! Resolve clears use the shared geometry and bit-exact color/depth shader.
//! Copy always precedes clear. Ownership is transactional, including growth.
use super::*;
use xenos_gfx::edram_clear::{self, ClearPlan};
use xenos_state::resolve::ResolveInfo;

#[derive(Default)]
pub(super) struct Pipelines {
    entries: HashMap<(F, u32), (GroupLayout<Platform>, Pipeline<Platform>)>,
}
impl Pipelines {
    fn get(
        &mut self,
        gpu: &Gpu<Platform>,
        format: F,
        samples: u32,
        scale: u32,
    ) -> Result<(GroupLayout<Platform>, Pipeline<Platform>)> {
        if let Some(p) = self.entries.get(&(format, samples)) {
            return Ok(p.clone());
        }
        let group = gpu.group_layout(&[types::BindGroupLayoutEntry {
            binding: 0,
            visibility: S::FRAGMENT,
            count: None,
            ty: types::BindingType::Buffer {
                ty: types::BufferBindingType::Uniform,
                has_dynamic_offset: false,
                min_binding_size: NonZeroU64::new(64),
            },
        }])?;
        let layout = gpu.pipeline_layout(&[group.clone()])?;
        let shader = gpu.shader(&kernels::owned_clear_module(scale, samples > 1))?;
        let depth = format.is_depth_stencil_format();
        let face = types::StencilFaceState {
            compare: types::CompareFunction::Always,
            fail_op: types::StencilOperation::Replace,
            depth_fail_op: types::StencilOperation::Replace,
            pass_op: types::StencilOperation::Replace,
        };
        let ds = depth.then_some(types::DepthStencilState {
            format,
            depth_write_enabled: Some(true),
            depth_compare: Some(types::CompareFunction::Always),
            stencil: types::StencilState {
                front: face,
                back: face,
                read_mask: 255,
                write_mask: 255,
            },
            bias: Default::default(),
        });
        let colors = [Some(types::ColorTargetState {
            format,
            blend: None,
            write_mask: types::ColorWrites::ALL,
        })];
        let pipeline = gpu.render_pipeline(&PipelineDescriptor {
            layout: &layout,
            vertex: &shader,
            vertex_entry: "vs",
            fragment: Some((&shader, if depth { "fs_depth" } else { "fs_color" })),
            colors: if depth { &[] } else { &colors },
            primitive: Default::default(),
            depth_stencil: ds,
            multisample: types::MultisampleState {
                count: samples,
                ..Default::default()
            },
        })?;
        let result = (group, pipeline);
        self.entries.insert((format, samples), result.clone());
        Ok(result)
    }
}

impl Frame<'_> {
    pub(super) fn plan_clears(&self, info: &ResolveInfo) -> Result<Vec<ClearPlan>> {
        if info.rect.is_empty() {
            return Ok(Vec::new());
        }
        let depth = info.is_clearing_depth();
        let color = info.is_clearing_color();
        if !depth && !color {
            return Ok(Vec::new());
        }
        if info.rect.width > 8192
            || info.rect.height > 8192
            || info.rect.width % 8 != 0
            || info.rect.height % 8 != 0
            || info.edram_offset_x_div_8 > 9
            || info.edram_offset_y_div_8 > 1
            || depth && (info.depth_edram.is_none() || info.depth_original_base >= EDRAM_TILE_COUNT)
            || color && (info.color_edram.is_none() || info.color_original_base >= EDRAM_TILE_COUNT)
        {
            return Err(Error::Rejected("native resolve clear rectangle/base"));
        }
        let mut common = None;
        for (edram, is_depth) in [
            (info.depth_edram.filter(|_| depth), true),
            (info.color_edram.filter(|_| color), false),
        ] {
            let Some(e) = edram else { continue };
            if e.is_depth != is_depth
                || e.msaa_samples.count().is_none()
                || !(1..=2048).contains(&e.pitch_tiles)
                || e.base_tiles > 0x00ff_ffff
                || e.format_is_64bpp && e.pitch_tiles % 2 != 0
                || is_depth && (e.format > 1 || e.format_is_64bpp)
                || !is_depth
                    && (color_storage(xenos_state::xenos::ColorRenderTargetFormat::from_raw(
                        e.format,
                    ))
                    .is_none()
                        || xenos_state::xenos::ColorRenderTargetFormat::from_raw(e.format)
                            .is_64bpp()
                            != e.format_is_64bpp)
            {
                return Err(Error::Rejected("native resolve clear format/pitch"));
            }
            let c = (
                e.pitch_tiles >> u32::from(e.format_is_64bpp),
                e.msaa_samples,
            );
            if common.is_some_and(|v| v != c) {
                return Err(Error::Rejected(
                    "native resolve clear inconsistent surfaces",
                ));
            }
            common = Some(c);
        }
        let max =
            (self.gpu.owner.limits.max_texture_dimension_2d / self.cache.options.scale).min(8192);
        let plans = edram_clear::plan_clears(info, max)
            .map_err(|_| Error::Rejected("native resolve clear plan"))?;
        for plan in &plans {
            self.host_format(plan.key)?;
        }
        Ok(plans)
    }
    /// Allocate/grow without taking unrelated tiles. Preserve every old owner.
    fn ensure_clear_target(&mut self, plan: &ClearPlan, keep: &[ClearPlan]) -> Result<()> {
        let k = plan.key;
        let scale = self.cache.options.scale;
        let tile_h = 16 >> k.msaa_samples.samples_y_log2();
        let max = (self.gpu.owner.limits.max_texture_dimension_2d / scale).min(8192);
        let max_height =
            (EDRAM_TILE_COUNT.div_ceil(k.pitch_tiles_at_32bpp) * tile_h).min(max / tile_h * tile_h);
        let height = plan.rect[3].next_multiple_of(tile_h * 8).min(max_height) * scale;
        if self
            .state
            .targets
            .get(&k)
            .is_some_and(|t| t.height >= height)
        {
            return Ok(());
        }
        if !self.state.targets.contains_key(&k)
            && self.state.targets.len() >= self.cache.options.max_targets
        {
            let victim = self
                .state
                .targets
                .iter()
                .filter(|(k, _)| {
                    !keep.iter().any(|p| p.key == **k)
                        && !self.state.ownership.ranges().iter().any(|r| r.owner == **k)
                })
                .min_by_key(|(_, t)| t.last_used)
                .map(|(k, _)| *k)
                .ok_or(Error::Rejected("EDRAM clear cannot evict owned data"))?;
            self.state.targets.remove(&victim);
        }
        let format = self.host_format(k)?;
        let target = self.create_target(k, k.width_pixels() * scale, height, format)?;
        self.stats.created_targets += 1;
        if let Some(old) = self.state.targets.insert(k, target.clone()) {
            self.stats.grown_targets += 1;
            let ranges: Vec<_> = self
                .state
                .ownership
                .ranges()
                .iter()
                .filter(|r| r.owner == k)
                .copied()
                .collect();
            let mut works = Vec::new();
            for r in ranges {
                works.push(self.work(old.clone(), target.clone(), [r.start, r.end - r.start])?);
            }
            self.encode_works(&works)?;
        }
        Ok(())
    }
    pub(super) fn encode_clears(&mut self, plans: &[ClearPlan]) -> Result<()> {
        if plans.is_empty() {
            return Ok(());
        }
        for plan in plans {
            self.ensure_clear_target(plan, plans)?;
        }
        for plan in plans {
            let target = self.state.targets[&plan.key].clone();
            let previous = self
                .state
                .ownership
                .claim(plan.start, plan.length, plan.key);
            let mut works = Vec::new();
            for r in previous {
                let source = self
                    .state
                    .targets
                    .get(&r.owner)
                    .ok_or(Error::Rejected("EDRAM clear owner missing"))?
                    .clone();
                works.push(self.work(source, target.clone(), [r.start, r.end - r.start])?);
            }
            // Transfer the previous whole span first, then overwrite only the
            // intersection of the clear rectangle and tiles actually claimed.
            self.encode_works(&works)?;
            let (layout, pipeline) = self.cache.clear_kernels.get(
                self.gpu,
                target.format,
                target.samples,
                self.cache.options.scale,
            )?;
            let scale = self.cache.options.scale;
            let rect = plan.rect.map(|v| v * scale);
            let words = [
                plan.value[0],
                plan.value[1],
                plan.key.format + if plan.key.is_depth { 16 } else { 0 },
                0,
                rect[0],
                rect[1],
                rect[2],
                rect[3],
                plan.key.base_tiles,
                plan.key.pitch_tiles(),
                plan.key.msaa_samples.raw(),
                u32::from(plan.key.is_depth) | (u32::from(plan.key.is_64bpp()) << 1),
                plan.start,
                plan.length,
                0,
                0,
            ];
            let params = self.gpu.create_buffer_init(
                "immutable guest clear",
                bytemuck::cast_slice(&words),
                U::UNIFORM | U::COPY_DST,
            )?;
            let group = self.gpu.bind_group(
                &layout,
                &[Entry {
                    binding: 0,
                    resource: Resource::Buffer {
                        buffer: &params,
                        offset: 0,
                        size: 64,
                    },
                }],
            )?;
            let groups = [BoundGroup {
                group: &group,
                dynamic_offsets: &[],
            }];
            let draws = [Draw {
                pipeline: &pipeline,
                groups: &groups,
                vertices: 0..3,
                instances: 0..1,
                indices: None,
                viewport: None,
                scissor: Some([rect[0], rect[1], rect[2] - rect[0], rect[3] - rect[1]]),
                blend: types::Color::TRANSPARENT,
                stencil_reference: plan.value[0] & 255,
            }];
            let colors = [Some(ColorAttachment {
                view: &target.attachment,
                clear: None,
            })];
            let depth = plan.key.is_depth.then_some(DepthAttachment {
                view: &target.attachment,
                clear_depth: None,
                clear_stencil: None,
            });
            let result = self.encoder.as_mut().unwrap().render_full(
                if depth.is_some() { &[] } else { &colors },
                depth,
                &[],
                &draws,
            )?;
            self.stats.clear_draws += result.draws;
            self.state.targets.get_mut(&plan.key).unwrap().last_used = self.state.epoch;
        }
        self.active_colors = [None; 4];
        self.active_depth = None;
        self.active_extent = [0; 2];
        self.active_owned_extent = [0; 2];
        Ok(())
    }
}
