//! Live Link Face packets, for VRCFaceTracking's LiveLink module (`--output livelink`).
//!
//! The module ("VRCFT-LiveLink", github.com/VRCFaceTracking/LiveLinkTrackingModule at commit
//! 5c2de745a3d39db18046f642b86dc90381bd5f4e, VRCFT-LiveLink/LiveLinkExtTrackingInterface.cs and
//! VRCFT-LiveLink/Assets/TrackingStructs.cs) listens on UDP 11111 on every interface. It takes any datagram of at
//! least 244 bytes, reads its last 244 bytes as 61 big-endian floats in ARKit order (`Constants.LiveLinkNames`) and
//! ignores the header in front. The header is still the one Epic's Live Link Face app sends (packet version 6), so
//! other Live Link receivers read the packets too: Unreal's AppleARKitFaceSupport/Private/AppleARKitLiveLinkSource.cpp
//! (read in a public mirror, github.com/OpenHUTB/engine at 9adc75b4; FNboSerializeToBuffer, network byte order: u8
//! version 6, the device id and the subject name as an i32 length and UTF-8 bytes, the FQualifiedFrameTime as i32
//! frame, f32 subframe, i32 rate numerator and denominator, u8 blendshape count 61, 61 f32), cross-checked with
//! PyLiveLinkFace (github.com/JimWest/PyLiveLinkFace at 18c0e8bc, pylivelinkface/pylivelinkface.py), whose encoder
//! writes the same bytes.
//!
//! How VRCFT reads the module's Gaze (github.com/benaclejames/VRCFaceTracking at 6432e6a8,
//! VRCFaceTracking.Core/Params/Expressions/UnifiedExpressionsParameters.cs, NativeParameters.cs, Types/Vector2.cs)
//! is explained at `shapes`.
//!
//! What the module does with the eye values (UpdateEyeData, UpdateEyeExpressions):
//! - `Openness = 1 - clamp(EyeBlink + EyeBlink * EyeSquint, 0, 1)` per eye; `EyeWide` becomes EyeWideLeft/Right.
//! - `Gaze.x = EyeYaw`, `Gaze.y = -EyePitch` per eye, as they come (its radian conversion is commented out).
//!   The EyeLook* shapes are read but not used.
//! - Brows (UpdateEyeExpressions, so an eye-only module sets them too): `BrowInnerUp` becomes BrowInnerUpLeft and
//!   Right, `BrowOuterUpLeft/Right` BrowOuterUpLeft/Right, and `BrowDownLeft/Right` BrowLowerer and BrowPinch of that
//!   side.
//! - When nothing arrives for a second it logs "LiveLink connection lost" and VRCFT keeps the last values. It only
//!   starts if a first packet arrives within 180 s of VRCFT loading it.

use std::time::{SystemTime, UNIX_EPOCH};

pub const DEFAULT_PORT: u16 = 11111;
const PACKET_VERSION: u8 = 6;
const DEVICE_ID: &str = "frameeyeosc";
const SUBJECT_NAME: &str = "SteamFrame";
// The frame time is only a time stamp here (nobody reads it); the eye server's nominal rate.
const FRAME_RATE: i32 = 90;
pub const BLENDSHAPES: usize = 61;
pub const PACKET_LEN: usize = 1 + 4 + DEVICE_ID.len() + 4 + SUBJECT_NAME.len() + 16 + 1 + BLENDSHAPES * 4;
const FRAME_OFFSET: usize = 1 + 4 + DEVICE_ID.len() + 4 + SUBJECT_NAME.len();
const SHAPES_OFFSET: usize = FRAME_OFFSET + 17;

