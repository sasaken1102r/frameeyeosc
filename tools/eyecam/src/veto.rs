//! The eyelid veto: a small CNN that may only cancel a low classical EyeLid when it is sure the eye is open.
//!
//! The classical engine (`live`) stays in charge. Looking far to the temporal side, the lids and lashes cut into the
//! pupil and the aperture reads narrow, so the classical EyeLid drops below 0.5 (a false half-shut eye: 18% of the
//! free-use frames past 20 degrees temporal in the 2026-10 recordings). On those frames only (EyeLid < GATE_LID, or
//! the closed flag set), the CNN looks at the frame; when it says "open" with P >= P_ON on K_OPEN frames in a row
//! (K_CLOSED while the closed flag is set, so a blink is never cut short), the frame is vetoed: EyeLid is raised to
//! at least FLOOR, EyeSquint is 0 and the closed flag is cleared. EyeWide is left as it is. The engine's own state
//! (baselines, holds, last values) never sees the veto: it only changes what goes out.
//!
//! The network: the upright frame's crop x 150..360, y 40..360, average-pooled to 32 x 32 and normalised per image,
//! eight 3x3 convolutions (4, 4, 8, 8, 16, 16, 32, 32 channels, every other one stride 2, ReLU), a global average
//! pool and a linear output: one logit for "open". Five of them (trained with different seeds on the same data) are
//! averaged, logit by logit: single nets differ by 2-5 points of squint recall and 6-9% of temporal false half-shut
//! frames, the mean of five does not. 2.1 MFLOP a run, a plain loop (no SIMD, no crates): about 0.3 ms on a desktop
//! PC.
//!
//! The weights (models/veto_S32c4x5.bin, 92,385 little-endian f32, batch norm folded into the convolutions) were
//! checked against the training framework's output to 5e-6 in the logit.

use crate::vision::{H, W};
use std::sync::OnceLock;
use std::time::Instant;

/// The CNN's input: S x S pixels pooled from the crop below (of the upright 400 x 400 frame).
pub const S: usize = 32;
const CROP_X: (usize, usize) = (150, 360);
const CROP_Y: (usize, usize) = (40, 360);
/// Channels of the first stage (doubled at each of the four), and how many nets are averaged.
const C0: usize = 4;
const STAGES: usize = 4;
const MEMBERS: usize = 5;

/// The CNN runs on frames with EyeLid below this (or closed).
pub const GATE_LID: f64 = 0.5;
/// P(open) needed on K_OPEN gated frames in a row to switch the veto on (K_CLOSED when the frame is closed), and to
/// keep it on.
pub const P_ON: f64 = 0.98;
pub const K_OPEN: u32 = 3;
pub const K_CLOSED: u32 = 10;
/// A vetoed frame's EyeLid is at least this (VRCFT's normal open eye).
pub const FLOOR: f64 = 0.75;

static WEIGHTS: &[u8] = include_bytes!("../models/veto_S32c4x5.bin");
const N_FLOATS: usize = 92_385;

struct Layer {
    cin: usize,
    cout: usize,
    stride: usize,
    /// [cout][cin][3][3]
    w: Vec<f32>,
    b: Vec<f32>,
}

struct Member {
    layers: Vec<Layer>,
    head_w: Vec<f32>,
    head_b: f32,
}

/// The five nets, read once from WEIGHTS (in file order: per net, per layer its weights then biases; then the head's).
fn members() -> &'static [Member] {
    static NETS: OnceLock<Vec<Member>> = OnceLock::new();
    NETS.get_or_init(|| {
        let flat: Vec<f32> = WEIGHTS.chunks_exact(4).map(|c| f32::from_le_bytes([c[0], c[1], c[2], c[3]])).collect();
        assert_eq!(flat.len(), N_FLOATS, "veto weights: wrong size");
        let mut off = 0;
        let mut take = |n: usize| {
            off += n;
            flat[off - n..off].to_vec()
        };
        let nets: Vec<Member> = (0..MEMBERS)
            .map(|_| {
                let mut layers = Vec::new();
                let mut cin = 1;
                for stage in 0..STAGES {
                    for stride in [2, 1] {
                        let cout = C0 << stage;
                        let w = take(cout * cin * 9);
                        let b = take(cout);
                        layers.push(Layer { cin, cout, stride, w, b });
                        cin = cout;
                    }
                }
                let head_w = take(cin);
                let head_b = take(1)[0];
                Member { layers, head_w, head_b }
            })
            .collect();
        assert_eq!(off, N_FLOATS);
        nets
    })
}

