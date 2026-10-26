//! Immutable layouts and bindings shared by compute and render plans.
//! Validate descriptors/resources at creation; only dynamic offsets remain hot.
use super::texture::TextureView;
use super::*;
use types::{
    BindingType as Ty, BufferBindingType as Bt, SamplerBindingType as St, ShaderStages as Stages,
    TextureSampleType as Sample, TextureUses as T,
};

pub(crate) struct GroupLayoutInner<B: Backend> {
    owner: Rc<Owner<B>>,
    raw: Option<B::BindGroupLayout>,
    id: u64,
    pub(crate) entries: Vec<types::BindGroupLayoutEntry>,
}
impl<B: Backend> Drop for GroupLayoutInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.owner
                .device
                .destroy_bind_group_layout(self.raw.take().unwrap());
        }
    }
}
pub struct GroupLayout<B: Backend>(pub(crate) Rc<GroupLayoutInner<B>>);
impl<B: Backend> Clone for GroupLayout<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> GroupLayout<B> {
    pub fn id(&self) -> u64 {
        self.0.id
    }
    pub fn entries(&self) -> &[types::BindGroupLayoutEntry] {
        &self.0.entries
    }
    pub(crate) fn raw(&self) -> &B::BindGroupLayout {
        self.0.raw.as_ref().unwrap()
    }
    pub(crate) fn compatible(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
            || Rc::ptr_eq(&self.0.owner, &other.0.owner) && self.0.entries == other.0.entries
    }
}
pub(crate) struct PipelineLayoutInner<B: Backend> {
    pub(crate) owner: Rc<Owner<B>>,
    raw: Option<B::PipelineLayout>,
    pub(crate) groups: Vec<GroupLayout<B>>,
    id: u64,
}
impl<B: Backend> Drop for PipelineLayoutInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.owner
                .device
                .destroy_pipeline_layout(self.raw.take().unwrap());
        }
    }
}
pub struct PipelineLayout<B: Backend>(pub(crate) Rc<PipelineLayoutInner<B>>);
impl<B: Backend> Clone for PipelineLayout<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> PipelineLayout<B> {
    pub fn id(&self) -> u64 {
        self.0.id
    }
    pub(crate) fn raw(&self) -> &B::PipelineLayout {
        self.0.raw.as_ref().unwrap()
    }
    pub(crate) fn entry(&self, group: u32, binding: u32) -> Option<&types::BindGroupLayoutEntry> {
        self.0
            .groups
            .get(group as usize)?
            .entries()
            .iter()
            .find(|e| e.binding == binding)
    }
}
struct SamplerInner<B: Backend> {
    owner: Rc<Owner<B>>,
    raw: Option<B::Sampler>,
    filtering: bool,
    comparison: bool,
}
impl<B: Backend> Drop for SamplerInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.owner.device.destroy_sampler(self.raw.take().unwrap());
        }
    }
}
pub struct Sampler<B: Backend>(Rc<SamplerInner<B>>);
impl<B: Backend> Clone for Sampler<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}