// Everything before the blendshapes is fixed except the frame and subframe.
const HEADER: [u8; SHAPES_OFFSET] = {
    const fn append(header: &mut [u8; SHAPES_OFFSET], at: &mut usize, bytes: &[u8]) {
        let mut i = 0;
        while i < bytes.len() {
            header[*at] = bytes[i];
            *at += 1;
            i += 1;
        }
    }
    let mut header = [0; SHAPES_OFFSET];
    let mut at = 0;
    append(&mut header, &mut at, &[PACKET_VERSION]);
    append(&mut header, &mut at, &(DEVICE_ID.len() as i32).to_be_bytes());
    append(&mut header, &mut at, DEVICE_ID.as_bytes());
    append(&mut header, &mut at, &(SUBJECT_NAME.len() as i32).to_be_bytes());
    append(&mut header, &mut at, SUBJECT_NAME.as_bytes());
    at += 8; // Frame and subframe are filled for each packet.
    append(&mut header, &mut at, &FRAME_RATE.to_be_bytes());
    append(&mut header, &mut at, &1i32.to_be_bytes());
    append(&mut header, &mut at, &[BLENDSHAPES as u8]);
    header
};
// Indices in ARKit / Live Link order (the module's Constants.LiveLinkNames).
const EYE_BLINK_LEFT: usize = 0;
const EYE_SQUINT_LEFT: usize = 5;
const EYE_WIDE_LEFT: usize = 6;
const EYE_BLINK_RIGHT: usize = 7;
const EYE_SQUINT_RIGHT: usize = 12;
const EYE_WIDE_RIGHT: usize = 13;
const BROW_DOWN_LEFT: usize = 41;
const BROW_DOWN_RIGHT: usize = 42;
const BROW_INNER_UP: usize = 43;
const BROW_OUTER_UP_LEFT: usize = 44;
const BROW_OUTER_UP_RIGHT: usize = 45;
const EYE_YAW_LEFT: usize = 55;
const EYE_PITCH_LEFT: usize = 56;
const EYE_YAW_RIGHT: usize = 58;
const EYE_PITCH_RIGHT: usize = 59;
// frameeyeosc's VRCFT eyelid of a relaxed open eye.
const RELAXED: f32 = 0.75;

/// How far a VRCFT eyelid (0 closed, 0.75 relaxed open, 1 widened) is closed: 1 shut, 0 from relaxed open up.
/// With no squint the module's openness is `1 - blink`, and VRCFT's EyeLid (`openness * 0.75 + wide * 0.25`) comes
/// back to the eyelid sent.
pub fn blink(lid: f32) -> f32 {
    (1.0 - lid / RELAXED).clamp(0.0, 1.0)
}

/// How far a VRCFT eyelid is widened: 0 up to relaxed open, 1 fully widened.
pub fn wide(lid: f32) -> f32 {
    ((lid - RELAXED) / (1.0 - RELAXED)).clamp(0.0, 1.0)
}

/// The 61 blendshapes for VRCFT eyelids and gaze (left x/y, right x/y in -1..1, 1.0 = 45°, +x right, +y up).
/// Everything else (squint, the EyeLook* shapes, mouth, head) stays 0, and so do the brows unless `with_brows` raises
/// them.
///
/// The gaze goes in as is, with pitch flipped: the module sets `Gaze = (EyeYaw, -EyePitch)` without converting
/// units, and VRCFT sends `Gaze.x/y` straight on as v2/EyeLeftX/Y (EParam) and turns them into VRChat's own eye
/// angles as `atan(x)` yaw and `-atan(y)` pitch (Vector2.ToYaw/ToPitch, positive pitch looking down), so Gaze is
/// +x right, +y up with 1 = 45°, like frameeyeosc's values. EyeYaw = x and EyePitch = -y therefore give VRCFT the
/// Gaze the ETVR module gets from our EyeLeftX/Y, and the avatar the v2/EyeLeftX/Y that `--output vrchat` sends.
/// (Pitch positive looking down is also ARKit's own convention.)
pub fn shapes([lid_left, lid_right]: [f32; 2], [left_x, left_y, right_x, right_y]: [f32; 4]) -> [f32; BLENDSHAPES] {
    let mut shapes = [0.0; BLENDSHAPES];
    shapes[EYE_BLINK_LEFT] = blink(lid_left);
    shapes[EYE_WIDE_LEFT] = wide(lid_left);
    shapes[EYE_BLINK_RIGHT] = blink(lid_right);
    shapes[EYE_WIDE_RIGHT] = wide(lid_right);
    // No squint: the module would close the eye further with it (and gives the right eye the left one's squint).
    shapes[EYE_SQUINT_LEFT] = 0.0;
    shapes[EYE_SQUINT_RIGHT] = 0.0;
    shapes[EYE_YAW_LEFT] = left_x;
    shapes[EYE_PITCH_LEFT] = -left_y;
    shapes[EYE_YAW_RIGHT] = right_x;
    shapes[EYE_PITCH_RIGHT] = -right_y;
    shapes
}

