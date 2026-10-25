//! Export only public/authored microcode, typed reflection and comparison plans.
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::Path,
};
use xenos_render::sampled_probe;
use xenos_shader::msl::{self, ResourceKind, TextureScalar, TextureShape, TextureType};
fn put(root: &Path, name: &str, bytes: &[u8]) {
    OpenOptions::new().write(true).create_new(true).open(root.join(name)).unwrap().write_all(bytes).unwrap();
}
fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    assert_eq!(args.len(), 2);
    let root = Path::new(&args[1]);
    assert!(!root.exists(), "fresh authored-only output required");
    fs::create_dir_all(root).unwrap();
    let [_, pixel] = sampled_probe::shaders().unwrap();
    let m = msl::lower(&pixel).unwrap();
    assert!(m.entry_point.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_'));
    put(root, "pixel.metal", m.source.as_bytes());
    let buffers: Vec<_> =
        m.bindings.iter().filter(|b| matches!(b.kind, ResourceKind::Uniform | ResourceKind::StorageRead)).collect();
    let samples: Vec<_> =
        m.bindings.iter().filter(|b| matches!(b.kind, ResourceKind::Texture | ResourceKind::Sampler)).collect();
    assert_eq!(buffers.len() + samples.len(), m.bindings.len());
    let mut h = String::from(
        "// Generated exclusively from self-authored Xenos shader instructions.\n#pragma once\n#include <array>\n#include <cstdint>\n#include \"../public-shaders/PublicFrameShaders.h\"\nnamespace ohsample {\nstruct SampledBinding {uint32_t binding,slot,kind,used;};\n",
    );
    h += &format!("inline constexpr char pixel_entry[]=\"{}\";\n", m.entry_point);
    h += &format!("inline constexpr std::array<ohframe::ShaderBinding,{}> pixel_buffers={{{{\n", buffers.len());
    for b in buffers {
        h += &format!("{{{},{},{},{},{}}},\n", b.group, b.binding, b.slot, b.minimum_bytes, u32::from(b.used));
    }
    h += "}};\n";
    h += &format!("inline constexpr std::array<SampledBinding,{}> pixel_samples={{{{\n", samples.len());
    for b in samples {
        assert_eq!(b.group, 2);
        let kind = match b.kind {
            ResourceKind::Texture => {
                assert_eq!(
                    b.texture_type,
                    Some(TextureType { shape: TextureShape::D2, scalar: TextureScalar::Float, multisampled: false })
                );
                1
            }
            ResourceKind::Sampler => {
                assert!(!b.comparison_sampler);
                2
            }
            _ => unreachable!(),
        };
        h += &format!("{{{},{},{},{}}},\n", b.binding, b.slot, kind, u32::from(b.used));
    }
    h += "}};\n";
    h += &format!("inline constexpr std::array<ohframe::SizeWord,{}> pixel_sizes={{{{\n", m.runtime_sizes.len());
    for s in &m.runtime_sizes {
        h += &format!("{{{},{},{}}},\n", s.group, s.binding, s.word);
    }
    h += "}};\n";
    h += &format!("inline constexpr int pixel_sizes_slot={};\n}}\n", m.sizes_buffer_slot.map_or(-1, i32::from));
    put(root, "PublicSampledShaders.h", h.as_bytes());
    let fixtures = sampled_probe::fixtures();
    let n = fixtures.len();
    for f in fixtures {
        put(root, &format!("sampled-{}.bin", f.id), &f.frame.packet());
        put(root, &format!("sampled-{}.rgba", f.id), &f.expected);
    }
    println!("authored pixel shaders=1 sampled plans={n}; no original game data or GPU execution");
}