/// The CNN's input from an upright frame: the crop's adaptive average pool to S x S (output (i, j) averages crop rows
/// floor(i * 320 / S) .. ceil((i + 1) * 320 / S) - 1 and the same for the 210 columns), rounded half to even, as the
/// training data was made.
pub fn crop(img: &[u8]) -> [u8; S * S] {
    let (cw, ch) = (CROP_X.1 - CROP_X.0, CROP_Y.1 - CROP_Y.0);
    let mut out = [0u8; S * S];
    for i in 0..S {
        let (ya, yb) = (i * ch / S, ((i + 1) * ch).div_ceil(S));
        for j in 0..S {
            let (xa, xb) = (j * cw / S, ((j + 1) * cw).div_ceil(S));
            let mut acc = 0u32;
            for y in ya..yb {
                let row = &img[(CROP_Y.0 + y) * W + CROP_X.0..][..cw];
                acc += row[xa..xb].iter().map(|&v| v as u32).sum::<u32>();
            }
            out[i * S + j] = (acc as f32 / ((yb - ya) * (xb - xa)) as f32).round_ties_even() as u8;
        }
    }
    out
}

/// Activations and padding, kept between runs.
#[derive(Default)]
struct Scratch {
    a: Vec<f32>,
    b: Vec<f32>,
    pad: Vec<f32>,
}

/// 3x3 convolution, zero padding 1, + bias + ReLU. `inp` is [cin][h][w]; returns the output size.
fn conv3x3(l: &Layer, inp: &[f32], h: usize, w: usize, pad: &mut Vec<f32>, out: &mut Vec<f32>) -> (usize, usize) {
    let (ph, pw) = (h + 2, w + 2);
    pad.clear();
    pad.resize(l.cin * ph * pw, 0.0);
    for c in 0..l.cin {
        for y in 0..h {
            pad[(c * ph + y + 1) * pw + 1..][..w].copy_from_slice(&inp[(c * h + y) * w..][..w]);
        }
    }
    let s = l.stride;
    let (oh, ow) = ((h - 1) / s + 1, (w - 1) / s + 1);
    out.clear();
    out.resize(l.cout * oh * ow, 0.0);
    for oc in 0..l.cout {
        let o = &mut out[oc * oh * ow..(oc + 1) * oh * ow];
        o.fill(l.b[oc]);
        for ic in 0..l.cin {
            let wk = &l.w[(oc * l.cin + ic) * 9..][..9];
            let pc = &pad[ic * ph * pw..(ic + 1) * ph * pw];
            for oy in 0..oh {
                let orow = &mut o[oy * ow..(oy + 1) * ow];
                for ky in 0..3 {
                    let prow = &pc[(oy * s + ky) * pw..(oy * s + ky + 1) * pw];
                    let (w0, w1, w2) = (wk[ky * 3], wk[ky * 3 + 1], wk[ky * 3 + 2]);
                    if s == 1 {
                        for (ox, v) in orow.iter_mut().enumerate() {
                            *v += w0 * prow[ox] + w1 * prow[ox + 1] + w2 * prow[ox + 2];
                        }
                    } else {
                        for (ox, v) in orow.iter_mut().enumerate() {
                            *v += w0 * prow[2 * ox] + w1 * prow[2 * ox + 1] + w2 * prow[2 * ox + 2];
                        }
                    }
                }
            }
        }
        for v in o.iter_mut() {
            *v = v.max(0.0);
        }
    }
    (oh, ow)
}

