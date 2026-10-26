use crate::*;
use std::ffi::c_void;
#[derive(Clone, Copy, Debug)]
pub enum SurfaceTarget<'a> {
    Canvas(&'a str),
    Xlib {
        display: *mut c_void,
        window: u64,
    },
    Xcb {
        connection: *mut c_void,
        window: u32,
    },
    Wayland {
        display: *mut c_void,
        surface: *mut c_void,
    },
    Windows {
        hinstance: *mut c_void,
        hwnd: *mut c_void,
    },
    Metal {
        layer: *mut c_void,
    },
    Android {
        window: *mut c_void,
    },
}
#[cfg(target_vendor = "apple")]
#[path = "surface_metal.rs"]
mod metal;
#[cfg(target_vendor = "apple")]
pub use metal::{Surface, SurfaceFrame};
#[cfg(not(target_vendor = "apple"))]
#[derive(Clone, Debug)]
pub struct Surface;
#[derive(Clone, Debug, Default)]
pub struct SurfaceCapabilities {
    pub usages: TextureUsage,
    pub formats: Vec<TextureFormat>,
    pub present_modes: Vec<PresentMode>,
    pub alpha_modes: Vec<CompositeAlphaMode>,
}
#[derive(Clone, Copy, Debug)]
pub struct SurfaceConfiguration<'a> {
    pub format: TextureFormat,
    pub usage: TextureUsage,
    pub width: u32,
    pub height: u32,
    pub view_formats: &'a [TextureFormat],
    pub alpha_mode: CompositeAlphaMode,
    pub present_mode: PresentMode,
}
#[cfg(not(target_vendor = "apple"))]
#[derive(Debug)]
pub struct SurfaceFrame {
    pub texture: Texture,
    pub suboptimal: bool,
    surface: Surface,
}
#[cfg(not(target_vendor = "apple"))]
impl Instance {
    /// # Safety
    /// The platform window must outlive the surface.
    pub unsafe fn create_surface(&self, _target: SurfaceTarget<'_>) -> Result<Surface> {
        Err(Error::Unsupported(
            "this direct backend supports Metal surfaces and headless Vulkan only",
        ))
    }
}
#[cfg(not(target_vendor = "apple"))]
impl Surface {
    pub fn capabilities(&self, _adapter: &Adapter) -> Result<SurfaceCapabilities> {
        Err(Error::Unsupported(
            "this direct backend supports Metal surfaces and headless Vulkan only",
        ))
    }
    pub fn configure(&self, device: &Device, _config: &SurfaceConfiguration<'_>) {
        device.owner.borrow_mut().report(
            Error::Unsupported(
                "this direct backend supports Metal surfaces and headless Vulkan only",
            ),
            true,
        );
    }
    pub fn unconfigure(&self) {}
    pub fn current_frame(&self) -> Result<SurfaceFrame> {
        Err(Error::Unsupported(
            "this direct backend supports Metal surfaces and headless Vulkan only",
        ))
    }
}
#[cfg(not(target_vendor = "apple"))]
impl SurfaceFrame {
    pub fn present(self) -> Result<()> {
        Err(Error::Unsupported(
            "this direct backend supports Metal surfaces and headless Vulkan only",
        ))
    }
    pub fn surface(&self) -> &Surface {
        &self.surface
    }
}

#[cfg(not(target_vendor = "apple"))]
impl SurfaceFrame {
    pub fn present_with(self, _commands: CommandBuffer) -> Result<()> {
        Err(Error::Unsupported("Metal surface on a non-Apple platform"))
    }
}
