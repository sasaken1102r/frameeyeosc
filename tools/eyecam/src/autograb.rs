//! Automatic eyecam-grab: a copy installed by `install_grab.sh` (root-owned, with the file capability
//! cap_sys_ptrace=ep) that `eyecam-rec --serve` can start itself instead of asking for sudo.
//!
//! Before starting it, eyecam-rec checks that the copy is safe to trust: a regular file (not a symlink) owned by
//! root and not writable by anyone else, in a root-owned directory nobody else can write to, carrying
//! CAP_SYS_PTRACE in its permitted set with the effective flag. (Only root can set file capabilities, and writing
//! to the file clears them, so a copy that passes cannot have been altered by the user.)

use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::path::{Path, PathBuf};

/// Where install_grab.sh puts it (the first that exists is used). /home is the partition that survives SteamOS
/// updates; /var is per A/B slot, so a copy there can disappear after an update.
pub const PATHS: [&str; 2] = ["/home/.eyecam/eyecam-grab", "/var/lib/eyecam/eyecam-grab"];

/// The eye tracker the grab looks for (eyecam-grab checks it again itself).
pub const EYETRACKING_EXE: &str = "/opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking";

const CAP_SYS_PTRACE: u32 = 19;
const VFS_CAP_FLAGS_EFFECTIVE: u32 = 0x1;
const VFS_CAP_REVISION_MASK: u32 = 0xff00_0000;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum State {
    /// Installed and trustworthy.
    Ready(PathBuf),
    /// Not installed.
    Missing,
    /// Installed, but not owned by root / writable by others / in a writable directory: not used.
    Unsafe(String),
    /// Installed safely, but without cap_sys_ptrace=ep.
    NoCap(PathBuf),
    /// Installed safely, but older than MIN_SAFE_GRAB_VERSION (a version with a known problem): not used until
    /// install_grab.sh puts the shipped copy in place.
    TooOld(PathBuf),
}

impl State {
    /// The short form for status.json's `auto_grab`.
    pub fn label(&self) -> String {
        match self {
            State::Ready(_) => "ready".into(),
            State::Missing => "missing".into(),
            State::Unsafe(why) => format!("unsafe: {why}"),
            State::NoCap(_) => "no_cap".into(),
            State::TooOld(_) => "too_old".into(),
        }
    }
}

/// Whether the raw `security.capability` xattr grants CAP_SYS_PTRACE permitted + effective.
pub fn has_ptrace_cap(xattr: &[u8]) -> bool {
    if xattr.len() < 12 {
        return false;
    }
    let word = |i: usize| u32::from_le_bytes(xattr[i * 4..i * 4 + 4].try_into().unwrap());
    let magic = word(0);
    let revision = magic & VFS_CAP_REVISION_MASK;
    // Revision 1 (0x01000000) has one data word pair, revisions 2 and 3 have two; CAP_SYS_PTRACE is in the first.
    if !matches!(revision, 0x0100_0000 | 0x0200_0000 | 0x0300_0000) {
        return false;
    }
    let permitted = word(1);
    magic & VFS_CAP_FLAGS_EFFECTIVE != 0 && permitted & (1 << CAP_SYS_PTRACE) != 0
}

fn read_cap_xattr(path: &Path) -> Option<Vec<u8>> {
    let c = std::ffi::CString::new(path.as_os_str().as_encoded_bytes()).ok()?;
    let mut buf = vec![0u8; 64];
    let n = unsafe { libc::lgetxattr(c.as_ptr(), c"security.capability".as_ptr(), buf.as_mut_ptr().cast(), buf.len()) };
    if n < 0 {
        return None;
    }
    buf.truncate(n as usize);
    Some(buf)
}

/// Owned by root and not writable by group or others (no symlinks followed).
fn root_only(path: &Path) -> Result<std::fs::Metadata, String> {
    let m = std::fs::symlink_metadata(path).map_err(|e| format!("{}: {e}", path.display()))?;
    if m.file_type().is_symlink() {
        return Err(format!("{} is a symlink", path.display()));
    }
    if m.uid() != 0 {
        return Err(format!("{} is not owned by root", path.display()));
    }
    if m.permissions().mode() & 0o022 != 0 {
        return Err(format!("{} is writable by group or others", path.display()));
    }
    Ok(m)
}