/// One net's logit for a pooled crop.
fn member_logit(m: &Member, x0: &[f32], sc: &mut Scratch) -> f32 {
    let Scratch { a, b, pad } = sc;
    a.clear();
    a.extend_from_slice(x0);
    let (mut h, mut w) = (S, S);
    for l in &m.layers {
        (h, w) = conv3x3(l, a, h, w, pad, b);
        std::mem::swap(a, b);
    }
    let c = m.layers.last().map_or(1, |l| l.cout);
    let mut z = m.head_b;
    for ch in 0..c {
        z += a[ch * h * w..(ch + 1) * h * w].iter().sum::<f32>() / (h * w) as f32 * m.head_w[ch];
    }
    z
}

/// The five nets' mean logit for "open" on a pooled crop.
fn logit_of(crop: &[u8; S * S], sc: &mut Scratch) -> f32 {
    // Mean and spread in f64: an f32 sum over the crop alone moves the logit by ~1e-5.
    let n = (S * S) as f64;
    let mean = crop.iter().map(|&v| v as f64).sum::<f64>() / n;
    let var = crop.iter().map(|&v| (v as f64 - mean) * (v as f64 - mean)).sum::<f64>() / (n - 1.0);
    let k = 1.0 / (var.sqrt() + 1.0);
    let x0: Vec<f32> = crop.iter().map(|&v| ((v as f64 - mean) * k) as f32).collect();
    let nets = members();
    nets.iter().map(|m| member_logit(m, &x0, sc)).sum::<f32>() / nets.len() as f32
}

/// The mean logit for "open" of an upright 400 x 400 frame (tests and benchmarks; the engine goes through `Veto`).
pub fn logit(img: &[u8]) -> f32 {
    logit_of(&crop(img), &mut Scratch::default())
}

/// What the veto did on one frame (`live::Out::veto`).
#[derive(Clone, Copy, Debug)]
pub struct Step {
    /// The CNN ran on this frame, its P(open) (NaN when it did not run) and how long it took (microseconds).
    pub ran: bool,
    pub p_open: f64,
    pub us: f64,
    /// The frame is vetoed (EyeLid raised, EyeSquint 0, not closed), and it is the first of a run of vetoed frames.
    pub on: bool,
    pub fired: bool,
}

impl Default for Step {
    fn default() -> Self {
        Self { ran: false, p_open: f64::NAN, us: 0.0, on: false, fired: false }
    }
}

/// The veto's state for one eye: the run of gated frames the CNN called open, and whether the veto is on.
#[derive(Default)]
pub struct Veto {
    run: u32,
    on: bool,
    scratch: Scratch,
}

impl Veto {
    pub fn reset(&mut self) {
        (self.run, self.on) = (0, false);
    }

    /// One frame: `img` the upright frame, `eye_lid` and `closed` the classical values. Frames outside the gate reset
    /// the run and switch the veto off without running the CNN.
    pub fn step(&mut self, img: &[u8], eye_lid: f64, closed: bool) -> Step {
        let gated = closed || (0.0..GATE_LID).contains(&eye_lid);
        if !gated || img.len() != W * H {
            self.reset();
            return Step::default();
        }
        let t0 = Instant::now();
        let z = logit_of(&crop(img), &mut self.scratch) as f64;
        let us = t0.elapsed().as_secs_f64() * 1e6;
        let p_open = 1.0 / (1.0 + (-z).exp());
        let fired = self.advance(p_open, closed);
        Step { ran: true, p_open, us, on: self.on, fired }
    }

