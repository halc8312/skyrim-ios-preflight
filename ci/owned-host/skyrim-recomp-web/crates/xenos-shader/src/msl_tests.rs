use super::*;
use crate::ShaderStage as Stage;

const VERTEX: &str = "
@group(1) @binding(4) var<storage, read> table: array<u32>;
@group(0) @binding(3) var<uniform> transform: vec4<f32>;
@vertex fn main(@builtin(vertex_index) i:u32) -> @builtin(position) vec4<f32> {
 return vec4<f32>(f32(table[min(i,arrayLength(&table)-1u)]),0.,0.,1.) + transform;
}";

#[test]
fn explicit_slots_and_runtime_byte_sizes_are_reflected() {
    let m = lower_wgsl(VERTEX, Stage::Vertex, "main").unwrap();
    assert_eq!(
        m.bindings,
        vec![
            Binding {
                group: 0,
                binding: 3,
                slot: 0,
                kind: ResourceKind::Uniform,
                used: true,
                minimum_bytes: 16,
                texture_type: None,
                comparison_sampler: false
            },
            Binding {
                group: 1,
                binding: 4,
                slot: 1,
                kind: ResourceKind::StorageRead,
                used: true,
                minimum_bytes: 4,
                texture_type: None,
                comparison_sampler: false
            },
        ]
    );
    assert_eq!(m.runtime_sizes, vec![RuntimeSize { group: 1, binding: 4, word: 0, global_index: 0 }]);
    assert_eq!(m.sizes_buffer_slot, Some(30));
    assert!(m.source.contains("[[buffer(30)]]"));
    assert!(m.source.contains("_mslBufferSizes"));
    assert!(!m.entry_point.is_empty());
}

#[test]
fn texture_and_sampler_slots_are_independent_and_keep_guest_bindings() {
    let source = "@group(2) @binding(97) var smp:sampler;
      @group(2) @binding(96) var tex:texture_2d<f32>;
      @fragment fn main()->@location(0) vec4<f32>{return textureSampleLevel(tex,smp,vec2<f32>(0.5),0.);}";
    let m = lower_wgsl(source, Stage::Pixel, "main").unwrap();
    assert_eq!(
        m.bindings.iter().map(|b| (b.binding, b.slot, b.kind)).collect::<Vec<_>>(),
        [(96, 0, ResourceKind::Texture), (97, 0, ResourceKind::Sampler)]
    );
    assert_eq!(m.sizes_buffer_slot, None);
    assert_eq!(
        m.bindings[0].texture_type,
        Some(TextureType { shape: TextureShape::D2, scalar: TextureScalar::Float, multisampled: false })
    );
    assert!(!m.bindings[1].comparison_sampler);
}

#[test]
fn sampled_resource_types_do_not_silently_become_filterable_2d_float() {
    for (declaration, shape, scalar, multisampled) in [
        ("texture_2d<u32>", TextureShape::D2, TextureScalar::Uint, false),
        ("texture_cube<f32>", TextureShape::Cube, TextureScalar::Float, false),
        ("texture_2d_array<i32>", TextureShape::D2Array, TextureScalar::Sint, false),
        ("texture_multisampled_2d<f32>", TextureShape::D2, TextureScalar::Float, true),
        ("texture_depth_2d", TextureShape::D2, TextureScalar::Depth, false),
    ] {
        let source = format!(
            "@group(2) @binding(0) var t:{declaration}; @group(2) @binding(1) var s:sampler_comparison; @fragment fn main()->@location(0) vec4<f32>{{return vec4<f32>(1.);}}"
        );
        let m = lower_wgsl(&source, Stage::Pixel, "main").unwrap();
        assert_eq!(m.bindings[0].texture_type, Some(TextureType { shape, scalar, multisampled }));
        assert!(m.bindings[1].comparison_sampler);
    }
}

