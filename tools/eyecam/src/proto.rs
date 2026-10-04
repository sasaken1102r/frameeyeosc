//! The message eyecam-grab sends with the descriptors (keep in sync with src/bin/eyecam-grab.rs).

pub const SOCKET_PATH: &str = "/run/user/1000/eyecam.sock";
pub const USER_UID: u32 = 1000;
pub const MAGIC: [u8; 8] = *b"EYECAM01";
pub const MAX_BUFFERS: usize = 4;
pub const HEADER_LEN: usize = 16 + 8 * MAX_BUFFERS;

/// What the header says: how many descriptors, the eye tracker's pid, and each buffer's size.
#[derive(Debug, PartialEq)]
pub struct Header {
    pub count: usize,
    pub pid: i32,
    pub sizes: Vec<u64>,
}

/// Parse a header, refusing anything malformed.
pub fn parse_header(bytes: &[u8]) -> Result<Header, String> {
    if bytes.len() != HEADER_LEN || bytes[..8] != MAGIC {
        return Err(format!("bad header ({} bytes)", bytes.len()));
    }
    let u32_at = |i: usize| u32::from_le_bytes(bytes[i..i + 4].try_into().unwrap());
    let count = u32_at(8) as usize;
    if count == 0 || count > MAX_BUFFERS {
        return Err(format!("bad buffer count {count}"));
    }
    let sizes = (0..count)
        .map(|i| u64::from_le_bytes(bytes[16 + 8 * i..24 + 8 * i].try_into().unwrap()))
        .collect();
    Ok(Header { count, pid: u32_at(12) as i32, sizes })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_what_grab_sends() {
        let mut bytes = MAGIC.to_vec();
        bytes.extend_from_slice(&2u32.to_le_bytes());
        bytes.extend_from_slice(&2626u32.to_le_bytes());
        for size in [16u64 << 20, 32 << 20, 0, 0] {
            bytes.extend_from_slice(&size.to_le_bytes());
        }
        assert_eq!(
            parse_header(&bytes),
            Ok(Header { count: 2, pid: 2626, sizes: vec![16 << 20, 32 << 20] })
        );
        bytes[8] = 9;
        assert!(parse_header(&bytes).is_err());
        assert!(parse_header(&bytes[..20]).is_err());
    }
}
