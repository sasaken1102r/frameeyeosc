//! FrameEyeOSC packets (FEO1, version 1), for the FrameEyeOSC module for VRCFaceTracking (`--output frameeyeosc`).
//!
//! The layout, and how the module (VRCFT-FrameEyeOSC, FrameEyeOSC/EyeMapper.cs) turns a packet into VRCFT's eye data,
//! are in docs/protocol.md. In short: 72 bytes, little-endian; a header (magic, version, length, sequence, flags, time)
//! and per eye the gaze, the VRCFT eyelid, the eye camera's squint and the pupil in mm and as a dilation, with flags
//! saying which of the camera's values are there. Unlike the LiveLink module, this one has pupils, so VRCFT sends them
//! to each avatar in the form it takes them.

use std::time::{SystemTime, UNIX_EPOCH};

pub const DEFAULT_PORT: u16 = 11112;
pub const PACKET_LEN: usize = 72;
const MAGIC: &[u8; 4] = b"FEO1";
const VERSION: u16 = 1;
// Flags
const TRACKING: u32 = 1;
const CAMERA_LID: [u32; 2] = [1 << 1, 1 << 2];
const PUPIL: [u32; 2] = [1 << 3, 1 << 4];
// frameeyeosc's VRCFT eyelid of a relaxed open eye.
const RELAXED: f32 = 0.75;

/// One eye as it goes out.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Eye {
    /// x (+ right) and y (+ up) in -1..1, 1 = 45°.
    pub gaze: [f32; 2],
    /// VRCFT eyelid: 0 closed, 0.75 relaxed open, 1 widened.
    pub lid: f32,
    /// The eye camera's squint, while the camera drives this eye's eyelid.
    pub squint: Option<f32>,
    /// The pupil in mm and its dilation (0..1), while the camera's pupil is used.
    pub pupil: Option<(f32, f32)>,
}

impl Eye {
    /// Relaxed open, looking straight ahead, nothing from the eye camera.
    pub const NEUTRAL: Eye = Eye {
        gaze: [0.0; 2],
        lid: RELAXED,
        squint: None,
        pupil: None,
    };
}

/// Microseconds since the Unix epoch (the packet's time stamp, only for diagnostics).
pub fn time_us(now: SystemTime) -> u64 {
    now.duration_since(UNIX_EPOCH).map_or(0, |since| since.as_micros() as u64)
}

/// One packet. `tracking` false says the eye values are not live (they should then be `Eye::NEUTRAL`).
pub fn packet(sequence: u32, tracking: bool, time_us: u64, eyes: [Eye; 2]) -> [u8; PACKET_LEN] {
    let mut flags = if tracking { TRACKING } else { 0 };
    for (side, eye) in eyes.iter().enumerate() {
        if eye.squint.is_some() {
            flags |= CAMERA_LID[side];
        }
        if eye.pupil.is_some() {
            flags |= PUPIL[side];
        }
    }
    let mut packet = [0; PACKET_LEN];
    packet[0..4].copy_from_slice(MAGIC);
    packet[4..6].copy_from_slice(&VERSION.to_le_bytes());
    packet[6..8].copy_from_slice(&(PACKET_LEN as u16).to_le_bytes());
    packet[8..12].copy_from_slice(&sequence.to_le_bytes());
    packet[12..16].copy_from_slice(&flags.to_le_bytes());
    packet[16..24].copy_from_slice(&time_us.to_le_bytes());
    for (side, eye) in eyes.iter().enumerate() {
        let (mm, dilation) = eye.pupil.unwrap_or((0.0, 0.0));
        let values = [eye.gaze[0], eye.gaze[1], eye.lid, eye.squint.unwrap_or(0.0), mm, dilation];
        for (k, value) in values.iter().enumerate() {
            let at = 24 + side * 24 + k * 4;
            packet[at..at + 4].copy_from_slice(&value.to_le_bytes());
        }
    }
    packet
}

#[cfg(test)]
pub mod tests {
    use super::*;

    /// What the module makes of one eye (docs/protocol.md, "How the module maps it to VRCFT"): Gaze x/y, Openness and
    /// the EyeWide and EyeSquint shapes.
    #[derive(Clone, Copy, Debug, PartialEq)]
    pub struct ModuleEye {
        pub gaze: [f32; 2],
        pub openness: f32,
        pub wide: f32,
        pub squint: f32,
    }

