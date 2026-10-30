//! The eye camera buffer from Valve's dsp_service, without eyecam-grab (`source: dsp`, a test route for now).
//!
//! As measured on the developer's Frame (2026-10-30): dsp_service (root) listens on the abstract unix socket
//! "dsp_service", and vrserver, eyetracking and XRService (all the user) are its clients. On connecting, the service
//! speaks first: a 4-byte payload "ADSP" with SCM_RIGHTS carrying two descriptors, both udmabuf DMA-BUFs of the sizes
//! eyecam-grab hands over. The first (16 MiB) holds the eye camera ring eyecam looks for (the control block at
//! 0x234000, eight slots of a 64-byte header and a 512 x 400 frame); the second (32 MiB) is not used here, so it is
//! closed right away and not watched. The client then answers with a u32 request type: 0 asks for no notifications
//! (other bits subscribe to messages this module would never read, so they are never sent). The buffer is mapped
//! read-only by eyecam-rec (its `Mapping`), exactly like eyecam-grab's, and nothing is ever written to it. The
//! connection is kept open for as long as the buffer is in use (the service may drop a client that closes it) and
//! closed with it; a closed connection counts as the buffers being lost.

use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use std::os::linux::net::SocketAddrExt;
use std::os::unix::net::{SocketAddr, UnixStream};
use std::time::Duration;

/// The abstract socket name (no leading or trailing NUL here; SocketAddr adds the leading one).
pub const SERVICE: &str = "dsp_service";
/// What the service sends first, with the descriptors.
pub const HELLO: [u8; 4] = *b"ADSP";
/// The request type sent back: no notifications.
pub const REQUEST_NONE: u32 = 0;
/// The buffer's size; anything else is refused.
pub const BUFFER_LEN: u64 = 16 << 20;
/// What /proc/self/fd says a DMA-BUF is.
pub const DMABUF_LINK: &str = "/dmabuf:";
/// Its exporter (fdinfo `exp_name`), as eyecam-rec also requires of eyecam-grab's buffers.
pub const EXPORTER: &str = "udmabuf";
/// At most this many descriptors are taken from the first message (more are refused: truncated).
const MAX_FDS: usize = 8;

/// Where eyecam-rec takes the camera buffers from (settings.json `source`, or --source).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Source {
    /// eyecam-grab (sudo, or the installed capability copy): the behaviour before `source` existed.
    #[default]
    Grab,
    /// dsp_service only.
    Dsp,
    /// The installed eyecam-grab when it is there and working, else dsp_service.
    Auto,
}

impl Source {
    pub fn parse(s: &str) -> Option<Self> {
        match s {
            "grab" => Some(Source::Grab),
            "dsp" => Some(Source::Dsp),
            "auto" => Some(Source::Auto),
            _ => None,
        }
    }

    /// The name in settings.json and status.json.
    pub fn label(self) -> &'static str {
        match self {
            Source::Grab => "grab",
            Source::Dsp => "dsp",
            Source::Auto => "auto",
        }
    }
}

/// How many automatic grabs in a row may fail before `auto` goes over to dsp_service.
pub const AUTO_GRAB_TRIES: u32 = 2;

/// Which route to take now for `setting` (Grab or Dsp, never Auto): `auto` takes the grab only when the installed copy
/// is ready to use (`grab_ready`, autograb::State::Ready); otherwise, and once AUTO_GRAB_TRIES automatic grabs in a
/// row failed (eyecam-rec then leaves the grab's wait by itself), dsp_service.
pub fn pick(setting: Source, grab_ready: bool) -> Source {
    match setting {
        Source::Auto if grab_ready => Source::Grab,
        Source::Auto => Source::Dsp,
        s => s,
    }
}

/// The first message's payload and descriptor count, as the service sends them.
pub fn check_hello(payload: &[u8], fds: usize) -> Result<(), String> {
    if payload != HELLO {
        return Err(format!("unexpected first message from {SERVICE} ({} bytes: {:02x?})", payload.len(), payload));
    }
    if fds == 0 {
        return Err(format!("{SERVICE} sent no descriptor"));
    }
    Ok(())
}

/// The process on the other end: root, and named dsp_service where its /proc entry can be read (`comm`, None when not).
pub fn check_peer(uid: u32, comm: Option<&str>) -> Result<(), String> {
    if uid != 0 {
        return Err(format!("{SERVICE} is not run by root (uid {uid}); not using it"));
    }
    match comm.map(str::trim_end) {
        Some(c) if c != SERVICE => Err(format!("the process behind {SERVICE} is {c:?}; not using it")),
        _ => Ok(()),
    }
}

