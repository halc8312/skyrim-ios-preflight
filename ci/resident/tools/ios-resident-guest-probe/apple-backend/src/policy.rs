//! Target-independent policy. A separately trusted *complete signed-file* SHA
//! is required. No signature stripping, normalization or self-reported binding.
use crate::{
    Check,
    abi::{CONFIG, FUNCTIONS},
};
use sha2::{Digest, Sha256};
use std::{
    fs::{File, OpenOptions},
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
};
pub const FIXTURE_RELATIVE: &str = "Frameworks/ResidentAppleFixture.framework/ResidentAppleFixture";
const MAX_FIXTURE: u64 = 16 * 1024 * 1024;

pub fn validate_descriptor(config: [u64; 4], functions: &[u32]) -> Check<[u32; 4]> {
    let mut c = [0u32; 4];
    for (to, from) in c.iter_mut().zip(config) {
        *to = u32::try_from(from).map_err(|_| "config exceeds 32 bits")?;
    }
    if c != CONFIG || functions != FUNCTIONS {
        return Err("descriptor is not this independently expected self-authored fixture".into());
    }
    if functions.windows(2).any(|p| p[0] >= p[1])
        || functions
            .iter()
            .any(|v| v % 4 != 0 || *v < c[2] || u64::from(*v) >= u64::from(c[2]) + u64::from(c[3]))
    {
        return Err("invalid function ordering/alignment/range".into());
    }
    Ok(c)
}
fn digest(bytes: &[u8]) -> [u8; 32] {
    Sha256::digest(bytes).into()
}
pub fn parse_hash(text: &str) -> Check<[u8; 32]> {
    if text.len() != 64
        || !text
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return Err("trusted full SHA-256 must be 64 lowercase hex characters".into());
    }
    let mut out = [0; 32];
    for (i, byte) in out.iter_mut().enumerate() {
        *byte = u8::from_str_radix(&text[i * 2..i * 2 + 2], 16).map_err(|_| "invalid full SHA")?;
    }
    Ok(out)
}

