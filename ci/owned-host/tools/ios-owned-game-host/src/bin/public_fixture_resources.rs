//! Packages only the fixed self-authored XEX and independent expectations for
//! an unsigned public fixture produced by the source-only SDK build.
#[cfg(feature = "fixture-tools")]
fn main() {
    if run().is_err() {
        eprintln!("public fixture resource preparation refused");
        std::process::exit(1);
    }
}
#[cfg(not(feature = "fixture-tools"))]
fn main() {
    eprintln!("fixture-tools feature required");
    std::process::exit(1);
}
#[cfg(feature = "fixture-tools")]
fn run() -> ios_owned_game_host::Check<()> {
    use std::{fs::OpenOptions, io::Write, path::Path};
    let a = std::env::args_os().skip(1).collect::<Vec<_>>();
    if a.len() != 2 {
        return Err("expected public unsigned fixture and new output directory");
    }
    let metadata = std::fs::symlink_metadata(&a[0]).map_err(|_| "public fixture unavailable")?;
    if !metadata.file_type().is_file() || metadata.len() == 0 || metadata.len() > 16 * 1024 * 1024 {
        return Err("bounded public fixture required");
    }
    let original = std::fs::read(&a[0]).map_err(|_| "public fixture read failed")?;
    if original.len() as u64 != metadata.len() {
        return Err("public fixture changed during read");
    }
    let binding = ios_owned_game_host::public_fixture::expected_binding(&original)?;
    let root = Path::new(&a[1]);
    std::fs::create_dir(root).map_err(|_| "output directory must be new")?;
    for (name, data) in [
        (
            "default.xex",
            ios_owned_game_host::public_fixture::xex_bytes(),
        ),
        (
            "OwnedGuestBinding.json",
            serde_json::to_vec_pretty(&binding.to_json()?)
                .map_err(|_| "binding serialization failed")?,
        ),
    ] {
        let mut f = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(root.join(name))
            .map_err(|_| "exclusive resource creation failed")?;
        f.write_all(&data).map_err(|_| "resource write failed")?;
        f.sync_all().map_err(|_| "resource sync failed")?;
    }
    println!(
        "{{\"fixed_self_authored_xex\":true,\"functions\":3,\"function_imports\":2,\"game_xex_input_accepted\":false,\"framework_source_provenance_verified\":false,\"iphone_verified\":false}}"
    );
    Ok(())
}
