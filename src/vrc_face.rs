//! VRChat's Face Tracking input (`--vrc-face-tracking`): one OSC message to `/tracking/face/v1` with a single blob
//! of 102 bytes, one per Unified Expressions tracking key, each the key's strength 0..1 as `round(value * 255)`. VRChat
//! ignores a message whose blob has another size (creators.vrchat.com, avatars/face-tracking/osc; the beta docs of
//! October 2026). Only the eye and brow keys are filled; everything else stays 0.
//!
//! What VRChat does with it, tried by hand on an avatar with the descriptor's Face Tracking section (VRChat open beta
//! 2026.4.1, SDK 3.10.6-beta.2; issue #42):
//! - A key drives only the blendshapes the avatar maps it to, so the zeros for the mouth and the rest change nothing
//!   on an avatar that maps only the eyes. Each blob replaces the whole state, though: a second program sending
//!   `/tracking/face/v1` takes turns with this one on every key both fill.
//! - On such an avatar the blob wins over `/tracking/eye/*` (native_eyes) for the eyelids and the gaze, so both can be
//!   on: native_eyes still moves avatars without the section.
//! - Eye Gaze Mode "Bone Rotation" turns the eye bones by the `EyeLook*` keys, linearly, 1.0 reaching the avatar's
//!   Rotation States.
//! - Two keys mapped to one blendshape give it the larger of the two (the inner brows to ARKit's one browInnerUp).
//!
//! The values follow VRCFaceTracking's own sender for this address (github.com/benaclejames/VRCFaceTracking,
//! VRCFaceTracking.Core/Params/Data/UnifiedData.cs, UnifiedEyeData.CopyTo; read, not copied): each eye's gaze x/y
//! (+x right, +y up, 1 = 45°, as in v2/EyeLeftX/Y) split into its four EyeLook keys, and EyeClosed = 1 - openness.

use crate::livelink;

/// The blob's size, and so the number of tracking keys.
pub const LEN: usize = 102;

// Tracking key indices (zero-based; right eye first, as VRChat lists them).
const EYE_LOOK_OUT_RIGHT: usize = 0;
const EYE_LOOK_IN_RIGHT: usize = 1;
const EYE_LOOK_UP_RIGHT: usize = 2;
const EYE_LOOK_DOWN_RIGHT: usize = 3;
const EYE_LOOK_OUT_LEFT: usize = 4;
const EYE_LOOK_IN_LEFT: usize = 5;
const EYE_LOOK_UP_LEFT: usize = 6;
const EYE_LOOK_DOWN_LEFT: usize = 7;
const EYE_CLOSED_RIGHT: usize = 8;
const EYE_CLOSED_LEFT: usize = 9;
const EYE_DILATION_RIGHT: usize = 10;
const EYE_DILATION_LEFT: usize = 11;
const EYE_CONSTRICT_RIGHT: usize = 12;
const EYE_CONSTRICT_LEFT: usize = 13;
const EYE_SQUINT_RIGHT: usize = 14;
const EYE_SQUINT_LEFT: usize = 15;
const EYE_WIDE_RIGHT: usize = 16;
const EYE_WIDE_LEFT: usize = 17;
const BROW_INNER_UP_RIGHT: usize = 22;
const BROW_INNER_UP_LEFT: usize = 23;
const BROW_OUTER_UP_RIGHT: usize = 24;
const BROW_OUTER_UP_LEFT: usize = 25;

/// The pupil dilation (0..1) of a pupil at its usual size: above it is EyeDilation, below it EyeConstrict.
const PUPIL_USUAL: f32 = 0.5;

/// One eye's values for the blob.
pub struct Eye {
    /// The VRCFT eyelid as sent: 0 closed, 0.75 relaxed open, 1 widened.
    pub lid: f32,
    /// The gaze as sent, x and y in -1..1 (+x right, +y up, 1 = 45°).
    pub gaze: [f32; 2],
    /// The eye camera's squint, while it is used for this eye.
    pub squint: Option<f32>,
    /// The eye camera's pupil dilation (0..1), while it is used for this eye.
    pub pupil_dilation: Option<f32>,
    /// The brow raised with widening (brow_from_widen), while that is on.
    pub brow: Option<f32>,
}

