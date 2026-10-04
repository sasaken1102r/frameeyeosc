//! eyecam-grab: the only privileged part of eyecam. One shot, then it exits. It runs either as root (sudo) or,
//! installed by install_grab.sh, as the user with the single file capability cap_sys_ptrace (eyecam-rec --serve
//! then starts it by itself). Nothing in it depends on which: it never looks at its own uid, reads no environment
//! variables, and a capability binary runs in secure-exec mode anyway (LD_PRELOAD and the like are ignored).
//!
//! What it does, in order:
//!   1. Checks that eyecam-rec is waiting: /run/user/1000/eyecam.sock must be a socket owned by uid 1000.
//!   2. Finds Valve's eye tracker: the one process whose /proc/<pid>/exe is exactly EYETRACKING_EXE and whose
//!      uids are all 1000. Anything else (none, several, another owner) is refused.
//!   3. Lists that process's open files and keeps only DMA-BUFs whose fdinfo says `exp_name: udmabuf` (the
//!      shared buffers XRService hands it, where the eye camera frames pass through).
//!   4. Copies just those descriptors into this process with pidfd_open + pidfd_getfd. This is the only step that
//!      needs privilege: root or CAP_SYS_PTRACE (kernel.yama.ptrace_scope=1 blocks pidfd_getfd for other processes
//!      otherwise). It does not stop, attach to, trace or signal the eye tracker, and does not read or write its
//!      memory.
//!   5. Connects to the socket, checks the process on the other end runs as uid 1000, and passes the descriptors
//!      to it (SCM_RIGHTS) with a small header: count, sizes, eye tracker pid. Then exits.
//!
//! It never writes a file, never maps or reads the buffers itself, and sends nothing but those descriptors.
//! eyecam-rec maps them read-only.

use std::fs;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::fs::{FileTypeExt, MetadataExt};
use std::os::unix::net::UnixStream;
use std::path::Path;
use std::process::ExitCode;

const EYETRACKING_EXE: &str = "/opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking";
const USER_UID: u32 = 1000;
const SOCKET_PATH: &str = "/run/user/1000/eyecam.sock";
// Keep in sync with eyecam::proto (src/proto.rs).
const MAGIC: [u8; 8] = *b"EYECAM01";
const MAX_BUFFERS: usize = 4;
/// This program's version for update checks. eyecam-rec looks for this marker in the installed copy and in the one
/// shipped beside it, without running either (eyecam::autograb::grab_version), and asks for install_grab.sh again
/// only when the shipped one is newer. Raise the number whenever this file changes. A copy built before the marker
/// existed counts as version 1.
static VERSION_MARKER: &[u8] = b"EYECAM_GRAB_VERSION=1;";

/// A DMA-BUF the eye tracker holds: its descriptor number there, and what fdinfo says about it.
struct Dmabuf {
    fd: RawFd,
    size: u64,
    ino: u64,
}

