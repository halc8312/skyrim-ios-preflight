//! Owned textures with explicit subresource state and eager zeroing. Depth and
//! multisampled images initialize through clear-only render passes; transfers
//! stay restricted to single-sampled color until aspect-copy contracts exist.
use super::*;
use types::{
    Extent3d, Origin3d, TexelCopyBufferLayout as Layout, TextureDimension as Dim,
    TextureFormat as Format, TextureUses as T, TextureViewDimension as ViewDim,
};

#[derive(Clone, Debug)]
pub struct TextureDescriptor {
    pub size: Extent3d,
    pub mip_levels: u32,
    pub sample_count: u32,
    pub dimension: Dim,
    pub format: Format,
    pub usage: T,
}
struct TextureInner<B: Backend> {
    owner: Rc<Owner<B>>,
    raw: Option<B::Texture>,
    id: u64,
    desc: TextureDescriptor,
    bytes: u64,
    states: Vec<Cell<T>>,
}
impl<B: Backend> Drop for TextureInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.owner.device.destroy_texture(self.raw.take().unwrap());
        }
        self.owner
            .texture_bytes
            .set(self.owner.texture_bytes.get() - self.bytes);
        self.owner
            .texture_objects
            .set(self.owner.texture_objects.get() - 1);
    }
}
pub struct Texture<B: Backend>(Rc<TextureInner<B>>);
#[cfg(target_vendor = "apple")]
impl Gpu<backend::Platform> {
    pub(crate) fn import_drawable_texture(
        &mut self,
        raw: objc2::rc::Retained<objc2::runtime::ProtocolObject<dyn objc2_metal::MTLTexture>>,
        size: [u32; 2],
    ) -> Result<Texture<backend::Platform>> {
        self.owner.healthy()?;
        let bytes = u64::from(size[0]) * u64::from(size[1]) * 4;
        let total = self
            .owner
            .texture_bytes
            .get()
            .checked_add(bytes)
            .filter(|&n| n <= self.owner.config.max_resident_texture_bytes)
            .ok_or(Error::Rejected("drawable texture budget"))?;
        if self.owner.texture_objects.get() >= self.owner.config.max_texture_objects {
            return Err(Error::Rejected("drawable object budget"));
        }
        let identity = id()?;
        // The sole caller has verified actual type/format/size/device. The HAL
        // wrapper owns the retained image; account its import before destruction.
        let raw = unsafe {
            hal::metal::Device::texture_from_raw(
                raw,
                Format::Bgra8Unorm,
                objc2_metal::MTLTextureType::Type2D,
                1,
                1,
                hal::CopyExtent {
                    width: size[0],
                    height: size[1],
                    depth: 1,
                },
            )
        };
        unsafe {
            self.owner.device.add_raw_texture(&raw);
        }
        self.owner.texture_bytes.set(total);
        self.owner
            .texture_objects
            .set(self.owner.texture_objects.get() + 1);
        Ok(Texture(Rc::new(TextureInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            id: identity,
            desc: TextureDescriptor {
                size: Extent3d {
                    width: size[0],
                    height: size[1],
                    depth_or_array_layers: 1,
                },
                mip_levels: 1,
                sample_count: 1,
                dimension: Dim::D2,
                format: Format::Bgra8Unorm,
                usage: T::COLOR_TARGET,
            },
            bytes,
            states: vec![Cell::new(T::UNINITIALIZED)],
        })))
    }
}
impl<B: Backend> Clone for Texture<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> Texture<B> {
    pub fn id(&self) -> u64 {
        self.0.id
    }
    pub fn descriptor(&self) -> &TextureDescriptor {
        &self.0.desc
    }
    pub fn mip_size(&self, mip: u32) -> Result<Extent3d> {
        let d = &self.0.desc;
        if mip >= d.mip_levels {
            return Err(Error::Rejected("texture mip"));
        }
        Ok(Extent3d {
            width: (d.size.width >> mip).max(1),
            height: (d.size.height >> mip).max(1),
            depth_or_array_layers: if d.dimension == Dim::D3 {
                (d.size.depth_or_array_layers >> mip).max(1)
            } else {
                d.size.depth_or_array_layers
            },
        })
    }
    pub(crate) fn raw(&self) -> &B::Texture {
        self.0.raw.as_ref().unwrap()
    }
    fn check_color_copy(&self) -> Result<()> {
        if self.0.desc.sample_count != 1 || self.0.desc.format.is_depth_stencil_format() {
            return Err(Error::Rejected(
                "depth/multisample texture copies not implemented",
            ));
        }
        Ok(())
    }
    fn layers(&self) -> u32 {
        if self.0.desc.dimension == Dim::D2 {
            self.0.desc.size.depth_or_array_layers
        } else {
            1
        }
    }
    pub(crate) fn state(&self, mip: u32, layer: u32) -> T {
        self.0.states[(mip * self.layers() + layer) as usize].get()
    }
    pub(crate) fn publish(&self, mip: u32, layer: u32, usage: T) {
        self.0.states[(mip * self.layers() + layer) as usize].set(usage);
    }
    fn check_region(&self, mip: u32, origin: Origin3d, size: Extent3d) -> Result<()> {
        let bounds = self.mip_size(mip)?.physical_size(self.0.desc.format);
        let (bw, bh) = self.0.desc.format.block_dimensions();
        if origin.x % bw != 0 || origin.y % bh != 0 || size.width % bw != 0 || size.height % bh != 0
        {
            return Err(Error::Rejected("texture copy block alignment"));
        }
        if size.width == 0 || size.height == 0 || size.depth_or_array_layers == 0 {
            return Err(Error::Rejected("empty texture copies are unsupported"));
        }
        for (start, count, limit) in [
            (origin.x, size.width, bounds.width),
            (origin.y, size.height, bounds.height),
            (
                origin.z,
                size.depth_or_array_layers,
                bounds.depth_or_array_layers,
            ),
        ] {
            if start.checked_add(count).is_none_or(|end| end > limit) {
                return Err(Error::Rejected("texture copy bounds"));
            }
        }
        Ok(())
    }
}
struct ViewInner<B: Backend> {
    texture: Texture<B>,
    raw: Option<B::TextureView>,
    id: u64,
    dimension: ViewDim,
    range: types::ImageSubresourceRange,
}
impl<B: Backend> Drop for ViewInner<B> {
    fn drop(&mut self) {
        unsafe {
            self.texture
                .0
                .owner
                .device
                .destroy_texture_view(self.raw.take().unwrap());
        }
    }
}
pub struct TextureView<B: Backend>(Rc<ViewInner<B>>);
impl<B: Backend> Clone for TextureView<B> {
    fn clone(&self) -> Self {
        Self(self.0.clone())
    }
}
impl<B: Backend> TextureView<B> {
    pub(crate) fn belongs_to(&self, owner: &Rc<Owner<B>>) -> bool {
        Rc::ptr_eq(owner, &self.0.texture.0.owner)
    }
    pub fn id(&self) -> u64 {
        self.0.id
    }
    pub fn dimension(&self) -> ViewDim {
        self.0.dimension
    }
    pub fn range(&self) -> types::ImageSubresourceRange {
        self.0.range.clone()
    }
    pub fn texture(&self) -> &Texture<B> {
        &self.0.texture
    }
    pub(crate) fn retained(&self) -> Rc<dyn std::any::Any>
    where
        B: 'static,
    {
        self.0.clone()
    }
    pub(crate) fn raw(&self) -> &B::TextureView {
        self.0.raw.as_ref().unwrap()
    }
}

