//! Links the webgpu.h implementation.
//!
//! Native: wgpu-native from `WGPU_NATIVE_DIR` (an unpacked release), linked as a shared library
//! because its static archive carries a second Rust std (duplicate `rust_eh_personality`).
//! Binaries outside this crate need the library on their loader path (rpath or LD_LIBRARY_PATH).
//! iOS: explicitly select a matching thin WgpuNative.framework using
//! WGPU_NATIVE_FRAMEWORK_DIR (its parent directory). Build it from the pinned
//! v29.0.1.1 source with the official Apple SDK on a Mac, metal+wgsl enabled.
//! WGPU_NATIVE_LINK_MODE=static is experimental and uses WGPU_NATIVE_DIR/lib:
//! duplicate Rust std/personality and transitive native libraries still need a
//! final link proof; this option does not solve those issues or suppress them.
//! Web: emdawnwebgpu is an Emscripten port and gets linked by emcc (`--use-port=emdawnwebgpu`);
//! the final binary's link flags are the host's responsibility, except for this crate's examples.

use std::env;
use std::fs::File;
use std::io::Read;
use std::path::Path;
use std::path::PathBuf;

fn main() {
    if env::var_os("CARGO_FEATURE_NATIVE_METAL").is_some() { return; }
    println!("cargo:rerun-if-env-changed=WGPU_NATIVE_DIR");
    println!("cargo:rerun-if-env-changed=WGPU_NATIVE_FRAMEWORK_DIR");
    println!("cargo:rerun-if-env-changed=WGPU_NATIVE_LINK_MODE");
    println!("cargo:rerun-if-changed=build.rs");
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("emscripten") {
        web_example_link_args();
        return;
    }
    if env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("ios") {
        ios_link();
        return;
    }
    if let Ok(mode) = env::var("WGPU_NATIVE_LINK_MODE") {
        assert!(mode == "dylib", "WGPU_NATIVE_LINK_MODE={mode:?} is only supported for iOS; native default is dylib");
    }
    let Some(dir) = env::var_os("WGPU_NATIVE_DIR").map(PathBuf::from) else {
        panic!(
            "WGPU_NATIVE_DIR is not set: point it at an unpacked wgpu-native release \
             (https://github.com/gfx-rs/wgpu-native/releases/tag/v29.0.1.1, wgpu-<os>-<arch>-release.zip)"
        );
    };
    let lib = dir.join("lib");
    let shared = match env::var("CARGO_CFG_TARGET_OS").as_deref() {
        Ok("windows") => "wgpu_native.dll.lib",
        Ok("macos") => "libwgpu_native.dylib",
        _ => "libwgpu_native.so",
    };
    if !lib.join(shared).is_file() {
        panic!("{} not found (WGPU_NATIVE_DIR={})", lib.join(shared).display(), dir.display());
    }
    println!("cargo:rustc-link-search=native={}", lib.display());
    println!("cargo:rustc-link-lib=dylib=wgpu_native");
    // Examples and tests of this crate run straight from target/.
    if env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("windows") {
        println!("cargo:rustc-link-arg-examples=-Wl,-rpath,{}", lib.display());
        println!("cargo:rustc-link-arg-tests=-Wl,-rpath,{}", lib.display());
    }
}

#[derive(Debug, PartialEq, Eq)]
enum IosLinkMode {
    Framework,
    ExperimentalStatic,
}

fn ios_link_mode(mode: Option<&str>, has_framework: bool) -> Result<IosLinkMode, &'static str> {
    match (mode, has_framework) {
        (None | Some("framework"), true) => Ok(IosLinkMode::Framework),
        (Some("static"), false) => Ok(IosLinkMode::ExperimentalStatic),
        (Some("static"), true) => Err("static and WGPU_NATIVE_FRAMEWORK_DIR conflict"),
        _ => Err(
            "iOS requires WGPU_NATIVE_FRAMEWORK_DIR or explicit experimental WGPU_NATIVE_LINK_MODE=static; a .so/.dylib fallback is not supported",
        ),
    }
}

