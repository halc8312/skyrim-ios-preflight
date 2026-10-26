//! Direct-HAL implementation of the shared renderer device interface.
//! No WebGPU C ABI calls, synthetic FFI object pointers, or guest-memory policy.
//! The existing executor/cache code remains the authority for read positions,
//! queue-prefix upload order and GPU-resident resolve ownership.
use native::types;
use skyrim_native_gpu as native;
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    time::{Duration, Instant},
};
type Platform = native::backend::Platform;
type NativeGpu = native::Gpu<Platform>;
macro_rules! flag_ops {
    ($t:ident) => {
        impl core::ops::BitOr for $t {
            type Output = Self;
            fn bitor(self, rhs: Self) -> Self {
                Self(self.0 | rhs.0)
            }
        }
        impl core::ops::BitOrAssign for $t {
            fn bitor_assign(&mut self, rhs: Self) {
                self.0 |= rhs.0;
            }
        }
        impl core::ops::BitAnd for $t {
            type Output = Self;
            fn bitand(self, rhs: Self) -> Self {
                Self(self.0 & rhs.0)
            }
        }
        impl $t {
            pub const fn contains(self, other: Self) -> bool {
                self.0 & other.0 == other.0
            }
        }
    };
}
#[path = "../../../../skyrim-recomp-web/crates/gpu-webgpu/src/error.rs"]
mod error;
#[path = "../../../../skyrim-recomp-web/crates/gpu-webgpu/src/ffi.rs"]
pub mod ffi;
pub use error::{Error, GpuError, Result};
mod api;
pub use api::*;
mod convert;
mod device;
pub use device::*;
mod resources;
pub use resources::*;
mod commands;
pub use commands::*;
mod pending;
pub use ffi::{
    WGPUAdapterType as AdapterType, WGPUAddressMode as AddressMode, WGPUBackendType as BackendType,
    WGPUBlendFactor as BlendFactor, WGPUBlendOperation as BlendOperation,
    WGPUBufferBindingType as BufferBindingType, WGPUBufferUsage as BufferUsage, WGPUColor as Color,
    WGPUColorWriteMask as ColorWriteMask, WGPUCompareFunction as CompareFunction,
    WGPUCompositeAlphaMode as CompositeAlphaMode, WGPUCullMode as CullMode,
    WGPUErrorFilter as ErrorFilter, WGPUErrorType as ErrorType, WGPUExtent3D as Extent3d,
    WGPUFeatureName as FeatureName, WGPUFilterMode as FilterMode, WGPUFrontFace as FrontFace,
    WGPUIndexFormat as IndexFormat, WGPULimits as Limits, WGPULoadOp as LoadOp,
    WGPUMapMode as MapMode, WGPUMipmapFilterMode as MipmapFilterMode, WGPUOrigin3D as Origin3d,
    WGPUPowerPreference as PowerPreference, WGPUPresentMode as PresentMode,
    WGPUPrimitiveTopology as PrimitiveTopology, WGPUSamplerBindingType as SamplerBindingType,
    WGPUShaderStage as ShaderStage, WGPUStencilOperation as StencilOperation,
    WGPUStorageTextureAccess as StorageTextureAccess, WGPUStoreOp as StoreOp,
    WGPUTextureAspect as TextureAspect, WGPUTextureDimension as TextureDimension,
    WGPUTextureFormat as TextureFormat, WGPUTextureSampleType as TextureSampleType,
    WGPUTextureUsage as TextureUsage, WGPUTextureViewDimension as TextureViewDimension,
    WGPUVertexFormat as VertexFormat, WGPUVertexStepMode as VertexStepMode,
};
pub use pending::Pending;

pub const fn extent(width: u32, height: u32, depth_or_array_layers: u32) -> Extent3d {
    Extent3d {
        width,
        height,
        depthOrArrayLayers: depth_or_array_layers,
    }
}
pub const fn origin(x: u32, y: u32, z: u32) -> Origin3d {
    Origin3d { x, y, z }
}
pub fn format_block_info(format: TextureFormat) -> Option<(u32, u32)> {
    use TextureFormat as F;
    Some(match format {
        F::R8Unorm | F::R8Snorm | F::R8Uint | F::R8Sint | F::Stencil8 => (1, 1),
        F::R16Uint
        | F::R16Sint
        | F::R16Float
        | F::RG8Unorm
        | F::RG8Snorm
        | F::RG8Uint
        | F::RG8Sint => (2, 1),
        F::R16Unorm | F::R16Snorm | F::Depth16Unorm => (2, 1),
        F::R32Float
        | F::R32Uint
        | F::R32Sint
        | F::RG16Uint
        | F::RG16Sint
        | F::RG16Float
        | F::RG16Unorm => (4, 1),
        F::RG16Snorm
        | F::RGBA8Unorm
        | F::RGBA8UnormSrgb
        | F::RGBA8Snorm
        | F::RGBA8Uint
        | F::RGBA8Sint => (4, 1),
        F::BGRA8Unorm | F::BGRA8UnormSrgb | F::RGB10A2Uint | F::RGB10A2Unorm | F::RG11B10Ufloat => {
            (4, 1)
        }
        F::RGB9E5Ufloat | F::Depth32Float => (4, 1),
        F::RG32Float
        | F::RG32Uint
        | F::RG32Sint
        | F::RGBA16Uint
        | F::RGBA16Sint
        | F::RGBA16Float => (8, 1),
        F::RGBA16Unorm | F::RGBA16Snorm => (8, 1),
        F::RGBA32Float | F::RGBA32Uint | F::RGBA32Sint => (16, 1),
        F::BC1RGBAUnorm | F::BC1RGBAUnormSrgb | F::BC4RUnorm | F::BC4RSnorm => (8, 4),
        F::BC2RGBAUnorm | F::BC2RGBAUnormSrgb | F::BC3RGBAUnorm | F::BC3RGBAUnormSrgb => (16, 4),
        F::BC5RGUnorm | F::BC5RGSnorm | F::BC6HRGBUfloat | F::BC6HRGBFloat | F::BC7RGBAUnorm => {
            (16, 4)
        }
        F::BC7RGBAUnormSrgb => (16, 4),
        _ => return None,
    })
}
pub fn implementation() -> String {
    "direct HAL 29.0.3 (native bridge)".into()
}

#[path = "../../../../skyrim-recomp-web/crates/gpu-webgpu/src/bounded_cache.rs"]
pub mod bounded_cache;
#[path = "../../../../skyrim-recomp-web/crates/gpu-webgpu/src/counters.rs"]
pub mod counters;
#[path = "../../../../skyrim-recomp-web/crates/gpu-webgpu/src/memory.rs"]
pub mod memory;

mod surface;
pub use surface::*;

#[path = "../../../../skyrim-recomp-web/crates/gpu-webgpu/src/staging.rs"]
pub mod staging;
pub mod timing;
pub static UNSHARED_UPLOADS: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);