#[cfg(unix)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Identity {
    dev: u64,
    ino: u64,
    bytes: u64,
    mtime: i64,
    mtime_ns: i64,
    ctime: i64,
    ctime_ns: i64,
}
#[cfg(unix)]
fn identity(metadata: &std::fs::Metadata) -> Check<Identity> {
    use std::os::unix::fs::MetadataExt;
    if !metadata.file_type().is_file() || metadata.len() == 0 || metadata.len() > MAX_FIXTURE {
        return Err("fixture must be a bounded nonempty regular file".into());
    }
    Ok(Identity {
        dev: metadata.dev(),
        ino: metadata.ino(),
        bytes: metadata.len(),
        mtime: metadata.mtime(),
        mtime_ns: metadata.mtime_nsec(),
        ctime: metadata.ctime(),
        ctime_ns: metadata.ctime_nsec(),
    })
}
/// Open descriptor retained until process exit by the successful resident owner.
/// Path checks are race-detection, not proof against an actively compromised OS.
#[cfg(unix)]
pub struct PinnedFile {
    pub path: PathBuf,
    pub bytes: Vec<u8>,
    file: File,
    id: Identity,
    expected: [u8; 32],
}
#[cfg(unix)]
impl PinnedFile {
    pub fn open(bundle: &Path, expected: &str) -> Check<Self> {
        let expected = parse_hash(expected)?;
        let root = bundle
            .canonicalize()
            .map_err(|_| "main bundle path unavailable")?;
        if !root.is_dir() {
            return Err("main bundle is not a directory".into());
        }
        let mut path = root.clone();
        for part in FIXTURE_RELATIVE.split('/') {
            path.push(part);
            if std::fs::symlink_metadata(&path)
                .map_err(|_| "fixed bundle fixture path unavailable")?
                .file_type()
                .is_symlink()
            {
                return Err("symlink component inside bundle refused".into());
            }
        }
        if path
            .canonicalize()
            .map_err(|_| "fixture path unavailable")?
            != path
            || !path.starts_with(&root)
        {
            return Err("fixture escaped fixed bundle path".into());
        }
        use std::os::unix::fs::OpenOptionsExt;
        let file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&path)
            .map_err(|_| "cannot open fixture")?;
        let id = identity(&file.metadata().map_err(|_| "fixture fstat failed")?)?;
        if identity(&std::fs::symlink_metadata(&path).map_err(|_| "fixture stat failed")?)? != id {
            return Err("fixture changed while opening".into());
        }
        let mut value = Self {
            path,
            bytes: Vec::new(),
            file,
            id,
            expected,
        };
        value.bytes = value.read_checked()?;
        Ok(value)
    }
    fn read_checked(&mut self) -> Check<Vec<u8>> {
        if identity(
            &self
                .file
                .metadata()
                .map_err(|_| "retained fixture fstat failed")?,
        )? != self.id
        {
            return Err("retained fixture identity changed".into());
        }
        self.file
            .seek(SeekFrom::Start(0))
            .map_err(|_| "fixture seek failed")?;
        let mut bytes = Vec::with_capacity(self.id.bytes as usize);
        self.file
            .by_ref()
            .take(MAX_FIXTURE + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| "fixture read failed")?;
        if bytes.len() as u64 != self.id.bytes
            || digest(&bytes) != self.expected
            || identity(
                &self
                    .file
                    .metadata()
                    .map_err(|_| "retained fixture fstat failed")?,
            )? != self.id
            || identity(
                &std::fs::symlink_metadata(&self.path).map_err(|_| "fixture path disappeared")?,
            )? != self.id
        {
            return Err("complete fixture SHA/identity mismatch".into());
        }
        Ok(bytes)
    }
    pub fn recheck(&mut self) -> Check<()> {
        if self.read_checked()? != self.bytes {
            return Err("fixture bytes changed after loading".into());
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn strict_descriptor_and_full_hash() {
        assert_eq!(
            validate_descriptor(CONFIG.map(u64::from), &FUNCTIONS).unwrap(),
            CONFIG
        );
        assert!(validate_descriptor([u64::MAX; 4], &FUNCTIONS).is_err());
        assert!(
            validate_descriptor(
                CONFIG.map(u64::from),
                &[FUNCTIONS[1], FUNCTIONS[0], FUNCTIONS[2]]
            )
            .is_err()
        );
        assert!(validate_descriptor(CONFIG.map(u64::from), &FUNCTIONS[..2]).is_err());
        assert!(validate_descriptor(CONFIG.map(u64::from), &[FUNCTIONS[0]; 3]).is_err());
        assert!(parse_hash(&"a".repeat(64)).is_ok());
        assert!(parse_hash(&"A".repeat(64)).is_err());
        assert!(parse_hash(&"0".repeat(63)).is_err());
    }
    #[cfg(unix)]
    fn root() -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "resident-apple-policy-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&path).unwrap();
        std::fs::create_dir(path.join("Frameworks")).unwrap();
        std::fs::create_dir(path.join("Frameworks/ResidentAppleFixture.framework")).unwrap();
        path
    }
    #[cfg(unix)]
    #[test]
    fn pinned_file_detects_mutation_and_wrong_hash() {
        let root = root();
        let path = root.join(FIXTURE_RELATIVE);
        let bytes = b"self-authored metadata fixture";
        std::fs::write(&path, bytes).unwrap();
        let hash = digest(bytes)
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<String>();
        assert!(PinnedFile::open(&root, &"0".repeat(64)).is_err());
        let mut pinned = PinnedFile::open(&root, &hash).unwrap();
        pinned.recheck().unwrap();
        std::fs::write(&path, b"mutated synthetic data").unwrap();
        assert!(pinned.recheck().is_err());
    }
    #[cfg(unix)]
    #[test]
    fn symlink_component_is_refused() {
        let root = root();
        let external = root.join("external");
        std::fs::write(&external, b"fixture").unwrap();
        std::os::unix::fs::symlink(&external, root.join(FIXTURE_RELATIVE)).unwrap();
        assert!(PinnedFile::open(&root, &"0".repeat(64)).is_err());
    }
}
