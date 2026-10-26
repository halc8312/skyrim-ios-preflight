//! Compute on the shared immutable resource/layout model. This covers uniform,
//! sampled-image and storage-buffer kernels used by target transfers/resolves;
//! storage images, overrides and workgroup memory remain explicitly unsupported.
use super::super::*;
use crate::{
    bindings::PipelineLayout, render::BoundGroup, shader::Shader, types::TextureUses as T,
};

struct Inner<B: Backend> {
    owner: Rc<Owner<B>>,
    id: u64,
    raw: Option<B::ComputePipeline>,
    layout: PipelineLayout<B>,
    required_sizes: Vec<(u32,u32,u64)>,
    _shader: Shader<B>,
}
impl<B: Backend> Drop for Inner<B> {
    fn drop(&mut self) {
        unsafe {
            self.owner
                .device
                .destroy_compute_pipeline(self.raw.take().unwrap());
        }
    }
}
pub struct SharedComputePipeline<B: Backend>(Rc<Inner<B>>);
impl<B: Backend> Clone for SharedComputePipeline<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> Gpu<B> {
    pub fn shared_compute_pipeline(
        &self,
        layout: &PipelineLayout<B>,
        shader: &Shader<B>,
        entry: &str,
    ) -> Result<SharedComputePipeline<B>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &layout.0.owner) || !Rc::ptr_eq(&self.owner, &shader.0.owner) {
            return Err(Error::Rejected("compute pipeline device"));
        }
        let index = shader.entry(entry, naga::ShaderStage::Compute)?;
        shader.check_layout(index, layout)?;
        let [x, y, z] = shader.0.module.entry_points[index].workgroup_size;
        let l = &self.owner.limits;
        if x == 0
            || y == 0
            || z == 0
            || x > l.max_compute_workgroup_size_x
            || y > l.max_compute_workgroup_size_y
            || z > l.max_compute_workgroup_size_z
            || u64::from(x) * u64::from(y) * u64::from(z)
                > u64::from(l.max_compute_invocations_per_workgroup)
        {
            return Err(Error::Rejected("compute workgroup size"));
        }
        let id = id()?;
        let raw = unsafe {
            self.owner
                .device
                .create_compute_pipeline(&hal::ComputePipelineDescriptor {
                    label: Some("native shared-resource compute"),
                    layout: layout.raw(),
                    stage: hal::ProgrammableStage {
                        module: shader.raw(),
                        entry_point: entry,
                        constants: &Default::default(),
                        zero_initialize_workgroup_memory: true,
                    },
                    cache: None,
                })
        }
        .map_err(|e| self.owner.pipeline_error(e))?;
        Ok(SharedComputePipeline(Rc::new(Inner {
            owner: self.owner.clone(),
            id,
            raw: Some(raw),
            layout: layout.clone(),
            required_sizes: shader.buffer_requirements(index)?,
            _shader: shader.clone(),
        })))
    }
}
struct Scope<'a, B: Backend> {
    encoder: &'a mut Encoder<B>,
}
impl<B: Backend> Drop for Scope<'_, B> {
    fn drop(&mut self) {
        if std::thread::panicking() {
            let _ = self
                .encoder
                .owner
                .fail_next("native compute encoding unwound".into());
        }
        unsafe {
            self.encoder.raw.as_mut().unwrap().end_compute_pass();
        }
    }
}

