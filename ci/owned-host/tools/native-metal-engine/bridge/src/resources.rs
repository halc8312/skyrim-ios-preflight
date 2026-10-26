use crate::*;
use device::Owner;
use native::{bindings as b, render as r, texture as t};

pub(crate) struct Handle<T> {
    pub id: usize,
    pub owner: Owner,
    pub value: Result<T>,
    pub destroyed: Cell<bool>,
}
impl<T> Handle<T> {
    fn new(device: &Device, value: Result<T>) -> Rc<Self> {
        Rc::new(Self {
            id: next_identity(),
            owner: device.owner.clone(),
            value,
            destroyed: Cell::new(false),
        })
    }
    pub fn get(&self) -> Result<&T> {
        if self.destroyed.get() {
            return Err(Error::Invalid("destroyed resource".into()));
        }
        self.value.as_ref().map_err(Clone::clone)
    }
}

fn next_identity() -> usize {
    use std::sync::atomic::{AtomicUsize, Ordering};
    static NEXT: AtomicUsize = AtomicUsize::new(1);
    NEXT.try_update(Ordering::Relaxed, Ordering::Relaxed, |v| v.checked_add(1))
        .expect("native resource identity exhausted")
}
macro_rules! handle {
    ($name:ident,$ty:ty) => {
        #[derive(Clone)]
        pub struct $name(pub(crate) Rc<Handle<$ty>>);
        impl $name {
            pub fn identity(&self) -> usize {
                self.0.id
            }
            pub(crate) fn native(&self) -> Result<&$ty> {
                self.0.get()
            }
        }
        impl std::fmt::Debug for $name {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.debug_tuple(stringify!($name))
                    .field(&self.identity())
                    .finish()
            }
        }
    };
}
handle!(Sampler, b::Sampler<Platform>);
handle!(BindGroupLayout, b::GroupLayout<Platform>);
#[derive(Clone)]
pub struct PipelineLayout {
    pub(crate) handle: Rc<Handle<b::PipelineLayout<Platform>>>,
    pub(crate) groups: Vec<BindGroupLayout>,
}
impl std::fmt::Debug for PipelineLayout {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativePipelineLayout")
    }
}
impl PipelineLayout {
    pub(crate) fn native(&self) -> Result<&b::PipelineLayout<Platform>> {
        self.handle.get()
    }
}
#[derive(Clone)]
pub struct ShaderModule {
    pub(crate) handle: Rc<Handle<native::shader::Shader<Platform>>>,
    pub(crate) source: Rc<str>,
}
impl std::fmt::Debug for ShaderModule {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativeShaderModule")
    }
}
#[derive(Clone)]
pub(crate) enum Dependency {
    Buffer(Buffer),
    Texture(TextureView),
    WholeTexture(Texture),
    Group(BindGroup),
}
impl Dependency {
    pub(crate) fn valid(&self) -> Result<()> {
        match self {
            Self::Buffer(b) => b.gpu_ready(),
            Self::Texture(t) => t.native().map(|_| ()),
            Self::WholeTexture(t) => t.native().map(|_| ()),
            Self::Group(g) => {
                g.native()?;
                for d in g.dependencies.iter() {
                    d.valid()?;
                }
                Ok(())
            }
        }
    }
}
#[derive(Clone)]
pub struct BindGroup {
    pub(crate) handle: Rc<Handle<b::Group<Platform>>>,
    pub(crate) dependencies: Rc<[Dependency]>,
}
impl std::fmt::Debug for BindGroup {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("NativeBindGroup")
            .field(&self.identity())
            .finish()
    }
}
impl BindGroup {
    pub fn identity(&self) -> usize {
        self.handle.id
    }
    pub(crate) fn native(&self) -> Result<&b::Group<Platform>> {
        self.handle.get()
    }
}
#[derive(Clone)]
pub struct RenderPipeline {
    pub(crate) handle: Rc<Handle<r::Pipeline<Platform>>>,
    pub(crate) layout: Option<PipelineLayout>,
}
#[derive(Clone)]
pub struct ComputePipeline {
    pub(crate) handle: Rc<Handle<native::compute::SharedComputePipeline<Platform>>>,
    pub(crate) layout: Option<PipelineLayout>,
}
macro_rules! pipeline_impl {
    ($ty:ident,$native:ty) => {
        impl $ty {
            pub fn identity(&self) -> usize {
                self.handle.id
            }
            pub(crate) fn native(&self) -> Result<&$native> {
                self.handle.get()
            }
            pub fn get_bind_group_layout(&self, index: u32) -> BindGroupLayout {
                if let Some(v) = self
                    .layout
                    .as_ref()
                    .and_then(|l| l.groups.get(index as usize))
                {
                    return v.clone();
                }
                let e = Error::Invalid("pipeline group index".into());
                self.handle.owner.borrow_mut().report(e.clone(), false);
                BindGroupLayout(Rc::new(Handle {
                    id: next_identity(),
                    owner: self.handle.owner.clone(),
                    value: Err(e),
                    destroyed: Cell::new(false),
                }))
            }
        }
        impl std::fmt::Debug for $ty {
            fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.debug_tuple(stringify!($ty))
                    .field(&self.identity())
                    .finish()
            }
        }
    };
}
pipeline_impl!(RenderPipeline, r::Pipeline<Platform>);
pipeline_impl!(
    ComputePipeline,
    native::compute::SharedComputePipeline<Platform>
);