/// The blob for the left and right eye. Brows are only ever raised (inner = outer, as in the FrameEyeOSC packet):
/// BrowLowerer and BrowPinch stay 0.
pub fn blob([left, right]: [Eye; 2]) -> [u8; LEN] {
    let mut keys = [0.0; LEN];
    // Out is toward the ear: right for the right eye, left for the left eye.
    let [x, y] = right.gaze;
    keys[EYE_LOOK_OUT_RIGHT] = x;
    keys[EYE_LOOK_IN_RIGHT] = -x;
    keys[EYE_LOOK_UP_RIGHT] = y;
    keys[EYE_LOOK_DOWN_RIGHT] = -y;
    let [x, y] = left.gaze;
    keys[EYE_LOOK_OUT_LEFT] = -x;
    keys[EYE_LOOK_IN_LEFT] = x;
    keys[EYE_LOOK_UP_LEFT] = y;
    keys[EYE_LOOK_DOWN_LEFT] = -y;
    for (eye, closed, squint, wide, dilation, constrict, inner, outer) in [
        (&right, EYE_CLOSED_RIGHT, EYE_SQUINT_RIGHT, EYE_WIDE_RIGHT, EYE_DILATION_RIGHT, EYE_CONSTRICT_RIGHT,
            BROW_INNER_UP_RIGHT, BROW_OUTER_UP_RIGHT),
        (&left, EYE_CLOSED_LEFT, EYE_SQUINT_LEFT, EYE_WIDE_LEFT, EYE_DILATION_LEFT, EYE_CONSTRICT_LEFT,
            BROW_INNER_UP_LEFT, BROW_OUTER_UP_LEFT),
    ] {
        keys[closed] = livelink::blink(eye.lid);
        keys[wide] = livelink::wide(eye.lid);
        keys[squint] = eye.squint.unwrap_or(0.0);
        if let Some(pupil) = eye.pupil_dilation {
            keys[dilation] = (pupil - PUPIL_USUAL) / (1.0 - PUPIL_USUAL);
            keys[constrict] = (PUPIL_USUAL - pupil) / PUPIL_USUAL;
        }
        if let Some(brow) = eye.brow {
            keys[inner] = brow;
            keys[outer] = brow;
        }
    }
    keys.map(byte)
}

/// Relaxed open eyes looking straight ahead, pupils and brows at rest: every key 0. Sent once when tracking stops, so
/// the avatar doesn't hold a blink or a sideways look.
pub fn neutral() -> [u8; LEN] {
    [0; LEN]
}