pub enum Resource<'a, B: Backend> {
    Buffer {
        buffer: &'a Buffer<B>,
        offset: u64,
        size: u64,
    },
    Texture(&'a TextureView<B>),
    Sampler(&'a Sampler<B>),
}
pub struct Entry<'a, B: Backend> {
    pub binding: u32,
    pub resource: Resource<'a, B>,
}
struct Dynamic {
    buffer_index: usize,
    base: u64,
    size: u64,
    alignment: u64,
}
pub(crate) struct GroupInner<B: Backend> {
    pub(crate) layout: GroupLayout<B>,
    pub(crate) raw: Option<B::BindGroup>,
    pub(crate) id: u64,
    pub(crate) buffers: Vec<(Buffer<B>, U)>,
    pub(crate) textures: Vec<TextureView<B>>,
    _samplers: Vec<Sampler<B>>,
    dynamic: Vec<Dynamic>,
    pub(crate) binding_sizes: BTreeMap<u32, u64>,
    pub(crate) plan_epoch: Cell<u64>,
}
impl<B: Backend> Drop for GroupInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.layout
                .0
                .owner
                .device
                .destroy_bind_group(self.raw.take().unwrap());
        }
    }
}
pub struct Group<B: Backend>(pub(crate) Rc<GroupInner<B>>);
impl<B: Backend> Clone for Group<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> Group<B> {
    pub(crate) fn belongs_to(&self, owner: &Rc<Owner<B>>) -> bool {
        Rc::ptr_eq(&self.0.layout.0.owner, owner)
    }
    pub(crate) fn check_sizes(&self, group: u32, required: &[(u32,u32,u64)]) -> Result<()> {
        for &(g,b,min) in required {
            if g==group && self.0.binding_sizes.get(&b).is_none_or(|size| *size<min) {
                return Err(Error::Rejected("shader buffer binding too small"));
            }
        }
        Ok(())
    }
    pub fn id(&self) -> u64 {
        self.0.id
    }
    pub(crate) fn raw(&self) -> &B::BindGroup {
        self.0.raw.as_ref().unwrap()
    }
    pub(crate) fn validate_offsets(&self, offsets: &[u32]) -> Result<()> {
        if offsets.len() != self.0.dynamic.len() {
            return Err(Error::Rejected("dynamic offset count"));
        }
        for (slot, &offset) in self.0.dynamic.iter().zip(offsets) {
            let buffer = &self.0.buffers[slot.buffer_index].0;
            let offset = u64::from(offset);
            if offset % slot.alignment != 0
                || slot
                    .base
                    .checked_add(offset)
                    .and_then(|n| n.checked_add(slot.size))
                    .is_none_or(|end| end > buffer.size())
            {
                return Err(Error::Rejected("dynamic offset bounds/alignment"));
            }
        }
        Ok(())
    }
}

