//! Acquires and presents on the SAME owner queue and watched command buffer.
//! Apple metadata-checkable on Windows; SDK/runtime conformance remains a gate.
use super::*;
use crate::{backend::Platform, presentation::Presenter};
use objc2::{
    Message,
    rc::{Retained, autoreleasepool},
    runtime::ProtocolObject,
};
use objc2_metal::{
    MTLCommandBuffer, MTLDrawable, MTLPixelFormat, MTLResource, MTLTexture, MTLTextureType,
};
use objc2_quartz_core::{CAMetalDrawable, CAMetalLayer};

/// Only layer setters run on main. The owner keeps its layer/device alive and
/// waits synchronously; no GPU ownership or renderer callbacks cross threads.
/// The UI must not synchronously wait for this renderer while configuring.
fn configure_layer(layer: &CAMetalLayer, configure: impl FnOnce(&CAMetalLayer) + Send) {
    #[cfg(target_os = "ios")]
    {
        if objc2::MainThreadMarker::new().is_some() {
            configure(layer);
        } else {
            let address = core::ptr::from_ref(layer).expose_provenance();
            dispatch2::DispatchQueue::main().exec_sync(move || {
                assert!(objc2::MainThreadMarker::new().is_some());
                // SAFETY: the owner retains this exact layer throughout this
                // synchronous call. The public exclusive-layer contract rules
                // out another renderer mutating its configuration.
                let layer = unsafe { &*core::ptr::with_exposed_provenance::<CAMetalLayer>(address) };
                configure(layer);
            });
        }
    }
    #[cfg(not(target_os = "ios"))]
    configure(layer);
}