fn ios_link() {
    let framework_dir = env::var_os("WGPU_NATIVE_FRAMEWORK_DIR").map(PathBuf::from);
    let mode = env::var("WGPU_NATIVE_LINK_MODE").ok();
    match ios_link_mode(mode.as_deref(), framework_dir.is_some()).unwrap_or_else(|e| panic!("{e}")) {
        IosLinkMode::Framework => {
            let directory = framework_dir.unwrap();
            let binary = directory.join("WgpuNative.framework/WgpuNative");
            println!("cargo:rerun-if-changed={}", binary.display());
            let target_arch = env::var("CARGO_CFG_TARGET_ARCH").expect("target architecture");
            let simulator = env::var("CARGO_CFG_TARGET_ABI").as_deref() == Ok("sim") || target_arch == "x86_64";
            check_ios_framework(&binary, &target_arch, simulator)
                .unwrap_or_else(|e| panic!("{}: {e}", binary.display()));
            println!("cargo:rustc-link-search=framework={}", directory.display());
            println!("cargo:rustc-link-lib=framework=WgpuNative");
            // The final signed host owns @rpath/embedding and framework signing.
            // This header/platform check proves neither SDK provenance nor ABI
            // version, exported functions, Metal support, or runtime behavior.
        }
        IosLinkMode::ExperimentalStatic => {
            let directory = env::var_os("WGPU_NATIVE_DIR")
                .map(PathBuf::from)
                .expect("experimental iOS static mode requires WGPU_NATIVE_DIR");
            let lib = directory.join("lib");
            let archive = lib.join("libwgpu_native.a");
            println!("cargo:rerun-if-changed={}", archive.display());
            let mut magic = [0; 8];
            File::open(&archive)
                .and_then(|mut file| file.read_exact(&mut magic))
                .unwrap_or_else(|e| panic!("{}: {e}", archive.display()));
            assert_eq!(&magic, b"!<arch>\n", "{} is not a static archive", archive.display());
            println!("cargo:rustc-link-search=native={}", lib.display());
            println!("cargo:rustc-link-lib=static=wgpu_native");
            // Final host must use the exact build's --print=native-static-libs
            // output and resolve duplicate Rust runtimes without force-loading,
            // multiple-definition allowances or other warning suppression.
        }
    }
}