    /// The rule on a gated frame the CNN called `p_open`: on after a run of K_OPEN (K_CLOSED while closed) frames at
    /// P_ON or more, then on while it stays there. Returns whether it switched on with this frame.
    fn advance(&mut self, p_open: f64, closed: bool) -> bool {
        self.run = if p_open >= P_ON { self.run + 1 } else { 0 };
        let was = self.on;
        self.on = if was { p_open >= P_ON } else { self.run >= if closed { K_CLOSED } else { K_OPEN } };
        self.on && !was
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// xorshift32, as the generator of the expected values (the training side's numpy reference) has it.
    fn xorshift(s: &mut u32) -> u32 {
        *s ^= *s << 13;
        *s ^= *s >> 17;
        *s ^= *s << 5;
        *s
    }

    /// Made-up 400 x 400 frames (no eye in them): a gradient with noise, dark discs and a dark band, from integers only
    /// so the Python generator makes the same pixels. `kind` 0..5.
    fn synth_frame(kind: u32) -> Vec<u8> {
        let mut s = 0x9e37_79b9u32 ^ (kind + 1).wrapping_mul(0x85eb_ca6b);
        let mut img = vec![0u8; W * H];
        for y in 0..H as i64 {
            for x in 0..W as i64 {
                let noise = (xorshift(&mut s) % 21) as i64 - 10;
                let mut v = 90 + x * 60 / 400 + y * 40 / 400;
                let disc = |cx: i64, cy: i64, r: i64| (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
                match kind {
                    1 => {
                        if disc(250, 200, 40) {
                            v = 25;
                        }
                    }
                    2 => {
                        if disc(180, 210, 35) {
                            v = 30;
                        }
                        if (120..132).contains(&y) {
                            v = 40;
                        }
                    }
                    3 => v = (xorshift(&mut s) % 256) as i64,
                    4 => {
                        v = if disc(260, 220, 70) { 20 } else { 210 };
                        if disc(240, 200, 8) {
                            v = 250;
                        }
                    }
                    5 => v = 200 - y * 120 / 400,
                    _ => {}
                }
                img[(y as usize) * W + x as usize] = (v + noise).clamp(0, 255) as u8;
            }
        }
        img
    }

    /// The numpy reference's output (f64, the same weights) for synth_frame(0..5): the pooled crop's sum and the logit.
    const EXPECTED: [(u32, f32); 6] = [
        (150775, -1.963_032_2),  // P(open) 0.123
        (141425, 5.974_573_4),   // 0.997
        (140819, -0.387_576_46), // 0.404
        (130502, 17.097_528),    // 1.000
        (171267, 16.751_217),    // 1.000
        (143985, 7.796_651_3),   // 0.9996
    ];

    #[test]
    fn weights_load() {
        let nets = members();
        assert_eq!(nets.len(), MEMBERS);
        assert!(nets.iter().all(|m| m.layers.len() == 2 * STAGES && m.head_w.len() == C0 << (STAGES - 1)));
    }

    #[test]
    fn forward_matches_reference() {
        for (kind, &(sum, want)) in EXPECTED.iter().enumerate() {
            let img = synth_frame(kind as u32);
            let c = crop(&img);
            assert_eq!(c.iter().map(|&v| v as u32).sum::<u32>(), sum, "crop of frame {kind}");
            let z = logit(&img);
            assert!((z - want).abs() < 2e-5, "frame {kind}: logit {z}, expected {want}");
        }
    }

    #[test]
    fn rule_needs_a_run_and_never_runs_outside_the_gate() {
        let mut v = Veto::default();
        let img = synth_frame(0);
        // Not gated: nothing runs.
        let s = v.step(&img, 0.75, false);
        assert!(!s.ran && !s.on && s.p_open.is_nan());
        let s = v.step(&img, f64::NAN, false);
        assert!(!s.ran);
        // Gated (low or closed): it runs. Frame 0 reads closed to the CNN (P 0.12), frame 3 open (P 1.0).
        let s = v.step(&img, 0.3, false);
        assert!(s.ran && !s.on && s.p_open < 0.2);
        let open = synth_frame(3);
        let on: Vec<bool> = (0..K_OPEN).map(|_| v.step(&open, 0.3, false).on).collect();
        assert_eq!(on, [false, false, true]);
        assert!(v.step(&open, 0.0, true).on, "stays on when the eye reads closed");
        assert!(!v.step(&open, 0.8, false).on, "off outside the gate");
        // Closed: K_CLOSED frames in a row, so a blink is never cut short.
        for k in 1..K_CLOSED {
            let s = v.step(&open, 0.0, true);
            assert!(!s.on, "closed: on after {K_CLOSED} frames, not {k}");
        }
        let s = v.step(&open, 0.0, true);
        assert!(s.on && s.fired);
        assert!(!v.step(&open, 0.0, true).fired, "fired only on the first vetoed frame");
        // A frame below P_ON switches it off and starts the run again.
        v.reset();
        assert!(!v.advance(0.99, false) && !v.advance(0.99, false) && v.advance(0.99, false));
        assert!(!v.advance(0.97, false) && !v.on && v.run == 0);
    }
}