    impl ModuleEye {
        /// VRCFT's v2/EyeLid of this eye (UnifiedExpressionsParameters.cs).
        pub fn vrcft_lid(&self) -> f32 {
            self.openness * 0.75 + self.wide * 0.25
        }
    }

    /// What the module makes of a packet: both eyes, and the pupils as VRCFT sends them (PupilDiameterLeft/Right in
    /// cm and PupilDilation, VRCFT's normalisation of the mean diameter in the dilation range the module sets), if
    /// any eye has one. None for a packet the module drops; neutral eyes for one without TRACKING.
    pub fn module_eyes(packet: &[u8]) -> Option<([ModuleEye; 2], Option<[f32; 3]>)> {
        let u16_at = |at: usize| u16::from_le_bytes(packet[at..at + 2].try_into().unwrap());
        let u32_at = |at: usize| u32::from_le_bytes(packet[at..at + 4].try_into().unwrap());
        let f32_at = |at: usize| f32::from_le_bytes(packet[at..at + 4].try_into().unwrap());
        if packet.len() < 24 || &packet[0..4] != b"FEO1" || u16_at(4) != 1 {
            return None;
        }
        let length = usize::from(u16_at(6));
        if length < 72 || packet.len() < length {
            return None;
        }
        let eyes: Vec<[f32; 6]> = (0..2).map(|side| std::array::from_fn(|k| f32_at(24 + side * 24 + k * 4))).collect();
        if eyes.iter().flatten().any(|value| !value.is_finite()) {
            return None;
        }
        let flags = u32_at(12);
        let neutral = ModuleEye { gaze: [0.0; 2], openness: 1.0, wide: 0.0, squint: 0.0 };
        let mapped = |side: usize| {
            let [x, y, lid, squint, ..] = eyes[side];
            if flags & 1 == 0 {
                return neutral;
            }
            let lid = lid.clamp(0.0, 1.0);
            ModuleEye {
                gaze: [x.clamp(-1.0, 1.0), y.clamp(-1.0, 1.0)],
                openness: (lid / 0.75).clamp(0.0, 1.0),
                wide: ((lid - 0.75) / 0.25).clamp(0.0, 1.0),
                squint: if flags & (2 << side) != 0 { squint.clamp(0.0, 1.0) } else { 0.0 },
            }
        };
        let has = [flags & 8 != 0, flags & 16 != 0];
        let pupils = (flags & 1 != 0 && has.contains(&true)).then(|| {
            let [left, right] = [0, 1].map(|side| if has[side] { eyes[side] } else { eyes[1 - side] });
            let (mean_mm, dilation) = ((left[4] + right[4]) / 2.0, (left[5] + right[5]) / 2.0);
            let min = mean_mm - dilation.clamp(0.0, 1.0) * 2.655;
            [left[4] * 0.1, right[4] * 0.1, (mean_mm - min) / 2.655]
        });
        Some(([mapped(0), mapped(1)], pupils))
    }

    #[test]
    fn packet_matches_the_example_in_the_protocol() {
        let left = Eye {
            gaze: [0.25, -0.5],
            lid: 0.875,
            squint: Some(0.5),
            pupil: Some((4.0, 0.25)),
        };
        let right = Eye {
            gaze: [0.125, -0.5],
            lid: 0.375,
            squint: None,
            pupil: Some((3.5, 0.75)),
        };
        let packet = packet(7, true, 1_700_000_000_123_456, [left, right]);
        let expected = "46454f3101004800070000001b00000040222018240a0600\
                        0000803e000000bf0000603f0000003f000080400000803e\
                        0000003e000000bf0000c03e00000000000060400000403f";
        let hex: String = packet.iter().map(|byte| format!("{byte:02x}")).collect();
        assert_eq!(hex, expected);
    }

    #[test]
    fn flags_say_which_camera_values_are_there() {
        let flags = |tracking, eyes| u32::from_le_bytes(packet(0, tracking, 0, eyes)[12..16].try_into().unwrap());
        assert_eq!(flags(false, [Eye::NEUTRAL; 2]), 0);
        assert_eq!(flags(true, [Eye::NEUTRAL; 2]), 1);
        let squint = Eye { squint: Some(0.2), ..Eye::NEUTRAL };
        let pupil = Eye { pupil: Some((4.0, 0.5)), ..Eye::NEUTRAL };
        assert_eq!(flags(true, [squint, Eye::NEUTRAL]), 1 | 2);
        assert_eq!(flags(true, [Eye::NEUTRAL, squint]), 1 | 4);
        assert_eq!(flags(true, [pupil, Eye::NEUTRAL]), 1 | 8);
        assert_eq!(flags(true, [Eye::NEUTRAL, pupil]), 1 | 16);
        // Values that are not there go out as 0
        let packet = packet(0, true, 0, [Eye { lid: 0.5, ..Eye::NEUTRAL }; 2]);
        assert_eq!(packet[36..48], [0; 12]);
        assert_eq!(packet[60..72], [0; 12]);
    }

