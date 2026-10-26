//! Backend-specific completion evidence. A Metal fence value alone is NOT a
//! success signal. Watch all actual command buffers, including explicit empties.
use wgpu_hal as hal;

// The Metal bindings require CoreGraphics to initialize default-device
// discovery, including standalone conformance binaries without UIKit/AppKit.
#[cfg(target_vendor = "apple")]
#[link(name = "CoreGraphics", kind = "framework")]
unsafe extern "C" {}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Completion {
    Pending,
    Success,
    Failure(String),
}

mod sealed {
    pub trait Sealed {}
    #[cfg(not(target_vendor = "apple"))]
    impl Sealed for wgpu_hal::api::Vulkan {}
    #[cfg(target_vendor = "apple")]
    impl Sealed for wgpu_hal::api::Metal {}
}

pub trait Backend: hal::Api + sealed::Sealed {
    type Watch;
    const FIXUP_BARRIERS: bool;
    const STATIC_SAMPLE_MASK: bool;
    fn watch(encoder: &Self::CommandEncoder) -> Self::Watch;
    fn completion(watch: &Self::Watch, fence_reached: bool) -> Completion;
}

#[cfg(not(target_vendor = "apple"))]
pub type Platform = hal::api::Vulkan;
#[cfg(target_vendor = "apple")]
pub type Platform = hal::api::Metal;

#[cfg(not(target_vendor = "apple"))]
impl Backend for hal::api::Vulkan {
    type Watch = ();
    const FIXUP_BARRIERS: bool = true;
    const STATIC_SAMPLE_MASK: bool = true;
    fn watch(_: &Self::CommandEncoder) {}
    fn completion(_: &(), reached: bool) -> Completion {
        // Vulkan has no per-command-buffer status. Device/submit/fence errors
        // are latched separately by Gpu and invalidate successful completion.
        if reached {
            Completion::Success
        } else {
            Completion::Pending
        }
    }
}

#[cfg(target_vendor = "apple")]
impl Backend for hal::api::Metal {
    type Watch =
        objc2::rc::Retained<objc2::runtime::ProtocolObject<dyn objc2_metal::MTLCommandBuffer>>;
    const FIXUP_BARRIERS: bool = false;
    // hal29 Metal neither applies the pipeline mask nor lowers it into MSL.
    // Shader-authored SampleMask remains supported and separately tested.
    const STATIC_SAMPLE_MASK: bool = false;
    fn watch(encoder: &Self::CommandEncoder) -> Self::Watch {
        // Capture before end_encoding transfers ownership out of the encoder.
        encoder
            .raw_command_buffer()
            .expect("live Metal encoding")
            .into()
    }
    fn completion(watch: &Self::Watch, _: bool) -> Completion {
        use objc2_metal::{MTLCommandBuffer, MTLCommandBufferStatus as S};
        match watch.status() {
            S::Completed => Completion::Success,
            S::Error => Completion::Failure(watch.error().map_or_else(
                || "Metal command buffer failed without NSError".into(),
                |error| {
                    format!(
                        "Metal {} {}: {}",
                        error.domain(),
                        error.code(),
                        error.localizedDescription()
                    )
                },
            )),
            _ => Completion::Pending,
        }
    }
}