pub(crate) struct BufferInner {
    pub handle: Rc<Handle<native::Buffer<Platform>>>,
    pub label: String,
    pub size: u64,
    pub usage: BufferUsage,
    pub mapped: RefCell<Option<Mapping>>,
    pub write_shadow: RefCell<Option<Vec<u8>>>,
    pub mapping: Cell<bool>,
    pub epoch: Cell<u64>,
}
pub(crate) struct Mapping {
    pub range: std::ops::Range<u64>,
    pub bytes: Vec<u8>,
    pub write: bool,
}
impl Drop for BufferInner {
    fn drop(&mut self) {
        memory::untrack(self.handle.id);
    }
}
#[derive(Clone)]
pub struct Buffer(pub(crate) Rc<BufferInner>);
impl std::fmt::Debug for Buffer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("NativeBuffer")
            .field("label", &self.0.label)
            .field("size", &self.0.size)
            .finish()
    }
}
impl Buffer {
    pub fn identity(&self) -> usize {
        self.0.handle.id
    }
    pub fn size(&self) -> u64 {
        self.0.size
    }
    pub fn label(&self) -> &str {
        &self.0.label
    }
    pub fn usage(&self) -> BufferUsage {
        self.0.usage
    }
    pub fn is_mapped(&self) -> bool {
        self.0.mapped.borrow().is_some()
    }
    pub(crate) fn gpu_ready(&self) -> Result<()> {
        self.0.handle.get()?;
        if self.is_mapped() || self.0.mapping.get() {
            return Err(Error::Invalid(
                "mapped or pending-map buffer used by GPU".into(),
            ));
        }
        Ok(())
    }
    // Mapping is validated at submission, after recorded staging copies have
    // been unmapped. Recording commands with a currently mapped buffer is legal.
    pub(crate) fn native(&self) -> Result<&native::Buffer<Platform>> {
        self.0.handle.get()
    }
    pub fn map_async(&self, mode: MapMode, offset: u64, size: Option<u64>) -> Pending<Result<()>> {
        let result = (|| {
            self.0.handle.get()?;
            let size = size.unwrap_or_else(|| self.size().saturating_sub(offset));
            let end = offset
                .checked_add(size)
                .ok_or_else(|| Error::Map("range overflow".into()))?;
            if offset % 8 != 0
                || size % 4 != 0
                || end > self.size()
                || self.is_mapped()
                || self.0.mapping.get()
            {
                return Err(Error::Map("range/alignment/already mapped".into()));
            }
            if (mode != MapMode::Read && mode != MapMode::Write)
                || (mode == MapMode::Read && !self.usage().contains(BufferUsage::MapRead))
                || (mode == MapMode::Write && !self.usage().contains(BufferUsage::MapWrite))
            {
                return Err(Error::Map("map usage".into()));
            }
            Ok((offset..end, mode == MapMode::Write))
        })();
        let (range, write) = match result {
            Ok(v) => v,
            Err(e) => return Pending::ready(Err(e)),
        };
        self.0.mapping.set(true);
        let this = self.clone();
        let epoch = self.0.epoch.get();
        let cancel = self.clone();
        let serial = self.0.handle.owner.borrow().serial;
        let deadline = Instant::now() + Duration::from_secs(15);
        Pending::polling(move || {
            let result = (|| {
                if this.0.epoch.get() != epoch || this.0.handle.destroyed.get() {
                    return Err(Error::Map("mapping cancelled".into()));
                }
                let mut c = this.0.handle.owner.borrow_mut();
                if c.poll()? < serial {
                    if Instant::now() >= deadline {
                        return Err(Error::Map("completion deadline".into()));
                    }
                    return Ok(None);
                }
                let bytes = if write {
                    this.0
                        .write_shadow
                        .borrow()
                        .as_ref()
                        .ok_or_else(|| Error::Map("write shadow missing".into()))?
                        [range.start as usize..range.end as usize]
                        .to_vec()
                } else {
                    c.gpu
                        .read_buffer(this.0.handle.get()?, range.clone(), Duration::from_secs(5))
                        .map_err(convert::err)?
                };
                *this.0.mapped.borrow_mut() = Some(Mapping {
                    range: range.clone(),
                    bytes,
                    write,
                });
                Ok(Some(()))
            })();
            match result {
                Ok(None) => None,
                Ok(Some(())) => {
                    this.0.mapping.set(false);
                    Some(Ok(()))
                }
                Err(e) => {
                    if this.0.epoch.get() == epoch {
                        this.0.mapping.set(false);
                    }
                    Some(Err(e))
                }
            }
        })
        .on_drop(move || {
            if cancel.0.epoch.get() == epoch && cancel.0.mapping.get() {
                cancel.0.mapping.set(false);
                cancel.0.epoch.set(epoch.wrapping_add(1));
            }
        })
    }
    pub fn read_mapped(&self, offset: u64, dst: &mut [u8]) -> Result<()> {
        let m = self.0.mapped.borrow();
        let m = m.as_ref().ok_or_else(|| Error::Map("not mapped".into()))?;
        let r = mapping_range(m, offset, dst.len())?;
        dst.copy_from_slice(&m.bytes[r]);
        Ok(())
    }
    pub fn write_mapped(&self, offset: u64, src: &[u8]) -> Result<()> {
        let mut m = self.0.mapped.borrow_mut();
        let m = m.as_mut().ok_or_else(|| Error::Map("not mapped".into()))?;
        if !m.write {
            return Err(Error::Map("read-only mapping".into()));
        }
        let r = mapping_range(m, offset, src.len())?;
        m.bytes[r].copy_from_slice(src);
        Ok(())
    }
    pub fn unmap(&self) {
        self.0.epoch.set(self.0.epoch.get().wrapping_add(1));
        self.0.mapping.set(false);
        if let Some(m) = self.0.mapped.borrow_mut().take() {
            if m.write {
                if let Some(shadow) = self.0.write_shadow.borrow_mut().as_mut() {
                    shadow[m.range.start as usize..m.range.end as usize].copy_from_slice(&m.bytes);
                }
                let mut c = self.0.handle.owner.borrow_mut();
                let result = c
                    .check()
                    .and_then(|_| c.write(self.0.handle.get()?, m.range.start, &m.bytes));
                if let Err(e) = result {
                    c.report(e, true);
                }
            }
        }
    }
    pub fn destroy(&self) {
        self.0.handle.destroyed.set(true);
        self.0.epoch.set(self.0.epoch.get().wrapping_add(1));
        self.0.mapping.set(false);
        self.0.mapped.borrow_mut().take();
        memory::untrack(self.identity());
    }
}
fn mapping_range(m: &Mapping, offset: u64, len: usize) -> Result<std::ops::Range<usize>> {
    let end = offset
        .checked_add(len as u64)
        .ok_or_else(|| Error::Map("overflow".into()))?;
    if offset < m.range.start || end > m.range.end {
        return Err(Error::Map("mapped range".into()));
    }
    Ok((offset - m.range.start) as usize..(end - m.range.start) as usize)
}

