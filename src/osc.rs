//! The OSC 1.0 messages this sender sends: an address and float, int or bool arguments, or one blob (VRChat's Face
//! Tracking input), each on its own or (an extra output's `bundle`) in bundles. Encoded here
//! rather than with the rosc crate, which is only a dev-dependency now: the tests check every byte against it.
//! (From PR #33 by cinnacappu.)

/// One OSC argument.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Value {
    Float(f32),
    Int(i32),
    Bool(bool),
}

impl Value {
    /// Its type tag: f, i, or T / F (a bool has no data, only the tag).
    fn tag(&self) -> u8 {
        match self {
            Value::Float(_) => b'f',
            Value::Int(_) => b'i',
            Value::Bool(true) => b'T',
            Value::Bool(false) => b'F',
        }
    }
}

/// One OSC message into `packet` (cleared first, so a sender can keep reusing one buffer): the address and the type
/// tags as OSC strings (NUL-terminated, padded with NULs to a multiple of four bytes), then each float or int as
/// four big-endian bytes.
pub fn encode(address: &str, args: &[Value], packet: &mut Vec<u8>) {
    packet.clear();
    packet.extend_from_slice(address.as_bytes());
    pad(packet);
    packet.push(b',');
    packet.extend(args.iter().map(Value::tag));
    pad(packet);
    for arg in args {
        match *arg {
            Value::Float(value) => packet.extend_from_slice(&value.to_be_bytes()),
            Value::Int(value) => packet.extend_from_slice(&value.to_be_bytes()),
            Value::Bool(_) => {}
        }
    }
}

/// One OSC message with a single blob argument into `packet` (cleared first): the address and the `,b` type tag as OSC
/// strings, then the blob's size as a big-endian i32 and its bytes, padded with NULs to a multiple of four bytes (none
/// when it already is one). For VRChat's Face Tracking input (vrc_face).
pub fn encode_blob(address: &str, blob: &[u8], packet: &mut Vec<u8>) {
    packet.clear();
    packet.extend_from_slice(address.as_bytes());
    pad(packet);
    packet.extend_from_slice(b",b");
    pad(packet);
    packet.extend_from_slice(&(blob.len() as i32).to_be_bytes());
    packet.extend_from_slice(blob);
    packet.resize((packet.len() + 3) & !3, 0);
}

/// The largest bundle `bundles` makes: what fits one Ethernet frame's UDP payload (1500 - 20 IPv4 - 8 UDP bytes), so a
/// bundle is never split into IP fragments on the way.
pub const MAX_BUNDLE: usize = 1472;

/// Messages as OSC bundles of at most `max` bytes each, in order: as many messages to a bundle as fit, each bundle
/// "#bundle", the time tag 1 ("immediately") and each message as its size (a big-endian i32) and its bytes. A message
/// too big for a bundle of its own still gets one.
pub fn bundles(messages: &[(String, Vec<Value>)], max: usize) -> Vec<Vec<u8>> {
    const HEAD: [u8; 16] = *b"#bundle\0\0\0\0\0\0\0\0\x01";
    let mut out: Vec<Vec<u8>> = Vec::new();
    let mut message = Vec::new();
    for (address, args) in messages {
        encode(address, args, &mut message);
        let fits = out.last().is_some_and(|bundle| bundle.len() + 4 + message.len() <= max);
        if !fits {
            out.push(HEAD.to_vec());
        }
        let bundle = out.last_mut().expect("just pushed");
        bundle.extend_from_slice(&(message.len() as i32).to_be_bytes());
        bundle.extend_from_slice(&message);
    }
    out
}

