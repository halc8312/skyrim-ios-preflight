use super::*;
use objc2::rc::Retained;
use objc2_quartz_core::CAMetalLayer;
struct State {
    layer: Retained<CAMetalLayer>,
    native: Option<native::surface::MetalSurface>,
    device: Option<Device>,
}
#[derive(Clone)]
pub struct Surface(Rc<RefCell<State>>);
pub struct SurfaceFrame {
    pub texture: Texture,
    pub suboptimal: bool,
    surface: Surface,
    frame: native::surface::MetalFrame,
    device: Device,
}
impl std::fmt::Debug for Surface {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativeMetalSurface")
    }
}
impl std::fmt::Debug for SurfaceFrame {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("NativeMetalFrame")
    }
}
impl Instance {
    /// # Safety
    /// A valid retained CAMetalLayer must be supplied; UIKit attachment and
    /// window destruction stay on the main thread. One renderer owns its device,
    /// pixel format, acquisition and presentation until this surface is dropped.
    pub unsafe fn create_surface(&self, target: SurfaceTarget<'_>) -> Result<Surface> {
        let SurfaceTarget::Metal { layer } = target else {
            return Err(Error::Unsupported("native Metal surface target"));
        };
        let layer = unsafe { Retained::retain(layer.cast::<CAMetalLayer>()) }
            .ok_or_else(|| Error::Surface("null Metal layer".into()))?;
        Ok(Surface(Rc::new(RefCell::new(State {
            layer,
            native: None,
            device: None,
        }))))
    }
}
impl Surface {
    pub fn capabilities(&self, _adapter: &Adapter) -> Result<SurfaceCapabilities> {
        Ok(SurfaceCapabilities {
            usages: TextureUsage::RenderAttachment,
            formats: vec![TextureFormat::BGRA8Unorm],
            present_modes: vec![PresentMode::Fifo],
            alpha_modes: vec![CompositeAlphaMode::Opaque],
        })
    }
    pub fn configure(&self, device: &Device, c: &SurfaceConfiguration<'_>) {
        let result = (|| {
            if c.format != TextureFormat::BGRA8Unorm
                || c.usage != TextureUsage::RenderAttachment
                || !c.view_formats.is_empty()
                || !matches!(
                    c.alpha_mode,
                    CompositeAlphaMode::Opaque | CompositeAlphaMode::Auto
                )
                || !matches!(c.present_mode, PresentMode::Fifo | PresentMode::Undefined)
            {
                return Err(Error::Unsupported("native surface configuration"));
            }
            let mut state = self.0.borrow_mut();
            if let Some(old) = &state.device {
                if !Rc::ptr_eq(&old.owner, &device.owner) {
                    return Err(Error::Surface(
                        "surface device replacement requires a new surface".into(),
                    ));
                }
            }
            if state.native.is_none() {
                let native = unsafe { device.owner.borrow().gpu.metal_surface(&state.layer) }
                    .map_err(convert::err)?;
                state.native = Some(native);
                state.device = Some(device.clone());
            }
            state
                .native
                .as_mut()
                .unwrap()
                .configure([c.width, c.height], c.width != 0 && c.height != 0)
                .map_err(convert::err)
        })();
        if let Err(e) = result {
            device.owner.borrow_mut().report(e, true);
        }
    }
    pub fn unconfigure(&self) {
        let mut s = self.0.borrow_mut();
        if let Some(n) = s.native.as_mut() {
            let result = n.configure([0, 0], false);
            if let Err(e) = result {
                if let Some(d) = &s.device {
                    d.owner.borrow_mut().report(convert::err(e), true);
                }
            }
        }
    }
    pub fn current_frame(&self) -> Result<SurfaceFrame> {
        let mut s = self.0.borrow_mut();
        let device = s
            .device
            .as_ref()
            .ok_or_else(|| Error::Surface("unconfigured Metal layer".into()))?
            .clone();
        let frame = {
            let mut c = device.owner.borrow_mut();
            c.check()?;
            s.native
                .as_mut()
                .unwrap()
                .acquire(&mut c.gpu)
                .map_err(convert::err)?
        }
        .ok_or(Error::SurfaceStatus(
            ffi::WGPUSurfaceGetCurrentTextureStatus::Timeout,
        ))?;
        let texture = Texture::from_drawable(&device, frame.texture().clone());
        Ok(SurfaceFrame {
            texture,
            suboptimal: false,
            surface: self.clone(),
            frame,
            device,
        })
    }
}
impl SurfaceFrame {
    pub fn surface(&self) -> &Surface {
        &self.surface
    }
    /// Rendering and presentation share the same watched command buffer.
    /// The native host uses this whole-frame submission instead of separately
    /// committing render work and later manufacturing another present commit.
    pub fn present_with(self, commands: CommandBuffer) -> Result<()> {
        let copies = staging::take_copies();
        let prepared = commands::submission_commands(
            &self.device.owner,
            copies.into_iter().chain([commands]).collect(),
        );
        let result = (|| {
            let commands = prepared?;
            let mut state = self.surface.0.borrow_mut();
            let surface = state
                .native
                .as_mut()
                .ok_or_else(|| Error::Surface("unconfigured Metal surface".into()))?;
            let mut c = self.device.owner.borrow_mut();
            c.check()?;
            let current = surface.accepts(&self.frame).map_err(convert::err)?;
            if let Some(e) = c.texture_uploads.take() {
                c.prefix.push(e.finish().map_err(convert::err)?);
            }
            let mut all = std::mem::take(&mut c.prefix);
            c.prefix_bytes = 0;
            all.extend(commands);
            c.serial = if current {
                surface.present_commands(&mut c.gpu, self.frame, all)
            } else {
                surface.retire_frame_commands(&mut c.gpu, self.frame, all)
            }
            .map_err(convert::err)?;
            counters::add(&counters::count::submits, 1);
            if !current {
                return Err(Error::SurfaceStatus(
                    ffi::WGPUSurfaceGetCurrentTextureStatus::Outdated,
                ));
            }
            Ok(())
        })();
        if let Err(e) = &result {
            if !matches!(e, Error::SurfaceStatus(s) if *s == ffi::WGPUSurfaceGetCurrentTextureStatus::Outdated)
            {
                self.device.owner.borrow_mut().report(e.clone(), true);
            }
        }
        staging::submitted();
        result
    }
    pub fn present(self) -> Result<()> {
        Err(Error::Unsupported(
            "native surfaces require present_with(render_commands)",
        ))
    }
}