/// A strength as the blob's byte: clamped to 0..1, 0..255. Not finite (it can't be from the filters) is 0.
fn byte(value: f32) -> u8 {
    if value.is_finite() {
        (value.clamp(0.0, 1.0) * 255.0).round() as u8
    } else {
        0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn eye(lid: f32, gaze: [f32; 2]) -> Eye {
        Eye {
            lid,
            gaze,
            squint: None,
            pupil_dilation: None,
            brow: None,
        }
    }

    #[test]
    fn eyelids_close_and_widen_per_eye() {
        let keys = blob([eye(0.0, [0.0; 2]), eye(1.0, [0.0; 2])]);
        assert_eq!((keys[EYE_CLOSED_LEFT], keys[EYE_WIDE_LEFT]), (255, 0));
        assert_eq!((keys[EYE_CLOSED_RIGHT], keys[EYE_WIDE_RIGHT]), (0, 255));
        // Relaxed open is neither closed nor widened; halfway shut is half closed
        let keys = blob([eye(0.75, [0.0; 2]), eye(0.375, [0.0; 2])]);
        assert_eq!((keys[EYE_CLOSED_LEFT], keys[EYE_WIDE_LEFT]), (0, 0));
        assert_eq!((keys[EYE_CLOSED_RIGHT], keys[EYE_WIDE_RIGHT]), (128, 0));
    }

    #[test]
    fn gaze_splits_into_look_keys_toward_ear_and_nose() {
        // Both eyes looking to the user's left (-x) and up: the left eye looks out, the right eye in
        let keys = blob([eye(0.75, [-0.5, 0.25]), eye(0.75, [-0.5, 0.25])]);
        assert_eq!(keys[EYE_LOOK_OUT_LEFT], 128);
        assert_eq!(keys[EYE_LOOK_IN_LEFT], 0);
        assert_eq!(keys[EYE_LOOK_IN_RIGHT], 128);
        assert_eq!(keys[EYE_LOOK_OUT_RIGHT], 0);
        assert_eq!((keys[EYE_LOOK_UP_LEFT], keys[EYE_LOOK_UP_RIGHT]), (64, 64));
        assert_eq!((keys[EYE_LOOK_DOWN_LEFT], keys[EYE_LOOK_DOWN_RIGHT]), (0, 0));
        // To the right and down, past 45° (clamped)
        let keys = blob([eye(0.75, [1.5, -1.0]), eye(0.75, [1.5, -1.0])]);
        assert_eq!((keys[EYE_LOOK_IN_LEFT], keys[EYE_LOOK_OUT_RIGHT]), (255, 255));
        assert_eq!((keys[EYE_LOOK_DOWN_LEFT], keys[EYE_LOOK_DOWN_RIGHT]), (255, 255));
    }

    #[test]
    fn camera_values_and_brows_go_to_their_eye() {
        let left = Eye {
            squint: Some(0.6),
            pupil_dilation: Some(0.75),
            brow: Some(0.4),
            ..eye(0.75, [0.0; 2])
        };
        let right = Eye {
            pupil_dilation: Some(0.25),
            ..eye(0.75, [0.0; 2])
        };
        let keys = blob([left, right]);
        assert_eq!((keys[EYE_SQUINT_LEFT], keys[EYE_SQUINT_RIGHT]), (153, 0));
        // Above the usual pupil it dilates, below it constricts
        assert_eq!((keys[EYE_DILATION_LEFT], keys[EYE_CONSTRICT_LEFT]), (128, 0));
        assert_eq!((keys[EYE_DILATION_RIGHT], keys[EYE_CONSTRICT_RIGHT]), (0, 128));
        assert_eq!((keys[BROW_INNER_UP_LEFT], keys[BROW_OUTER_UP_LEFT]), (102, 102));
        assert_eq!((keys[BROW_INNER_UP_RIGHT], keys[BROW_OUTER_UP_RIGHT]), (0, 0));
    }

    #[test]
    fn only_eye_and_brow_keys_are_filled() {
        let full = |lid| Eye {
            squint: Some(1.0),
            pupil_dilation: Some(1.0),
            brow: Some(1.0),
            ..eye(lid, [1.0, 1.0])
        };
        let keys = blob([full(0.0), full(0.0)]);
        // Lowerer and pinch (18..21) and everything past the brows stay 0
        assert!(keys[18..22].iter().chain(&keys[26..]).all(|&key| key == 0));
        assert_eq!(keys.len(), 102);
        assert_eq!(neutral(), [0; LEN]);
        assert_eq!(blob([eye(0.75, [0.0; 2]), eye(0.75, [0.0; 2])]), neutral());
    }

    #[test]
    fn values_that_are_not_finite_go_out_as_zero() {
        let keys = blob([eye(f32::NAN, [f32::INFINITY, f32::NAN]), eye(0.75, [0.0; 2])]);
        assert_eq!(keys[EYE_CLOSED_LEFT], 0);
        assert_eq!(keys[EYE_LOOK_IN_LEFT], 0);
        assert_eq!(byte(f32::NEG_INFINITY), 0);
    }
}
