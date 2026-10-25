//! Export only authored shaders/plans. Refuse existing output; no game input API.
use std::{
    fs::{self, OpenOptions},
    io::Write,
    path::Path,
};
use xenos_render::frame_probe;
use xenos_shader::msl::{self, ResourceKind};

fn put(root: &Path, name: &str, bytes: &[u8]) {
    OpenOptions::new().write(true).create_new(true).open(root.join(name)).unwrap().write_all(bytes).unwrap();
}
fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    assert_eq!(args.len(), 2, "fresh output directory required");
    let root = Path::new(&args[1]);
    assert!(!root.exists(), "refuse existing output");
    fs::create_dir_all(root).unwrap();
    let shaders = frame_probe::shaders().unwrap();
    let mut header = String::from(
        "// Generated only from self-authored Xenos shader fixtures.\n#pragma once\n#include <array>\n#include <cstdint>\nnamespace ohframe {\nstruct ShaderBinding {uint32_t group,binding,slot,minimum_bytes,used;};\nstruct SizeWord {uint32_t group,binding,word;};\n",
    );
    for (label, shader) in ["vertex", "pixel"].into_iter().zip(&shaders) {
        let m = msl::lower(shader).unwrap();
        assert!(m.entry_point.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_'));
        put(root, &format!("{label}.metal"), m.source.as_bytes());
        header += &format!("inline constexpr char {label}_entry[]=\"{}\";\n", m.entry_point);
        header += &format!("inline constexpr std::array<ShaderBinding,{}> {label}_bindings={{{{\n", m.bindings.len());
        for b in &m.bindings {
            assert!(matches!(b.kind, ResourceKind::Uniform | ResourceKind::StorageRead));
            header += &format!("{{{},{},{},{},{}}},\n", b.group, b.binding, b.slot, b.minimum_bytes, u32::from(b.used));
        }
        header += "}};\n";
        header += &format!("inline constexpr std::array<SizeWord,{}> {label}_sizes={{{{\n", m.runtime_sizes.len());
        for s in &m.runtime_sizes {
            header += &format!("{{{},{},{}}},\n", s.group, s.binding, s.word);
        }
        header += "}};\n";
        header += &format!("inline constexpr int {label}_sizes_slot={};\n", m.sizes_buffer_slot.map_or(-1, i32::from));
    }
    header += "}\n";
    put(root, "PublicFrameShaders.h", header.as_bytes());
    let fixtures = frame_probe::fixtures();
    let count = fixtures.len();
    for fixture in fixtures {
        put(root, &format!("frame-{}.bin", fixture.id), &fixture.frame.packet());
        put(root, &format!("frame-{}.rgba", fixture.id), &fixture.expected);
    }
    println!("authored shaders=2 plans={count}; no game data or GPU execution");
}
