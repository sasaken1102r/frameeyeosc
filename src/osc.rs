//! The OSC messages this sender uses: padded addresses and float, int or bool arguments.
//! A buffer is kept by each output, so sending a sample needs no message allocations.

#[cfg(test)]
pub use rosc::OscType as Value;

#[cfg(not(test))]
#[derive(Clone, Copy)]
pub enum Value {
    Float(f32),
    Int(i32),
    Bool(bool),
}

pub type Sink<'a> = dyn FnMut(&[&str], &[Value]) + 'a;

pub fn encode(address: &[&str], args: &[Value], packet: &mut Vec<u8>) {
    packet.clear();
    for part in address {
        packet.extend_from_slice(part.as_bytes());
    }
    pad(packet);
    packet.push(b',');
    for arg in args {
        packet.push(match arg {
            Value::Float(_) => b'f',
            Value::Int(_) => b'i',
            Value::Bool(true) => b'T',
            Value::Bool(false) => b'F',
            #[cfg(test)]
            _ => panic!("unsupported output argument"),
        });
    }
    pad(packet);
    for arg in args {
        match arg {
            Value::Float(value) => packet.extend_from_slice(&value.to_be_bytes()),
            Value::Int(value) => packet.extend_from_slice(&value.to_be_bytes()),
            Value::Bool(_) => {},
            #[cfg(test)]
            _ => unreachable!(),
        }
    }
}

// OSC strings include a terminating zero, padded to a multiple of four bytes.
fn pad(packet: &mut Vec<u8>) {
    packet.resize((packet.len() + 4) & !3, 0);
}

#[cfg(test)]
mod tests {
    use super::*;
    use rosc::{OscMessage, OscPacket, encoder};

    #[test]
    fn matches_rosc_for_every_output_type_and_address_alignment() {
        let mut packet = Vec::new();
        let floats = [0.0, -0.0, 0.75, -1.25, f32::INFINITY, f32::NEG_INFINITY, f32::from_bits(0x7fc12345)];
        let cases = [
            vec![],
            vec![Value::Bool(false)],
            vec![Value::Bool(true)],
            vec![Value::Int(i32::MIN), Value::Int(0), Value::Int(i32::MAX)],
            floats.map(Value::Float).into(),
            vec![Value::Bool(true), Value::Float(-0.0), Value::Int(-1), Value::Bool(false)],
        ];
        for prefix in ["", "/FT", "/x", "/xx", "/日本語", "/with\0zero"] {
            let address = ["/avatar/parameters", prefix, "/v2/EyeLeftX"];
            for args in &cases {
                encode(&address, args, &mut packet);
                let expected = encoder::encode(&OscPacket::Message(OscMessage {
                    addr: address.concat(), args: args.clone(),
                })).unwrap();
                assert_eq!(packet, expected, "{prefix:?}");
            }
        }
        let capacity = packet.capacity();
        for _ in 0..100 {
            encode(&["/x"], &[Value::Float(0.5)], &mut packet);
        }
        assert_eq!(packet.capacity(), capacity);
    }
}
