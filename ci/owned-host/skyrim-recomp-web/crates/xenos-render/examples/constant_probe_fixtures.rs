//! Export only authored test fixtures for the portable Metal-kernel core test.
#[cfg(feature = "public-probes")]
fn main() -> Result<(), Box<dyn std::error::Error>> {
    use std::io::Write;
    let directory = std::env::args_os().nth(1).ok_or("fresh output directory required")?;
    std::fs::create_dir(&directory)?;
    for batch in xenos_render::backend_probe::batches() {
        let file = std::path::Path::new(&directory).join(format!("case-{}.bin", batch.records));
        let mut file = std::fs::OpenOptions::new().write(true).create_new(true).open(file)?;
        file.write_all(b"OHCP0001")?;
        for n in [batch.words.len(), batch.values.len(), batch.expected.len()] { file.write_all(&(n as u32).to_le_bytes())?; }
        file.write_all(&batch.record_bytes)?; file.write_all(&batch.values)?; file.write_all(&batch.expected)?;
    }
    Ok(())
}
#[cfg(not(feature = "public-probes"))]
fn main() { panic!("public-probes feature required"); }