pub(crate) struct TextureInner {
    pub handle: Rc<Handle<t::Texture<Platform>>>,
    pub format: TextureFormat,
    pub usage: TextureUsage,
    pub size: Extent3d,
    pub mip_levels: u32,
    pub samples: u32,
    pub dimension: TextureDimension,
}
impl Drop for TextureInner {
    fn drop(&mut self) {
        memory::untrack(self.handle.id);
    }
}
#[derive(Clone)]
pub struct Texture(pub(crate) Rc<TextureInner>);
impl std::fmt::Debug for Texture {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("NativeTexture")
            .field(&self.identity())
            .finish()
    }
}
#[derive(Clone)]
pub struct TextureView {
    pub(crate) handle: Rc<Handle<t::TextureView<Platform>>>,
    pub(crate) texture: Texture,
}
impl std::fmt::Debug for TextureView {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_tuple("NativeTextureView")
            .field(&self.identity())
            .finish()
    }
}
impl TextureView {
    pub fn identity(&self) -> usize {
        self.handle.id
    }
    pub(crate) fn native(&self) -> Result<&t::TextureView<Platform>> {
        self.texture.native()?;
        self.handle.get()
    }
}
impl Texture {
    #[cfg(target_vendor = "apple")]
    pub(crate) fn from_drawable(
        device: &Device,
        texture: native::texture::Texture<Platform>,
    ) -> Self {
        let d = texture.descriptor().clone();
        Self(Rc::new(TextureInner {
            handle: Handle::new(device, Ok(texture)),
            format: TextureFormat::BGRA8Unorm,
            usage: TextureUsage::RenderAttachment,
            size: extent(d.size.width, d.size.height, 1),
            mip_levels: 1,
            samples: 1,
            dimension: TextureDimension::D2,
        }))
    }