impl<B: Backend> Gpu<B> {
    pub fn group_layout(&self, entries: &[types::BindGroupLayoutEntry]) -> Result<GroupLayout<B>> {
        self.owner.healthy()?;
        let l = &self.owner.limits;
        let mut entries = entries.to_vec();
        entries.sort_by_key(|e| e.binding);
        if entries.len() > l.max_bindings_per_bind_group as usize
            || entries.windows(2).any(|w| w[0].binding == w[1].binding)
        {
            return Err(Error::Rejected("binding count/duplicates"));
        }
        for e in &entries {
            if e.binding >= l.max_bindings_per_bind_group
                || e.count.is_some()
                || e.visibility.is_empty()
                || !(Stages::VERTEX | Stages::FRAGMENT | Stages::COMPUTE).contains(e.visibility)
            {
                return Err(Error::Rejected("binding index/stage/array"));
            }
            match e.ty {
                Ty::Buffer {
                    ty,
                    min_binding_size,
                    ..
                } => {
                    if matches!(ty, Bt::Storage { .. })
                        && e.visibility.contains(Stages::VERTEX)
                        && !self
                            .owner
                            .downlevel
                            .contains(types::DownlevelFlags::VERTEX_STORAGE)
                    {
                        return Err(Error::Rejected("vertex storage capability"));
                    }
                    let max = match ty {
                        Bt::Uniform => u64::from(l.max_uniform_buffer_binding_size),
                        Bt::Storage { .. } => l.max_storage_buffer_binding_size,
                    };
                    if min_binding_size.is_some_and(|size| size.get() > max) {
                        return Err(Error::Rejected(
                            "minimum buffer binding size exceeds limit",
                        ));
                    }
                    if matches!(ty, Bt::Storage { read_only: false })
                        && e.visibility.intersects(Stages::VERTEX | Stages::FRAGMENT)
                    {
                        return Err(Error::Rejected(
                            "writable graphics storage bindings not implemented",
                        ));
                    }
                }
                Ty::Texture {
                    multisampled,
                    view_dimension,
                    sample_type,
                } => {
                    if multisampled
                        && (view_dimension != types::TextureViewDimension::D2
                            || sample_type == Sample::Float { filterable: true })
                    {
                        return Err(Error::Rejected("multisample binding dimension/sample type"));
                    }
                }
                Ty::Sampler(_) => {}
                _ => return Err(Error::Rejected("binding class not implemented")),
            }
        }
        // Cold, bounded structural cache; hot render paths compare identities.
        let mut cache = self.owner.group_layouts.borrow_mut();
        cache.retain(|w| w.strong_count() != 0);
        if let Some(layout) = cache
            .iter()
            .filter_map(|w| w.upgrade())
            .find(|p| p.entries == entries)
        {
            return Ok(GroupLayout(layout));
        }
        let id = id()?;
        let raw = unsafe {
            self.owner
                .device
                .create_bind_group_layout(&hal::BindGroupLayoutDescriptor {
                    label: Some("native group layout"),
                    flags: hal::BindGroupLayoutFlags::empty(),
                    entries: &entries,
                })
        }
        .map_err(|e| self.owner.device_error(e))?;
        let inner = Rc::new(GroupLayoutInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            entries,
            id,
        });
        if cache.len() < 256 {
            cache.push(Rc::downgrade(&inner));
        }
        Ok(GroupLayout(inner))
    }
    pub fn pipeline_layout(&self, groups: &[GroupLayout<B>]) -> Result<PipelineLayout<B>> {
        self.owner.healthy()?;
        let l = &self.owner.limits;
        if groups.len() > 32
            || groups.len() > l.max_bind_groups as usize
            || groups.iter().any(|g| !Rc::ptr_eq(&self.owner, &g.0.owner))
        {
            return Err(Error::Rejected("pipeline group count/device"));
        }
        let mut dynamic = [0u32; 2];
        let mut counts = [[0u32; 4]; 3];
        for g in groups {
            for e in g.entries() {
                let category = match e.ty {
                    Ty::Buffer {
                        ty: Bt::Uniform,
                        has_dynamic_offset,
                        ..
                    } => {
                        dynamic[0] += u32::from(has_dynamic_offset);
                        0
                    }
                    Ty::Buffer {
                        has_dynamic_offset, ..
                    } => {
                        dynamic[1] += u32::from(has_dynamic_offset);
                        1
                    }
                    Ty::Texture { .. } => 2,
                    Ty::Sampler(_) => 3,
                    _ => unreachable!(),
                };
                for (i, stage) in [Stages::VERTEX, Stages::FRAGMENT, Stages::COMPUTE]
                    .into_iter()
                    .enumerate()
                {
                    if e.visibility.contains(stage) {
                        counts[i][category] += 1;
                    }
                }
            }
        }
        if dynamic[0] > l.max_dynamic_uniform_buffers_per_pipeline_layout
            || dynamic[1] > l.max_dynamic_storage_buffers_per_pipeline_layout
            || counts.iter().any(|c| {
                c[0] > l.max_uniform_buffers_per_shader_stage
                    || c[1] > l.max_storage_buffers_per_shader_stage
                    || c[2] > l.max_sampled_textures_per_shader_stage
                    || c[3] > l.max_samplers_per_shader_stage
            })
        {
            return Err(Error::Rejected("pipeline binding limits"));
        }
        let raw_groups: Vec<_> = groups.iter().map(|g| Some(g.raw())).collect();
        let id = id()?;
        let raw = unsafe {
            self.owner
                .device
                .create_pipeline_layout(&hal::PipelineLayoutDescriptor {
                    label: Some("native pipeline layout"),
                    flags: hal::PipelineLayoutFlags::empty(),
                    bind_group_layouts: &raw_groups,
                    immediate_size: 0,
                })
        }
        .map_err(|e| self.owner.device_error(e))?;
        Ok(PipelineLayout(Rc::new(PipelineLayoutInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            groups: groups.to_vec(),
            id,
        })))
    }
    pub fn sampler(&self, desc: &types::SamplerDescriptor<Option<&str>>) -> Result<Sampler<B>> {
        self.owner.healthy()?;
        let modes = [
            desc.address_mode_u,
            desc.address_mode_v,
            desc.address_mode_w,
        ];
        if modes.contains(&types::AddressMode::ClampToBorder)
            || desc.border_color.is_some()
            || !(1..=16).contains(&desc.anisotropy_clamp)
            || !desc.lod_min_clamp.is_finite()
            || !desc.lod_max_clamp.is_finite()
            || desc.lod_min_clamp < 0.
            || desc.lod_max_clamp < desc.lod_min_clamp
        {
            return Err(Error::Rejected("sampler bounds/features"));
        }
        if desc.anisotropy_clamp > 1
            && (!self
                .owner
                .downlevel
                .contains(types::DownlevelFlags::ANISOTROPIC_FILTERING)
                || desc.min_filter != types::FilterMode::Linear
                || desc.mag_filter != types::FilterMode::Linear
                || desc.mipmap_filter != types::MipmapFilterMode::Linear)
        {
            return Err(Error::Rejected("sampler anisotropy capability/filters"));
        }
        if desc.compare.is_some()
            && !self
                .owner
                .downlevel
                .contains(types::DownlevelFlags::COMPARISON_SAMPLERS)
        {
            return Err(Error::Rejected("comparison sampler capability"));
        }
        let raw = unsafe {
            self.owner.device.create_sampler(&hal::SamplerDescriptor {
                label: desc.label,
                address_modes: modes,
                mag_filter: desc.mag_filter,
                min_filter: desc.min_filter,
                mipmap_filter: desc.mipmap_filter,
                lod_clamp: desc.lod_min_clamp..desc.lod_max_clamp,
                compare: desc.compare,
                anisotropy_clamp: desc.anisotropy_clamp,
                border_color: None,
            })
        }
        .map_err(|e| self.owner.device_error(e))?;
        let filtering = desc.mag_filter == types::FilterMode::Linear
            || desc.min_filter == types::FilterMode::Linear
            || desc.mipmap_filter == types::MipmapFilterMode::Linear;
        Ok(Sampler(Rc::new(SamplerInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            filtering,
            comparison: desc.compare.is_some(),
        })))
    }
    pub fn bind_group(
        &self,
        layout: &GroupLayout<B>,
        entries: &[Entry<'_, B>],
    ) -> Result<Group<B>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &layout.0.owner) || entries.len() != layout.entries().len() {
            return Err(Error::Rejected("bind group layout/device/count"));
        }
        let mut ordered: Vec<_> = entries.iter().collect();
        ordered.sort_by_key(|e| e.binding);
        let mut binding_sizes = BTreeMap::new();
        let mut buffers = Vec::new();
        let mut textures = Vec::new();
        let mut samplers = Vec::new();
        let mut dynamic = Vec::new();
        let mut raw_buffers = Vec::new();
        let mut raw_textures = Vec::new();
        let mut raw_samplers = Vec::new();
        let mut raw_entries = Vec::new();
        let mut scope = BTreeMap::new();
        for (e, slot) in ordered.iter().zip(layout.entries()) {
            if e.binding != slot.binding {
                return Err(Error::Rejected("binding index"));
            }
            let index = match (&e.resource, slot.ty) {
                (
                    Resource::Buffer {
                        buffer,
                        offset,
                        size,
                    },
                    Ty::Buffer {
                        ty,
                        has_dynamic_offset,
                        min_binding_size,
                    },
                ) => {
                    let (usage, alignment, max) = match ty {
                        Bt::Uniform => (
                            U::UNIFORM,
                            u64::from(self.owner.limits.min_uniform_buffer_offset_alignment),
                            u64::from(self.owner.limits.max_uniform_buffer_binding_size),
                        ),
                        Bt::Storage { read_only } => (
                            if read_only {
                                U::STORAGE_READ_ONLY
                            } else {
                                U::STORAGE_READ_WRITE
                            },
                            u64::from(self.owner.limits.min_storage_buffer_offset_alignment),
                            self.owner.limits.max_storage_buffer_binding_size,
                        ),
                    };
                    if !Rc::ptr_eq(&self.owner, &buffer.0.owner)
                        || !buffer.0.usage.contains(usage)
                        || *size == 0
                        || min_binding_size.is_some_and(|n| *size < n.get())
                        || *size > max
                        || offset % alignment != 0
                    {
                        return Err(Error::Rejected(
                            "binding buffer device/usage/size/alignment",
                        ));
                    }
                    buffer.range(*offset, *size)?;
                    if let Some(prior) = scope.insert(buffer.id(), usage) {
                        if !U::INCLUSIVE.contains(prior | usage) {
                            return Err(Error::Rejected(
                                "writable binding aliases another resource",
                            ));
                        }
                    }
                    let index = buffers.len();
                    if has_dynamic_offset {
                        dynamic.push(Dynamic {
                            buffer_index: index,
                            base: *offset,
                            size: *size,
                            alignment,
                        });
                    }
                    binding_sizes.insert(e.binding, *size);
                    buffers.push(((*buffer).clone(), usage));
                    raw_buffers.push(hal::BufferBinding::new_unchecked(
                        buffer.raw(),
                        *offset,
                        NonZeroU64::new(*size),
                    ));
                    index
                }
                (
                    Resource::Texture(view),
                    Ty::Texture {
                        sample_type,
                        view_dimension,
                        multisampled,
                    },
                ) => {
                    if !view.belongs_to(&self.owner)
                        || view.dimension() != view_dimension
                        || !view.texture().descriptor().usage.contains(T::RESOURCE)
                        || (view.texture().descriptor().sample_count > 1) != multisampled
                    {
                        return Err(Error::Rejected("binding texture device/dimension/usage"));
                    }
                    let actual = view
                        .texture()
                        .descriptor()
                        .format
                        .sample_type(Some(view.range().aspect), Some(self.owner.features));
                    let compatible = actual == Some(sample_type)
                        || matches!(
                            (actual, sample_type),
                            (
                                Some(Sample::Float { .. }),
                                Sample::Float { filterable: false }
                            )
                        );
                    if !compatible {
                        return Err(Error::Rejected("binding texture sample type"));
                    }
                    let index = textures.len();
                    textures.push((*view).clone());
                    raw_textures.push(hal::TextureBinding {
                        view: view.raw(),
                        usage: T::RESOURCE,
                    });
                    index
                }
                (Resource::Sampler(sampler), Ty::Sampler(kind)) => {
                    if !Rc::ptr_eq(&self.owner, &sampler.0.owner)
                        || (kind == St::Comparison) != sampler.0.comparison
                        || kind == St::NonFiltering && sampler.0.filtering
                    {
                        return Err(Error::Rejected("binding sampler kind/device"));
                    }
                    let index = samplers.len();
                    samplers.push((*sampler).clone());
                    raw_samplers.push(sampler.0.raw.as_ref().unwrap());
                    index
                }
                _ => return Err(Error::Rejected("binding resource class")),
            };
            raw_entries.push(hal::BindGroupEntry {
                binding: e.binding,
                resource_index: index as u32,
                count: 1,
            });
        }
        let id = id()?;
        let raw = unsafe {
            self.owner
                .device
                .create_bind_group(&hal::BindGroupDescriptor {
                    label: Some("native immutable group"),
                    layout: layout.raw(),
                    buffers: &raw_buffers,
                    textures: &raw_textures,
                    samplers: &raw_samplers,
                    entries: &raw_entries,
                    acceleration_structures: &[],
                    external_textures: &[],
                })
        }
        .map_err(|e| self.owner.device_error(e))?;
        Ok(Group(Rc::new(GroupInner {
            layout: layout.clone(),
            raw: Some(raw),
            id,
            buffers,
            textures,
            _samplers: samplers,
            dynamic,
            binding_sizes,
            plan_epoch: Cell::new(0),
        })))
    }
}