pub struct MetalSurface {
    owner: Rc<Owner<Platform>>,
    layer: Retained<CAMetalLayer>,
    state: SurfaceState,
}
/// A single acquisition. Ownership, generation and drawable retention travel
/// with the render submission; it cannot be cloned or presented twice.
pub struct MetalFrame {
    owner: Rc<Owner<Platform>>,
    epoch: SurfaceEpoch,
    texture: crate::texture::Texture<Platform>,
    drawable: Retained<ProtocolObject<dyn MTLDrawable>>,
}
impl MetalFrame {
    pub fn texture(&self) -> &crate::texture::Texture<Platform> { &self.texture }
    pub fn size(&self) -> [u32;2] { self.epoch.size }
}
impl Gpu<Platform> {
    /// # Safety
    /// The caller owns the layer's display configuration for this session and
    /// routes resize/foreground changes through configure. No other renderer
    /// may acquire/present from or change its device/format/drawable size.
    /// UIKit attachment/detachment must obey its own main-thread lifetime rules.
    /// Main must remain available for synchronous setter-only configuration.
    pub unsafe fn metal_surface(&self, layer: &CAMetalLayer) -> Result<MetalSurface> {
        self.owner.healthy()?;
        let device = self.owner.device.raw_device(); // MTLDevice is Send + Sync.
        configure_layer(layer, |layer| {
            layer.setDevice(Some(device));
            layer.setPixelFormat(MTLPixelFormat::BGRA8Unorm);
            layer.setFramebufferOnly(true);
            // The native surface admits opaque compositing only. Do not inherit
            // a transparent UIKit layer from the caller or its previous owner.
            layer.setOpaque(true);
            layer.setMaximumDrawableCount(3);
            layer.setAllowsNextDrawableTimeout(true);
            layer.setPresentsWithTransaction(false);
        });
        Ok(MetalSurface {
            owner: self.owner.clone(),
            layer: layer.retain(),
            state: SurfaceState::new()?,
        })
    }
}
impl MetalSurface {
    pub fn acquire(&mut self, gpu:&mut Gpu<Platform>) -> Result<Option<MetalFrame>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner,&gpu.owner) { return Err(Error::Rejected("surface acquisition device")); }
        let Some(epoch)=self.state.epoch() else{return Ok(None);};
        autoreleasepool(|_|{
            let Some(drawable)=self.layer.nextDrawable() else{return Ok(None);};
            let texture=drawable.texture();
            if texture.width()!=epoch.size[0] as usize||texture.height()!=epoch.size[1] as usize
                ||texture.pixelFormat()!=MTLPixelFormat::BGRA8Unorm||texture.textureType()!=MTLTextureType::Type2D
                ||texture.sampleCount()!=1||!std::ptr::eq(&*texture.device(),&**self.owner.device.raw_device()) {
                return Err(Error::Rejected("acquired drawable configuration"));
            }
            let texture=gpu.import_drawable_texture(texture,epoch.size)?;
            Ok(Some(MetalFrame{owner:self.owner.clone(),epoch,texture,drawable:ProtocolObject::from_retained(drawable)}))
        })
    }
    /// Arm presentation on the last actual render command buffer BEFORE submit.
    /// Its retained watch is the very same Metal command buffer; no extra copy,
    /// queue, empty commit or CPU wait is introduced by the facade.
    pub fn present_commands(&mut self,gpu:&mut Gpu<Platform>,frame:MetalFrame,commands:Vec<Commands<Platform>>)->Result<Serial>{
        self.submit_frame_commands(gpu,frame,commands,true)
    }
    pub fn accepts(&self, frame:&MetalFrame)->Result<bool>{
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner,&frame.owner){return Err(Error::Rejected("cross-device drawable"));}
        Ok(self.state.accepts(frame.epoch))
    }
    /// Keep already recorded resource writes ordered on resize, retaining the
    /// obsolete drawable until completion without presenting it to the window.
    pub fn retire_frame_commands(&mut self,gpu:&mut Gpu<Platform>,frame:MetalFrame,commands:Vec<Commands<Platform>>)->Result<Serial>{
        self.submit_frame_commands(gpu,frame,commands,false)
    }
    fn submit_frame_commands(&mut self,gpu:&mut Gpu<Platform>,frame:MetalFrame,mut commands:Vec<Commands<Platform>>,present:bool)->Result<Serial>{
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner,&gpu.owner)||!Rc::ptr_eq(&self.owner,&frame.owner)||(present&&!self.state.accepts(frame.epoch)){return Err(Error::Rejected("stale/cross-device drawable"));}
        if commands.iter().any(|c|!Rc::ptr_eq(&self.owner,&c.owner)||c.submitted)
            ||!commands.iter().any(|c|c.texture_uses.keys().any(|&(id,_,_)|id==frame.texture.id())) {
            return Err(Error::Rejected("drawable absent from render submission"));
        }
        let last=commands.last_mut().ok_or(Error::Rejected("empty drawable submission"))?;
        let hold_id=id()?;
        if present { last.watch.presentDrawable(&frame.drawable); }
        last.holds.insert(hold_id,Rc::new(frame.drawable));
        gpu.submit(commands)
    }
    pub fn configure(&mut self, size: [u32; 2], active: bool) -> Result<()> {
        self.owner.healthy()?;
        self.state.configure(size, active)?;
        if size.iter().all(|&s| s != 0) {
            configure_layer(&self.layer, |layer| {
                let mut dimensions = layer.drawableSize();
                dimensions.width = f64::from(size[0]);
                dimensions.height = f64::from(size[1]);
                layer.setDrawableSize(dimensions);
            });
        }
        Ok(())
    }
    pub fn epoch(&self) -> Option<SurfaceEpoch> {
        self.state.epoch()
    }
    /// None means no active/current drawable; no GPU work was submitted. A
    /// successful serial still needs normal Gpu completion/error observation.
    /// nextDrawable has Apple's timeout, not a promised caller hard deadline.
    pub fn present(
        &mut self,
        gpu: &mut Gpu<Platform>,
        presenter: &Presenter<Platform>,
        epoch: SurfaceEpoch,
    ) -> Result<Option<Serial>> {
        self.owner.healthy()?;
        if !Rc::ptr_eq(&self.owner, &gpu.owner) {
            return Err(Error::Rejected("native surface device"));
        }
        if !self.state.accepts(epoch) {
            return Ok(None);
        }
        autoreleasepool(|_| {
            let Some(drawable) = self.layer.nextDrawable() else {
                return Ok(None);
            };
            let texture = drawable.texture();
            // Check the ACTUAL drawable, including unexpected external changes.
            if texture.width() != epoch.size[0] as usize
                || texture.height() != epoch.size[1] as usize
                || texture.pixelFormat() != MTLPixelFormat::BGRA8Unorm
                || texture.textureType() != MTLTextureType::Type2D
                || texture.sampleCount() != 1
                || !std::ptr::eq(&*texture.device(), &**self.owner.device.raw_device())
            {
                return Err(Error::Rejected("native drawable configuration changed"));
            }
            let target = gpu.import_drawable_texture(texture, epoch.size)?;
            let view = gpu.texture_view(&target, types::TextureViewDimension::D2, 0..1, 0..1)?;
            let mut encoder = gpu.encoder()?;
            presenter.encode(&mut encoder, &view)?;
            let drawable: Retained<ProtocolObject<dyn MTLDrawable>> =
                ProtocolObject::from_retained(drawable);
            encoder
                .raw
                .as_ref()
                .unwrap()
                .raw_command_buffer()
                .ok_or(Error::Rejected("native presentation command buffer"))?
                .presentDrawable(&drawable);
            // HAL command buffers use unretained references. Keep the drawable
            // itself alive in the normal flight/quarantine, not just its image.
            encoder.holds.insert(id()?, Rc::new(drawable));
            let serial = gpu.submit(vec![encoder.finish()?])?;
            Ok(Some(serial))
        })
    }
}