/// Check one installed path.
pub fn check(path: &Path) -> State {
    if std::fs::symlink_metadata(path).is_err() {
        return State::Missing;
    }
    let file = match root_only(path) {
        Ok(m) => m,
        Err(e) => return State::Unsafe(e),
    };
    if !file.is_file() {
        return State::Unsafe(format!("{} is not a regular file", path.display()));
    }
    if let Some(dir) = path.parent()
        && let Err(e) = root_only(dir)
    {
        return State::Unsafe(e);
    }
    match read_cap_xattr(path) {
        Some(x) if has_ptrace_cap(&x) => {
            if std::fs::read(path).is_ok_and(|b| grab_version(&b) < MIN_SAFE_GRAB_VERSION) {
                State::TooOld(path.to_path_buf())
            } else {
                State::Ready(path.to_path_buf())
            }
        }
        _ => State::NoCap(path.to_path_buf()),
    }
}

/// The first installed path's state (Missing if none is installed).
pub fn find() -> State {
    PATHS.iter().map(|p| check(Path::new(p))).find(|s| *s != State::Missing).unwrap_or(State::Missing)
}

/// Size and modification time of a file, to notice when it changes without reading it.
fn stamp(path: &Path) -> Option<(u64, i64, i64)> {
    let m = std::fs::metadata(path).ok()?;
    Some((m.len(), m.mtime(), m.mtime_nsec()))
}

type OutdatedKey = (PathBuf, (u64, i64, i64), PathBuf, (u64, i64, i64));
static OUTDATED: std::sync::Mutex<Option<(OutdatedKey, bool)>> = std::sync::Mutex::new(None);

/// Whether `bundled`, the eyecam-grab shipped beside eyecam-rec, is a newer version than the installed copy (the
/// first of PATHS that exists): an update brought a new eyecam-grab and install_grab.sh has to be run again. False
/// when either is missing. The version markers are read again only after one of the files changed.
pub fn grab_outdated(bundled: &Path) -> bool {
    let Some(installed) = PATHS.iter().map(Path::new).find(|p| std::fs::symlink_metadata(p).is_ok()) else {
        return false;
    };
    let (Some(si), Some(sb)) = (stamp(installed), stamp(bundled)) else { return false };
    let key = (installed.to_path_buf(), si, bundled.to_path_buf(), sb);
    let mut cache = OUTDATED.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((k, v)) = cache.as_ref()
        && *k == key
    {
        return *v;
    }
    let differs = match (std::fs::read(installed), std::fs::read(bundled)) {
        (Ok(a), Ok(b)) => grab_version(&a) < grab_version(&b),
        _ => false,
    };
    *cache = Some((key, differs));
    differs
}

const VERSION_PREFIX: &[u8] = b"EYECAM_GRAB_VERSION=";

/// The oldest eyecam-grab version eyecam-rec still starts by itself. Raise it only when an installed version must
/// not be used any more (a security problem in eyecam-grab): older copies then count as TooOld, the camera waits,
/// and the panel asks for install_grab.sh. For ordinary changes raise only EYECAM_GRAB_VERSION (grab_outdated).
pub const MIN_SAFE_GRAB_VERSION: u32 = 1;

/// The version marker an eyecam-grab binary carries (`EYECAM_GRAB_VERSION=<n>;`, see src/bin/eyecam-grab.rs), or 1
/// for a binary built before the marker existed. Builds of the same source differ in their bytes (strip, build
/// paths), so the marker, not the bytes, tells whether the installed copy needs replacing.
pub fn grab_version(binary: &[u8]) -> u32 {
    binary
        .windows(VERSION_PREFIX.len())
        .position(|w| w == VERSION_PREFIX)
        .and_then(|i| {
            let rest = &binary[i + VERSION_PREFIX.len()..];
            let end = rest.iter().position(|&b| b == b';').filter(|&e| (1..=9).contains(&e))?;
            std::str::from_utf8(&rest[..end]).ok()?.parse().ok()
        })
        .unwrap_or(1)
}

/// The eyecam-grab shipped beside this executable (~/.local/lib/eyecam/eyecam-grab), if there is one.
pub fn bundled_grab() -> Option<PathBuf> {
    let p = std::env::current_exe().ok()?.parent()?.join("eyecam-grab");
    p.is_file().then_some(p)
}

