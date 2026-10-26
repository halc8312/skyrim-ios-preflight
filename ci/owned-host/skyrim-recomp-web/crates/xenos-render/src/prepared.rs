//! CPU-owned draw meaning, independent of GPU devices, handles and allocation.
//!
//! Geometry conversion happens at the original draw position in the command
//! stream. Converted indices are owned; direct DMA indices are a recipe, not a
//! snapshot. The executor must still reserve and protect those bytes before
//! recording a draw. Textures, shader bindings and EDRAM are not represented yet.

use xenos_state::primitive::{
    ConversionOptions, DrawDescription, HostDraw, IndexBufferInfo, IndexSource, convert_primitive, process_draw,
};
use xenos_state::xenos::{IndexFormat, PrimitiveType};

pub struct GeometryPlan {
    pub draw: HostDraw,
    pub direct_indices: Option<IndexBufferInfo>,
}

/// Preserve the existing converter's policy without constructing a GPU object.
pub fn prepare_geometry(
    desc: &DrawDescription,
    memory: &(impl xenos_state::GuestMemory + ?Sized),
) -> Result<GeometryPlan, xenos_state::primitive::PrimitiveError> {
    let direct_indices = direct_index_buffer(desc);
    let options = ConversionOptions { bake_index_params: false, allow_16bit_indices: true, expand_point_sprites: true };
    let draw = match direct_indices {
        Some(ib) => convert_primitive(
            desc.primitive_type,
            IndexSource::Auto { count: ib.count },
            None,
            desc.index_params,
            &options,
        ),
        None => process_draw(desc, memory, &options),
    }?;
    if direct_indices.is_some() {
        debug_assert!(matches!(draw.indices, xenos_state::primitive::HostIndices::None));
        debug_assert_eq!(draw.vertex_count, direct_indices.unwrap().count);
    }
    Ok(GeometryPlan { draw, direct_indices })
}

/// Lists and strips can keep the DMA indices in their original endian layout;
/// the generated vertex shader applies the guest index transform. Strips require
/// the host's all-ones restart index, exactly as in the original executor.
/// The guest reset register is 24-bit: a 32-bit 0x00ff_ffff restart therefore
/// stays on the converter path; it is not reinterpreted as host 0xffff_ffff.
fn direct_index_buffer(desc: &DrawDescription) -> Option<IndexBufferInfo> {
    let ib = desc.index_buffer?;
    if desc.tessellation.is_some() || ib.count == 0 {
        return None;
    }
    let strip = match desc.primitive_type {
        PrimitiveType::LineList | PrimitiveType::TriangleList => false,
        PrimitiveType::LineStrip
        | PrimitiveType::LineStrip2D
        | PrimitiveType::TriangleStrip
        | PrimitiveType::TriStrip2D => true,
        _ => return None,
    };
    if strip {
        let all_ones = match ib.format {
            IndexFormat::Int16 => 0xffff,
            IndexFormat::Int32 => u32::MAX,
        };
        if desc.reset_index.is_some_and(|reset| reset != all_ones) {
            return None;
        }
    }
    Some(ib)
}

/// Backend-independent raster state, kept at its original public path.
pub use crate::gfx::sink::DrawState;

#[cfg(test)]
#[path = "prepared_tests.rs"]
mod tests;
