//! Identity-bound, bounded copies of ContentManager save trees.
//!
//! Callers supply independently verified XEX identity and canonical absolute
//! roots. The game root is protected without reading its contents. Keep the
//! returned SaveSession alive until the guest process ends: its OS lock makes
//! the preceding session quiescent for cooperating launches. External seed
//! producers must also be quiescent. Descriptor-relative no-follow operations,
//! hashes and metadata rechecks detect ordinary replacement/mutation races;
//! they are not protection against a compromised process or operating system.
//!
//! Prior sessions and failed copies are never removed. A latest pointer is
//! replaced atomically only after complete copy/readback/source validation.
//! This asserts process-interruption publication, not power-loss durability.
//! Unix (Linux/iOS/macOS) is supported; other hosts fail closed.
//!
//! Layout: `<store>/<identity SHA256>/{identity.json,store.lock,latest.json}`
//! and `sessions/<32 lowercase hex ID>/{session.json,content/}`. The identity
//! covers title ID, media ID, version, base version and the XEX SHA256. `latest`
//! identifies a session, not an immutable hash of its subsequently written
//! content. No runtime/diagnostic directory admission is performed here: keep
//! the original fresh diagnostic root and point ContentManager at `content`.
//!
//! Only uppercase hex XUID/title/type trees for the expected title are copied.
//! Package names may contain spaces and UTF-8; matching `.title` sidecars and
//! nested ordinary package files are retained. Unknown or malformed payloads
//! refuse the operation. A fixed list of host diagnostic files at the root is
//! excluded; even excluded entries must be ordinary, singly linked files.
//! Limits are 4,096 files, 8,192 entries, 64 MiB/file, 512 MiB total, depth 16,
//! 1,024 UTF-8 bytes/relative path, and 2 MiB/manifest. Empty packages and an
//! empty first session are valid. Refused destinations can contain partial
//! copies and must not be launched or retried as fresh output.
//!
//! Seed producers stop guest writers, then export a new manifest outside the
//! source. Consumers supply its independently conveyed SHA256 and the verified
//! executable identity to `copy_seed` after their normal empty-root admission.
//! A manifest is exact canonical JSON emitted by this module; unknown fields,
//! duplicate keys, alternate encodings and mismatched content are refused.
use std::{fs::File, path::PathBuf};

pub type SaveResult<T> = Result<T, &'static str>;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SaveIdentity {
    pub title_id: u32,
    pub media_id: u32,
    pub version: u32,
    pub base_version: u32,
    pub xex_sha256: String,
}

/// Retain this value for the complete guest lifetime, not just preparation.
#[derive(Debug)]
pub struct SaveSession {
    pub content: PathBuf,
    pub session: PathBuf,
    _lease: File,
}

pub const MAX_FILES: usize = 4096;
pub const MAX_ENTRIES: usize = 8192;
pub const MAX_FILE_BYTES: u64 = 64 * 1024 * 1024;
pub const MAX_TOTAL_BYTES: u64 = 512 * 1024 * 1024;
pub const MAX_DEPTH: usize = 16;
pub const MAX_PATH_BYTES: usize = 1024;
pub const MAX_MANIFEST_BYTES: u64 = 2 * 1024 * 1024;

pub use imp::{copy_seed, export_seed_manifest, prepare_session};

#[cfg(unix)]
mod imp {
    use super::*;
    use serde_json::{Value, json};
    use sha2::{Digest, Sha256};
    use std::{
        collections::BTreeMap,
        ffi::{CStr, CString},
        fs::Metadata,
        io::{Read, Write},
        os::{
            fd::{AsRawFd, FromRawFd},
            unix::fs::MetadataExt,
        },
        path::{Component, Path},
    };