/// Immutable dispatch description; an entire batch is checked before encoding.
pub struct ComputeDispatch<'a, B: Backend> {
    pub pipeline: &'a SharedComputePipeline<B>,
    pub groups: &'a [BoundGroup<'a, B>],
    pub workgroups: [u32; 3],
}
#[derive(Default, Debug, Clone, Copy)]
pub struct ComputeStats {
    pub dispatches: u64,
    pub skipped_empty: u64,
    pub passes: u64,
    pub pipeline_binds: u64,
    pub group_binds: u64,
}
struct Resources<'a, B: Backend> {
    buffers: BTreeMap<u64, (&'a Buffer<B>, U)>,
    images: BTreeMap<(u64, u32, u32), &'a texture::Texture<B>>,
}
impl<B: Backend + 'static> Encoder<B> {
    pub fn dispatch_shared(
        &mut self,
        pipeline: &SharedComputePipeline<B>,
        groups: &[BoundGroup<'_, B>],
        workgroups: [u32; 3],
    ) -> Result<()> {
        self.dispatch_shared_batch(&[ComputeDispatch {
            pipeline,
            groups,
            workgroups,
        }])
        .map(|_| ())
    }

    fn validate_dispatch<'a>(&self, d: &ComputeDispatch<'a, B>) -> Result<Resources<'a, B>> {
        self.validate_shape(d)?;
        let p = &d.pipeline.0;
        let groups = d.groups;
        let mut buffers = BTreeMap::new();
        let mut images = BTreeMap::new();
        for (slot,(g, expected)) in groups.iter().zip(&p.layout.0.groups).enumerate() {
            g.group.check_sizes(slot as u32, &p.required_sizes)?;
            if !expected.compatible(&g.group.0.layout) {
                return Err(Error::Rejected("compute group layout/device"));
            }
            g.group.validate_offsets(g.dynamic_offsets)?;
            for (b, u) in &g.group.0.buffers {
                if let Some((_, previous)) = buffers.get(&b.id()) {
                    if !U::INCLUSIVE.contains(*previous | *u) {
                        return Err(Error::Rejected(
                            "compute writable buffer aliases across groups",
                        ));
                    }
                }
                buffers
                    .entry(b.id())
                    .and_modify(|(_, old)| *old |= *u)
                    .or_insert((b, *u));
            }
            for v in &g.group.0.textures {
                let r = v.range();
                for mip in r.base_mip_level..r.base_mip_level + r.mip_level_count.unwrap() {
                    for layer in
                        r.base_array_layer..r.base_array_layer + r.array_layer_count.unwrap()
                    {
                        images.insert((v.texture().id(), mip, layer), v.texture());
                    }
                }
            }
        }
        Ok(Resources { buffers, images })
    }

    fn validate_shape(&self, d: &ComputeDispatch<'_, B>) -> Result<()> {
        self.owner.healthy()?;
        let ComputeDispatch {
            pipeline,
            groups,
            workgroups,
        } = d;
        let p = &pipeline.0;
        if !Rc::ptr_eq(&self.owner, &p.owner) || groups.len() != p.layout.0.groups.len() {
            return Err(Error::Rejected("compute pipeline/device/group count"));
        }
        if workgroups
            .iter()
            .any(|&n| n > self.owner.limits.max_compute_workgroups_per_dimension)
        {
            return Err(Error::Rejected("compute dispatch dimensions"));
        }
        Ok(())
    }

    /// One serial compute pass, with per-dispatch hazard transitions retained.
    /// The pinned Metal HAL uses tracked resources and a serial compute encoder;
    /// Vulkan barriers may occur between dispatches inside its compute scope.
    /// This is not permission to use untracked heaps or concurrent dispatch.
    pub fn dispatch_shared_batch(
        &mut self,
        dispatches: &[ComputeDispatch<'_, B>],
    ) -> Result<ComputeStats> {
        self.owner.healthy()?;
        if dispatches.len() > 8192 {
            return Err(Error::Rejected("compute batch budget"));
        }
        let mut resources = Vec::new();
        let mut resource_indices = Vec::with_capacity(dispatches.len());
        let mut previous: Option<&ComputeDispatch<'_, B>> = None;
        for d in dispatches {
            let same = previous.is_some_and(|p| {
                p.pipeline.0.id == d.pipeline.0.id
                    && p.groups.len() == d.groups.len()
                    && p.groups
                        .iter()
                        .zip(d.groups)
                        .all(|(a, b)| a.group.id() == b.group.id())
            });
            if same {
                self.validate_shape(d)?;
                // Immutable group identity proves its usage scope. Dynamic
                // ranges still change and are checked on every dispatch.
                for g in d.groups {
                    g.group.validate_offsets(g.dynamic_offsets)?;
                }
            } else {
                resources.push(self.validate_dispatch(d)?);
            }
            resource_indices.push(resources.len() - 1);
            previous = Some(d);
        }
        let mut stats = ComputeStats::default();
        // No transitions, holds or native commands before ALL dispatches pass.
        // Invalid bindings cannot hide behind a zero-sized dispatch either.
        for d in dispatches {
            if d.workgroups.contains(&0) {
                stats.skipped_empty += 1;
                continue;
            }
            let p = &d.pipeline.0;
            self.holds.entry(p.id).or_insert_with(|| p.clone());
            for g in d.groups {
                self.holds
                    .entry(g.group.id())
                    .or_insert_with(|| g.group.0.clone());
            }
        }
        if stats.skipped_empty == dispatches.len() as u64 {
            return Ok(stats);
        }
        unsafe {
            self.raw
                .as_mut()
                .unwrap()
                .begin_compute_pass(&hal::ComputePassDescriptor {
                    label: Some("native shared-resource compute"),
                    timestamp_writes: None,
                });
            let scope = Scope { encoder: self };
            let mut previous_pipeline = None;
            let mut previous_groups: Vec<Option<(u64, &[u32])>> = Vec::new();
            for (d, ri) in dispatches.iter().zip(resource_indices) {
                let r = &resources[ri];
                if d.workgroups.contains(&0) {
                    continue;
                }
                for (buffer, usage) in r.buffers.values() {
                    scope.encoder.transition(buffer, *usage);
                }
                for ((_, mip, layer), texture) in &r.images {
                    scope
                        .encoder
                        .transition_texture(texture, *mip, *layer, T::RESOURCE);
                }
                let p = &d.pipeline.0;
                let raw = scope.encoder.raw.as_mut().unwrap();
                if previous_pipeline != Some(p.id) {
                    raw.set_compute_pipeline(p.raw.as_ref().unwrap());
                    previous_pipeline = Some(p.id);
                    stats.pipeline_binds += 1;
                    // Conservative across pipeline changes (including compatible
                    // layouts): backend auxiliary buffer slots may change.
                    previous_groups.clear();
                    previous_groups.resize(d.groups.len(), None);
                }
                for (i, g) in d.groups.iter().enumerate() {
                    let key = (g.group.id(), g.dynamic_offsets);
                    if previous_groups[i] != Some(key) {
                        raw.set_bind_group(
                            p.layout.raw(),
                            i as u32,
                            g.group.raw(),
                            g.dynamic_offsets,
                        );
                        previous_groups[i] = Some(key);
                        stats.group_binds += 1;
                    }
                }
                raw.dispatch(d.workgroups);
                stats.dispatches += 1;
            }
        }
        stats.passes = 1;
        Ok(stats)
    }
}