pub(crate) struct TextureUse<B: Backend> {
    pub texture: Texture<B>,
    pub first: T,
    pub last: T,
}
pub(crate) fn subresource(mip: u32, layer: u32) -> types::ImageSubresourceRange {
    types::ImageSubresourceRange {
        aspect: types::TextureAspect::All,
        base_mip_level: mip,
        mip_level_count: Some(1),
        base_array_layer: layer,
        array_layer_count: Some(1),
    }
}

impl<B: Backend> Gpu<B> {
    #[cfg(feature = "fault-injection")]
    pub fn test_texture_allocations(&self) -> (u64, u64) {
        (
            self.owner.texture_bytes.get(),
            self.owner.texture_objects.get(),
        )
    }
    pub fn create_texture(&mut self, label: &str, desc: TextureDescriptor) -> Result<Texture<B>> {
        let texture = self.allocate_texture(label, desc)?;
        self.initialize_texture(&texture, None)?;
        Ok(texture)
    }
    /// Owner-only transactional creation: initialization dies with the owning
    /// encoder if a later operation rejects the frame. The uninitialized image
    /// never escapes before all subresources have been encoded for clearing.
    pub(crate) fn create_texture_recorded(
        &mut self,
        encoder: &mut Encoder<B>,
        label: &str,
        desc: TextureDescriptor,
    ) -> Result<Texture<B>> {
        if !Rc::ptr_eq(&self.owner, &encoder.owner) {
            return Err(Error::Rejected("texture initialization device"));
        }
        let texture = self.allocate_texture(label, desc)?;
        self.initialize_texture(&texture, Some(encoder))?;
        Ok(texture)
    }
    fn allocate_texture(&mut self, label: &str, desc: TextureDescriptor) -> Result<Texture<B>> {
        self.owner.healthy()?;
        let d = &desc;
        let l = &self.owner.limits;
        let depth = d.format.is_depth_stencil_format();
        let allowed = if depth {
            T::RESOURCE | T::DEPTH_STENCIL_READ | T::DEPTH_STENCIL_WRITE
        } else {
            T::COPY_SRC | T::COPY_DST | T::RESOURCE | T::COLOR_TARGET
        };
        let (bw, bh) = d.format.block_dimensions();
        let compressed = (bw, bh) != (1, 1);
        if d.usage.is_empty()
            || !allowed.contains(d.usage)
            || !self.owner.features.contains(d.format.required_features())
        {
            return Err(Error::Rejected("texture format/usage not implemented"));
        }
        if compressed
            && (d.dimension == Dim::D1
                || d.size.width % bw != 0
                || d.size.height % bh != 0
                || d.sample_count != 1
                || d.usage.contains(T::COLOR_TARGET)
                || d.dimension == Dim::D3
                    && !self
                        .owner
                        .features
                        .contains(types::Features::TEXTURE_COMPRESSION_BC_SLICED_3D))
        {
            return Err(Error::Rejected(
                "compressed texture dimension/usage/features",
            ));
        }
        if depth
            && !matches!(
                d.format,
                Format::Depth32Float
                    | Format::Depth24Plus
                    | Format::Depth24PlusStencil8
                    | Format::Depth32FloatStencil8
            )
        {
            return Err(Error::Rejected("depth format not implemented"));
        }
        if (depth || d.usage.contains(T::COLOR_TARGET) || d.sample_count > 1)
            && d.dimension != Dim::D2
        {
            return Err(Error::Rejected("only 2D attachments are implemented"));
        }
        if !matches!(d.sample_count, 1 | 4)
            || d.sample_count > 1
                && (d.mip_levels != 1
                    || d.size.depth_or_array_layers != 1
                    || d.usage.intersects(T::COPY_SRC | T::COPY_DST))
        {
            return Err(Error::Rejected("multisample texture shape/usage"));
        }
        // Conservative logical accounting for an implementation-dependent
        // Depth24PlusStencil8 allocation (D32S8 is eight bytes per sample).
        let pixel = if depth {
            if d.format.has_stencil_aspect() { 8 } else { 4 }
        } else {
            d.format
                .block_copy_size(None)
                .ok_or(Error::Rejected("texture copy format"))?
        };
        if !depth
            && d.sample_count == 1
            && u64::from(d.size.width.div_ceil(bw)) * u64::from(pixel) > 1 << 20
        {
            return Err(Error::Rejected(
                "texture initialization row exceeds zero source",
            ));
        }
        if d.size.width == 0
            || d.size.height == 0
            || d.size.depth_or_array_layers == 0
            || d.mip_levels == 0
        {
            return Err(Error::Rejected("texture dimensions"));
        }
        let max_axis = match d.dimension {
            Dim::D1
                if d.size.height == 1 && d.size.depth_or_array_layers == 1 && d.mip_levels == 1 =>
            {
                if d.size.width > l.max_texture_dimension_1d {
                    return Err(Error::Rejected("1D texture limit"));
                }
                d.size.width
            }
            Dim::D2 => {
                if d.size.width > l.max_texture_dimension_2d
                    || d.size.height > l.max_texture_dimension_2d
                    || d.size.depth_or_array_layers > l.max_texture_array_layers
                {
                    return Err(Error::Rejected("2D texture limit"));
                }
                d.size.width.max(d.size.height)
            }
            Dim::D3 => {
                let axis = d
                    .size
                    .width
                    .max(d.size.height)
                    .max(d.size.depth_or_array_layers);
                if axis > l.max_texture_dimension_3d {
                    return Err(Error::Rejected("3D texture limit"));
                }
                axis
            }
            _ => return Err(Error::Rejected("1D texture shape")),
        };
        if d.mip_levels > 32 - max_axis.leading_zeros() {
            return Err(Error::Rejected("texture mip count"));
        }
        let capabilities = unsafe { self.owner._adapter.texture_format_capabilities(d.format) };
        use hal::TextureFormatCapabilities as C;
        let initialization_usage = if depth {
            T::DEPTH_STENCIL_WRITE
        } else if d.sample_count > 1 {
            T::COLOR_TARGET
        } else {
            T::COPY_DST
        };
        let native_usage = d.usage | initialization_usage;
        if d.sample_count == 4 && !capabilities.contains(C::MULTISAMPLE_X4) {
            return Err(Error::Rejected("format does not support 4 samples"));
        }
        for (usage, cap) in [
            (T::COPY_SRC, C::COPY_SRC),
            (T::COPY_DST, C::COPY_DST),
            (T::RESOURCE, C::SAMPLED),
            (T::COLOR_TARGET, C::COLOR_ATTACHMENT),
            (T::DEPTH_STENCIL_READ, C::DEPTH_STENCIL_ATTACHMENT),
            (T::DEPTH_STENCIL_WRITE, C::DEPTH_STENCIL_ATTACHMENT),
        ] {
            if native_usage.contains(usage) && !capabilities.contains(cap) {
                return Err(Error::Rejected("texture format capability"));
            }
        }
        let mut bytes = 0u64;
        for mip in 0..d.mip_levels {
            let width = u64::from((d.size.width >> mip).max(1).div_ceil(bw));
            let height = u64::from((d.size.height >> mip).max(1).div_ceil(bh));
            let depth = u64::from(if d.dimension == Dim::D3 {
                (d.size.depth_or_array_layers >> mip).max(1)
            } else {
                d.size.depth_or_array_layers
            });
            bytes = width
                .checked_mul(height)
                .and_then(|n| n.checked_mul(depth))
                .and_then(|n| n.checked_mul(u64::from(pixel)))
                .and_then(|n| n.checked_mul(u64::from(d.sample_count)))
                .and_then(|n| n.checked_add(bytes))
                .ok_or(Error::Rejected("texture byte overflow"))?;
        }
        if self.owner.texture_objects.get() >= self.owner.config.max_texture_objects {
            return Err(Error::Rejected("texture object budget"));
        }
        let total = self
            .owner
            .texture_bytes
            .get()
            .checked_add(bytes)
            .filter(|&n| n <= self.owner.config.max_resident_texture_bytes)
            .ok_or(Error::Rejected("texture budget"))?;
        let id = id()?;
        let raw = unsafe {
            self.owner.device.create_texture(&hal::TextureDescriptor {
                label: Some(label),
                size: d.size,
                mip_level_count: d.mip_levels,
                sample_count: d.sample_count,
                dimension: d.dimension,
                format: d.format,
                usage: native_usage,
                memory_flags: hal::MemoryFlags::empty(),
                view_formats: vec![],
            })
        }
        .map_err(|e| self.owner.device_error(e))?;
        self.owner.texture_bytes.set(total);
        self.owner
            .texture_objects
            .set(self.owner.texture_objects.get() + 1);
        let layers = if d.dimension == Dim::D2 {
            d.size.depth_or_array_layers
        } else {
            1
        };
        let states = (0..d.mip_levels * layers)
            .map(|_| Cell::new(T::UNINITIALIZED))
            .collect();
        let texture = Texture(Rc::new(TextureInner {
            owner: self.owner.clone(),
            raw: Some(raw),
            id,
            desc,
            bytes,
            states,
        }));
        Ok(texture)
    }
    fn initialize_texture(
        &mut self,
        texture: &Texture<B>,
        mut target: Option<&mut Encoder<B>>,
    ) -> Result<()> {
        macro_rules! encoder {
            () => {
                match target.as_deref_mut() {
                    Some(e) => e,
                    None => self.pending()?,
                }
            };
        }
        if texture.0.desc.format.is_depth_stencil_format() || texture.0.desc.sample_count > 1 {
            for mip in 0..texture.0.desc.mip_levels {
                for layer in 0..texture.layers() {
                    let view = self.texture_view_impl(
                        texture,
                        ViewDim::D2,
                        mip..mip + 1,
                        layer..layer + 1,
                        types::TextureAspect::All,
                        true,
                    )?;
                    #[cfg(feature = "fault-injection")]
                    {
                        if self.owner.faults.poison_textures.get() {
                            encoder!().clear_attachment(&view, true)?;
                        }
                        if self.owner.faults.skip_last_texture_zero.get()
                            && mip + 1 == texture.0.desc.mip_levels
                            && layer + 1 == texture.layers()
                        {
                            continue;
                        }
                    }
                    encoder!().clear_attachment(&view, false)?;
                }
            }
            return Ok(());
        }
        // One immutable GPU-zeroed source covers rows in bounded chunks. No
        // full-size CPU allocation or uninitialized image memory is exposed.
        const ZERO_BYTES: u64 = 1 << 20;
        if self.zero_buffer.is_none() {
            self.zero_buffer = Some(self.create_buffer(
                "texture initialization zeroes",
                ZERO_BYTES,
                U::COPY_SRC,
            )?);
        }
        let zero = self.zero_buffer.as_ref().unwrap().clone();
        let pixel = texture.0.desc.format.block_copy_size(None).unwrap();
        let (bw, bh) = texture.0.desc.format.block_dimensions();
        let mut chunks = Vec::new();
        for mip in 0..texture.0.desc.mip_levels {
            let size = texture.mip_size(mip)?.physical_size(texture.0.desc.format);
            let pitch = (size.width / bw * pixel).next_multiple_of(256);
            let rows = (ZERO_BYTES / u64::from(pitch)) as u32;
            debug_assert!(rows > 0, "preflight bounds guarantee a nonempty zero chunk");
            for z in 0..size.depth_or_array_layers {
                let mut y = 0;
                while y < size.height {
                    let height = (rows * bh).min(size.height - y);
                    chunks.push((
                        Layout {
                            offset: 0,
                            bytes_per_row: Some(pitch),
                            rows_per_image: Some(height / bh),
                        },
                        mip,
                        Origin3d { x: 0, y, z },
                        Extent3d {
                            width: size.width,
                            height,
                            depth_or_array_layers: 1,
                        },
                    ));
                    y += height;
                }
            }
        }
        #[cfg(feature = "fault-injection")]
        if self.owner.faults.poison_textures.get() {
            // The oracle must not rely on freshly allocated VRAM already being
            // zero. Poison through a real GPU transfer, then run real zeroing.
            let poison = self.create_buffer(
                "test-only texture poison",
                ZERO_BYTES,
                U::COPY_SRC | U::COPY_DST,
            )?;
            self.write_buffer(&poison, 0, &vec![0xa5; ZERO_BYTES as usize])?;
            for &(layout, mip, origin, size) in &chunks {
                encoder!()
                    .copy_buffer_to_texture_impl(&poison, layout, texture, mip, origin, size)?;
            }
        }
        for (_index, &(layout, mip, origin, size)) in chunks.iter().enumerate() {
            #[cfg(feature = "fault-injection")]
            if self.owner.faults.skip_last_texture_zero.get() && _index + 1 == chunks.len() {
                continue;
            }
            encoder!().copy_buffer_to_texture_impl(&zero, layout, texture, mip, origin, size)?;
        }
        Ok(())
    }
    pub fn texture_view(
        &self,
        texture: &Texture<B>,
        dimension: ViewDim,
        mips: Range<u32>,
        layers: Range<u32>,
    ) -> Result<TextureView<B>> {
        self.texture_view_aspect(texture, dimension, mips, layers, types::TextureAspect::All)
    }
    pub fn texture_view_aspect(
        &self,
        texture: &Texture<B>,
        dimension: ViewDim,
        mips: Range<u32>,
        layers: Range<u32>,
        aspect: types::TextureAspect,
    ) -> Result<TextureView<B>> {
        self.texture_view_impl(texture, dimension, mips, layers, aspect, false)
    }
    fn texture_view_impl(
        &self,
        texture: &Texture<B>,
        dimension: ViewDim,
        mips: Range<u32>,
        layers: Range<u32>,
        aspect: types::TextureAspect,
        initialization: bool,
    ) -> Result<TextureView<B>> {
        self.owner.healthy()?;
        if !initialization
            && !texture.0.desc.usage.intersects(
                T::RESOURCE | T::COLOR_TARGET | T::DEPTH_STENCIL_READ | T::DEPTH_STENCIL_WRITE,
            )
        {
            return Err(Error::Rejected("transfer-only texture cannot have a view"));
        }
        if !Rc::ptr_eq(&self.owner, &texture.0.owner)
            || mips.start >= mips.end
            || mips.end > texture.0.desc.mip_levels
            || layers.start >= layers.end
            || layers.end > texture.layers()
        {
            return Err(Error::Rejected("texture view range/device"));
        }
        let count = layers.end - layers.start;
        if aspect != types::TextureAspect::All
            && !(aspect == types::TextureAspect::DepthOnly
                && texture.0.desc.format.has_depth_aspect())
            && !(aspect == types::TextureAspect::StencilOnly
                && texture.0.desc.format.has_stencil_aspect())
        {
            return Err(Error::Rejected("view aspect not implemented"));
        }
        if texture.0.desc.sample_count > 1 && dimension != ViewDim::D2 {
            return Err(Error::Rejected("multisample view dimension"));
        }
        let valid = match (texture.0.desc.dimension, dimension) {
            (Dim::D1, ViewDim::D1) | (Dim::D3, ViewDim::D3) => count == 1,
            (Dim::D2, ViewDim::D2) => count == 1,
            (Dim::D2, ViewDim::D2Array) => true,
            (Dim::D2, ViewDim::Cube) => count == 6 && layers.start % 6 == 0,
            (Dim::D2, ViewDim::CubeArray) => count % 6 == 0 && layers.start % 6 == 0,
            _ => false,
        };
        if !valid
            || matches!(dimension, ViewDim::Cube | ViewDim::CubeArray)
                && (texture.0.desc.size.width != texture.0.desc.size.height
                    || texture.layers() % 6 != 0)
        {
            return Err(Error::Rejected("texture view dimension"));
        }
        if dimension == ViewDim::CubeArray
            && !self
                .owner
                .downlevel
                .contains(types::DownlevelFlags::CUBE_ARRAY_TEXTURES)
        {
            return Err(Error::Rejected("cube-array view capability"));
        }
        let range = types::ImageSubresourceRange {
            aspect,
            base_mip_level: mips.start,
            mip_level_count: Some(mips.end - mips.start),
            base_array_layer: layers.start,
            array_layer_count: Some(count),
        };
        let id = id()?;
        let raw = unsafe {
            self.owner.device.create_texture_view(
                texture.raw(),
                &hal::TextureViewDescriptor {
                    label: Some("native texture view"),
                    format: texture.0.desc.format,
                    dimension,
                    usage: if initialization {
                        if texture.0.desc.format.is_depth_stencil_format() {
                            T::DEPTH_STENCIL_WRITE
                        } else {
                            T::COLOR_TARGET
                        }
                    } else {
                        texture.0.desc.usage - (T::COPY_SRC | T::COPY_DST)
                    },
                    range: range.clone(),
                },
            )
        }
        .map_err(|e| self.owner.device_error(e))?;
        Ok(TextureView(Rc::new(ViewInner {
            texture: texture.clone(),
            raw: Some(raw),
            id,
            dimension,
            range,
        })))
    }
}

