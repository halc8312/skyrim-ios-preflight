//! Regular-file pinning and sandbox directory checks. These detect ordinary
//! replacement races; they are not inode attestation before dyld execution or
//! protection against an actively compromised process/OS.
use crate::Check;
use sha2::{Digest, Sha256};
use std::{
    fs::{File, OpenOptions},
    io::{Read, Seek, SeekFrom},
    path::{Component, Path, PathBuf},
};
pub const FRAMEWORK_RELATIVE: &str = "Frameworks/PrivateGuest.framework/PrivateGuest";
pub const BINDING_RELATIVE: &str = "OwnedGuestBinding.json";

pub fn checked_child(root: &Path, relative: &Path) -> Check<PathBuf> {
    if relative.as_os_str().is_empty()
        || relative
            .components()
            .any(|c| !matches!(c, Component::Normal(_)))
    {
        return Err("non-normal relative path refused");
    }
    let root = root.canonicalize().map_err(|_| "root unavailable")?;
    if !root.is_dir() {
        return Err("root is not a directory");
    }
    let mut path = root.clone();
    for c in relative.components() {
        path.push(c);
        if std::fs::symlink_metadata(&path)
            .map_err(|_| "fixed child unavailable")?
            .file_type()
            .is_symlink()
        {
            return Err("symlink component refused");
        }
    }
    if path.canonicalize().map_err(|_| "child unavailable")? != path || !path.starts_with(root) {
        return Err("child escaped root");
    }
    Ok(path)
}
pub fn sandbox_game(sandbox: &Path, game: &Path) -> Check<PathBuf> {
    let original = sandbox;
    let sandbox = sandbox.canonicalize().map_err(|_| "sandbox unavailable")?;
    // The platform may spell its own /var root as /private/var. Preserve the
    // relative components supplied inside that root and reject their symlinks.
    let game = game
        .strip_prefix(original)
        .or_else(|_| game.strip_prefix(&sandbox))
        .map_err(|_| "game must be inside app sandbox")?;
    let game = checked_child(&sandbox, game)?;
    if !game.is_dir() {
        return Err("game root is not a directory");
    }
    // VFS follows host symlinks; refuse them in the supplied read-only tree.
    let mut pending = vec![game.clone()];
    let mut count = 0usize;
    while let Some(dir) = pending.pop() {
        for e in std::fs::read_dir(dir).map_err(|_| "game directory unreadable")? {
            count += 1;
            if count > 100_000 {
                return Err("game tree entry limit exceeded");
            }
            let e = e.map_err(|_| "game tree entry unreadable")?;
            let kind = e.file_type().map_err(|_| "game entry type unavailable")?;
            if kind.is_symlink() || (!kind.is_file() && !kind.is_dir()) {
                return Err("unsupported game filesystem entry");
            }
            if kind.is_dir() {
                pending.push(e.path());
            }
        }
    }
    Ok(game)
}
pub fn fresh_content(sandbox: &Path, game: &Path, requested: &Path) -> Check<PathBuf> {
    let original = sandbox;
    let sandbox = sandbox.canonicalize().map_err(|_| "sandbox unavailable")?;
    let name = requested
        .file_name()
        .ok_or("content directory has no name")?;
    let parent = requested.parent().ok_or("content parent missing")?;
    let relative = parent
        .strip_prefix(original)
        .or_else(|_| parent.strip_prefix(&sandbox))
        .map_err(|_| "content must be in app sandbox")?;
    let parent = if relative.as_os_str().is_empty() {
        sandbox.clone()
    } else {
        checked_child(&sandbox, relative)?
    };
    let path = parent.join(name);
    if path.starts_with(game) || game.starts_with(&path) || path == sandbox {
        return Err("content and read-only game roots overlap");
    }
    std::fs::create_dir(&path).map_err(|_| "content must be a new exclusive directory")?;
    Ok(path)
}
/// File sync followed by an exclusive hard-link publishes a complete file or
/// no final name on process interruption. It never replaces an existing final
/// file. Power-loss directory durability is deliberately not asserted.
pub fn publish_json_new(root: &Path, name: &str, data: &[u8]) -> Check<()> {
    use std::io::Write;
    if name.is_empty() || name.contains('/') || name.contains('\\') || name == "." || name == ".." {
        return Err("diagnostic file name refused");
    }
    let tmp = root.join(format!(".{name}.pending"));
    let final_path = root.join(name);
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&tmp)
        .map_err(|_| "diagnostic temp exclusive create failed")?;
    file.write_all(data)
        .map_err(|_| "diagnostic write failed")?;
    file.sync_all().map_err(|_| "diagnostic sync failed")?;
    std::fs::hard_link(&tmp, final_path)
        .map_err(|_| "diagnostic exclusive atomic publish failed")?;
    std::fs::remove_file(tmp).map_err(|_| "diagnostic temp cleanup failed")?;
    Ok(())
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
fn identity(m: &std::fs::Metadata, max: u64) -> Check<Identity> {
    use std::os::unix::fs::MetadataExt;
    if !m.file_type().is_file() || m.len() == 0 || m.len() > max {
        return Err("bounded nonempty regular file required");
    }
    Ok(Identity {
        dev: m.dev(),
        ino: m.ino(),
        bytes: m.len(),
        mtime: m.mtime(),
        mtime_ns: m.mtime_nsec(),
        ctime: m.ctime(),
        ctime_ns: m.ctime_nsec(),
    })
}
#[cfg(unix)]
pub struct PinnedFile {
    pub path: PathBuf,
    pub bytes: Vec<u8>,
    file: File,
    id: Identity,
    max: u64,
    signed_hash: [u8; 32],
}
#[cfg(unix)]
impl PinnedFile {
    pub fn open(root: &Path, relative: &Path, max: u64) -> Check<Self> {
        use std::os::unix::fs::OpenOptionsExt;
        let path = checked_child(root, relative)?;
        let mut file = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NOFOLLOW | libc::O_CLOEXEC)
            .open(&path)
            .map_err(|_| "file open refused")?;
        let id = identity(&file.metadata().map_err(|_| "fstat unavailable")?, max)?;
        let mut bytes = Vec::with_capacity(id.bytes as usize);
        file.by_ref()
            .take(max + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| "file read failed")?;
        if bytes.len() as u64 != id.bytes {
            return Err("file size changed during read");
        }
        let signed_hash = Sha256::digest(&bytes).into();
        let result = Self {
            path,
            bytes,
            file,
            id,
            max,
            signed_hash,
        };
        result.check_identity()?;
        Ok(result)
    }
    fn check_identity(&self) -> Check<()> {
        if identity(
            &self
                .file
                .metadata()
                .map_err(|_| "retained file disappeared")?,
            self.max,
        )? != self.id
            || identity(
                &std::fs::symlink_metadata(&self.path).map_err(|_| "pinned path disappeared")?,
                self.max,
            )? != self.id
        {
            return Err("pinned file identity changed");
        }
        Ok(())
    }
    pub fn recheck(&mut self) -> Check<()> {
        self.check_identity()?;
        self.file
            .seek(SeekFrom::Start(0))
            .map_err(|_| "retained seek failed")?;
        let mut h = Sha256::new();
        let mut buffer = [0u8; 65536];
        let mut count = 0u64;
        loop {
            let n = self
                .file
                .read(&mut buffer)
                .map_err(|_| "retained read failed")?;
            if n == 0 {
                break;
            }
            count = count.checked_add(n as u64).ok_or("file size overflow")?;
            if count > self.id.bytes {
                return Err("retained file grew");
            }
            h.update(&buffer[..n]);
        }
        let hash: [u8; 32] = h.finalize().into();
        if count != self.id.bytes || hash != self.signed_hash {
            return Err("pinned complete file changed");
        }
        self.check_identity()
    }
    pub fn release_bytes(&mut self) {
        self.bytes = Vec::new();
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    fn temp() -> PathBuf {
        let p = std::env::temp_dir().join(format!(
            "owned-path-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir(&p).unwrap();
        p.canonicalize().unwrap()
    }
    #[test]
    fn fresh_disjoint_content_and_normal_paths() {
        let root = temp();
        let game = root.join("game");
        std::fs::create_dir(&game).unwrap();
        assert!(checked_child(&root, Path::new("../game")).is_err());
        assert!(checked_child(&root, Path::new("/game")).is_err());
        assert_eq!(sandbox_game(&root, &game).unwrap(), game);
        assert!(fresh_content(&root, &game, &game.join("content")).is_err());
        let content = root.join("content");
        fresh_content(&root, &game, &content).unwrap();
        assert!(fresh_content(&root, &game, &content).is_err());
    }
    #[cfg(unix)]
    #[test]
    fn pinned_same_file_and_symlink_refusal() {
        let root = temp();
        let path = root.join("meta");
        std::fs::write(&path, b"public synthetic metadata").unwrap();
        let mut p = PinnedFile::open(&root, Path::new("meta"), 64).unwrap();
        p.recheck().unwrap();
        p.release_bytes();
        p.recheck().unwrap();
        std::fs::write(&path, b"public changed metadata!").unwrap();
        assert!(p.recheck().is_err());
        std::os::unix::fs::symlink(&path, root.join("link")).unwrap();
        assert!(PinnedFile::open(&root, Path::new("link"), 64).is_err());
    }
    #[test]
    fn complete_exclusive_publication() {
        let root = temp();
        let data = br#"{"public":true}"#;
        publish_json_new(&root, "final.json", data).unwrap();
        assert_eq!(std::fs::read(root.join("final.json")).unwrap(), data);
        assert!(publish_json_new(&root, "final.json", b"replacement").is_err());
        assert_eq!(std::fs::read(root.join("final.json")).unwrap(), data);
        assert!(publish_json_new(&root, "../escape", data).is_err());
        std::fs::write(root.join(".incomplete.json.pending"), b"{").unwrap();
        assert!(publish_json_new(&root, "incomplete.json", data).is_err());
        assert!(!root.join("incomplete.json").exists());
    }
}