/// Whether Valve's eye tracker is running as this user (its /proc entry is readable to us).
pub fn eyetracking_running() -> bool {
    let Ok(dir) = std::fs::read_dir("/proc") else { return false };
    dir.flatten().any(|e| {
        e.file_name().to_string_lossy().bytes().all(|b| b.is_ascii_digit())
            && std::fs::read_link(e.path().join("exe")).is_ok_and(|x| x == Path::new(EYETRACKING_EXE))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reads_the_version_marker() {
        assert_eq!(grab_version(b"ELF header..EYECAM_GRAB_VERSION=3;.."), 3);
        assert_eq!(grab_version(b"no marker here"), 1);
        assert_eq!(grab_version(b"EYECAM_GRAB_VERSION=x;"), 1);
        assert_eq!(grab_version(b"EYECAM_GRAB_VERSION=12"), 1);
    }

    /// FNV-1a of src/bin/eyecam-grab.rs without carriage returns. When this fails, eyecam-grab.rs changed: raise
    /// EYECAM_GRAB_VERSION there (so installed copies get replaced) and then update GRAB_SOURCE_FNV and
    /// GRAB_SOURCE_VERSION here.
    const GRAB_SOURCE_FNV: u64 = 0x67ec737b668c300f;
    const GRAB_SOURCE_VERSION: u32 = 1;

    #[test]
    fn eyecam_grab_changes_come_with_a_new_version() {
        let src = include_str!("bin/eyecam-grab.rs").replace('\r', "");
        let fnv = src.bytes().fold(0xcbf2_9ce4_8422_2325_u64, |h, b| (h ^ u64::from(b)).wrapping_mul(0x0100_0000_01b3));
        assert_eq!(grab_version(src.as_bytes()), GRAB_SOURCE_VERSION, "update GRAB_SOURCE_VERSION");
        assert_eq!(fnv, GRAB_SOURCE_FNV, "eyecam-grab.rs changed: raise EYECAM_GRAB_VERSION, then set GRAB_SOURCE_FNV to {fnv:#x}");
    }

    #[test]
    fn a_missing_bundled_copy_is_not_outdated() {
        assert!(!grab_outdated(Path::new("/nonexistent/eyecam-grab")));
    }

    fn caps(magic: u32, permitted: u32) -> Vec<u8> {
        let mut v = Vec::new();
        for w in [magic, permitted, 0, 0, 0] {
            v.extend_from_slice(&w.to_le_bytes());
        }
        v
    }

    #[test]
    fn reads_the_capability_xattr() {
        // `setcap cap_sys_ptrace=ep`: revision 2, effective flag, bit 19 permitted.
        assert!(has_ptrace_cap(&caps(0x0200_0001, 1 << 19)));
        assert!(has_ptrace_cap(&caps(0x0300_0001, 1 << 19 | 1 << 12)));
        assert!(!has_ptrace_cap(&caps(0x0200_0000, 1 << 19)), "permitted but not effective");
        assert!(!has_ptrace_cap(&caps(0x0200_0001, 1 << 12)), "another capability");
        assert!(!has_ptrace_cap(&caps(0x0900_0001, 1 << 19)), "unknown revision");
        assert!(!has_ptrace_cap(&[1, 2, 3]));
    }

    #[test]
    fn refuses_what_is_missing_unsafe_or_without_the_capability() {
        assert_eq!(check(Path::new("/nonexistent/eyecam-grab")), State::Missing);
        // A file the user owns (the test runs as the user) is never trusted.
        let mine = std::env::temp_dir().join(format!("eyecam-autograb-test-{}", std::process::id()));
        std::fs::write(&mine, b"x").unwrap();
        assert!(matches!(check(&mine), State::Unsafe(why) if why.contains("not owned by root")));
        std::fs::remove_file(&mine).unwrap();
        // A root-owned binary in a root-owned directory without capabilities.
        let ls = Path::new("/usr/bin/ls");
        if ls.exists() {
            assert_eq!(check(ls), State::NoCap(ls.to_path_buf()));
        }
        // A root-owned file in a directory others can write to (/tmp is 1777).
        let tmp_root = Path::new("/tmp/.X11-unix");
        if std::fs::symlink_metadata(tmp_root).is_ok_and(|m| m.uid() == 0) {
            let s = check(tmp_root);
            assert!(matches!(s, State::Unsafe(_)), "{s:?}");
        }
    }
}