fn buffer_region<B: Backend>(
    buffer: &Buffer<B>,
    layout: Layout,
    size: Extent3d,
    format: Format,
) -> Result<()> {
    let (bw, bh) = format.block_dimensions();
    let pixel = format.block_copy_size(None).unwrap();
    let row = u64::from(size.width / bw) * u64::from(pixel);
    let block_rows = size.height / bh;
    let pitch = u64::from(
        layout
            .bytes_per_row
            .ok_or(Error::Rejected("explicit texture row pitch required"))?,
    );
    let rows = u64::from(
        layout
            .rows_per_image
            .ok_or(Error::Rejected("explicit image rows required"))?,
    );
    if pitch < row
        || pitch % 256 != 0
        || rows < u64::from(block_rows)
        || layout.offset % u64::from(pixel) != 0
    {
        return Err(Error::Rejected("texture buffer row/alignment"));
    }
    let end = rows
        .checked_mul(u64::from(size.depth_or_array_layers - 1))
        .and_then(|n| n.checked_add(u64::from(block_rows - 1)))
        .and_then(|n| n.checked_mul(pitch))
        .and_then(|n| n.checked_add(row))
        .and_then(|n| n.checked_add(layout.offset))
        .ok_or(Error::Rejected("texture buffer range overflow"))?;
    if end > buffer.size() {
        return Err(Error::Rejected("texture buffer bounds"));
    }
    Ok(())
}
fn regions<B: Backend>(
    texture: &Texture<B>,
    mip: u32,
    origin: Origin3d,
    size: Extent3d,
    layout: Layout,
) -> Vec<hal::BufferTextureCopy> {
    let is_3d = texture.0.desc.dimension == Dim::D3;
    let count = if is_3d { 1 } else { size.depth_or_array_layers };
    (0..count)
        .map(|z| hal::BufferTextureCopy {
            buffer_layout: Layout {
                offset: layout.offset
                    + u64::from(z)
                        * u64::from(layout.rows_per_image.unwrap())
                        * u64::from(layout.bytes_per_row.unwrap()),
                ..layout
            },
            texture_base: hal::TextureCopyBase {
                mip_level: mip,
                array_layer: if is_3d { 0 } else { origin.z + z },
                origin: Origin3d {
                    z: if is_3d { origin.z } else { 0 },
                    ..origin
                },
                aspect: hal::FormatAspects::COLOR,
            },
            size: hal::CopyExtent {
                width: size.width,
                height: size.height,
                depth: if is_3d { size.depth_or_array_layers } else { 1 },
            },
        })
        .collect()
}
impl<B: Backend> Encoder<B> {
    fn clear_attachment(&mut self, view: &TextureView<B>, poison: bool) -> Result<()> {
        self.owner.healthy()?;
        // Private initialization views are one complete mip/layer with All
        // aspects. Neither existing pixels nor discarded storage can leak.
        let texture = view.texture();
        let r = view.range();
        let desc = texture.descriptor();
        let depth = desc.format.has_depth_aspect();
        let usage = if depth {
            T::DEPTH_STENCIL_WRITE
        } else {
            T::COLOR_TARGET
        };
        let mut extent = texture.mip_size(r.base_mip_level)?;
        extent.depth_or_array_layers = 1;
        self.transition_texture(texture, r.base_mip_level, r.base_array_layer, usage);
        self.holds
            .entry(view.id())
            .or_insert_with(|| view.retained());
        let ops = hal::AttachmentOps::LOAD_CLEAR | hal::AttachmentOps::STORE;
        let colors = if depth {
            vec![]
        } else {
            vec![Some(hal::ColorAttachment {
                target: hal::Attachment {
                    view: view.raw(),
                    usage,
                },
                depth_slice: None,
                resolve_target: None,
                ops,
                clear_value: if poison {
                    types::Color {
                        r: 1.,
                        g: 0.,
                        b: 1.,
                        a: 1.,
                    }
                } else {
                    types::Color::TRANSPARENT
                },
            })]
        };
        let raw = self.raw.as_mut().unwrap();
        unsafe {
            raw.begin_render_pass(&hal::RenderPassDescriptor {
                label: Some("native complete attachment initialization"),
                extent,
                sample_count: desc.sample_count,
                color_attachments: &colors,
                depth_stencil_attachment: depth.then(|| hal::DepthStencilAttachment {
                    target: hal::Attachment {
                        view: view.raw(),
                        usage,
                    },
                    depth_ops: ops,
                    stencil_ops: if desc.format.has_stencil_aspect() {
                        ops
                    } else {
                        hal::AttachmentOps::LOAD_DONT_CARE | hal::AttachmentOps::STORE_DISCARD
                    },
                    clear_value: if poison { (0.5, 0xa5) } else { (0., 0) },
                }),
                multiview_mask: None,
                timestamp_writes: None,
                occlusion_query_set: None,
            })
        }
        .map_err(|e| self.owner.device_error(e))?;
        let _scope = super::render::RenderScope {
            raw,
            owner: &self.owner,
        };
        Ok(())
    }
    fn check_texture(&self, texture: &Texture<B>, usage: T) -> Result<()> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &texture.0.owner) || !texture.0.desc.usage.contains(usage) {
            return Err(Error::Rejected("texture device/usage"));
        }
        Ok(())
    }
    pub(crate) fn transition_texture(
        &mut self,
        texture: &Texture<B>,
        mip: u32,
        layer: u32,
        usage: T,
    ) {
        let key = (texture.id(), mip, layer);
        if let Some(state) = self.texture_uses.get_mut(&key) {
            if state.last != usage || !T::INCLUSIVE.contains(usage) {
                unsafe {
                    self.raw
                        .as_mut()
                        .unwrap()
                        .transition_textures(std::iter::once(hal::TextureBarrier {
                            texture: texture.raw(),
                            range: subresource(mip, layer),
                            usage: hal::StateTransition {
                                from: state.last,
                                to: usage,
                            },
                        }));
                }
            }
            state.last = usage;
        } else {
            self.texture_uses.insert(
                key,
                TextureUse {
                    texture: texture.clone(),
                    first: usage,
                    last: usage,
                },
            );
        }
    }
    fn transition_region(
        &mut self,
        texture: &Texture<B>,
        mip: u32,
        origin: Origin3d,
        size: Extent3d,
        usage: T,
    ) {
        if texture.0.desc.dimension == Dim::D3 {
            self.transition_texture(texture, mip, 0, usage);
        } else {
            for layer in origin.z..origin.z + size.depth_or_array_layers {
                self.transition_texture(texture, mip, layer, usage);
            }
        }
    }
    pub fn copy_buffer_to_texture(
        &mut self,
        src: &Buffer<B>,
        layout: Layout,
        dst: &Texture<B>,
        mip: u32,
        origin: Origin3d,
        size: Extent3d,
    ) -> Result<()> {
        self.check_texture(dst, T::COPY_DST)?;
        self.copy_buffer_to_texture_impl(src, layout, dst, mip, origin, size)
    }
    fn copy_buffer_to_texture_impl(
        &mut self,
        src: &Buffer<B>,
        layout: Layout,
        dst: &Texture<B>,
        mip: u32,
        origin: Origin3d,
        size: Extent3d,
    ) -> Result<()> {
        dst.check_color_copy()?;
        self.check(src, U::COPY_SRC)?;
        dst.check_region(mip, origin, size)?;
        buffer_region(src, layout, size, dst.0.desc.format)?;
        let regions = regions(dst, mip, origin, size, layout);
        self.transition(src, U::COPY_SRC);
        self.transition_region(dst, mip, origin, size, T::COPY_DST);
        unsafe {
            self.raw.as_mut().unwrap().copy_buffer_to_texture(
                src.raw(),
                dst.raw(),
                regions.into_iter(),
            );
        }
        Ok(())
    }
    pub fn copy_texture_to_buffer(
        &mut self,
        src: &Texture<B>,
        mip: u32,
        origin: Origin3d,
        size: Extent3d,
        dst: &Buffer<B>,
        layout: Layout,
    ) -> Result<()> {
        src.check_color_copy()?;
        self.check_texture(src, T::COPY_SRC)?;
        self.check(dst, U::COPY_DST)?;
        src.check_region(mip, origin, size)?;
        buffer_region(dst, layout, size, src.0.desc.format)?;
        let regions = regions(src, mip, origin, size, layout);
        self.transition_region(src, mip, origin, size, T::COPY_SRC);
        self.transition(dst, U::COPY_DST);
        unsafe {
            self.raw.as_mut().unwrap().copy_texture_to_buffer(
                src.raw(),
                T::COPY_SRC,
                dst.raw(),
                regions.into_iter(),
            );
        }
        Ok(())
    }
    pub fn copy_texture(
        &mut self,
        src: &Texture<B>,
        src_mip: u32,
        src_origin: Origin3d,
        dst: &Texture<B>,
        dst_mip: u32,
        dst_origin: Origin3d,
        size: Extent3d,
    ) -> Result<()> {
        src.check_color_copy()?;
        dst.check_color_copy()?;
        self.check_texture(src, T::COPY_SRC)?;
        self.check_texture(dst, T::COPY_DST)?;
        src.check_region(src_mip, src_origin, size)?;
        dst.check_region(dst_mip, dst_origin, size)?;
        if src.id() == dst.id()
            || src.0.desc.format != dst.0.desc.format
            || src.0.desc.dimension != dst.0.desc.dimension
        {
            return Err(Error::Rejected("texture copy alias/format/dimension"));
        }
        let layout = Layout {
            offset: 0,
            bytes_per_row: Some(256),
            rows_per_image: Some(size.height),
        };
        let sources = regions(src, src_mip, src_origin, size, layout);
        let dests = regions(dst, dst_mip, dst_origin, size, layout);
        self.transition_region(src, src_mip, src_origin, size, T::COPY_SRC);
        self.transition_region(dst, dst_mip, dst_origin, size, T::COPY_DST);
        unsafe {
            self.raw.as_mut().unwrap().copy_texture_to_texture(
                src.raw(),
                T::COPY_SRC,
                dst.raw(),
                sources
                    .into_iter()
                    .zip(dests)
                    .map(|(s, d)| hal::TextureCopy {
                        src_base: s.texture_base,
                        dst_base: d.texture_base,
                        size: s.size,
                    }),
            );
        }
        Ok(())
    }
}