#[test]
fn unused_runtime_globals_keep_the_lengths_struct_declaration_order() {
    let source = "@group(1) @binding(3) var<storage,read> unused:array<u32>;
      @group(1) @binding(1) var<storage,read> used:array<u32>;
      @vertex fn main()->@builtin(position) vec4<f32>{return vec4<f32>(f32(used[0]),0.,0.,1.);}";
    let m = lower_wgsl(source, Stage::Vertex, "main").unwrap();
    assert_eq!(m.runtime_sizes.iter().map(|r| (r.binding, r.word)).collect::<Vec<_>>(), [(3, 0), (1, 1)]);
    assert_eq!(m.bindings.iter().map(|r| (r.binding, r.used)).collect::<Vec<_>>(), [(1, true), (3, false)]);
}

#[test]
fn slot_thirty_is_reserved_and_limits_refuse_overflow() {
    let make = |n| {
        let mut s =
            (0..n).map(|i| format!("@group(0) @binding({i}) var<uniform> v{i}:vec4<f32>;\n")).collect::<String>();
        s.push_str("@vertex fn main()->@builtin(position) vec4<f32>{return vec4<f32>(0.);}");
        s
    };
    assert_eq!(lower_wgsl(&make(30), Stage::Vertex, "main").unwrap().bindings.last().unwrap().slot, 29);
    assert_eq!(lower_wgsl(&make(31), Stage::Vertex, "main").unwrap_err(), Error::SlotLimit);
    let source = (0..17).map(|i| format!("@group(0) @binding({i}) var s{i}:sampler;\n")).collect::<String>()
        + "@fragment fn main()->@location(0) vec4<f32>{return vec4<f32>(0.);}";
    assert_eq!(lower_wgsl(&source, Stage::Pixel, "main").unwrap_err(), Error::SlotLimit);
}

#[test]
fn malformed_source_wrong_entry_and_unsupported_storage_texture_refuse() {
    assert_eq!(lower_wgsl("this is not WGSL", Stage::Vertex, "main").unwrap_err(), Error::Parse);
    assert_eq!(lower_wgsl(VERTEX, Stage::Vertex, "missing").unwrap_err(), Error::EntryPoint);
    assert_eq!(lower_wgsl(VERTEX, Stage::Pixel, "main").unwrap_err(), Error::EntryPoint);
    let source = "@group(0) @binding(0) var image:texture_storage_2d<rgba8unorm,write>;
       @fragment fn main()->@location(0) vec4<f32>{return vec4<f32>(0.);}";
    assert_eq!(lower_wgsl(source, Stage::Pixel, "main").unwrap_err(), Error::UnsupportedResource);
}

#[test]
fn selected_entry_is_resolved_in_modules_with_multiple_entries() {
    let source = "@vertex fn one()->@builtin(position) vec4<f32>{return vec4<f32>(0.);}
       @vertex fn two()->@builtin(position) vec4<f32>{return vec4<f32>(1.);}";
    let m = lower_wgsl(source, Stage::Vertex, "two").unwrap();
    assert!(m.entry_point.contains("two"));
}

#[test]
fn unused_dynamic_globals_do_not_require_a_stage_size_binding() {
    let source = "@group(1) @binding(0) var<storage,read> unused:array<u32>;
      @vertex fn main()->@builtin(position) vec4<f32>{return vec4<f32>(0.);}";
    let m = lower_wgsl(source, Stage::Vertex, "main").unwrap();
    assert_eq!(m.runtime_sizes.len(), 1);
    assert_eq!(m.sizes_buffer_slot, None);
    assert!(!m.source.contains("[[buffer(30)]]"));
    let broken = m.source.replace("size0;", "size1;");
    assert_eq!(validate_size_layout(&broken, &m.runtime_sizes, None), Err(Error::Layout));
    assert_eq!(validate_size_layout(&m.source, &m.runtime_sizes, Some(30)), Err(Error::Layout));
}