/// `shapes` with the brows raised (brow_from_widen; each 0..1, left and right): BrowOuterUpLeft/Right are each eye's,
/// BrowInnerUp (one shape, which the module gives both inner brows) their mean. BrowDownLeft/Right stay 0: brows are
/// only ever raised. A value that is not finite goes out as 0 (it can't be NaN from BrowRaise).
pub fn with_brows(mut shapes: [f32; BLENDSHAPES], brows: [f32; 2]) -> [f32; BLENDSHAPES] {
    let [left, right] = brows.map(|brow| if brow.is_finite() { brow.clamp(0.0, 1.0) } else { 0.0 });
    shapes[BROW_INNER_UP] = (left + right) / 2.0;
    shapes[BROW_OUTER_UP_LEFT] = left;
    shapes[BROW_OUTER_UP_RIGHT] = right;
    shapes[BROW_DOWN_LEFT] = 0.0;
    shapes[BROW_DOWN_RIGHT] = 0.0;
    shapes
}

/// Relaxed open eyes looking straight ahead: sent when the eye data stops, so VRCFT (which keeps the last values)
/// does not hold a blink or a sideways look.
pub fn neutral() -> [f32; BLENDSHAPES] {
    shapes([RELAXED; 2], [0.0; 4])
}

/// Seconds since midnight (UTC), for the frame time; Live Link Face stamps its frames with the time of day too.
pub fn time_of_day(now: SystemTime) -> f64 {
    now.duration_since(UNIX_EPOCH).map_or(0.0, |since| since.as_secs_f64() % 86_400.0)
}