/// The NUL ending an OSC string and the NULs up to the next multiple of four bytes (one to four of them).
fn pad(packet: &mut Vec<u8>) {
    packet.resize((packet.len() + 4) & !3, 0);
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use rosc::{OscMessage, OscPacket, OscType, encoder};

    /// The rosc argument for one of ours.
    pub fn to_rosc(value: Value) -> OscType {
        match value {
            Value::Float(value) => OscType::Float(value),
            Value::Int(value) => OscType::Int(value),
            Value::Bool(value) => OscType::Bool(value),
        }
    }

    /// Ours for a rosc argument (one this sender never sends is a test failure).
    pub fn from_rosc(value: OscType) -> Value {
        match value {
            OscType::Float(value) => Value::Float(value),
            OscType::Int(value) => Value::Int(value),
            OscType::Bool(value) => Value::Bool(value),
            other => panic!("not an argument this sender sends: {other:?}"),
        }
    }

    /// The message as rosc encodes it (what was sent before PR #33).
    pub fn rosc_encode(address: &str, args: &[Value]) -> Vec<u8> {
        let message = OscMessage {
            addr: address.to_owned(),
            args: args.iter().copied().map(to_rosc).collect(),
        };
        encoder::encode(&OscPacket::Message(message)).unwrap()
    }

    #[test]
    fn bundles_decode_as_rosc_bundles_in_order_and_stay_under_the_limit() {
        let messages: Vec<(String, Vec<Value>)> = (0..40)
            .map(|n| (format!("/avatar/parameters/FT/v2/Name{}", "x".repeat(n % 7)), vec![Value::Float(n as f32)]))
            .collect();
        for max in [MAX_BUNDLE, 200, 1] {
            let bundles = bundles(&messages, max);
            let mut seen = Vec::new();
            for bundle in &bundles {
                // (a single message bigger than `max` still goes, alone)
                assert!(bundle.len() <= max || bundle.len() <= 16 + 4 + 64, "{max}: {}", bundle.len());
                let (rest, packet) = rosc::decoder::decode_udp(bundle).unwrap();
                assert!(rest.is_empty());
                let OscPacket::Bundle(bundle) = packet else { panic!("not a bundle") };
                assert_eq!((bundle.timetag.seconds, bundle.timetag.fractional), (0, 1));
                for content in bundle.content {
                    let OscPacket::Message(message) = content else { panic!("nested bundle") };
                    seen.push((message.addr, message.args.into_iter().map(from_rosc).collect::<Vec<_>>()));
                }
            }
            assert_eq!(seen, messages, "{max}");
            if max == MAX_BUNDLE {
                assert_eq!(bundles.len(), 2);
            }
            if max == 1 {
                assert_eq!(bundles.len(), messages.len());
            }
        }
        assert!(bundles(&[], MAX_BUNDLE).is_empty());
    }

    #[test]
    fn matches_rosc_for_every_argument_pattern_and_address_alignment() {
        let floats = [
            0.0,
            -0.0,
            0.75,
            -1.25,
            f32::MIN_POSITIVE,
            f32::MAX,
            f32::INFINITY,
            f32::NEG_INFINITY,
            f32::from_bits(0x7fc1_2345),
            f32::from_bits(0xffc5_4321),
        ];
        let mut cases = vec![
            vec![],
            vec![Value::Bool(false)],
            vec![Value::Bool(true)],
            vec![Value::Int(i32::MIN)],
            vec![Value::Int(-1)],
            vec![Value::Int(0)],
            vec![Value::Int(1)],
            vec![Value::Int(i32::MAX)],
            vec![Value::Int(i32::MIN), Value::Int(0), Value::Int(i32::MAX)],
            floats.map(Value::Float).into(),
            // native_eyes' LeftRightVec and CenterVec
            [0.1, -0.2, 0.97, -0.3, 0.4, 0.86].map(Value::Float).into(),
            [0.0, 0.0, 1.0].map(Value::Float).into(),
            vec![Value::Bool(true), Value::Float(-0.0), Value::Int(-1), Value::Bool(false)],
            vec![Value::Bool(true); 3],
            vec![Value::Bool(false); 4],
        ];
        cases.extend(floats.map(|value| vec![Value::Float(value)]));
        // The prefix setting changes the address's length; with these every length mod 4 is covered
        let prefixes = ["", "/", "/x", "/FT", "/FTx", "/日本語", "/with\0zero"];
        let mut alignments = [false; 4];
        let mut packet = Vec::new();
        for prefix in prefixes {
            for address in [
                format!("/avatar/parameters{prefix}/v2/EyeLeftX"),
                format!("/avatar/parameters{prefix}/EyeTrackingActive"),
                format!("/avatar/parameters{prefix}/v2/PupilDilation1"),
            ] {
                alignments[address.len() % 4] = true;
                for args in &cases {
                    encode(&address, args, &mut packet);
                    assert_eq!(packet, rosc_encode(&address, args), "{address:?} {args:?}");
                }
            }
        }
        assert_eq!(alignments, [true; 4]);
        // Reusing the buffer doesn't grow it
        let capacity = packet.capacity();
        for _ in 0..100 {
            encode("/x", &[Value::Float(0.5)], &mut packet);
        }
        assert_eq!(packet.capacity(), capacity);
    }

    #[test]
    fn blobs_match_rosc_for_every_size_and_address_alignment() {
        let mut packet = Vec::new();
        for address in ["/tracking/face/v1", "/x", "/xy", "/xyz", "/xyzw"] {
            for len in [0, 1, 2, 3, 4, 5, 101, 102, 103, 104] {
                let blob: Vec<u8> = (0..len).map(|i| (i * 7 + 1) as u8).collect();
                encode_blob(address, &blob, &mut packet);
                let message = OscMessage {
                    addr: address.to_owned(),
                    args: vec![OscType::Blob(blob.clone())],
                };
                assert_eq!(packet, encoder::encode(&OscPacket::Message(message)).unwrap(), "{address:?} {len}");
                assert_eq!(packet.len() % 4, 0);
            }
        }
        // VRChat's Face Tracking message: 20 + 4 + 4 + 102 + 2 bytes
        encode_blob("/tracking/face/v1", &[0; 102], &mut packet);
        assert_eq!(packet.len(), 132);
    }

    #[test]
    fn rosc_reads_it_back() {
        let args = [Value::Float(0.25), Value::Int(-7), Value::Bool(true), Value::Bool(false)];
        let mut packet = Vec::new();
        encode("/avatar/parameters/FT/v2/EyeLidLeft", &args, &mut packet);
        let (rest, decoded) = rosc::decoder::decode_udp(&packet).unwrap();
        assert!(rest.is_empty());
        let OscPacket::Message(message) = decoded else { panic!("not a message") };
        assert_eq!(message.addr, "/avatar/parameters/FT/v2/EyeLidLeft");
        assert_eq!(message.args.into_iter().map(from_rosc).collect::<Vec<_>>(), args);
    }
}