/// The buffer descriptor: a udmabuf DMA-BUF (`link` from /proc/self/fd, `exp_name` from fdinfo) of BUFFER_LEN bytes.
pub fn check_buffer(link: &str, exp_name: &str, size: u64) -> Result<(), String> {
    if !link.starts_with(DMABUF_LINK) {
        return Err(format!("{SERVICE}'s descriptor is not a DMA-BUF ({link})"));
    }
    if exp_name != EXPORTER {
        return Err(format!("{SERVICE}'s buffer is not a {EXPORTER} (exp_name {exp_name:?})"));
    }
    if size != BUFFER_LEN {
        return Err(format!("{SERVICE}'s buffer is {size} bytes, expected {BUFFER_LEN}"));
    }
    Ok(())
}

/// What /proc/self/fd and fstat say about a descriptor: its link, size and fdinfo `exp_name` (for the log, and for
/// check_buffer).
#[derive(Clone, Debug, Default)]
pub struct FdInfo {
    pub link: String,
    pub size: u64,
    pub exp_name: String,
}

impl FdInfo {
    pub fn of(fd: RawFd) -> Self {
        let link = std::fs::read_link(format!("/proc/self/fd/{fd}")).map_or_else(|e| format!("? ({e})"), |p| p.display().to_string());
        let mut st: libc::stat = unsafe { std::mem::zeroed() };
        let size = if unsafe { libc::fstat(fd, &mut st) } == 0 { st.st_size as u64 } else { 0 };
        let exp_name = std::fs::read_to_string(format!("/proc/self/fdinfo/{fd}"))
            .ok()
            .and_then(|info| {
                info.lines().find_map(|l| l.split_once(':').filter(|(k, _)| k.trim() == "exp_name").map(|(_, v)| v.trim().to_string()))
            })
            .unwrap_or_default();
        Self { link, size, exp_name }
    }

    pub fn describe(&self) -> String {
        let exp = if self.exp_name.is_empty() { String::new() } else { format!(", exp_name {}", self.exp_name) };
        format!("{} ({} bytes{exp})", self.link, self.size)
    }
}

/// An open connection to dsp_service and the buffer it handed over.
pub struct Connection {
    stream: UnixStream,
    buffer: Option<OwnedFd>,
    pub peer_pid: i32,
    /// The buffer descriptor, then the others (closed right away), as FdInfo described them.
    pub fds: Vec<FdInfo>,
    /// Bytes the service sent after the first message (nothing is expected; they are read and dropped).
    extra: std::cell::Cell<u64>,
}

impl Connection {
    /// Take the buffer descriptor (once), to map it; the connection stays open.
    pub fn take_buffer(&mut self) -> Option<OwnedFd> {
        self.buffer.take()
    }