    const META_MAX: u64 = 4096;
    const DIAGNOSTICS: &[&str] = &[
        "first-failure.json",
        "diagnostic-final.json",
        "guest-exit.json",
        "guest-native-trace.json",
        "diagnostic-30s.json",
        "diagnostic-120s.json",
        "diagnostic-300s.json",
        ".first-failure.json.pending",
        ".diagnostic-final.json.pending",
        ".guest-exit.json.pending",
        ".guest-native-trace.json.pending",
        ".diagnostic-30s.json.pending",
        ".diagnostic-120s.json.pending",
        ".diagnostic-300s.json.pending",
    ];

    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    struct Stamp {
        dev: u64,
        ino: u64,
        bytes: u64,
        links: u64,
        mtime: i64,
        mtime_ns: i64,
        ctime: i64,
        ctime_ns: i64,
    }
    impl Stamp {
        fn of(m: &Metadata) -> Self {
            Self {
                dev: m.dev(),
                ino: m.ino(),
                bytes: m.len(),
                links: m.nlink(),
                mtime: m.mtime(),
                mtime_ns: m.mtime_nsec(),
                ctime: m.ctime(),
                ctime_ns: m.ctime_nsec(),
            }
        }
        fn same_node(self, other: Self) -> bool {
            self.dev == other.dev && self.ino == other.ino
        }
    }
    struct Dir {
        file: File,
        path: PathBuf,
    }
    fn component(s: &str) -> SaveResult<CString> {
        if s.is_empty()
            || s == "."
            || s == ".."
            || s.len() > 255
            || s.chars()
                .any(|c| c.is_control() || matches!(c, '/' | '\\' | ':'))
        {
            return Err("save path component refused");
        }
        CString::new(s).map_err(|_| "save path component refused")
    }
    fn raw_file(fd: libc::c_int) -> SaveResult<File> {
        if fd < 0 {
            Err("save filesystem operation failed")
        } else {
            Ok(unsafe { File::from_raw_fd(fd) })
        }
    }
    fn metadata(file: &File) -> SaveResult<Metadata> {
        file.metadata().map_err(|_| "save metadata unavailable")
    }
    impl Dir {
        fn absolute(path: &Path) -> SaveResult<Self> {
            if !path.is_absolute()
                || path
                    .components()
                    .any(|c| !matches!(c, Component::RootDir | Component::Normal(_)))
                || path.canonicalize().map_err(|_| "save root unavailable")? != path
            {
                return Err("canonical absolute save root required");
            }
            let mut dir = Self {
                file: raw_file(unsafe {
                    libc::open(
                        c"/".as_ptr(),
                        libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
                    )
                })?,
                path: PathBuf::from("/"),
            };
            for c in path.components() {
                if let Component::Normal(c) = c {
                    dir = dir.child(c.to_str().ok_or("save path is not UTF-8")?)?;
                }
            }
            Ok(dir)
        }
        fn child(&self, name: &str) -> SaveResult<Self> {
            let n = component(name)?;
            let file = raw_file(unsafe {
                libc::openat(
                    self.file.as_raw_fd(),
                    n.as_ptr(),
                    libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
                )
            })?;
            Ok(Self {
                file,
                path: self.path.join(name),
            })
        }
        fn create(&self, name: &str) -> SaveResult<Self> {
            let n = component(name)?;
            if unsafe { libc::mkdirat(self.file.as_raw_fd(), n.as_ptr(), 0o700) } != 0 {
                return Err("save destination must be new and exclusive");
            }
            self.child(name)
        }
        fn ensure(&self, name: &str) -> SaveResult<Self> {
            if self.kind(name)?.is_some() {
                self.child(name)
            } else {
                self.create(name)
            }
        }
        fn kind(&self, name: &str) -> SaveResult<Option<u32>> {
            let n = component(name)?;
            let mut st = std::mem::MaybeUninit::<libc::stat>::uninit();
            let rc = unsafe {
                libc::fstatat(
                    self.file.as_raw_fd(),
                    n.as_ptr(),
                    st.as_mut_ptr(),
                    libc::AT_SYMLINK_NOFOLLOW,
                )
            };
            if rc != 0 {
                return if std::io::Error::last_os_error().kind() == std::io::ErrorKind::NotFound {
                    Ok(None)
                } else {
                    Err("save entry metadata unavailable")
                };
            }
            Ok(Some(
                (unsafe { st.assume_init() }.st_mode as u32) & libc::S_IFMT as u32,
            ))
        }
        fn file(&self, name: &str, create: bool) -> SaveResult<File> {
            let n = component(name)?;
            if !create && self.kind(name)? != Some(libc::S_IFREG as u32) {
                return Err("ordinary save file required");
            }
            let flags = if create {
                libc::O_RDWR | libc::O_CREAT | libc::O_EXCL
            } else {
                libc::O_RDONLY
            };
            let f = raw_file(unsafe {
                libc::openat(
                    self.file.as_raw_fd(),
                    n.as_ptr(),
                    flags | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK,
                    0o600,
                )
            })?;
            let m = metadata(&f)?;
            if !m.is_file() || m.nlink() != 1 {
                return Err("linked or special save file refused");
            }
            Ok(f)
        }
        fn names(&self) -> SaveResult<Vec<String>> {
            // A fresh open-file description avoids sharing directory offsets.
            let fd = unsafe {
                libc::openat(
                    self.file.as_raw_fd(),
                    c".".as_ptr(),
                    libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
                )
            };
            if fd < 0 {
                return Err("save directory unreadable");
            }
            let stream = unsafe { libc::fdopendir(fd) };
            if stream.is_null() {
                unsafe {
                    libc::close(fd);
                }
                return Err("save directory unreadable");
            }
            struct Close(*mut libc::DIR);
            impl Drop for Close {
                fn drop(&mut self) {
                    unsafe {
                        libc::closedir(self.0);
                    }
                }
            }
            let _close = Close(stream);
            let mut names = Vec::new();
            loop {
                set_errno_zero();
                let e = unsafe { libc::readdir(stream) };
                if e.is_null() {
                    if std::io::Error::last_os_error().raw_os_error().unwrap_or(0) != 0 {
                        return Err("save directory read failed");
                    }
                    break;
                }
                let bytes = unsafe { CStr::from_ptr((*e).d_name.as_ptr()) }.to_bytes();
                if bytes == b"." || bytes == b".." {
                    continue;
                }
                if names.len() >= MAX_ENTRIES {
                    return Err("save entry limit exceeded");
                }
                let name = std::str::from_utf8(bytes).map_err(|_| "save name is not UTF-8")?;
                component(name)?;
                names.push(name.to_owned());
            }
            names.sort();
            if names.windows(2).any(|n| n[0] == n[1]) {
                return Err("save directory changed during scan");
            }
            Ok(names)
        }
        fn stamp(&self) -> SaveResult<Stamp> {
            Ok(Stamp::of(&metadata(&self.file)?))
        }
        fn verify_path(&self) -> SaveResult<()> {
            if !self
                .stamp()?
                .same_node(Self::absolute(&self.path)?.stamp()?)
            {
                return Err("save root replaced");
            }
            Ok(())
        }
        fn relative_parent(&self, relative: &str) -> SaveResult<(Self, String)> {
            let mut parts: Vec<_> = relative.split('/').collect();
            let last = parts.pop().ok_or("save relative path refused")?;
            component(last)?;
            let mut dir = self.child_dot()?;
            for p in parts {
                dir = dir.child(p)?;
            }
            Ok((dir, last.to_owned()))
        }
        fn child_dot(&self) -> SaveResult<Self> {
            Ok(Self {
                file: raw_file(unsafe {
                    libc::openat(
                        self.file.as_raw_fd(),
                        c".".as_ptr(),
                        libc::O_RDONLY | libc::O_DIRECTORY | libc::O_CLOEXEC,
                    )
                })?,
                path: self.path.clone(),
            })
        }
    }
    #[cfg(any(target_os = "linux", target_os = "android"))]
    fn set_errno_zero() {
        unsafe {
            *libc::__errno_location() = 0;
        }
    }
    #[cfg(any(target_os = "macos", target_os = "ios", target_os = "freebsd"))]
    fn set_errno_zero() {
        unsafe {
            *libc::__error() = 0;
        }
    }
    #[cfg(not(any(
        target_os = "linux",
        target_os = "android",
        target_os = "macos",
        target_os = "ios",
        target_os = "freebsd"
    )))]
    compile_error!("save-store Unix errno access must be defined for this target");

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }
    fn hash(bytes: &[u8]) -> String {
        hex(&Sha256::digest(bytes))
    }
    fn lower_hex(s: &str, n: usize) -> bool {
        s.len() == n
            && s.bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    }
    fn upper_hex(s: &str, n: usize) -> bool {
        s.len() == n
            && s.bytes()
                .all(|b| b.is_ascii_digit() || (b'A'..=b'F').contains(&b))
    }
    fn identity_value(id: &SaveIdentity) -> SaveResult<Value> {
        if !lower_hex(&id.xex_sha256, 64) {
            return Err("save identity hash refused");
        }
        Ok(
            json!({"title_id":id.title_id,"media_id":id.media_id,"version":id.version,
            "base_version":id.base_version,"xex_sha256":id.xex_sha256}),
        )
    }
    fn encoded(v: &Value) -> SaveResult<Vec<u8>> {
        // Keep namespace hashes and manifests stable if another workspace
        // enables serde_json's preserve_order feature through unification.
        fn sorted(v: &Value) -> Value {
            match v {
                Value::Object(map) => {
                    let keys: BTreeMap<_, _> = map.iter().collect();
                    let mut out = serde_json::Map::new();
                    for (key, value) in keys {
                        out.insert(key.clone(), sorted(value));
                    }
                    Value::Object(out)
                }
                Value::Array(a) => Value::Array(a.iter().map(sorted).collect()),
                _ => v.clone(),
            }
        }
        serde_json::to_vec(&sorted(v)).map_err(|_| "save metadata encoding failed")
    }
    fn exact_keys(v: &Value, names: &[&str]) -> SaveResult<()> {
        let o = v.as_object().ok_or("save metadata object required")?;
        if o.len() != names.len() || names.iter().any(|k| !o.contains_key(*k)) {
            return Err("save metadata fields refused");
        }
        Ok(())
    }
    fn canonical_json(bytes: &[u8]) -> SaveResult<Value> {
        let v: Value = serde_json::from_slice(bytes).map_err(|_| "save metadata JSON refused")?;
        if encoded(&v)? != bytes {
            return Err("canonical save metadata required");
        }
        Ok(v)
    }
    fn disjoint(a: &Path, b: &Path) -> SaveResult<()> {
        if a.starts_with(b) || b.starts_with(a) {
            Err("save roots overlap")
        } else {
            Ok(())
        }
    }
    fn protected(path: &Path, game: &Dir) -> SaveResult<()> {
        disjoint(path, &game.path)
    }
    fn disjoint_dirs(a: &Dir, b: &Dir) -> SaveResult<()> {
        disjoint(&a.path, &b.path)?;
        if a.stamp()?.same_node(b.stamp()?) {
            return Err("save root alias refused");
        }
        Ok(())
    }
    fn new_parent(path: &Path) -> SaveResult<(Dir, String)> {
        let parent = Dir::absolute(path.parent().ok_or("save parent missing")?)?;
        let name = path
            .file_name()
            .and_then(|s| s.to_str())
            .ok_or("save name refused")?;
        component(name)?;
        Ok((parent, name.to_owned()))
    }
    fn read_small(dir: &Dir, name: &str, max: u64) -> SaveResult<Vec<u8>> {
        let mut f = dir.file(name, false)?;
        let before = Stamp::of(&metadata(&f)?);
        if before.bytes == 0 || before.bytes > max {
            return Err("save metadata size refused");
        }
        let mut bytes = Vec::with_capacity(before.bytes as usize);
        (&mut f)
            .take(max + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| "save metadata read failed")?;
        if bytes.len() as u64 != before.bytes
            || Stamp::of(&metadata(&f)?) != before
            || Stamp::of(&metadata(&dir.file(name, false)?)?) != before
        {
            return Err("save metadata changed");
        }
        Ok(bytes)
    }
    fn write_file(dir: &Dir, name: &str, bytes: &[u8]) -> SaveResult<()> {
        let mut f = dir.file(name, true)?;
        f.write_all(bytes).map_err(|_| "save write failed")?;
        f.sync_all().map_err(|_| "save sync failed")?;
        if read_small(dir, name, bytes.len() as u64)? != bytes {
            return Err("save metadata readback mismatch");
        }
        Ok(())
    }
    fn publish_new(dir: &Dir, name: &str, bytes: &[u8]) -> SaveResult<()> {
        let temp = format!(".{}.{}.pending", name, &hash(bytes)[..16]);
        write_file(dir, &temp, bytes)?;
        let t = component(&temp)?;
        let n = component(name)?;
        if unsafe {
            libc::linkat(
                dir.file.as_raw_fd(),
                t.as_ptr(),
                dir.file.as_raw_fd(),
                n.as_ptr(),
                0,
            )
        } != 0
        {
            return Err("save metadata destination already exists or publish failed");
        }
        // Only this operation's temporary metadata link is removed.
        if unsafe { libc::unlinkat(dir.file.as_raw_fd(), t.as_ptr(), 0) } != 0 {
            return Err("save metadata temporary unlink failed");
        }
        Ok(())
    }

    #[derive(Clone, Debug, PartialEq, Eq)]
    struct SavedFile {
        bytes: u64,
        sha256: String,
        stamp: Stamp,
    }
    #[derive(Clone, Debug, PartialEq, Eq)]
    struct Snapshot {
        root: Stamp,
        dirs: BTreeMap<String, Stamp>,
        files: BTreeMap<String, SavedFile>,
        ignored: BTreeMap<String, Stamp>,
        total: u64,
    }
    impl Snapshot {
        fn data(&self) -> Value {
            json!({"directories":self.dirs.keys().collect::<Vec<_>>(),
                "files":self.files.iter().map(|(p,f)| json!({"path":p,"bytes":f.bytes,"sha256":f.sha256})).collect::<Vec<_>>(),
                "file_count":self.files.len(),"total_bytes":self.total})
        }
    }
    fn hash_file(mut file: File) -> SaveResult<SavedFile> {
        let before = Stamp::of(&metadata(&file)?);
        if before.bytes > MAX_FILE_BYTES {
            return Err("save per-file byte limit exceeded");
        }
        let mut hash = Sha256::new();
        let mut bytes = 0u64;
        let mut buf = [0u8; 65536];
        loop {
            let n = file.read(&mut buf).map_err(|_| "save file read failed")?;
            if n == 0 {
                break;
            }
            bytes += n as u64;
            if bytes > before.bytes {
                return Err("save file grew during read");
            }
            hash.update(&buf[..n]);
        }
        if bytes != before.bytes || before != Stamp::of(&metadata(&file)?) {
            return Err("save file changed during read");
        }
        Ok(SavedFile {
            bytes,
            sha256: hex(&hash.finalize()),
            stamp: before,
        })
    }
    fn relative_valid(path: &str) -> SaveResult<Vec<&str>> {
        if path.len() > MAX_PATH_BYTES {
            return Err("save path byte limit exceeded");
        }
        let parts: Vec<_> = path.split('/').collect();
        if parts.len() > MAX_DEPTH {
            return Err("save depth limit exceeded");
        }
        for p in &parts {
            component(p)?;
        }
        Ok(parts)
    }
    fn layout(path: &str, is_dir: bool, id: &SaveIdentity) -> SaveResult<()> {
        let p = relative_valid(path)?;
        if !upper_hex(p[0], 16) {
            return Err("save XUID directory refused");
        }
        if p.len() >= 2 && (!upper_hex(p[1], 8) || p[1] != format!("{:08X}", id.title_id)) {
            return Err("save title identity mismatch");
        }
        if p.len() >= 3 && !upper_hex(p[2], 8) {
            return Err("save content type refused");
        }
        if p.len() < 4 && !is_dir {
            return Err("incomplete save content tree");
        }
        if p.len() >= 4 {
            let package = if p.len() == 4 && !is_dir {
                p[3].strip_suffix(".title")
                    .ok_or("save package sidecar refused")?
            } else {
                p[3]
            };
            component(package)?;
            if package.len() > 42 {
                return Err("save package name too long");
            }
        }
        Ok(())
    }
    fn validate_relationships(
        dirs: &BTreeMap<String, Stamp>,
        files: &BTreeMap<String, SavedFile>,
    ) -> SaveResult<()> {
        for p in dirs.keys().chain(files.keys()) {
            if let Some((parent, _)) = p.rsplit_once('/') {
                if !dirs.contains_key(parent) {
                    return Err("incomplete save parent tree");
                }
            }
        }
        for p in files.keys() {
            if p.split('/').count() == 4 {
                if !dirs.contains_key(p.strip_suffix(".title").ok_or("save sidecar refused")?) {
                    return Err("orphan save title sidecar refused");
                }
            }
        }
        Ok(())
    }
    fn snapshot(root: &Dir, id: &SaveIdentity) -> SaveResult<Snapshot> {
        identity_value(id)?;
        let mut s = Snapshot {
            root: root.stamp()?,
            dirs: BTreeMap::new(),
            files: BTreeMap::new(),
            ignored: BTreeMap::new(),
            total: 0,
        };
        let mut pending = vec![(root.child_dot()?, String::new())];
        let mut entries = 0usize;
        while let Some((dir, prefix)) = pending.pop() {
            let before = dir.stamp()?;
            for name in dir.names()? {
                entries += 1;
                if entries > MAX_ENTRIES {
                    return Err("save entry limit exceeded");
                }
                let path = if prefix.is_empty() {
                    name.clone()
                } else {
                    format!("{prefix}/{name}")
                };
                let kind = dir.kind(&name)?.ok_or("save entry disappeared")?;
                if prefix.is_empty() && DIAGNOSTICS.contains(&name.as_str()) {
                    s.ignored.insert(
                        name.clone(),
                        Stamp::of(&metadata(&dir.file(&name, false)?)?),
                    );
                    continue;
                }
                let is_dir = kind == libc::S_IFDIR as u32;
                if !is_dir && kind != libc::S_IFREG as u32 {
                    return Err("symlink or special save entry refused");
                }
                layout(&path, is_dir, id)?;
                if is_dir {
                    let child = dir.child(&name)?;
                    s.dirs.insert(path.clone(), child.stamp()?);
                    pending.push((child, path));
                } else {
                    if s.files.len() >= MAX_FILES {
                        return Err("save file count limit exceeded");
                    }
                    let f = hash_file(dir.file(&name, false)?)?;
                    if Stamp::of(&metadata(&dir.file(&name, false)?)?) != f.stamp {
                        return Err("save file replaced during scan");
                    }
                    s.total = s
                        .total
                        .checked_add(f.bytes)
                        .ok_or("save total byte limit exceeded")?;
                    if s.total > MAX_TOTAL_BYTES {
                        return Err("save total byte limit exceeded");
                    }
                    s.files.insert(path, f);
                }
            }
            if before != dir.stamp()? {
                return Err("save directory changed during scan");
            }
        }
        validate_relationships(&s.dirs, &s.files)?;
        root.verify_path()?;
        if s.root != root.stamp()? {
            return Err("save root changed during scan");
        }
        Ok(s)
    }
    #[derive(Clone, Copy, Debug, PartialEq, Eq)]
    enum Phase {
        Scanned,
        FileCopied,
        BeforePublish,
    }
    type Hook<'a> = &'a mut dyn FnMut(Phase) -> SaveResult<()>;
    fn copy_snapshot(
        source: &Dir,
        dest: &Dir,
        before: &Snapshot,
        id: &SaveIdentity,
        hook: Hook<'_>,
    ) -> SaveResult<()> {
        if !dest.names()?.is_empty() {
            return Err("seed destination must already be empty");
        }
        for path in before.dirs.keys() {
            let (parent, name) = dest.relative_parent(path)?;
            parent.create(&name)?;
        }
        for (path, expected) in &before.files {
            let (parent, name) = source.relative_parent(path)?;
            let mut input = parent.file(&name, false)?;
            if Stamp::of(&metadata(&input)?) != expected.stamp {
                return Err("save source replaced before copy");
            }
            let (output_parent, output_name) = dest.relative_parent(path)?;
            let mut output = output_parent.file(&output_name, true)?;
            let mut bytes = 0u64;
            let mut digest = Sha256::new();
            let mut buf = [0u8; 65536];
            loop {
                let n = input.read(&mut buf).map_err(|_| "save copy read failed")?;
                if n == 0 {
                    break;
                }
                bytes += n as u64;
                if bytes > expected.bytes {
                    return Err("save source grew during copy");
                }
                output
                    .write_all(&buf[..n])
                    .map_err(|_| "save copy write failed")?;
                digest.update(&buf[..n]);
            }
            output.sync_all().map_err(|_| "save copy sync failed")?;
            if bytes != expected.bytes
                || hex(&digest.finalize()) != expected.sha256
                || Stamp::of(&metadata(&input)?) != expected.stamp
            {
                return Err("save source changed during copy");
            }
            hook(Phase::FileCopied)?;
        }
        if snapshot(dest, id)?.data() != before.data() {
            return Err("save copy readback mismatch");
        }
        if snapshot(source, id)? != *before {
            return Err("save source changed before publication");
        }
        source.verify_path()?;
        dest.verify_path()?;
        Ok(())
    }
    fn manifest(id: &SaveIdentity, snapshot: &Snapshot) -> SaveResult<Vec<u8>> {
        let mut value = snapshot.data();
        value["schema"] = json!(1);
        value["identity"] = identity_value(id)?;
        let bytes = encoded(&value)?;
        if bytes.len() as u64 > MAX_MANIFEST_BYTES {
            return Err("save manifest byte limit exceeded");
        }
        Ok(bytes)
    }
    fn manifest_data(bytes: &[u8], id: &SaveIdentity) -> SaveResult<Value> {
        let v = canonical_json(bytes)?;
        exact_keys(
            &v,
            &[
                "schema",
                "identity",
                "directories",
                "files",
                "file_count",
                "total_bytes",
            ],
        )?;
        if v["schema"].as_u64() != Some(1) || v["identity"] != identity_value(id)? {
            return Err("seed manifest identity or schema mismatch");
        }
        let dirs = v["directories"]
            .as_array()
            .ok_or("seed directories refused")?;
        let files = v["files"].as_array().ok_or("seed files refused")?;
        if files.len() > MAX_FILES
            || dirs.len() + files.len() > MAX_ENTRIES
            || v["file_count"].as_u64() != Some(files.len() as u64)
        {
            return Err("seed manifest count refused");
        }
        let dummy = Stamp {
            dev: 0,
            ino: 0,
            bytes: 0,
            links: 0,
            mtime: 0,
            mtime_ns: 0,
            ctime: 0,
            ctime_ns: 0,
        };
        let mut ds = BTreeMap::new();
        let mut fs = BTreeMap::new();
        let mut total = 0u64;
        let mut prior = "";
        for d in dirs {
            let path = d.as_str().ok_or("seed directory path refused")?;
            layout(path, true, id)?;
            if path <= prior {
                return Err("seed paths must be unique and sorted");
            }
            prior = path;
            ds.insert(path.to_owned(), dummy);
        }
        prior = "";
        for f in files {
            exact_keys(f, &["path", "bytes", "sha256"])?;
            let path = f["path"].as_str().ok_or("seed file path refused")?;
            layout(path, false, id)?;
            if path <= prior || ds.contains_key(path) {
                return Err("seed paths must be unique and sorted");
            }
            prior = path;
            let size = f["bytes"]
                .as_u64()
                .filter(|n| *n <= MAX_FILE_BYTES)
                .ok_or("seed file byte limit refused")?;
            let hash = f["sha256"]
                .as_str()
                .filter(|h| lower_hex(h, 64))
                .ok_or("seed file hash refused")?;
            total = total
                .checked_add(size)
                .filter(|n| *n <= MAX_TOTAL_BYTES)
                .ok_or("seed total byte limit refused")?;
            fs.insert(
                path.to_owned(),
                SavedFile {
                    bytes: size,
                    sha256: hash.to_owned(),
                    stamp: dummy,
                },
            );
        }
        if v["total_bytes"].as_u64() != Some(total) {
            return Err("seed total bytes mismatch");
        }
        validate_relationships(&ds, &fs)?;
        let mut data = v.as_object().unwrap().clone();
        data.remove("schema");
        data.remove("identity");
        Ok(Value::Object(data))
    }
    /// Export a new immutable manifest outside source/Game; return its SHA256.
    /// The producer must stop all save writers before calling this function.
    pub fn export_seed_manifest(
        source_content: &Path,
        manifest_path: &Path,
        protected_game_root: &Path,
        expected: &SaveIdentity,
    ) -> SaveResult<String> {
        export_impl(
            source_content,
            manifest_path,
            protected_game_root,
            expected,
            &mut |_| Ok(()),
        )
    }
    fn export_impl(
        source_content: &Path,
        manifest_path: &Path,
        protected_game_root: &Path,
        expected: &SaveIdentity,
        hook: Hook<'_>,
    ) -> SaveResult<String> {
        let game = Dir::absolute(protected_game_root)?;
        let source = Dir::absolute(source_content)?;
        disjoint_dirs(&source, &game)?;
        let (parent, name) = new_parent(manifest_path)?;
        protected(manifest_path, &game)?;
        disjoint(manifest_path, &source.path)?;
        if parent.kind(&name)?.is_some() {
            return Err("seed manifest destination exists");
        }
        let before = snapshot(&source, expected)?;
        hook(Phase::Scanned)?;
        let bytes = manifest(expected, &before)?;
        hook(Phase::BeforePublish)?;
        if snapshot(&source, expected)? != before {
            return Err("save source changed before manifest publication");
        }
        source.verify_path()?;
        parent.verify_path()?;
        game.verify_path()?;
        publish_new(&parent, &name, &bytes)?;
        Ok(hash(&bytes))
    }
    /// Copy a verified immutable seed into an already admitted empty root.
    /// A refusal can leave an incomplete destination; it must never be launched.
    pub fn copy_seed(
        source_content: &Path,
        manifest_path: &Path,
        expected_manifest_sha256: &str,
        destination_content: &Path,
        protected_game_root: &Path,
        expected: &SaveIdentity,
    ) -> SaveResult<()> {
        copy_seed_impl(
            source_content,
            manifest_path,
            expected_manifest_sha256,
            destination_content,
            protected_game_root,
            expected,
            &mut |_| Ok(()),
        )
    }
    fn copy_seed_impl(
        source_content: &Path,
        manifest_path: &Path,
        expected_hash: &str,
        destination_content: &Path,
        protected_game_root: &Path,
        expected: &SaveIdentity,
        hook: Hook<'_>,
    ) -> SaveResult<()> {
        if !lower_hex(expected_hash, 64) {
            return Err("seed manifest expected hash refused");
        }
        let game = Dir::absolute(protected_game_root)?;
        let source = Dir::absolute(source_content)?;
        let dest = Dir::absolute(destination_content)?;
        let (manifest_parent, manifest_name) = new_parent(manifest_path)?;
        disjoint_dirs(&source, &game)?;
        disjoint_dirs(&dest, &game)?;
        protected(manifest_path, &game)?;
        disjoint_dirs(&source, &dest)?;
        disjoint(manifest_path, &source.path)?;
        disjoint(manifest_path, &dest.path)?;
        if !dest.names()?.is_empty() {
            return Err("seed destination must already be empty");
        }
        let bytes = read_small(&manifest_parent, &manifest_name, MAX_MANIFEST_BYTES)?;
        if hash(&bytes) != expected_hash {
            return Err("seed manifest hash mismatch");
        }
        let wanted = manifest_data(&bytes, expected)?;
        let before = snapshot(&source, expected)?;
        if before.data() != wanted {
            return Err("seed content differs from manifest");
        }
        hook(Phase::Scanned)?;
        copy_snapshot(&source, &dest, &before, expected, hook)?;
        hook(Phase::BeforePublish)?;
        if snapshot(&source, expected)? != before {
            return Err("seed source changed before completion");
        }
        if snapshot(&dest, expected)?.data() != wanted {
            return Err("seed destination changed before completion");
        }
        if read_small(&manifest_parent, &manifest_name, MAX_MANIFEST_BYTES)? != bytes {
            return Err("seed manifest replaced during copy");
        }
        manifest_parent.verify_path()?;
        game.verify_path()?;
        Ok(())
    }
    fn session_record(key: &str, id: &str) -> Value {
        json!({"schema":1,"identity_sha256":key,"session_id":id})
    }
    fn store_lock(dir: &Dir) -> SaveResult<File> {
        let n = c"store.lock";
        if let Some(kind) = dir.kind("store.lock")? {
            if kind != libc::S_IFREG as u32 {
                return Err("save store lock type refused");
            }
        }
        let f = raw_file(unsafe {
            libc::openat(
                dir.file.as_raw_fd(),
                n.as_ptr(),
                libc::O_RDWR
                    | libc::O_CREAT
                    | libc::O_NOFOLLOW
                    | libc::O_CLOEXEC
                    | libc::O_NONBLOCK,
                0o600,
            )
        })?;
        let m = metadata(&f)?;
        if !m.is_file() || m.nlink() != 1 || m.len() != 0 {
            return Err("save store lock refused");
        }
        f.try_lock()
            .map_err(|_| "save store already in use or lock unavailable")?;
        let check = dir.file("store.lock", false)?;
        if !Stamp::of(&m).same_node(Stamp::of(&metadata(&check)?)) {
            return Err("save store lock replaced");
        }
        Ok(f)
    }
    /// Create an exclusive session, seed it from latest, then atomically publish
    /// latest. Store parent must exist; session_id is exactly 32 lowercase hex.
    pub fn prepare_session(
        store_root: &Path,
        protected_game_root: &Path,
        expected: &SaveIdentity,
        session_id: &str,
    ) -> SaveResult<SaveSession> {
        prepare_impl(
            store_root,
            protected_game_root,
            expected,
            session_id,
            &mut |_| Ok(()),
        )
    }
    fn prepare_impl(
        store_root: &Path,
        protected_game_root: &Path,
        expected: &SaveIdentity,
        session_id: &str,
        hook: Hook<'_>,
    ) -> SaveResult<SaveSession> {
        if !lower_hex(session_id, 32) {
            return Err("save session ID refused");
        }
        let identity = encoded(&json!({"schema":1,"identity":identity_value(expected)?}))?;
        let key = hash(&identity);
        let game = Dir::absolute(protected_game_root)?;
        let (parent, name) = new_parent(store_root)?;
        protected(store_root, &game)?;
        let store = parent.ensure(&name)?;
        disjoint_dirs(&store, &game)?;
        let namespace = store.ensure(&key)?;
        let lease = store_lock(&namespace)?;
        if namespace.kind("identity.json")?.is_some() {
            if read_small(&namespace, "identity.json", META_MAX)? != identity {
                return Err("save namespace identity mismatch");
            }
        } else {
            publish_new(&namespace, "identity.json", &identity)?;
        }
        let sessions = namespace.ensure("sessions")?;
        let latest = if namespace.kind("latest.json")?.is_some() {
            Some(read_small(&namespace, "latest.json", META_MAX)?)
        } else {
            None
        };
        // A missing pointer is an empty first launch only while no session
        // exists. Never discard a retained or interrupted payload implicitly.
        if latest.is_none() && !sessions.names()?.is_empty() {
            return Err("save sessions exist without a latest pointer");
        }
        let previous = if let Some(bytes) = &latest {
            let value = canonical_json(bytes)?;
            exact_keys(&value, &["schema", "identity_sha256", "session_id"])?;
            let previous_id = value["session_id"]
                .as_str()
                .filter(|s| lower_hex(s, 32))
                .ok_or("latest save session ID refused")?;
            if value != session_record(&key, previous_id) {
                return Err("latest save identity mismatch");
            }
            let session = sessions.child(previous_id)?;
            if read_small(&session, "session.json", META_MAX)? != *bytes {
                return Err("latest save session metadata mismatch");
            }
            let source = session.child("content")?;
            let snap = snapshot(&source, expected)?;
            Some((source, snap))
        } else {
            None
        };
        hook(Phase::Scanned)?;
        let session = sessions.create(session_id)?;
        let content = session.create("content")?;
        if let Some((source, before)) = &previous {
            copy_snapshot(source, &content, before, expected, hook)?;
        }
        let bytes = encoded(&session_record(&key, session_id))?;
        publish_new(&session, "session.json", &bytes)?;
        hook(Phase::BeforePublish)?;
        if let Some((source, before)) = &previous {
            if snapshot(source, expected)? != *before {
                return Err("save source changed before latest publication");
            }
            if snapshot(&content, expected)?.data() != before.data() {
                return Err("save destination changed before latest publication");
            }
        } else if !content.names()?.is_empty() {
            return Err("first save session changed before publication");
        }
        content.verify_path()?;
        session.verify_path()?;
        sessions.verify_path()?;
        namespace.verify_path()?;
        store.verify_path()?;
        parent.verify_path()?;
        game.verify_path()?;
        let now = if namespace.kind("latest.json")?.is_some() {
            Some(read_small(&namespace, "latest.json", META_MAX)?)
        } else {
            None
        };
        if now != latest {
            return Err("latest save pointer changed");
        }
        if !Stamp::of(&metadata(&lease)?)
            .same_node(Stamp::of(&metadata(&namespace.file("store.lock", false)?)?))
        {
            return Err("save store lock replaced before publication");
        }
        let temp = format!(".latest.{session_id}.pending");
        write_file(&namespace, &temp, &bytes)?;
        let t = component(&temp)?;
        if unsafe {
            libc::renameat(
                namespace.file.as_raw_fd(),
                t.as_ptr(),
                namespace.file.as_raw_fd(),
                c"latest.json".as_ptr(),
            )
        } != 0
        {
            return Err("latest save atomic publication failed");
        }
        Ok(SaveSession {
            content: content.path,
            session: session.path,
            _lease: lease,
        })
    }
    #[cfg(test)]
    mod tests {
        use super::*;
        use std::{
            fs,
            sync::atomic::{AtomicU64, Ordering},
        };
        static NEXT: AtomicU64 = AtomicU64::new(0);
        const PACKAGE: &str = "0000000000000001/01020304/00000001/Manual Save 01";
        const DATA: &str = "0000000000000001/01020304/00000001/Manual Save 01/Save Data.bin";
        struct Fixture {
            root: PathBuf,
            game: PathBuf,
            store: PathBuf,
            source: PathBuf,
            manifest: PathBuf,
        }
        impl Fixture {
            fn new() -> Self {
                let root = std::env::temp_dir().canonicalize().unwrap().join(format!(
                    "public-save-store-{}-{}",
                    std::process::id(),
                    NEXT.fetch_add(1, Ordering::Relaxed)
                ));
                fs::create_dir(&root).unwrap();
                let game = root.join("Game");
                let source = root.join("source");
                fs::create_dir(&game).unwrap();
                fs::create_dir(&source).unwrap();
                Self {
                    store: root.join("OwnedSaves"),
                    manifest: root.join("seed.json"),
                    root,
                    game,
                    source,
                }
            }
            fn empty(&self, name: &str) -> PathBuf {
                let p = self.root.join(name);
                fs::create_dir(&p).unwrap();
                p
            }
            fn seed(&self) -> String {
                put_save(&self.source, b"authored first save");
                export_seed_manifest(&self.source, &self.manifest, &self.game, &identity()).unwrap()
            }
        }
        impl Drop for Fixture {
            fn drop(&mut self) {
                fs::remove_dir_all(&self.root).unwrap();
            }
        }
        fn identity() -> SaveIdentity {
            SaveIdentity {
                title_id: 0x01020304,
                media_id: 0x05060708,
                version: 0x10203040,
                base_version: 0x10200000,
                xex_sha256: "ab".repeat(32),
            }
        }
        fn sid(n: u64) -> String {
            format!("{n:032x}")
        }
        fn put_save(root: &Path, bytes: &[u8]) {
            fs::create_dir_all(root.join(PACKAGE)).unwrap();
            fs::write(root.join(DATA), bytes).unwrap();
            fs::write(
                root.join(format!("{PACKAGE}.title")),
                "Authored Save Display Name",
            )
            .unwrap();
        }
        fn latest(session: &SaveSession) -> PathBuf {
            session
                .session
                .parent()
                .unwrap()
                .parent()
                .unwrap()
                .join("latest.json")
        }
        fn rewrite_manifest(f: &Fixture, change: impl FnOnce(&mut Value)) -> String {
            let mut v: Value = serde_json::from_slice(&fs::read(&f.manifest).unwrap()).unwrap();
            change(&mut v);
            let bytes = encoded(&v).unwrap();
            fs::write(&f.manifest, &bytes).unwrap();
            hash(&bytes)
        }

        #[test]
        fn continuation_preserves_each_session_and_omits_diagnostics() {
            let f = Fixture::new();
            let id = identity();
            let first = prepare_session(&f.store, &f.game, &id, &sid(1)).unwrap();
            assert!(fs::read_dir(&first.content).unwrap().next().is_none());
            put_save(&first.content, b"authored original");
            fs::write(
                first.content.join("diagnostic-final.json"),
                b"authored diagnostic excluded",
            )
            .unwrap();
            fs::write(
                first.content.join(".first-failure.json.pending"),
                b"incomplete diagnostic excluded",
            )
            .unwrap();
            let first_path = first.content.clone();
            let pointer = latest(&first);
            drop(first);
            let second = prepare_session(&f.store, &f.game, &id, &sid(2)).unwrap();
            assert_eq!(
                fs::read(second.content.join(DATA)).unwrap(),
                b"authored original"
            );
            assert_eq!(
                fs::read(second.content.join(format!("{PACKAGE}.title"))).unwrap(),
                b"Authored Save Display Name"
            );
            assert!(!second.content.join("diagnostic-final.json").exists());
            assert!(!second.content.join(".first-failure.json.pending").exists());
            assert_ne!(
                fs::metadata(first_path.join(DATA)).unwrap().ino(),
                fs::metadata(second.content.join(DATA)).unwrap().ino()
            );
            let second_path = second.content.clone();
            fs::write(second.content.join(DATA), b"authored next save").unwrap();
            drop(second);
            let third = prepare_session(&f.store, &f.game, &id, &sid(3)).unwrap();
            assert_eq!(
                fs::read(third.content.join(DATA)).unwrap(),
                b"authored next save"
            );
            assert_eq!(
                fs::read(first_path.join(DATA)).unwrap(),
                b"authored original"
            );
            assert_eq!(
                fs::read(second_path.join(DATA)).unwrap(),
                b"authored next save"
            );
            let p: Value = serde_json::from_slice(&fs::read(pointer).unwrap()).unwrap();
            assert_eq!(p["session_id"], sid(3));
        }
        #[test]
        fn lifetime_lock_and_exclusive_session_admission() {
            let f = Fixture::new();
            let first = prepare_session(&f.store, &f.game, &identity(), &sid(1)).unwrap();
            let pointer = latest(&first);
            let before = fs::read(&pointer).unwrap();
            assert!(prepare_session(&f.store, &f.game, &identity(), &sid(2)).is_err());
            assert_eq!(fs::read(&pointer).unwrap(), before);
            drop(first);
            assert!(prepare_session(&f.store, &f.game, &identity(), &sid(1)).is_err());
            assert_eq!(fs::read(&pointer).unwrap(), before);
            assert!(prepare_session(&f.store, &f.game, &identity(), "../bad").is_err());
            assert!(prepare_session(&f.store, &f.game, &identity(), &"A".repeat(32)).is_err());
            assert!(prepare_session(&f.store, &f.game, &identity(), &sid(2)).is_ok());
        }
        #[test]
        fn every_verified_identity_field_separates_namespaces() {
            let f = Fixture::new();
            let original = identity();
            let mut variants = vec![original.clone(); 6];
            variants[1].title_id ^= 1;
            variants[2].media_id ^= 1;
            variants[3].version ^= 1;
            variants[4].base_version ^= 1;
            variants[5].xex_sha256 = "cd".repeat(32);
            let mut paths = std::collections::BTreeSet::new();
            for id in variants {
                let s = prepare_session(&f.store, &f.game, &id, &sid(1)).unwrap();
                assert!(paths.insert(s.session.clone()));
            }
            let session = prepare_session(&f.store, &f.game, &original, &sid(2)).unwrap();
            let pointer = latest(&session);
            let bytes = fs::read(&pointer).unwrap();
            let namespace = pointer.parent().unwrap().to_owned();
            drop(session);
            fs::write(namespace.join("identity.json"), b"{}").unwrap();
            assert!(prepare_session(&f.store, &f.game, &original, &sid(3)).is_err());
            assert_eq!(fs::read(pointer).unwrap(), bytes);
        }
        #[test]
        fn interrupted_copy_and_late_refusal_keep_latest_and_sources() {
            for phase in [Phase::FileCopied, Phase::BeforePublish] {
                let f = Fixture::new();
                let first = prepare_session(&f.store, &f.game, &identity(), &sid(1)).unwrap();
                put_save(&first.content, b"authored stable");
                let pointer = latest(&first);
                let before = fs::read(&pointer).unwrap();
                let source = first.content.clone();
                let sessions = first.session.parent().unwrap().to_owned();
                drop(first);
                let result = prepare_impl(&f.store, &f.game, &identity(), &sid(2), &mut |p| {
                    if p == phase {
                        Err("authored interruption")
                    } else {
                        Ok(())
                    }
                });
                assert_eq!(result.unwrap_err(), "authored interruption");
                assert_eq!(fs::read(&pointer).unwrap(), before);
                assert_eq!(fs::read(source.join(DATA)).unwrap(), b"authored stable");
                assert!(sessions.join(sid(2)).exists());
                let third = prepare_session(&f.store, &f.game, &identity(), &sid(3)).unwrap();
                assert_eq!(
                    fs::read(third.content.join(DATA)).unwrap(),
                    b"authored stable"
                );
                assert!(sessions.join(sid(2)).exists());
            }
        }
        #[test]
        fn source_mutation_and_destination_tamper_refuse_latest_update() {
            for mutate_source in [true, false] {
                let f = Fixture::new();
                let first = prepare_session(&f.store, &f.game, &identity(), &sid(1)).unwrap();
                put_save(&first.content, b"authored stable");
                let pointer = latest(&first);
                let before = fs::read(&pointer).unwrap();
                let source = first.content.clone();
                let dest = first.session.parent().unwrap().join(sid(2)).join("content");
                drop(first);
                let result = prepare_impl(&f.store, &f.game, &identity(), &sid(2), &mut |p| {
                    if p == Phase::BeforePublish {
                        fs::write(
                            if mutate_source {
                                source.join(DATA)
                            } else {
                                dest.join(DATA)
                            },
                            b"authored changed",
                        )
                        .unwrap();
                    }
                    Ok(())
                });
                assert!(result.is_err());
                assert_eq!(fs::read(&pointer).unwrap(), before);
            }
        }
        #[test]
        fn malformed_previous_payload_is_refused_instead_of_empty_success() {
            for name in [
                "unknown-save.bin",
                "0000000000000001/01020304/00000001/Orphan.title",
                "0000000000000001/11111111/00000001/Other/data",
            ] {
                let f = Fixture::new();
                let first = prepare_session(&f.store, &f.game, &identity(), &sid(1)).unwrap();
                let bad = first.content.join(name);
                fs::create_dir_all(bad.parent().unwrap()).unwrap();
                fs::write(&bad, b"authored malformed payload").unwrap();
                let pointer = latest(&first);
                let before = fs::read(&pointer).unwrap();
                drop(first);
                assert!(prepare_session(&f.store, &f.game, &identity(), &sid(2)).is_err());
                assert_eq!(fs::read(pointer).unwrap(), before);
                assert!(bad.exists());
            }
        }
        #[test]
        fn corrupt_missing_or_replaced_latest_is_never_treated_as_empty() {
            for mode in 0..3 {
                let f = Fixture::new();
                let first = prepare_session(&f.store, &f.game, &identity(), &sid(1)).unwrap();
                let pointer = latest(&first);
                let session = first.session.clone();
                drop(first);
                match mode {
                    0 => fs::write(&pointer, b"{\"schema\":1}").unwrap(),
                    1 => fs::rename(session.join("content"), session.join("retained-content"))
                        .unwrap(),
                    _ => fs::write(session.join("session.json"), b"{}").unwrap(),
                }
                let before = fs::read(&pointer).unwrap();
                assert!(prepare_session(&f.store, &f.game, &identity(), &sid(2)).is_err());
                assert_eq!(fs::read(pointer).unwrap(), before);
            }
        }
        #[test]
        fn missing_latest_or_interrupted_first_session_requires_explicit_recovery() {
            for missing_pointer in [true, false] {
                let f = Fixture::new();
                if missing_pointer {
                    let first = prepare_session(&f.store, &f.game, &identity(), &sid(1)).unwrap();
                    put_save(&first.content, b"authored retained save");
                    let pointer = latest(&first);
                    let source = first.content.clone();
                    drop(first);
                    fs::remove_file(&pointer).unwrap();
                    assert!(prepare_session(&f.store, &f.game, &identity(), &sid(2)).is_err());
                    assert!(!pointer.exists());
                    assert_eq!(
                        fs::read(source.join(DATA)).unwrap(),
                        b"authored retained save"
                    );
                } else {
                    assert!(
                        prepare_impl(&f.store, &f.game, &identity(), &sid(1), &mut |p| {
                            if p == Phase::BeforePublish {
                                Err("authored first-session interruption")
                            } else {
                                Ok(())
                            }
                        })
                        .is_err()
                    );
                    assert!(prepare_session(&f.store, &f.game, &identity(), &sid(2)).is_err());
                }
            }
        }
        #[test]
        fn seed_roundtrip_uses_exact_manifest_identity_and_fresh_destination() {
            let f = Fixture::new();
            let expected = f.seed();
            let dest = f.empty("dest");
            copy_seed(
                &f.source,
                &f.manifest,
                &expected,
                &dest,
                &f.game,
                &identity(),
            )
            .unwrap();
            assert_eq!(fs::read(dest.join(DATA)).unwrap(), b"authored first save");
            assert!(
                copy_seed(
                    &f.source,
                    &f.manifest,
                    &expected,
                    &dest,
                    &f.game,
                    &identity()
                )
                .is_err()
            );
            assert!(export_seed_manifest(&f.source, &f.manifest, &f.game, &identity()).is_err());
            let empty = f.empty("wrong");
            let mut wrong = identity();
            wrong.version ^= 1;
            assert!(copy_seed(&f.source, &f.manifest, &expected, &empty, &f.game, &wrong).is_err());
            assert!(
                copy_seed(
                    &f.source,
                    &f.manifest,
                    &"00".repeat(32),
                    &empty,
                    &f.game,
                    &identity()
                )
                .is_err()
            );
            assert!(fs::read_dir(empty).unwrap().next().is_none());
        }
        #[test]
        fn changed_seed_after_export_fails_before_writing_destination() {
            let f = Fixture::new();
            let expected = f.seed();
            let dest = f.empty("dest");
            fs::write(f.source.join(DATA), b"authored later save").unwrap();
            assert!(
                copy_seed(
                    &f.source,
                    &f.manifest,
                    &expected,
                    &dest,
                    &f.game,
                    &identity()
                )
                .is_err()
            );
            assert!(fs::read_dir(dest).unwrap().next().is_none());
        }
        #[test]
        fn same_bytes_source_replacement_and_root_replacement_are_detected() {
            for replace_root in [false, true] {
                let f = Fixture::new();
                let expected = f.seed();
                let dest = f.empty("dest");
                let result = copy_seed_impl(
                    &f.source,
                    &f.manifest,
                    &expected,
                    &dest,
                    &f.game,
                    &identity(),
                    &mut |phase| {
                        if phase == Phase::Scanned {
                            if replace_root {
                                fs::rename(&f.source, f.root.join("retained-source")).unwrap();
                                fs::create_dir(&f.source).unwrap();
                            } else {
                                let p = f.source.join(DATA);
                                fs::rename(&p, f.root.join("retained-file")).unwrap();
                                fs::write(p, b"authored first save").unwrap();
                            }
                        }
                        Ok(())
                    },
                );
                assert!(result.is_err());
            }
        }
        #[test]
        fn export_mutation_refuses_manifest_publication() {
            let f = Fixture::new();
            put_save(&f.source, b"authored original");
            let result = export_impl(&f.source, &f.manifest, &f.game, &identity(), &mut |p| {
                if p == Phase::BeforePublish {
                    fs::write(f.source.join(DATA), b"authored changed").unwrap();
                }
                Ok(())
            });
            assert!(result.is_err());
            assert!(!f.manifest.exists());
        }
        #[test]
        fn symlinks_special_files_hardlinks_and_path_overlap_are_refused() {
            use std::os::unix::fs::symlink;
            for mode in 0..4 {
                let f = Fixture::new();
                put_save(&f.source, b"authored save");
                let bad = f.source.join(PACKAGE).join("bad");
                match mode {
                    0 => symlink(&f.game, &bad).unwrap(),
                    1 => {
                        let p = CString::new(bad.to_str().unwrap()).unwrap();
                        assert_eq!(unsafe { libc::mkfifo(p.as_ptr(), 0o600) }, 0);
                    }
                    2 => fs::hard_link(f.source.join(DATA), &bad).unwrap(),
                    _ => {
                        fs::rename(&f.source, f.root.join("real-source")).unwrap();
                        symlink(f.root.join("real-source"), &f.source).unwrap();
                    }
                }
                assert!(
                    export_seed_manifest(&f.source, &f.manifest, &f.game, &identity()).is_err()
                );
                assert!(!f.manifest.exists());
            }
            let f = Fixture::new();
            let expected = f.seed();
            assert!(
                copy_seed(
                    &f.source,
                    &f.manifest,
                    &expected,
                    &f.source,
                    &f.game,
                    &identity()
                )
                .is_err()
            );
            assert!(
                copy_seed(
                    &f.source,
                    &f.manifest,
                    &expected,
                    &f.game,
                    &f.game,
                    &identity()
                )
                .is_err()
            );
            assert!(
                prepare_session(&f.game.join("OwnedSaves"), &f.game, &identity(), &sid(1)).is_err()
            );
            assert!(
                export_seed_manifest(&f.source, &f.source.join("bad.json"), &f.game, &identity())
                    .is_err()
            );
        }
        #[test]
        fn source_symlink_swap_after_scan_cannot_copy_outside_data() {
            use std::os::unix::fs::symlink;
            let f = Fixture::new();
            let expected = f.seed();
            let dest = f.empty("dest");
            let outside = f.root.join("outside-authored.bin");
            fs::write(&outside, b"outside authored marker").unwrap();
            let result = copy_seed_impl(
                &f.source,
                &f.manifest,
                &expected,
                &dest,
                &f.game,
                &identity(),
                &mut |p| {
                    if p == Phase::Scanned {
                        fs::rename(f.source.join(DATA), f.root.join("retained-original")).unwrap();
                        symlink(&outside, f.source.join(DATA)).unwrap();
                    }
                    Ok(())
                },
            );
            assert!(result.is_err());
            assert!(!dest.join(DATA).exists());
            assert_eq!(fs::read(outside).unwrap(), b"outside authored marker");
        }
        #[test]
        fn size_depth_and_manifest_fields_are_bounded() {
            let f = Fixture::new();
            put_save(&f.source, b"small");
            let big = fs::OpenOptions::new()
                .write(true)
                .open(f.source.join(DATA))
                .unwrap();
            big.set_len(MAX_FILE_BYTES + 1).unwrap();
            drop(big);
            assert!(export_seed_manifest(&f.source, &f.manifest, &f.game, &identity()).is_err());
            assert!(!f.manifest.exists());
            let f = Fixture::new();
            put_save(&f.source, b"small");
            let mut deep = f.source.join(PACKAGE);
            for _ in 0..MAX_DEPTH {
                deep.push("d");
            }
            fs::create_dir_all(deep).unwrap();
            assert!(export_seed_manifest(&f.source, &f.manifest, &f.game, &identity()).is_err());
            for case in 0..9 {
                let f = Fixture::new();
                f.seed();
                let dest = f.empty("dest");
                let expected = rewrite_manifest(&f, |v| match case {
                    0 => v["extra"] = json!(0),
                    1 => v["file_count"] = json!(true),
                    2 => v["total_bytes"] = json!(MAX_TOTAL_BYTES + 1),
                    3 => v["files"][0]["bytes"] = json!(MAX_FILE_BYTES + 1),
                    4 => v["files"][0]["path"] = json!("../escape"),
                    5 => v["directories"][0] = json!("0000000000000001/../escape"),
                    6 => v["files"][0]["sha256"] = json!("FF".repeat(32)),
                    7 => v["directories"] = json!([]),
                    _ => v["files"] = json!(vec![v["files"][0].clone(); MAX_FILES + 1]),
                });
                assert!(
                    copy_seed(
                        &f.source,
                        &f.manifest,
                        &expected,
                        &dest,
                        &f.game,
                        &identity()
                    )
                    .is_err()
                );
                assert!(fs::read_dir(dest).unwrap().next().is_none());
            }
        }
        #[test]
        fn noncanonical_duplicate_metadata_and_diagnostic_links_are_refused() {
            assert_eq!(
                encoded(&json!({"z":0,"a":{"y":2,"b":1}})).unwrap(),
                b"{\"a\":{\"b\":1,\"y\":2},\"z\":0}"
            );
            assert!(canonical_json(b"{\"z\":0,\"a\":1}").is_err());
            let f = Fixture::new();
            f.seed();
            let dest = f.empty("dest");
            let mut bytes = fs::read(&f.manifest).unwrap();
            bytes.push(b'\n');
            fs::write(&f.manifest, &bytes).unwrap();
            assert!(
                copy_seed(
                    &f.source,
                    &f.manifest,
                    &hash(&bytes),
                    &dest,
                    &f.game,
                    &identity()
                )
                .is_err()
            );
            let f = Fixture::new();
            put_save(&f.source, b"authored save");
            std::os::unix::fs::symlink(&f.game, f.source.join("diagnostic-final.json")).unwrap();
            assert!(export_seed_manifest(&f.source, &f.manifest, &f.game, &identity()).is_err());
            assert!(canonical_json(b"{\"schema\":1,\"schema\":1}").is_err());
        }
    }
}

#[cfg(not(unix))]
mod imp {
    use super::*;
    use std::path::Path;
    pub fn prepare_session(
        _: &Path,
        _: &Path,
        _: &SaveIdentity,
        _: &str,
    ) -> SaveResult<SaveSession> {
        Err("save-store filesystem unsupported on this platform")
    }
    pub fn export_seed_manifest(
        _: &Path,
        _: &Path,
        _: &Path,
        _: &SaveIdentity,
    ) -> SaveResult<String> {
        Err("save-store filesystem unsupported on this platform")
    }
    pub fn copy_seed(
        _: &Path,
        _: &Path,
        _: &str,
        _: &Path,
        _: &Path,
        _: &SaveIdentity,
    ) -> SaveResult<()> {
        Err("save-store filesystem unsupported on this platform")
    }
}