    pub fn identity(&self) -> usize {
        self.0.handle.id
    }
    pub(crate) fn native(&self) -> Result<&t::Texture<Platform>> {
        self.0.handle.get()
    }
    pub fn width(&self) -> u32 {
        self.0.size.width
    }
    pub fn height(&self) -> u32 {
        self.0.size.height
    }
    pub fn depth_or_array_layers(&self) -> u32 {
        self.0.size.depthOrArrayLayers
    }
    pub fn mip_level_count(&self) -> u32 {
        self.0.mip_levels
    }
    pub fn sample_count(&self) -> u32 {
        self.0.samples
    }
    pub fn format(&self) -> TextureFormat {
        self.0.format
    }
    pub fn usage(&self) -> TextureUsage {
        self.0.usage
    }
    pub fn destroy(&self) {
        self.0.handle.destroyed.set(true);
        memory::untrack(self.identity());
    }
    pub fn create_view(&self, desc: Option<&TextureViewDescriptor<'_>>) -> TextureView {
        let d = desc.copied().unwrap_or_default();
        let dev = Device::from_owner(self.0.handle.owner.clone());
        let value = dev.resource(|gpu| {
            let tex = self.native()?;
            if d.format != TextureFormat::Undefined && d.format != self.format() {
                return Err(Error::Unsupported("texture view format reinterpretation"));
            }
            if d.usage != TextureUsage::None && d.usage.0 & !self.usage().0 != 0 {
                return Err(Error::Invalid("texture view usage".into()));
            }
            let dimension = if d.dimension == TextureViewDimension::Undefined {
                match self.0.dimension {
                    TextureDimension::D1 => types::TextureViewDimension::D1,
                    TextureDimension::D3 => types::TextureViewDimension::D3,
                    _ if self.depth_or_array_layers() > 1 => types::TextureViewDimension::D2Array,
                    _ => types::TextureViewDimension::D2,
                }
            } else {
                convert::view_dimension(d.dimension)?
            };
            let layers = if self.0.dimension == TextureDimension::D3 {
                1
            } else {
                self.depth_or_array_layers()
            };
            let mips_end = d
                .base_mip_level
                .checked_add(
                    d.mip_level_count
                        .unwrap_or_else(|| self.mip_level_count().saturating_sub(d.base_mip_level)),
                )
                .ok_or_else(|| Error::Invalid("view mip range".into()))?;
            let layer_end = d
                .base_array_layer
                .checked_add(
                    d.array_layer_count
                        .unwrap_or_else(|| layers.saturating_sub(d.base_array_layer)),
                )
                .ok_or_else(|| Error::Invalid("view layer range".into()))?;
            let aspect = if d.aspect == TextureAspect::Undefined {
                types::TextureAspect::All
            } else {
                convert::aspect(d.aspect)?
            };
            gpu.texture_view_aspect(
                tex,
                dimension,
                d.base_mip_level..mips_end,
                d.base_array_layer..layer_end,
                aspect,
            )
            .map_err(convert::err)
        });
        counters::add(&counters::count::views, 1);
        TextureView {
            handle: Handle::new(&dev, value),
            texture: self.clone(),
        }
    }
}
impl Device {
    pub(crate) fn from_owner(owner: Owner) -> Self {
        Self {
            queue: Queue {
                owner: owner.clone(),
            },
            owner,
        }
    }
    pub fn create_buffer(&self, d: &BufferDescriptor<'_>) -> Buffer {
        let value = self.resource(|gpu| {
            if d.mapped_at_creation && d.size % 4 != 0 {
                return Err(Error::Invalid("mapped-at-creation size alignment".into()));
            }
            use types::BufferUses as U;
            let known = BufferUsage::MapRead
                | BufferUsage::MapWrite
                | BufferUsage::CopySrc
                | BufferUsage::CopyDst
                | BufferUsage::Storage
                | BufferUsage::Uniform
                | BufferUsage::Index;
            if d.usage.0 & !known.0 != 0 {
                return Err(Error::Unsupported("vertex/indirect/query buffer usage"));
            }
            if d.usage.contains(BufferUsage::MapRead)
                && d.usage.0 & !(BufferUsage::MapRead | BufferUsage::CopyDst).0 != 0
            {
                return Err(Error::Invalid("map-read usage combination".into()));
            }
            if d.usage.contains(BufferUsage::MapWrite)
                && d.usage.0 & !(BufferUsage::MapWrite | BufferUsage::CopySrc).0 != 0
            {
                return Err(Error::Invalid("map-write usage combination".into()));
            }
            let mut u = U::empty();
            for (a, b) in [
                (BufferUsage::MapRead, U::MAP_READ),
                (BufferUsage::CopySrc, U::COPY_SRC),
                (BufferUsage::CopyDst, U::COPY_DST),
                (
                    BufferUsage::Storage,
                    U::STORAGE_READ_ONLY | U::STORAGE_READ_WRITE,
                ),
                (BufferUsage::Uniform, U::UNIFORM),
                (BufferUsage::Index, U::INDEX),
            ] {
                if d.usage.contains(a) {
                    u |= b;
                }
            }
            if d.mapped_at_creation || d.usage.contains(BufferUsage::MapWrite) {
                u |= U::COPY_DST;
            }
            gpu.create_buffer(d.label, d.size, u).map_err(convert::err)
        });
        let mapped = if value.is_ok() && d.mapped_at_creation {
            Some(Mapping {
                range: 0..d.size,
                bytes: vec![0; d.size as usize],
                write: true,
            })
        } else {
            None
        };
        let write_shadow = if value.is_ok() && d.usage.contains(BufferUsage::MapWrite) {
            Some(vec![0; d.size as usize])
        } else {
            None
        };
        let buffer = Buffer(Rc::new(BufferInner {
            handle: Handle::new(self, value),
            label: d.label.into(),
            size: d.size,
            usage: d.usage,
            mapped: RefCell::new(mapped),
            write_shadow: RefCell::new(write_shadow),
            mapping: Cell::new(false),
            epoch: Cell::new(0),
        }));
        counters::add(&counters::count::buffers, 1);
        if buffer.0.handle.value.is_ok() {
            memory::track(buffer.identity(), "buffer", d.label, d.size);
        }
        buffer
    }
    pub fn create_buffer_init(&self, label: &str, usage: BufferUsage, data: &[u8]) -> Buffer {
        let size = (data.len() as u64).div_ceil(4) * 4;
        let b = self.create_buffer(&BufferDescriptor {
            label,
            size,
            usage,
            mapped_at_creation: true,
        });
        if let Err(e) = b.write_mapped(0, data) {
            self.owner.borrow_mut().report(e, false);
        }
        b.unmap();
        b
    }
    pub fn create_texture(&self, d: &TextureDescriptor<'_>) -> Texture {
        let value = self.resource(|gpu| {
            if d.view_formats.iter().any(|&f| f != d.format) {
                return Err(Error::Unsupported("texture view formats"));
            }
            let format = convert::format(d.format)?;
            gpu.create_texture(
                d.label,
                t::TextureDescriptor {
                    size: convert::size(d.size),
                    mip_levels: d.mip_level_count,
                    sample_count: d.sample_count,
                    dimension: convert::dimension(d.dimension)?,
                    format,
                    usage: convert::texture_usage(d.usage, format)?,
                },
            )
            .map_err(convert::err)
        });
        let texture = Texture(Rc::new(TextureInner {
            handle: Handle::new(self, value),
            format: d.format,
            usage: d.usage,
            size: d.size,
            mip_levels: d.mip_level_count,
            samples: d.sample_count,
            dimension: d.dimension,
        }));
        counters::add(&counters::count::textures, 1);
        counters::created("texture", d.label);
        if texture.0.handle.value.is_ok() {
            memory::track(
                texture.identity(),
                "texture",
                d.label,
                memory::texture_bytes(
                    d.format,
                    d.size.width,
                    d.size.height,
                    d.size.depthOrArrayLayers,
                    d.mip_level_count,
                    d.sample_count,
                ),
            );
        }
        texture
    }
    pub fn create_sampler(&self, d: &SamplerDescriptor<'_>) -> Sampler {
        let value = self.resource(|gpu| {
            gpu.sampler(&types::SamplerDescriptor {
                label: Some(d.label),
                address_mode_u: convert::address(d.address_mode_u)?,
                address_mode_v: convert::address(d.address_mode_v)?,
                address_mode_w: convert::address(d.address_mode_w)?,
                mag_filter: convert::filter(d.mag_filter)?,
                min_filter: convert::filter(d.min_filter)?,
                mipmap_filter: convert::mip_filter(d.mipmap_filter)?,
                lod_min_clamp: d.lod_min_clamp,
                lod_max_clamp: d.lod_max_clamp,
                compare: d.compare.map(convert::compare).transpose()?,
                anisotropy_clamp: d.max_anisotropy,
                border_color: None,
            })
            .map_err(convert::err)
        });
        counters::add(&counters::count::samplers, 1);
        Sampler(Handle::new(self, value))
    }
    pub fn create_shader_module_wgsl(&self, _label: &str, code: &str) -> ShaderModule {
        let value = self.resource(|gpu| match gpu.shader(code) {
            Err(native::Error::Rejected("explicit shader specialization required")) => {
                gpu.shader_specialized(code, &[]).map_err(convert::err)
            }
            other => other.map_err(convert::err),
        });
        counters::add(&counters::count::shader_modules, 1);
        counters::add(&counters::count::shader_bytes, code.len() as u64);
        ShaderModule {
            handle: Handle::new(self, value),
            source: Rc::from(code),
        }
    }
    pub fn create_bind_group_layout(
        &self,
        _label: &str,
        entries: &[BindGroupLayoutEntry],
    ) -> BindGroupLayout {
        let value = self.resource(|gpu| {
            let entries = entries
                .iter()
                .copied()
                .map(convert::binding)
                .collect::<Result<Vec<_>>>()?;
            gpu.group_layout(&entries).map_err(convert::err)
        });
        BindGroupLayout(Handle::new(self, value))
    }
    pub fn create_pipeline_layout(
        &self,
        _label: &str,
        groups: &[&BindGroupLayout],
    ) -> PipelineLayout {
        let value = self.resource(|gpu| {
            let groups = groups
                .iter()
                .map(|g| g.native().cloned())
                .collect::<Result<Vec<_>>>()?;
            gpu.pipeline_layout(&groups).map_err(convert::err)
        });
        PipelineLayout {
            handle: Handle::new(self, value),
            groups: groups.iter().map(|&g| g.clone()).collect(),
        }
    }
    pub fn create_bind_group(
        &self,
        label: &str,
        layout: &BindGroupLayout,
        entries: &[BindGroupEntry<'_>],
    ) -> BindGroup {
        let value = self.resource(|gpu| {
            let entries = entries
                .iter()
                .map(|e| {
                    Ok(b::Entry {
                        binding: e.binding,
                        resource: match e.resource {
                            BindingResource::Buffer {
                                buffer,
                                offset,
                                size,
                            } => b::Resource::Buffer {
                                buffer: buffer.native()?,
                                offset,
                                size: match size {
                                    Some(s) => s,
                                    None => buffer.size().checked_sub(offset).ok_or_else(|| {
                                        Error::Invalid("buffer binding offset".into())
                                    })?,
                                },
                            },
                            BindingResource::Sampler(s) => b::Resource::Sampler(s.native()?),
                            BindingResource::TextureView(v) => b::Resource::Texture(v.native()?),
                        },
                    })
                })
                .collect::<Result<Vec<_>>>()?;
            gpu.bind_group(layout.native()?, &entries)
                .map_err(convert::err)
        });
        let dependencies = entries
            .iter()
            .filter_map(|e| match e.resource {
                BindingResource::Buffer { buffer, .. } => Some(Dependency::Buffer(buffer.clone())),
                BindingResource::TextureView(v) => Some(Dependency::Texture(v.clone())),
                _ => None,
            })
            .collect();
        counters::add(&counters::count::bind_groups, 1);
        counters::created("bind group", label);
        BindGroup {
            handle: Handle::new(self, value),
            dependencies,
        }
    }
    fn specialized(
        &self,
        gpu: &NativeGpu,
        s: ProgrammableStage<'_>,
    ) -> Result<native::shader::Shader<Platform>> {
        if !Rc::ptr_eq(&self.owner, &s.module.handle.owner) {
            return Err(Error::Invalid("cross-device shader module".into()));
        }
        if s.constants.is_empty() {
            s.module.handle.get().cloned()
        } else {
            gpu.shader_specialized(&s.module.source, s.constants)
                .map_err(convert::err)
        }
    }
    fn inferred(
        &self,
        gpu: &NativeGpu,
        stages: &[(&native::shader::Shader<Platform>, &str, types::ShaderStages)],
    ) -> Result<PipelineLayout> {
        let (layout, groups) = gpu.inferred_layout(stages).map_err(convert::err)?;
        Ok(PipelineLayout {
            handle: Handle::new(self, Ok(layout)),
            groups: groups
                .into_iter()
                .map(|g| BindGroupLayout(Handle::new(self, Ok(g))))
                .collect(),
        })
    }
    pub fn create_render_pipeline(&self, d: &RenderPipelineDescriptor<'_>) -> RenderPipeline {
        let result = self.resource(|gpu| {
            if !d.vertex_buffers.is_empty() {
                return Err(Error::Unsupported(
                    "vertex layouts; renderer uses vertex pulling",
                ));
            }
            let vs = self.specialized(gpu, d.vertex)?;
            let fs = d
                .fragment
                .map(|f| self.specialized(gpu, f.stage))
                .transpose()?;
            let mut stages = vec![(&vs, d.vertex.entry_point, types::ShaderStages::VERTEX)];
            if let (Some(fs), Some(f)) = (&fs, d.fragment) {
                stages.push((fs, f.stage.entry_point, types::ShaderStages::FRAGMENT));
            }
            let layout = match d.layout {
                Some(l) => l.clone(),
                None => self.inferred(gpu, &stages)?,
            };
            let colors = d
                .fragment
                .map(|f| {
                    f.targets
                        .iter()
                        .copied()
                        .map(convert::target)
                        .collect::<Result<Vec<_>>>()
                })
                .transpose()?
                .unwrap_or_default();
            let pipeline = gpu
                .render_pipeline(&r::PipelineDescriptor {
                    layout: layout.native()?,
                    vertex: &vs,
                    vertex_entry: d.vertex.entry_point,
                    fragment: fs.as_ref().zip(d.fragment.map(|f| f.stage.entry_point)),
                    colors: &colors,
                    primitive: convert::primitive(d.primitive)?,
                    depth_stencil: d.depth_stencil.map(convert::depth).transpose()?,
                    multisample: types::MultisampleState {
                        count: d.multisample.count,
                        mask: d.multisample.mask as u64,
                        alpha_to_coverage_enabled: d.multisample.alpha_to_coverage_enabled,
                    },
                })
                .map_err(convert::err)?;
            Ok((pipeline, layout))
        });
        let (value, layout) = match result {
            Ok((p, l)) => (Ok(p), Some(l)),
            Err(e) => (Err(e), d.layout.cloned()),
        };
        counters::add(&counters::count::pipelines, 1);
        RenderPipeline {
            handle: Handle::new(self, value),
            layout,
        }
    }
    pub fn create_render_pipeline_async(
        &self,
        d: &RenderPipelineDescriptor<'_>,
    ) -> Pending<Result<RenderPipeline>> {
        let p = self.create_render_pipeline(d);
        Pending::ready(p.native().map(|_| p.clone()))
    }
    pub fn create_compute_pipeline(&self, d: &ComputePipelineDescriptor<'_>) -> ComputePipeline {
        let result = self.resource(|gpu| {
            let s = self.specialized(gpu, d.stage)?;
            let layout = match d.layout {
                Some(l) => l.clone(),
                None => self.inferred(
                    gpu,
                    &[(&s, d.stage.entry_point, types::ShaderStages::COMPUTE)],
                )?,
            };
            let p = gpu
                .shared_compute_pipeline(layout.native()?, &s, d.stage.entry_point)
                .map_err(convert::err)?;
            Ok((p, layout))
        });
        let (value, layout) = match result {
            Ok((p, l)) => (Ok(p), Some(l)),
            Err(e) => (Err(e), d.layout.cloned()),
        };
        counters::add(&counters::count::pipelines, 1);
        ComputePipeline {
            handle: Handle::new(self, value),
            layout,
        }
    }
}

macro_rules! identity_eq { ($($name:ident),*) => {$ (
    impl PartialEq for $name{fn eq(&self,other:&Self)->bool{self.identity()==other.identity()}}
    impl Eq for $name{}
)*}; }
identity_eq!(
    Buffer,
    Texture,
    TextureView,
    Sampler,
    BindGroupLayout,
    BindGroup,
    RenderPipeline,
    ComputePipeline
);
impl Dependency {
    pub(crate) fn identity(&self) -> usize {
        match self {
            Self::Buffer(b) => b.identity(),
            Self::Texture(t) => t.identity(),
            Self::WholeTexture(t) => t.identity(),
            Self::Group(g) => g.identity(),
        }
    }
}