    /// Whether the service closed the connection (or it failed). Anything it sent meanwhile is read and dropped (with
    /// any descriptors attached), so it can never fill the socket. Never blocks.
    pub fn closed(&self) -> bool {
        let mut pfd = libc::pollfd { fd: self.stream.as_raw_fd(), events: libc::POLLIN | libc::POLLRDHUP, revents: 0 };
        if unsafe { libc::poll(&mut pfd, 1, 0) } <= 0 {
            return false;
        }
        if pfd.revents & (libc::POLLHUP | libc::POLLERR | libc::POLLRDHUP) != 0 {
            return true;
        }
        // POLLIN: data (or the end of the stream)
        loop {
            match recv_some(self.stream.as_raw_fd(), libc::MSG_DONTWAIT) {
                Ok((0, _)) => return true,
                Ok((n, fds)) => {
                    let before = self.extra.get();
                    self.extra.set(before + n as u64);
                    if before == 0 {
                        eprintln!("{SERVICE} sent {n} unexpected byte(s) (and {} descriptor(s)); dropped", fds.len());
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock || e.kind() == std::io::ErrorKind::Interrupted => return false,
                Err(_) => return true,
            }
        }
    }

    pub fn extra_bytes(&self) -> u64 {
        self.extra.get()
    }
}

/// One recvmsg: the bytes read and the descriptors attached (closed when dropped). Fails on a truncated message.
fn recv_some(fd: RawFd, flags: libc::c_int) -> std::io::Result<(usize, Vec<OwnedFd>)> {
    let mut data = [0u8; 256];
    let space = unsafe { libc::CMSG_SPACE((MAX_FDS * size_of::<RawFd>()) as u32) } as usize;
    let mut control = vec![0u8; space];
    let (n, fds, msg_flags) = recvmsg(fd, &mut data, &mut control, flags)?;
    if msg_flags & libc::MSG_CTRUNC != 0 {
        return Err(std::io::Error::other("control message truncated"));
    }
    Ok((n, fds))
}

fn recvmsg(fd: RawFd, data: &mut [u8], control: &mut [u8], flags: libc::c_int) -> std::io::Result<(usize, Vec<OwnedFd>, libc::c_int)> {
    let mut iov = libc::iovec { iov_base: data.as_mut_ptr().cast(), iov_len: data.len() };
    let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
    msg.msg_iov = &mut iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.as_mut_ptr().cast();
    msg.msg_controllen = control.len() as _;
    let n = unsafe { libc::recvmsg(fd, &mut msg, flags | libc::MSG_CMSG_CLOEXEC) };
    if n < 0 {
        return Err(std::io::Error::last_os_error());
    }
    let mut fds = Vec::new();
    unsafe {
        let mut cmsg = libc::CMSG_FIRSTHDR(&msg);
        while !cmsg.is_null() {
            if (*cmsg).cmsg_level == libc::SOL_SOCKET && (*cmsg).cmsg_type == libc::SCM_RIGHTS {
                let count = ((*cmsg).cmsg_len as usize - libc::CMSG_LEN(0) as usize) / size_of::<RawFd>();
                let p = libc::CMSG_DATA(cmsg).cast::<RawFd>();
                for i in 0..count {
                    fds.push(OwnedFd::from_raw_fd(p.add(i).read_unaligned()));
                }
            }
            cmsg = libc::CMSG_NXTHDR(&msg, cmsg);
        }
    }
    Ok((n as usize, fds, msg.msg_flags))
}

fn peer_cred(stream: &UnixStream) -> Result<libc::ucred, String> {
    let mut cred = libc::ucred { pid: 0, uid: u32::MAX, gid: u32::MAX };
    let mut len = size_of::<libc::ucred>() as libc::socklen_t;
    let ret = unsafe {
        libc::getsockopt(stream.as_raw_fd(), libc::SOL_SOCKET, libc::SO_PEERCRED, (&raw mut cred).cast(), &mut len)
    };
    if ret != 0 {
        return Err(format!("SO_PEERCRED: {}", std::io::Error::last_os_error()));
    }
    Ok(cred)
}

/// Connect, check who is on the other end and what it sent, answer REQUEST_NONE, and keep the buffer descriptor.
pub fn connect() -> Result<Connection, String> {
    let addr = SocketAddr::from_abstract_name(SERVICE.as_bytes()).map_err(|e| format!("{SERVICE}: {e}"))?;
    let stream = UnixStream::connect_addr(&addr).map_err(|e| format!("connect {SERVICE}: {e}"))?;
    let cred = peer_cred(&stream)?;
    let comm = std::fs::read_to_string(format!("/proc/{}/comm", cred.pid)).ok();
    check_peer(cred.uid, comm.as_deref())?;
    stream.set_read_timeout(Some(Duration::from_secs(5))).map_err(|e| e.to_string())?;
    stream.set_write_timeout(Some(Duration::from_secs(2))).map_err(|e| e.to_string())?;

    let mut data = [0u8; 64];
    let space = unsafe { libc::CMSG_SPACE((MAX_FDS * size_of::<RawFd>()) as u32) } as usize;
    let mut control = vec![0u8; space];
    let (n, mut fds, flags) =
        recvmsg(stream.as_raw_fd(), &mut data, &mut control, 0).map_err(|e| format!("{SERVICE}: recvmsg: {e}"))?;
    if flags & (libc::MSG_CTRUNC | libc::MSG_TRUNC) != 0 {
        return Err(format!("{SERVICE}'s first message was truncated"));
    }
    check_hello(&data[..n], fds.len())?;
    let infos: Vec<FdInfo> = fds.iter().map(|fd| FdInfo::of(fd.as_raw_fd())).collect();
    check_buffer(&infos[0].link, &infos[0].exp_name, infos[0].size)?;
    // Only the first is used; the rest are closed here.
    fds.truncate(1);
    let buffer = fds.pop();

    let request = REQUEST_NONE.to_le_bytes();
    let sent = unsafe { libc::send(stream.as_raw_fd(), request.as_ptr().cast(), request.len(), libc::MSG_NOSIGNAL) };
    if sent != request.len() as isize {
        return Err(format!("{SERVICE}: send: {}", std::io::Error::last_os_error()));
    }
    stream.set_nonblocking(true).map_err(|e| e.to_string())?;
    Ok(Connection { stream, buffer, peer_pid: cred.pid, fds: infos, extra: std::cell::Cell::new(0) })
}

/// The wait before the next connection attempt after `failures` failed ones in a row: 1, 2, 4, 8, 16, then 30 s.
pub fn backoff(failures: u32) -> f64 {
    (1u64 << failures.saturating_sub(1).min(5)).min(30) as f64
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checks_the_first_message() {
        assert!(check_hello(b"ADSP", 2).is_ok());
        assert!(check_hello(b"ADSP", 1).is_ok());
        assert!(check_hello(b"ADSP", 0).unwrap_err().contains("no descriptor"));
        assert!(check_hello(b"ADS", 2).is_err());
        assert!(check_hello(b"ADSPX", 2).is_err());
        assert!(check_hello(b"EYEC", 2).is_err());
        assert!(check_hello(b"", 2).is_err());
    }

    #[test]
    fn checks_the_peer() {
        assert!(check_peer(0, Some("dsp_service\n")).is_ok());
        assert!(check_peer(0, None).is_ok(), "comm unreadable: the uid decides");
        assert!(check_peer(1000, Some("dsp_service\n")).unwrap_err().contains("not run by root"));
        assert!(check_peer(0, Some("impostor\n")).unwrap_err().contains("impostor"));
        assert!(check_peer(0, Some("dsp_service_x\n")).is_err());
    }

    #[test]
    fn checks_the_buffer() {
        assert!(check_buffer("/dmabuf:", "udmabuf", 16 << 20).is_ok());
        assert!(check_buffer("/dmabuf:dsp", "udmabuf", 16 << 20).is_ok());
        // The 32 MiB buffer that comes second is not the one.
        assert!(check_buffer("/dmabuf:", "udmabuf", 32 << 20).unwrap_err().contains("expected"));
        assert!(check_buffer("/dmabuf:", "system", 16 << 20).unwrap_err().contains("not a udmabuf"));
        assert!(check_buffer("/dmabuf:", "", 16 << 20).is_err(), "fdinfo unreadable");
        assert!(check_buffer("/memfd:x (deleted)", "udmabuf", 16 << 20).unwrap_err().contains("not a DMA-BUF"));
        assert!(check_buffer("/dev/shm/x", "", 16 << 20).is_err());
    }

    #[test]
    fn parses_and_names_the_source() {
        for s in [Source::Grab, Source::Dsp, Source::Auto] {
            assert_eq!(Source::parse(s.label()), Some(s));
        }
        assert_eq!(Source::parse("DSP"), None);
        assert_eq!(Source::parse(""), None);
        assert_eq!(Source::default(), Source::Grab);
    }

    #[test]
    fn picks_a_route() {
        assert_eq!(pick(Source::Grab, false), Source::Grab, "grab waits for the grab, as before");
        assert_eq!(pick(Source::Grab, true), Source::Grab);
        assert_eq!(pick(Source::Dsp, true), Source::Dsp, "dsp never uses the grab");
        assert_eq!(pick(Source::Dsp, false), Source::Dsp);
        assert_eq!(pick(Source::Auto, true), Source::Grab);
        assert_eq!(pick(Source::Auto, false), Source::Dsp);
    }

    #[test]
    fn backs_off() {
        let waits: Vec<f64> = (1..=8).map(backoff).collect();
        assert_eq!(waits, [1.0, 2.0, 4.0, 8.0, 16.0, 30.0, 30.0, 30.0]);
        assert_eq!(backoff(0), 1.0);
    }

    /// A socket pair stands in for the service: the closed check drops what is sent and sees the end.
    #[test]
    fn notices_a_closed_connection() {
        let (a, b) = UnixStream::pair().unwrap();
        a.set_nonblocking(true).unwrap();
        let conn = Connection { stream: a, buffer: None, peer_pid: 0, fds: Vec::new(), extra: std::cell::Cell::new(0) };
        assert!(!conn.closed());
        use std::io::Write;
        (&b).write_all(b"note").unwrap();
        assert!(!conn.closed(), "data is not the end");
        assert_eq!(conn.extra_bytes(), 4);
        drop(b);
        assert!(conn.closed());
    }

    /// On the headset: `cargo test --release -- --ignored live_connect --nocapture`. Connects, reads, closes.
    #[test]
    #[ignore]
    fn live_connect() {
        let mut c = connect().unwrap();
        eprintln!("pid {}: {:?}", c.peer_pid, c.fds);
        assert!(c.take_buffer().is_some());
        assert!(!c.closed());
    }
}