    #[test]
    fn the_header_counts_and_stamps() {
        let packet = packet(0xffff_ffff, true, 5, [Eye::NEUTRAL; 2]);
        assert_eq!(&packet[0..8], b"FEO1\x01\x00\x48\x00");
        assert_eq!(packet[8..12], [0xff; 4]);
        assert_eq!(packet[16..24], 5u64.to_le_bytes());
        let time = UNIX_EPOCH + std::time::Duration::from_micros(1_700_000_000_123_456);
        assert_eq!(time_us(time), 1_700_000_000_123_456);
        assert_eq!(time_us(UNIX_EPOCH - std::time::Duration::from_secs(1)), 0);
    }

    #[test]
    fn the_module_turns_it_back_into_the_same_eyelids_gaze_and_pupils() {
        for lid in [0.0, 0.1, 0.375, 0.6, 0.75, 0.8, 0.875, 1.0] {
            let left = Eye { gaze: [0.2, -0.3], lid, ..Eye::NEUTRAL };
            let right = Eye { gaze: [0.25, -0.3], lid: 1.0 - lid, ..Eye::NEUTRAL };
            let ([left, right], pupils) = module_eyes(&packet(1, true, 0, [left, right])).unwrap();
            // VRCFT's EyeLid is the eyelid --output vrchat sends
            assert!((left.vrcft_lid() - lid).abs() < 1e-6, "{lid}: {left:?}");
            assert!((right.vrcft_lid() - (1.0 - lid)).abs() < 1e-6, "{lid}: {right:?}");
            // Gaze.x/y is frameeyeosc's gaze: +x right, +y up
            assert_eq!((left.gaze, right.gaze), ([0.2, -0.3], [0.25, -0.3]));
            assert_eq!((left.squint, pupils), (0.0, None));
        }
        // Squint and pupils: VRCFT's PupilDiameterLeft/Right are --output vrchat's (cm), its PupilDilation the mean
        let left = Eye { squint: Some(0.4), pupil: Some((4.0, 0.25)), ..Eye::NEUTRAL };
        let right = Eye { pupil: Some((3.0, 0.75)), ..Eye::NEUTRAL };
        let ([left, right], pupils) = module_eyes(&packet(1, true, 0, [left, right])).unwrap();
        assert_eq!((left.squint, right.squint), (0.4, 0.0));
        let [diameter_left, diameter_right, dilation] = pupils.unwrap();
        assert_eq!((diameter_left, diameter_right), (0.4, 0.3));
        assert!((dilation - 0.5).abs() < 1e-6);
        // One eye's pupil stands for both
        let right = Eye { pupil: Some((3.0, 0.75)), ..Eye::NEUTRAL };
        let (_, pupils) = module_eyes(&packet(1, true, 0, [Eye::NEUTRAL, right])).unwrap();
        let [diameter_left, diameter_right, dilation] = pupils.unwrap();
        assert_eq!((diameter_left, diameter_right), (0.3, 0.3));
        assert!((dilation - 0.75).abs() < 1e-6);
    }

    #[test]
    fn not_tracking_is_relaxed_open_and_straight_ahead() {
        let neutral = ModuleEye { gaze: [0.0; 2], openness: 1.0, wide: 0.0, squint: 0.0 };
        let (eyes, pupils) = module_eyes(&packet(3, false, 0, [Eye::NEUTRAL; 2])).unwrap();
        assert_eq!((eyes, pupils), ([neutral; 2], None));
        assert_eq!(neutral.vrcft_lid(), 0.75);
        // So is a tracking packet with neutral eyes
        let (eyes, _) = module_eyes(&packet(3, true, 0, [Eye::NEUTRAL; 2])).unwrap();
        assert_eq!(eyes, [neutral; 2]);
        // The module drops what is not a packet
        assert!(module_eyes(&packet(3, true, 0, [Eye::NEUTRAL; 2])[..71]).is_none());
        assert!(module_eyes(b"FEO2").is_none());
    }
}
