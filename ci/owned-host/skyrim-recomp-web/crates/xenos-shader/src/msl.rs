//! WGSL-to-MSL lowering of the existing Xenos translation, with explicit binding
//! reflection. No GPU objects, guest memory, paths or compilation are accessed.
//! Slots come from the parsed module, not string matching or backend handles.

use naga::back::msl as backend;
use naga::proc::index::{BoundsCheckPolicies, BoundsCheckPolicy};
use naga::valid::{ValidationFlags, Validator};
use naga::{AddressSpace, ResourceBinding, ShaderStage, TypeInner};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResourceKind {
    Uniform,
    StorageRead,
    StorageReadWrite,
    Texture,
    Sampler,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Binding {
    pub group: u32,
    pub binding: u32,
    /// Buffer, texture and sampler slots are separate Metal namespaces.
    pub slot: u8,
    pub kind: ResourceKind,
    pub used: bool,
    pub minimum_bytes: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RuntimeSize {
    pub group: u32,
    pub binding: u32,
    /// Word position in Naga's _mslBufferSizes, in global declaration order.
    /// Values are bound buffer BYTE sizes, not element counts.
    pub word: u32,
    /// Pinned Naga global handle used by the emitted sizeN struct member.
    pub global_index: u32,
}

#[derive(Debug)]
pub struct MetalShader {
    pub source: String,
    pub entry_point: String,
    pub bindings: Vec<Binding>,
    pub runtime_sizes: Vec<RuntimeSize>,
    pub sizes_buffer_slot: Option<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    SourceLimit,
    Parse,
    Validate,
    EntryPoint,
    UnsupportedResource,
    SlotLimit,
    Layout,
    Lowering,
}

/// Fixed, conservative iPhone limits. Slot 30 is reserved for Naga's lengths;
/// unsupported features refuse lowering rather than generating invalid MSL.
pub fn lower(translation: &crate::Translation) -> Result<MetalShader, Error> {
    lower_wgsl(&translation.wgsl, translation.stage, translation.entry_point)
}

pub fn lower_wgsl(source: &str, stage: crate::ShaderStage, entry: &str) -> Result<MetalShader, Error> {
    if source.len() > 8 << 20 || entry.len() > 128 {
        return Err(Error::SourceLimit);
    }
    let module = naga::front::wgsl::parse_str(source).map_err(|_| Error::Parse)?;
    let info = Validator::new(ValidationFlags::all(), backend::supported_capabilities())
        .validate(&module)
        .map_err(|_| Error::Validate)?;
    let stage = match stage {
        crate::ShaderStage::Vertex => ShaderStage::Vertex,
        crate::ShaderStage::Pixel => ShaderStage::Fragment,
    };
    let entry_index =
        module.entry_points.iter().position(|e| e.stage == stage && e.name == entry).ok_or(Error::EntryPoint)?;
    let usage = info.get_entry_point(entry_index);
    let mut layout = naga::proc::Layouter::default();
    layout.update(module.to_ctx()).map_err(|_| Error::Layout)?;
    let mut globals = Vec::new();
    let mut runtime_sizes = Vec::new();
    for (handle, var) in module.global_variables.iter() {
        let Some(binding) = var.binding.clone() else {
            continue;
        };
        let ty = &module.types[var.ty].inner;
        let kind = match (var.space, ty) {
            (_, TypeInner::BindingArray { .. }) => return Err(Error::UnsupportedResource),
            (AddressSpace::Uniform, _) => ResourceKind::Uniform,
            (AddressSpace::Storage { access }, _) => {
                if access.contains(naga::StorageAccess::STORE) {
                    ResourceKind::StorageReadWrite
                } else {
                    ResourceKind::StorageRead
                }
            }
            (AddressSpace::Handle, TypeInner::Image { class: naga::ImageClass::Storage { .. }, .. }) => {
                return Err(Error::UnsupportedResource);
            }
            (AddressSpace::Handle, TypeInner::Image { .. }) => ResourceKind::Texture,
            (AddressSpace::Handle, TypeInner::Sampler { .. }) => ResourceKind::Sampler,
            _ => return Err(Error::UnsupportedResource),
        };
        if ty.needs_host_buffer_byte_size(&module.types) {
            // No binding arrays/vertex pulling: each dynamic buffer contributes
            // exactly one uint to the struct emitted by this pinned Naga version.
            runtime_sizes.push(RuntimeSize {
                group: binding.group,
                binding: binding.binding,
                word: runtime_sizes.len() as u32,
                global_index: handle.index() as u32,
            });
        }
        let minimum = match kind {
            ResourceKind::Uniform | ResourceKind::StorageRead | ResourceKind::StorageReadWrite => layout[var.ty].size,
            _ => 0,
        };
        globals.push((binding, kind, !usage[handle].is_empty(), minimum));
    }
    globals.sort_by_key(|(b, ..)| (b.group, b.binding));
    let mut slots = [0u8; 3];
    let mut bindings = Vec::new();
    let mut resources = backend::EntryPointResources::default();
    for (binding, kind, used, minimum_bytes) in globals {
        let namespace = match kind {
            ResourceKind::Texture => 1,
            ResourceKind::Sampler => 2,
            _ => 0,
        };
        if slots[namespace] >= [30, 128, 16][namespace] {
            return Err(Error::SlotLimit);
        }
        let slot = slots[namespace];
        slots[namespace] += 1;
        let mut target = backend::BindTarget::default();
        match namespace {
            0 => {
                target.buffer = Some(slot);
                target.mutable = kind == ResourceKind::StorageReadWrite;
            }
            1 => target.texture = Some(slot),
            _ => target.sampler = Some(backend::BindSamplerTarget::Resource(slot)),
        }
        resources.resources.insert(ResourceBinding { group: binding.group, binding: binding.binding }, target);
        bindings.push(Binding { group: binding.group, binding: binding.binding, slot, kind, used, minimum_bytes });
    }
    let sizes_buffer_slot = runtime_sizes
        .iter()
        .any(|r| bindings.iter().any(|b| b.group == r.group && b.binding == r.binding && b.used))
        .then_some(30);
    resources.sizes_buffer = (!runtime_sizes.is_empty()).then_some(30);
    let mut options = backend::Options {
        lang_version: (2, 4),
        fake_missing_bindings: false,
        bounds_check_policies: BoundsCheckPolicies {
            index: BoundsCheckPolicy::ReadZeroSkipWrite,
            buffer: BoundsCheckPolicy::ReadZeroSkipWrite,
            image_load: BoundsCheckPolicy::ReadZeroSkipWrite,
            binding_array: BoundsCheckPolicy::ReadZeroSkipWrite,
        },
        ..Default::default()
    };
    options.per_entry_point_map.insert(entry.into(), resources);
    let pipeline = backend::PipelineOptions { entry_point: Some((stage, entry.into())), ..Default::default() };
    let (source, lowered) = backend::write_string(&module, &info, &options, &pipeline).map_err(|_| Error::Lowering)?;
    validate_size_layout(&source, &runtime_sizes, sizes_buffer_slot)?;
    // With PipelineOptions selecting one entry, pinned Naga 30 emits one
    // translation result, even when that entry wasn't first in the input module.
    let entry_point = match lowered.entry_point_names.as_slice() {
        [Ok(name)] => name.clone(),
        _ => return Err(Error::Lowering),
    };
    Ok(MetalShader { source, entry_point, bindings, runtime_sizes, sizes_buffer_slot })
}

// This deliberately verifies the pinned backend's emitted ABI as well as the
// parsed reflection. A Naga upgrade must not silently reorder its host lengths.
fn validate_size_layout(source: &str, sizes: &[RuntimeSize], slot: Option<u8>) -> Result<(), Error> {
    const BEGIN: &str = "struct _mslBufferSizes {";
    let mut structs = source.match_indices(BEGIN);
    match (structs.next(), sizes.is_empty()) {
        (None, true) => {}
        (Some((at, _)), false) => {
            if structs.next().is_some() {
                return Err(Error::Layout);
            }
            let body = source[at + BEGIN.len()..].split_once("};").ok_or(Error::Layout)?.0;
            let actual: Vec<_> = body.split_whitespace().collect();
            let expected: Vec<_> =
                sizes.iter().flat_map(|r| ["uint".into(), format!("size{};", r.global_index)]).collect();
            if actual != expected {
                return Err(Error::Layout);
            }
        }
        _ => return Err(Error::Layout),
    }
    if source.matches("[[buffer(30)]]").count() != usize::from(slot.is_some()) {
        return Err(Error::Layout);
    }
    Ok(())
}

#[cfg(test)]
#[path = "msl_tests.rs"]
mod tests;