/// One Live Link Face packet (version 6, network byte order) at `time` seconds.
pub fn packet(time: f64, shapes: &[f32; BLENDSHAPES]) -> [u8; PACKET_LEN] {
    let frames = time.max(0.0) * f64::from(FRAME_RATE);
    let mut packet = [0; PACKET_LEN];
    packet[..SHAPES_OFFSET].copy_from_slice(&HEADER);
    // A day at 90 fps is 7.8 million frames, well within an i32.
    packet[FRAME_OFFSET..FRAME_OFFSET + 4].copy_from_slice(&(frames.floor() as i32).to_be_bytes());
    packet[FRAME_OFFSET + 4..FRAME_OFFSET + 8].copy_from_slice(&(frames.fract() as f32).to_be_bytes());
    for (bytes, value) in packet[SHAPES_OFFSET..].chunks_exact_mut(4).zip(shapes) {
        bytes.copy_from_slice(&value.to_be_bytes());
    }
    packet
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// A port of the module's ReadData and UpdateEyeData / UpdateEyeExpressions for the eyes: at least 244 bytes, the
    /// last 244 as 61 big-endian floats. Per eye: (openness, wide, gaze x, gaze y).
    pub fn module_eyes(packet: &[u8]) -> Option<[(f32, f32, f32, f32); 2]> {
        if packet.len() < 244 {
            return None;
        }
        let values: Vec<f32> = packet[packet.len() - 244..]
            .chunks(4)
            .map(|chunk| f32::from_be_bytes(chunk.try_into().unwrap()))
            .collect();
        if values.len() != 61 {
            return None;
        }
        // EyeBlink, EyeSquint, EyeWide, EyeYaw, EyePitch of each eye
        let eye = |blink: usize, squint: usize, wide: usize, yaw: usize, pitch: usize| {
            let openness = 1.0 - (values[blink] + values[blink] * values[squint]).clamp(0.0, 1.0);
            (openness, values[wide], values[yaw], -values[pitch])
        };
        Some([eye(0, 5, 6, 55, 56), eye(7, 12, 13, 58, 59)])
    }

    /// VRCFT's v2/EyeLid from the module's values (UnifiedExpressionsParameters.cs).
    fn vrcft_lid((openness, wide, _, _): (f32, f32, f32, f32)) -> f32 {
        openness * 0.75 + wide * 0.25
    }

    #[test]
    fn packet_matches_the_live_link_face_layout() {
        let mut shapes = [0.0; BLENDSHAPES];
        for (i, value) in shapes.iter_mut().enumerate() {
            *value = i as f32 / 100.0;
        }
        // 1000.5 s at 90 fps: frame 90045, subframe 0
        let packet = packet(1000.5, &shapes);
        let mut expected = vec![6];
        expected.extend_from_slice(&[0, 0, 0, 11]);
        expected.extend_from_slice(b"frameeyeosc");
        expected.extend_from_slice(&[0, 0, 0, 10]);
        expected.extend_from_slice(b"SteamFrame");
        expected.extend_from_slice(&[0x00, 0x01, 0x5f, 0xbd]); // 90045
        expected.extend_from_slice(&[0, 0, 0, 0]); // 0.0
        expected.extend_from_slice(&[0, 0, 0, 90, 0, 0, 0, 1]);
        expected.push(61);
        for i in 0..61 {
            expected.extend_from_slice(&(i as f32 / 100.0).to_be_bytes());
        }
        assert_eq!(packet.as_slice(), expected);
        assert_eq!(packet.len(), 291);
        // Big-endian floats: the second blendshape, 0.01, is 0x3c23d70a
        assert_eq!(packet[51..55], [0x3c, 0x23, 0xd7, 0x0a]);
        // A subframe
        let later = super::packet(1000.5 + 0.25 / 90.0, &shapes);
        assert_eq!(later[30..34], 90045i32.to_be_bytes());
        assert_eq!(later[34..38], 0.25f32.to_be_bytes());
    }

    #[test]
    fn every_shape_preserves_its_bits_at_timestamp_boundaries() {
        let mut shapes = [0.0; BLENDSHAPES];
        let mut bits = 0x1234_5678u32;
        for value in &mut shapes {
            bits = bits.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            *value = f32::from_bits(bits);
        }
        shapes[..6].copy_from_slice(&[
            0.0, -0.0, f32::INFINITY, f32::NEG_INFINITY,
            f32::from_bits(0x7fc1_2345), f32::from_bits(0xffc5_4321),
        ]);
        for time in [-1.0, -0.0, 0.0, 0.25 / 90.0, 86_399.999, f64::INFINITY, f64::NAN] {
            // The previous growable serialization of the protocol fields, independent of the fixed offsets/header.
            let frames = time.max(0.0) * 90.0;
            let mut expected = vec![6];
            for name in ["frameeyeosc", "SteamFrame"] {
                expected.extend_from_slice(&(name.len() as i32).to_be_bytes());
                expected.extend_from_slice(name.as_bytes());
            }
            expected.extend_from_slice(&(frames.floor() as i32).to_be_bytes());
            expected.extend_from_slice(&(frames.fract() as f32).to_be_bytes());
            expected.extend_from_slice(&[0, 0, 0, 90, 0, 0, 0, 1, 61]);
            for value in shapes {
                expected.extend_from_slice(&value.to_be_bytes());
            }
            assert_eq!(packet(time, &shapes).as_slice(), expected, "{time:?}");
        }
    }

    #[test]
    fn a_live_link_receiver_reads_it_back() {
        // Unreal's receiver: version, two length-prefixed strings, the frame time, the count, 61 floats and nothing more
        fn take<'a>(rest: &mut &'a [u8], n: usize) -> &'a [u8] {
            let (taken, left) = rest.split_at(n);
            *rest = left;
            taken
        }
        let packet = packet(10.0, &shapes([0.3, 0.9], [0.2, -0.1, 0.25, -0.1]));
        let rest = &mut packet.as_slice();
        assert_eq!(take(rest, 1), [6]);
        for name in ["frameeyeosc", "SteamFrame"] {
            let len = i32::from_be_bytes(take(rest, 4).try_into().unwrap()) as usize;
            assert_eq!(take(rest, len), name.as_bytes());
        }
        assert_eq!(i32::from_be_bytes(take(rest, 4).try_into().unwrap()), 900);
        take(rest, 12);
        assert_eq!(take(rest, 1), [61]);
        let floats: Vec<f32> = (0..61).map(|_| f32::from_be_bytes(take(rest, 4).try_into().unwrap())).collect();
        assert!(rest.is_empty());
        assert_eq!(floats[EYE_BLINK_LEFT], blink(0.3));
        assert_eq!(floats[EYE_WIDE_RIGHT], wide(0.9));
    }

    #[test]
    fn eyelids_split_into_blink_and_wide() {
        assert_eq!((blink(0.0), wide(0.0)), (1.0, 0.0));
        assert_eq!((blink(0.375), wide(0.375)), (0.5, 0.0));
        assert_eq!((blink(0.75), wide(0.75)), (0.0, 0.0));
        assert_eq!((blink(0.875), wide(0.875)), (0.0, 0.5));
        assert_eq!((blink(1.0), wide(1.0)), (0.0, 1.0));
        // Out of range stays in range
        assert_eq!((blink(-0.1), wide(-0.1)), (1.0, 0.0));
        assert_eq!((blink(1.2), wide(1.2)), (0.0, 1.0));
        let shapes = shapes([0.375, 1.0], [0.0; 4]);
        assert_eq!([shapes[EYE_SQUINT_LEFT], shapes[EYE_SQUINT_RIGHT]], [0.0; 2]);
    }

    #[test]
    fn the_module_turns_it_back_into_the_same_eyelids_and_gaze() {
        for lid in [0.0, 0.1, 0.375, 0.6, 0.75, 0.8, 0.875, 1.0] {
            let gaze = [0.2, -0.3, 0.25, -0.3];
            let [left, right] = module_eyes(&packet(0.0, &shapes([lid, 1.0 - lid], gaze))).unwrap();
            // VRCFT's EyeLid is the eyelid --output vrchat sends
            assert!((vrcft_lid(left) - lid).abs() < 1e-6, "{lid}: {left:?}");
            assert!((vrcft_lid(right) - (1.0 - lid)).abs() < 1e-6, "{lid}: {right:?}");
            // Gaze.x/y is frameeyeosc's gaze: +x right, +y up
            assert_eq!((left.2, left.3), (0.2, -0.3));
            assert_eq!((right.2, right.3), (0.25, -0.3));
        }
        // Widened: open, and EyeWide set; closed: openness 0
        let [left, right] = module_eyes(&packet(0.0, &shapes([1.0, 0.0], [0.0; 4]))).unwrap();
        assert_eq!((left.0, left.1), (1.0, 1.0));
        assert_eq!((right.0, right.1), (0.0, 0.0));
        // Looking up and to the right: positive x and y (VRChat yaw atan(x) > 0, pitch -atan(y) < 0 is up)
        let [left, _] = module_eyes(&packet(0.0, &shapes([0.75; 2], [0.5, 0.4, 0.5, 0.4]))).unwrap();
        assert!(left.2 > 0.0 && left.3 > 0.0);
        // Too short for the module
        assert!(module_eyes(&[0; 243]).is_none());
    }

    #[test]
    fn neutral_is_relaxed_open_and_straight_ahead() {
        let [left, right] = module_eyes(&packet(0.0, &neutral())).unwrap();
        for eye in [left, right] {
            assert_eq!(eye, (1.0, 0.0, 0.0, 0.0));
            assert_eq!(vrcft_lid(eye), 0.75);
        }
    }

    #[test]
    fn brows_go_to_the_module_s_brow_shapes() {
        // The module's Constants.LiveLinkNames, from BrowDownLeft on
        let names = ["BrowDownLeft", "BrowDownRight", "BrowInnerUp", "BrowOuterUpLeft", "BrowOuterUpRight", "CheekPuff"];
        for (offset, index) in [BROW_DOWN_LEFT, BROW_DOWN_RIGHT, BROW_INNER_UP, BROW_OUTER_UP_LEFT, BROW_OUTER_UP_RIGHT]
            .into_iter()
            .enumerate()
        {
            assert_eq!(index, 41 + offset, "{}", names[offset]);
        }
        // Between the mouth (MouthUpperUpRight, 40) and CheekPuff (46); the eyes' own stay where they were
        assert_eq!((EYE_WIDE_RIGHT, EYE_YAW_LEFT), (13, 55));
        let plain = shapes([1.0, 0.9], [0.1, 0.2, 0.3, 0.4]);
        assert!(plain[41..46].iter().all(|shape| *shape == 0.0));
        let raised = with_brows(plain, [0.6, 0.2]);
        assert_eq!(raised[BROW_INNER_UP], 0.4);
        assert_eq!((raised[BROW_OUTER_UP_LEFT], raised[BROW_OUTER_UP_RIGHT]), (0.6, 0.2));
        assert_eq!((raised[BROW_DOWN_LEFT], raised[BROW_DOWN_RIGHT]), (0.0, 0.0));
        // Nothing else changes
        for (i, (a, b)) in plain.iter().zip(&raised).enumerate() {
            if !(43..46).contains(&i) {
                assert_eq!(a.to_bits(), b.to_bits(), "shape {i}");
            }
        }
        // Out of range or not a number: within 0..1
        let odd = with_brows(plain, [f32::NAN, 1.5]);
        assert_eq!((odd[BROW_OUTER_UP_LEFT], odd[BROW_OUTER_UP_RIGHT], odd[BROW_INNER_UP]), (0.0, 1.0, 0.5));
        // The neutral packet (eye data stopped) has the brows down
        assert!(neutral()[41..46].iter().all(|shape| *shape == 0.0));
    }

    #[test]
    fn time_of_day_wraps_at_midnight() {
        let day = std::time::Duration::from_secs(86_400);
        let time = UNIX_EPOCH + day * 20_000 + std::time::Duration::from_millis(3_600_250);
        assert!((time_of_day(time) - 3600.25).abs() < 1e-6);
    }
}