fn main() -> ExitCode {
    // Keep the version marker in the binary (also after strip); it is only read from the file, never printed.
    std::hint::black_box(VERSION_MARKER);
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("eyecam-grab: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run() -> Result<(), String> {
    // 1. Only go on if the recorder's socket is there and belongs to the user (lstat: a symlink is refused).
    let meta = fs::symlink_metadata(SOCKET_PATH)
        .map_err(|e| format!("{SOCKET_PATH}: {e} (start eyecam-rec first)"))?;
    if !meta.file_type().is_socket() || meta.uid() != USER_UID {
        return Err(format!("{SOCKET_PATH} is not a socket owned by uid {USER_UID}"));
    }

    // 2. The eye tracker. Its identity is checked again once a pidfd pins it, so a pid reused in between is caught.
    let pid = find_eyetracking()?;
    let pidfd = syscall_fd(unsafe { libc::syscall(libc::SYS_pidfd_open, pid, 0) }, "pidfd_open")?;
    if !is_eyetracking(pid) {
        return Err(format!("process {pid} changed while being opened"));
    }

    // 3. Its udmabufs, one entry per buffer (the same buffer is often open under several fd numbers).
    let mut buffers: Vec<Dmabuf> = Vec::new();
    let fd_dir = format!("/proc/{pid}/fd");
    for entry in fs::read_dir(&fd_dir).map_err(|e| format!("{fd_dir}: {e}"))? {
        let Ok(entry) = entry else { continue };
        let Ok(fd) = entry.file_name().to_string_lossy().parse::<RawFd>() else { continue };
        let Ok(target) = fs::read_link(entry.path()) else { continue };
        if !target.to_string_lossy().contains("dmabuf") {
            continue;
        }
        let Some((size, ino)) = udmabuf_info(&format!("/proc/{pid}/fdinfo/{fd}")) else { continue };
        if !buffers.iter().any(|b| b.ino == ino) {
            buffers.push(Dmabuf { fd, size, ino });
        }
    }
    if buffers.is_empty() || buffers.len() > MAX_BUFFERS {
        return Err(format!("expected 1 to {MAX_BUFFERS} udmabufs in process {pid}, found {}", buffers.len()));
    }
    for b in &buffers {
        eprintln!("eyecam-grab: process {pid} fd {}: udmabuf, {} bytes ({} MiB)", b.fd, b.size, b.size >> 20);
    }

    // 4. Copy those descriptors (and nothing else) into this process. Each copy is checked to be the same buffer,
    // in case the fd number was closed and reused since it was listed.
    let mut copies: Vec<OwnedFd> = Vec::new();
    for b in &buffers {
        let ret = unsafe { libc::syscall(libc::SYS_pidfd_getfd, pidfd.as_raw_fd(), b.fd, 0) };
        let fd = syscall_fd(ret, "pidfd_getfd").map_err(|e| {
            if e.contains("EPERM") || e.contains("Operation not permitted") {
                format!("{e}: this step needs root or CAP_SYS_PTRACE (run it with sudo, or install it with install_grab.sh)")
            } else {
                e
            }
        })?;
        let own = format!("/proc/self/fdinfo/{}", fd.as_raw_fd());
        if udmabuf_info(&own) != Some((b.size, b.ino)) {
            return Err(format!("fd {} of process {pid} changed while being copied", b.fd));
        }
        copies.push(fd);
    }

    // 5. Hand them to the recorder, but only if it is the user's process.
    let stream = UnixStream::connect(SOCKET_PATH).map_err(|e| format!("connect {SOCKET_PATH}: {e}"))?;
    let mut cred = libc::ucred { pid: 0, uid: u32::MAX, gid: u32::MAX };
    let mut len = size_of::<libc::ucred>() as libc::socklen_t;
    let ret = unsafe {
        libc::getsockopt(
            stream.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            (&raw mut cred).cast(),
            &mut len,
        )
    };
    if ret != 0 || cred.uid != USER_UID {
        return Err(format!("the process behind {SOCKET_PATH} is not uid {USER_UID} (uid {})", cred.uid));
    }

    // Header: magic, count, eye tracker pid, then a size per buffer (u64, unused entries 0). Little-endian.
    let mut header = Vec::with_capacity(16 + 8 * MAX_BUFFERS);
    header.extend_from_slice(&MAGIC);
    header.extend_from_slice(&(copies.len() as u32).to_le_bytes());
    header.extend_from_slice(&(pid as u32).to_le_bytes());
    for i in 0..MAX_BUFFERS {
        header.extend_from_slice(&buffers.get(i).map_or(0, |b| b.size).to_le_bytes());
    }
    let fds: Vec<RawFd> = copies.iter().map(|fd| fd.as_raw_fd()).collect();
    send_fds(&stream, &header, &fds)?;
    eprintln!("eyecam-grab: passed {} buffer(s) to eyecam-rec (pid {}); done", fds.len(), cred.pid);
    Ok(())
}

/// The pid of the one process that is Valve's eye tracker running as the user.
fn find_eyetracking() -> Result<libc::pid_t, String> {
    let mut found = Vec::new();
    for entry in fs::read_dir("/proc").map_err(|e| format!("/proc: {e}"))?.flatten() {
        if let Ok(pid) = entry.file_name().to_string_lossy().parse::<libc::pid_t>()
            && is_eyetracking(pid)
        {
            found.push(pid);
        }
    }
    match found[..] {
        [pid] => Ok(pid),
        [] => Err(format!("no {EYETRACKING_EXE} running as uid {USER_UID} (is eye tracking on?)")),
        _ => Err(format!("several eye tracker processes: {found:?}")),
    }
}

/// Whether `pid` runs exactly EYETRACKING_EXE with real, effective, saved and filesystem uid all USER_UID.
fn is_eyetracking(pid: libc::pid_t) -> bool {
    let exe_ok = fs::read_link(format!("/proc/{pid}/exe")).is_ok_and(|exe| exe == Path::new(EYETRACKING_EXE));
    exe_ok
        && fs::read_to_string(format!("/proc/{pid}/status")).is_ok_and(|status| {
            status.lines().any(|line| {
                line.strip_prefix("Uid:").is_some_and(|uids| {
                    let uids: Vec<&str> = uids.split_whitespace().collect();
                    uids.len() == 4 && uids.iter().all(|uid| *uid == USER_UID.to_string())
                })
            })
        })
}

/// (size, inode) from an fdinfo file, if it describes a udmabuf.
fn udmabuf_info(path: &str) -> Option<(u64, u64)> {
    let text = fs::read_to_string(path).ok()?;
    let field = |key: &str| {
        text.lines()
            .find_map(|line| line.split_once(':').filter(|(k, _)| k.trim() == key).map(|(_, v)| v.trim()))
    };
    if field("exp_name")? != "udmabuf" {
        return None;
    }
    Some((field("size")?.parse().ok()?, field("ino")?.parse().ok()?))
}

/// Wrap a syscall's result as an owned descriptor, or describe the error.
fn syscall_fd(ret: libc::c_long, what: &str) -> Result<OwnedFd, String> {
    if ret < 0 {
        return Err(format!("{what}: {}", std::io::Error::last_os_error()));
    }
    Ok(unsafe { OwnedFd::from_raw_fd(ret as RawFd) })
}

/// Send `data` with `fds` attached as SCM_RIGHTS in one message.
fn send_fds(stream: &UnixStream, data: &[u8], fds: &[RawFd]) -> Result<(), String> {
    let fd_bytes = size_of_val(fds) as u32;
    let mut control = vec![0u8; unsafe { libc::CMSG_SPACE(fd_bytes) } as usize];
    let mut iov = libc::iovec { iov_base: data.as_ptr() as *mut libc::c_void, iov_len: data.len() };
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.as_mut_ptr().cast();
    msg.msg_controllen = control.len() as _;
    unsafe {
        let cmsg = libc::CMSG_FIRSTHDR(&msg);
        (*cmsg).cmsg_level = libc::SOL_SOCKET;
        (*cmsg).cmsg_type = libc::SCM_RIGHTS;
        (*cmsg).cmsg_len = libc::CMSG_LEN(fd_bytes) as _;
        std::ptr::copy_nonoverlapping(fds.as_ptr(), libc::CMSG_DATA(cmsg).cast::<RawFd>(), fds.len());
    }
    let sent = unsafe { libc::sendmsg(stream.as_raw_fd(), &msg, libc::MSG_NOSIGNAL) };
    if sent != data.len() as isize {
        return Err(format!("sendmsg: {}", std::io::Error::last_os_error()));
    }
    Ok(())
}