fn word(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

fn check_ios_framework(path: &Path, architecture: &str, simulator: bool) -> Result<(), String> {
    let mut file = File::open(path).map_err(|e| e.to_string())?;
    let mut header = [0; 32];
    file.read_exact(&mut header).map_err(|e| e.to_string())?;
    if word(&header, 0) != 0xFEED_FACF || word(&header, 12) != 6 {
        return Err("expected a thin little-endian 64-bit MH_DYLIB; select the matching XCFramework slice".into());
    }
    let expected_cpu = match architecture {
        "aarch64" => 0x0100_000C,
        "x86_64" if simulator => 0x0100_0007,
        _ => return Err("unsupported iOS framework architecture".into()),
    };
    if word(&header, 4) != expected_cpu || (architecture == "aarch64" && word(&header, 8) & 0x00FF_FFFF == 2) {
        return Err("framework architecture does not match target (arm64e is not supported)".into());
    }
    let count = word(&header, 16) as usize;
    let length = word(&header, 20) as usize;
    if count > 4096 || length > 4 * 1024 * 1024 {
        return Err("unreasonable Mach-O command bounds".into());
    }
    let mut commands = vec![0; length];
    file.read_exact(&mut commands).map_err(|e| e.to_string())?;
    check_ios_commands(&commands, count, simulator)
}

fn check_ios_commands(commands: &[u8], count: usize, simulator: bool) -> Result<(), String> {
    let mut position = 0usize;
    let mut platform = None;
    for _ in 0..count {
        if commands.len().saturating_sub(position) < 8 {
            return Err("truncated Mach-O load command".into());
        }
        let size = word(commands, position + 4) as usize;
        if size < 8 || !size.is_multiple_of(8) || size > commands.len() - position {
            return Err("invalid Mach-O load command size".into());
        }
        if word(commands, position) == 0x32 {
            if size < 24 || platform.is_some() {
                return Err("invalid or duplicate LC_BUILD_VERSION".into());
            }
            let tools = word(commands, position + 20) as usize;
            if tools > (size - 24) / 8 {
                return Err("truncated LC_BUILD_VERSION tools".into());
            }
            platform = Some(word(commands, position + 8));
        }
        position += size;
    }
    if position != commands.len() {
        return Err("load command count/size mismatch".into());
    }
    if platform != Some(if simulator { 7 } else { 2 }) {
        return Err("framework is not for the requested iOS device/simulator platform".into());
    }
    Ok(())
}

#[cfg(test)]
mod ios_tests {
    use super::*;

    fn build_version(platform: u32) -> Vec<u8> {
        [0x32u32, 24, platform, 26 << 16, 26 << 16, 0].into_iter().flat_map(u32::to_le_bytes).collect()
    }

    #[test]
    fn ios_has_no_shared_library_fallback_or_implicit_static_mode() {
        assert_eq!(ios_link_mode(None, true), Ok(IosLinkMode::Framework));
        assert_eq!(ios_link_mode(Some("framework"), true), Ok(IosLinkMode::Framework));
        assert_eq!(ios_link_mode(Some("static"), false), Ok(IosLinkMode::ExperimentalStatic));
        for mode in [None, Some("dylib"), Some("framework"), Some("typo")] {
            assert!(ios_link_mode(mode, false).is_err());
        }
        assert!(ios_link_mode(Some("static"), true).is_err());
    }

    #[test]
    fn device_and_simulator_platforms_are_distinguished() {
        assert!(check_ios_commands(&build_version(2), 1, false).is_ok());
        assert!(check_ios_commands(&build_version(7), 1, true).is_ok());
        assert!(check_ios_commands(&build_version(2), 1, true).is_err());
        assert!(check_ios_commands(&build_version(7), 1, false).is_err());
        assert!(check_ios_commands(&build_version(1), 1, false).is_err());
    }

    #[test]
    fn malformed_missing_and_duplicate_version_commands_are_rejected() {
        assert!(check_ios_commands(&[], 0, false).is_err());
        assert!(check_ios_commands(&build_version(2)[..20], 1, false).is_err());
        let mut twice = build_version(2);
        twice.extend(build_version(2));
        assert!(check_ios_commands(&twice, 2, false).is_err());
        let mut invalid = build_version(2);
        invalid[4..8].copy_from_slice(&0u32.to_le_bytes());
        assert!(check_ios_commands(&invalid, 1, false).is_err());
        let mut invalid = build_version(2);
        invalid[20..24].copy_from_slice(&1u32.to_le_bytes());
        assert!(check_ios_commands(&invalid, 1, false).is_err());
        assert!(check_ios_commands(&build_version(2), 0, false).is_err());
    }
}

/// emcc flags for `examples/` (mirrors hosts/host-web/build.rs plus what WebGPU needs).
fn web_example_link_args() {
    for arg in [
        "-pthread",
        "-sPROXY_TO_PTHREAD=1",
        "-sALLOW_MEMORY_GROWTH=1",
        "-Wno-pthreads-mem-growth",
        "-sMAXIMUM_MEMORY=3GB",
        "-sINITIAL_MEMORY=128MB",
        "-sGLOBAL_BASE=1048576",
        "-sSTACK_SIZE=1MB",
        "-sDEFAULT_PTHREAD_STACK_SIZE=1MB",
        "-sWASMFS=1",
        "-sFORCE_FILESYSTEM=1",
        "-fwasm-exceptions",
        "-sSUPPORT_LONGJMP=wasm",
        "-sEXIT_RUNTIME=1",
        "-sENVIRONMENT=web,worker,node",
        "-sEXPORTED_RUNTIME_METHODS=callMain",
        "-sEXPORTED_FUNCTIONS=_main",
        // WebGPU: webgpu.h on top of the browser API.
        "--use-port=emdawnwebgpu:cpp_bindings=false",
        // Lets pthread_create hand the page's <canvas> to the renderer thread
        // as an OffscreenCanvas; the transfer machinery lives in the GL library.
        "-sOFFSCREENCANVAS_SUPPORT=1",
        "-sDEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$GL",
    ] {
        println!("cargo:rustc-link-arg-examples={arg}");
    }
    let js = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("web/event_loop.js");
    println!("cargo:rerun-if-changed={}", js.display());
    println!("cargo:rustc-link-arg-examples=--js-library={}", js.display());
}
