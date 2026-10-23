#[cfg(test)]
mod root_open_tests {
    use super::*;
    use std::{fs, os::unix::fs::symlink};

    struct Temp(PathBuf);
    impl Temp {
        fn new(label: &str) -> Self {
            let path = std::env::temp_dir()
                .canonicalize()
                .unwrap()
                .join(format!("public-root-open-{}-{label}", std::process::id()));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.0).unwrap();
        }
    }

    #[test]
    fn ancestor_and_final_symlink_swaps_after_canonicalization_are_refused() {
        let f = Temp::new("symlink-swap");
        let parent = f.0.join("parent");
        let target = parent.join("target");
        let outside = f.0.join("outside");
        fs::create_dir_all(&target).unwrap();
        fs::create_dir_all(outside.join("target")).unwrap();
        assert_eq!(target.canonicalize().unwrap(), target);
        assert!(Dir::absolute(&target).is_ok());

        // Deterministic swap between the existing canonical check and the
        // exact production open helper. A final-only O_NOFOLLOW is insufficient.
        let retained = f.0.join("retained-parent");
        fs::rename(&parent, &retained).unwrap();
        symlink(&outside, &parent).unwrap();
        let c = CString::new(target.to_str().unwrap()).unwrap();
        let weak = raw_file(unsafe {
            libc::open(
                c.as_ptr(),
                libc::O_RDONLY | libc::O_DIRECTORY | libc::O_NOFOLLOW | libc::O_CLOEXEC,
            )
        })
        .unwrap();
        assert_eq!(
            metadata(&weak).unwrap().ino(),
            fs::metadata(outside.join("target")).unwrap().ino()
        );
        assert!(root_file_no_symlinks(&target).is_err());
        drop(weak);

        fs::remove_file(&parent).unwrap();
        fs::rename(&retained, &parent).unwrap();
        fs::rename(&target, parent.join("retained-target")).unwrap();
        symlink(outside.join("target"), &target).unwrap();
        assert!(root_file_no_symlinks(&target).is_err());
    }

    #[test]
    fn canonical_components_and_replacement_identity_checks_remain_active() {
        let f = Temp::new("identity");
        let target = f.0.join("target");
        fs::create_dir(&target).unwrap();
        let opened = Dir::absolute(&target).unwrap();
        fs::rename(&target, f.0.join("retained-target")).unwrap();
        fs::create_dir(&target).unwrap();
        assert_eq!(opened.verify_path(), Err("save root replaced"));

        let forbidden = f.0.join("bad:name");
        fs::create_dir(&forbidden).unwrap();
        assert!(matches!(
            Dir::absolute(&forbidden),
            Err("save path component refused")
        ));
        let linked = f.0.join("linked");
        symlink(&target, &linked).unwrap();
        assert!(matches!(
            Dir::absolute(&linked),
            Err("canonical absolute save root required")
        ));
        assert!(Dir::absolute(Path::new("relative")).is_err());
    }

    #[test]
    fn final_root_is_readable_cloexec_directory_and_preserves_valid_names() {
        let f = Temp::new("descriptor");
        let target = f.0.join("Save Space 日本語");
        fs::create_dir(&target).unwrap();
        fs::write(target.join("authored"), b"authored marker").unwrap();
        let dir = Dir::absolute(&target).unwrap();
        assert_eq!(dir.names().unwrap(), ["authored"]);
        let fd = dir.file.as_raw_fd();
        assert_ne!(
            unsafe { libc::fcntl(fd, libc::F_GETFD) } & libc::FD_CLOEXEC,
            0
        );
        assert_eq!(
            unsafe { libc::fcntl(fd, libc::F_GETFL) } & libc::O_ACCMODE,
            libc::O_RDONLY
        );
        assert!(Dir::absolute(&target.join("authored")).is_err());
    }
}
