//! Extra outputs: up to three more destinations for the eye data next to the main output (config.json's
//! extra_outputs, or --extra), each in one of two formats:
//!
//! - `feo1`: the FrameEyeOSC packets of docs/protocol.md, byte for byte the ones the main output's FrameEyeOSC sends
//!   (the same packet, built once per sample: one session and one sequence for every destination). Without eye data a
//!   packet without TRACKING every FEO_IDLE_INTERVAL, as there.
//! - `osc`: plain OSC messages, one datagram each (or, with `bundle`, each sample's messages in OSC bundles of up to
//!   osc::MAX_BUNDLE bytes: one or two datagrams a sample), the same set for every receiver whatever the main output's
//!   settings:
//!   VRChat's own eye tracking input (/tracking/eye/CenterVec from the combined gaze as sent, always CenterVec, also
//!   with independent_eyes, and EyesClosedAmount), then the main VRChat output's VRCFT v2 float parameters for the
//!   same sample (see osc_set) under the fixed prefix /FT. When the eye data stops, one set for relaxed open eyes
//!   looking ahead (with the squint and brows that went out set back to 0) and then nothing: silence is its "no data".
//!
//! An osc extra's `max_rate` sends at most that many samples a second and skips the rest (its final set always goes).
//!
//! Each extra goes through the address check (dest) like the main output, and one whose address and port are the main
//! output's, the pupils' (LiveLink mode) or an earlier extra's is refused. Pausing (sending false) pauses them too.
//! When what an extra sends or where changes, or it stops, its old destination gets its final message first (while
//! the eye data runs; without it the last message was one already), as the main output's streams do.

use crate::config::{ExtraFormat, ExtraOutput, Settings};
use crate::osc::Value as OscType;
use crate::status::ExtraStatus;
use crate::{
    CameraValues, LID_RELAXED, Output, RATE_WINDOW, Sample, Target, brow_release_at, eyes_closed_amount, gaze_vector,
    squint_release_at, status, v2_params,
};
use std::collections::VecDeque;
use std::net::SocketAddr;
use std::time::{Duration, Instant};

const CENTER_VEC: &str = "/tracking/eye/CenterVec";
const EYES_CLOSED: &str = "/tracking/eye/EyesClosedAmount";
/// The osc format's VRCFT parameters always go under VRCFT's own prefix, not the main output's prefix setting.
const PREFIX: &str = "/FT";

/// What an extra sends and where, while it sends: when this changes, the old destination gets its final message.
#[derive(Clone, PartialEq)]
struct Stream {
    format: ExtraFormat,
    target: Target,
}

/// What an osc extra's receiver holds from the messages that went out: which eyes' squint and whether the brows, to
/// set them back to 0 once they stop (as the main VRChat output does: squint_release, brow_release).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Held {
    pub squint: [bool; 2],
    pub brows: bool,
}

/// What `extra` sends with these settings, if anything: nothing while it is off or can't be used, or sending is paused.
fn stream(settings: &Settings, extra: &ExtraOutput) -> Option<Stream> {
    let format = extra.usable().filter(|_| settings.sending)?;
    Some(Stream { format, target: target(settings, extra) })
}

/// Where `extra` goes: "main" the main output's host (the Steam Link PC with its "auto"), "auto" the Steam Link PC,
/// else the host given; on its own port.
fn target(settings: &Settings, extra: &ExtraOutput) -> Target {
    let port = extra.port;
    match extra.host.as_str() {
        "main" => match Target::of(settings) {
            Target::Fixed { host, .. } => Target::Fixed { host, port },
            Target::SteamLink { .. } => Target::SteamLink { port },
        },
        "auto" => Target::SteamLink { port },
        host => Target::Fixed { host: host.into(), port },
    }
}

/// How an extra is named in the log: its name and format.
fn describe(extra: &ExtraOutput) -> String {
    let format = extra.format.map_or("?", ExtraFormat::name);
    format!("extra output \"{}\" ({format})", extra.name)
}

/// The osc messages for a sample's gaze (left x/y, right x/y, combined x/y), VRCFT eyelids, eye-camera values and
/// brows as sent, in this order:
/// - CenterVec for the combined gaze (x, y in -1..1, 1 = 45°; see gaze_vector), and EyesClosedAmount for the eyelids;
/// - the main VRChat output's VRCFT v2 float parameters for them (v2_params: the gaze per eye and combined, the
///   eyelids, the squint and pupils for the eyes they are used for, the brows with brow_from_widen) under
///   /avatar/parameters/FT/v2/, never the main output's EyeTrackingActive, pupil bits or Steam Link names;
/// - 0 for the squint and brows that went out before (`held`) and stop with these (squint_release, brow_release).
///
/// `held` then holds what went out now.
pub fn osc_set(
    gaze: [f32; 6],
    lids: [f32; 2],
    camera: &CameraValues,
    brows: Option<[f32; 2]>,
    held: &mut Held,
) -> Vec<(String, Vec<OscType>)> {
    let floats = |values: &[f32]| -> Vec<OscType> { values.iter().copied().map(OscType::Float).collect() };
    let mut messages = vec![
        (CENTER_VEC.to_owned(), floats(&gaze_vector([gaze[4], gaze[5]]))),
        (EYES_CLOSED.to_owned(), floats(&[eyes_closed_amount(lids)])),
    ];
    let v2 = v2_params(gaze, lids, camera, brows).into_iter();
    messages.extend(v2.map(|(name, value)| (format!("/avatar/parameters{PREFIX}/v2/{name}"), floats(&[value]))));
    let released = squint_release_at(PREFIX, held.squint, camera.squint).into_iter();
    let released = released.chain(brow_release_at(PREFIX, held.brows, brows));
    messages.extend(released.map(|(addr, value)| (addr, vec![value])));
    *held = Held { squint: camera.squint.map(|squint| squint.is_some()), brows: brows.is_some() };
    messages
}

/// The osc set for one sample.
pub fn osc_sample(sample: &Sample, held: &mut Held) -> Vec<(String, Vec<OscType>)> {
    osc_set(sample.gaze, sample.lids, &sample.camera, sample.brows, held)
}

/// The osc format's final set: relaxed open eyes looking straight ahead, without squint, pupils or brows (CenterVec
/// 0, 0, 1, EyesClosedAmount 0, the gaze 0 and the eyelids 0.75), and 0 for the squint and brows that went out. The
/// pupils are left as they were, as the main VRChat output leaves them.
pub fn osc_final(held: &mut Held) -> Vec<(String, Vec<OscType>)> {
    osc_set([0.0; 6], [LID_RELAXED; 2], &CameraValues::default(), None, held)
}

/// One extra output: its entry, its sender, and what it sends now.
struct Extra {
    config: ExtraOutput,
    output: Output,
    stream: Option<Stream>,
    // Why it is refused although its address is allowed: "same as main", "same as the pupils" or "duplicate"
    // (see Extras::refresh).
    duplicate: Option<&'static str>,
    // When its samples of the last RATE_WINDOW went out.
    sent: VecDeque<Instant>,
    // osc: what its receiver holds from it.
    held: Held,
    // osc with max_rate: when its next sample is due.
    next_due: Option<Instant>,
}

impl Extra {
    /// Whether what it gets goes out: it is on and not refused as a duplicate (its address may still be unknown, or
    /// refused by the address check, and then nothing goes out).
    fn sending(&self) -> bool {
        self.stream.is_some() && self.duplicate.is_none()
    }

    /// Its final message, where it sends now: `feo` (a packet without TRACKING) for feo1, osc_final for osc.
    fn send_final(&mut self, feo: &[u8]) {
        let Some(stream) = self.stream.as_ref().filter(|_| self.duplicate.is_none()) else {
            return;
        };
        match stream.format {
            ExtraFormat::Feo1 => self.output.send_datagram(feo),
            ExtraFormat::Osc => {
                let messages = osc_final(&mut self.held);
                self.send_osc(&messages);
            }
        }
    }

    /// osc messages, one datagram each or (`bundle`) in bundles.
    fn send_osc(&mut self, messages: &[(String, Vec<OscType>)]) {
        if self.config.bundle {
            for bundle in crate::osc::bundles(messages, crate::osc::MAX_BUNDLE) {
                self.output.send_datagram(&bundle);
            }
        } else {
            for (addr, args) in messages {
                // (never an error: see Output::send)
                let _ = self.output.send(addr, args);
            }
        }
    }

    /// Whether the sample at `now` goes out under max_rate (always without one), and if so when the next is due.
    fn due(&mut self, now: Instant) -> bool {
        let Some(rate) = self.config.max_rate else {
            return true;
        };
        let period = Duration::from_secs_f32(1.0 / rate);
        // A quarter period early still counts, so 90 samples a second capped at 45 send every other one even with
        // the samples' jitter
        if self.next_due.is_some_and(|due| now + period / 4 < due) {
            return false;
        }
        // Keep the cadence while on time; after a gap start again from now
        let from = self.next_due.filter(|due| now < *due + period).unwrap_or(now);
        self.next_due = Some(from + period);
        true
    }
}

/// Every extra output, in the order of extra_outputs.
pub struct Extras {
    list: Vec<Extra>,
}

impl Extras {
    pub fn new(settings: &Settings) -> Self {
        let mut extras = Self { list: Vec::new() };
        extras.apply(settings, false, &mut Vec::new);
        extras
    }

    /// Take changed settings. While the eye data runs (`tracking`), an extra whose stream stops or changes first gets
    /// its final message where it went; `feo_idle` makes the packet without TRACKING for feo1 (once, if needed).
    pub fn apply(&mut self, settings: &Settings, tracking: bool, feo_idle: &mut dyn FnMut() -> Vec<u8>) {
        let mut idle: Option<Vec<u8>> = None;
        let mut idle_packet = |format: ExtraFormat| match format {
            ExtraFormat::Feo1 => idle.get_or_insert_with(&mut *feo_idle).clone(),
            ExtraFormat::Osc => Vec::new(),
        };
        let empty = ExtraOutput {
            name: String::new(),
            enabled: false,
            format: None,
            host: String::new(),
            port: 0,
            bundle: false,
            max_rate: None,
            error: None,
        };
        for index in 0..self.list.len().max(settings.extra_outputs.len()) {
            let config = settings.extra_outputs.get(index).unwrap_or(&empty);
            let new = stream(settings, config);
            if let Some(extra) = self.list.get_mut(index) {
                if extra.stream == new && extra.config == *config {
                    extra.output.set_lan_only(settings.lan_only);
                    continue;
                }
                if let Some(old) = extra.stream.as_ref().filter(|old| Some(*old) != new.as_ref()) {
                    if tracking {
                        let packet = idle_packet(old.format);
                        extra.send_final(&packet);
                    }
                    let to = extra.output.addr().map_or_else(|| "nowhere yet".into(), |addr| addr.to_string());
                    eprintln!("Stopped sending to {to} for {}", describe(&extra.config));
                }
            } else {
                self.list.push(Extra {
                    config: config.clone(),
                    output: Output::new(target(settings, config)),
                    stream: None,
                    duplicate: None,
                    sent: VecDeque::new(),
                    held: Held::default(),
                    next_due: None,
                });
            }
            let extra = &mut self.list[index];
            let starts = new.is_some() && extra.stream != new;
            if extra.stream != new {
                // (its old receiver got its final message, or had it already when the eye data stopped)
                extra.held = Held::default();
                extra.next_due = None;
            }
            extra.config = config.clone();
            extra.output.set_target(target(settings, config));
            extra.output.set_lan_only(settings.lan_only);
            extra.output.label = format!(" for {}", describe(config));
            extra.stream = new;
            if starts {
                let host = match config.host.as_str() {
                    "main" => "the main output's host",
                    "auto" => "the Steam Link PC",
                    host => host,
                };
                eprintln!("Starting {} to {host}, port {}", describe(config), config.port);
            }
        }
        self.list.truncate(settings.extra_outputs.len());
    }

    /// Look each extra that sends up again when due, and refuse one whose address and port are already taken:
    /// `taken` the main output's and the pupils' (with their reasons, "same as main" and "same as the pupils"), then each earlier extra's
    /// ("duplicate").
    pub fn refresh(&mut self, taken: &[(SocketAddr, &'static str)]) {
        let mut taken = taken.to_vec();
        for extra in &mut self.list {
            if extra.stream.is_none() {
                extra.duplicate = None;
                continue;
            }
            extra.output.refresh();
            let addr = extra.output.addr();
            let duplicate = addr.and_then(|addr| taken.iter().find(|(at, _)| *at == addr).map(|(_, why)| *why));
            if duplicate != extra.duplicate {
                let at = addr.map_or_else(String::new, |addr| addr.to_string());
                match duplicate {
                    Some(why) => status::log_error(&format!("Not sending to {at}{}: {why}", extra.output.label)),
                    None => eprintln!("Sending to {at}{} again: no longer a duplicate", extra.output.label),
                }
                extra.duplicate = duplicate;
            }
            if let (Some(addr), None) = (addr, duplicate) {
                taken.push((addr, "duplicate"));
            }
        }
    }

    /// Whether any extra sends feo1 now (so each sample's FrameEyeOSC packet is built even when the main output is not
    /// FrameEyeOSC).
    pub fn wants_feo(&self) -> bool {
        self.list.iter().any(|extra| extra.sending() && extra.config.format == Some(ExtraFormat::Feo1))
    }

    /// Send one sample: `feo` (this sample's FrameEyeOSC packet, if built) to feo1, and the sample's osc set to osc.
    pub fn send_sample(&mut self, now: Instant, feo: Option<&[u8]>, sample: &Sample) {
        for extra in self.list.iter_mut().filter(|extra| extra.sending()) {
            let Some(stream) = &extra.stream else { continue };
            match stream.format {
                ExtraFormat::Feo1 => match feo {
                    Some(packet) => extra.output.send_datagram(packet),
                    None => continue,
                },
                ExtraFormat::Osc => {
                    if extra.due(now) {
                        let messages = osc_sample(sample, &mut extra.held);
                        extra.send_osc(&messages);
                    } else {
                        continue;
                    }
                }
            }
            if extra.output.addr().is_some() {
                extra.sent.push_back(now);
            }
            while extra.sent.front().is_some_and(|time| now.duration_since(*time) >= RATE_WINDOW) {
                extra.sent.pop_front();
            }
        }
    }

    /// The eye data stopped: each extra's final message (`feo` the packet without TRACKING, built if wants_feo).
    pub fn send_lost(&mut self, feo: Option<&[u8]>) {
        for extra in &mut self.list {
            extra.send_final(feo.unwrap_or_default());
        }
    }

    /// While there is no eye data: the packet without TRACKING to each feo1 extra (osc stays silent).
    pub fn send_feo_idle(&mut self, packet: &[u8]) {
        for extra in self.list.iter_mut().filter(|extra| extra.sending()) {
            if extra.config.format == Some(ExtraFormat::Feo1) {
                extra.output.send_datagram(packet);
            }
        }
    }

    /// status.json's `extras`: one per extra_outputs entry, in order.
    pub fn status(&self, now: Instant) -> Vec<ExtraStatus> {
        self.list
            .iter()
            .enumerate()
            .map(|(index, extra)| {
                let config = &extra.config;
                // (not while its address is refused either)
                let sending = extra.sending() && extra.output.refused.is_none();
                let refused = config.error.clone().or(extra.duplicate.map(Into::into)).or_else(|| {
                    extra.stream.as_ref().and(extra.output.refused).map(Into::into)
                });
                ExtraStatus {
                    index: index + 1,
                    name: config.name.clone(),
                    enabled: config.enabled,
                    format: config.format,
                    host: config.host.clone(),
                    port: config.port,
                    sending,
                    target: extra.output.addr().filter(|_| sending).map(|addr| addr.to_string()),
                    net: extra.output.net().filter(|_| sending),
                    rate: extra.sent.iter().filter(|time| now.duration_since(**time) < RATE_WINDOW).count() as f32,
                    dropped_rate: extra.output.drops.per_second(now),
                    refused,
                    last_error: extra.output.problem().filter(|_| sending),
                }
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn floats(args: &[OscType]) -> Vec<f32> {
        args.iter()
            .map(|arg| match arg {
                OscType::Float(value) => *value,
                other => panic!("not a float: {other:?}"),
            })
            .collect()
    }

    /// The osc set for a gaze (left x/y, right x/y, combined x/y) and eyelids, without camera values or brows.
    fn plain(gaze: [f32; 6], lids: [f32; 2]) -> Vec<(String, Vec<OscType>)> {
        osc_set(gaze, lids, &CameraValues::default(), None, &mut Held::default())
    }

    fn addresses(messages: &[(String, Vec<OscType>)]) -> Vec<&str> {
        messages.iter().map(|(addr, _)| addr.as_str()).collect()
    }

    #[test]
    fn center_vec_points_where_the_gaze_does() {
        // (the combined gaze; each eye's own never moves it)
        let center = |[x, y]: [f32; 2]| floats(&plain([0.9, -0.9, -0.9, 0.9, x, y], [0.75; 2])[0].1);
        let close = |a: Vec<f32>, b: [f32; 3]| a.iter().zip(b).all(|(a, b)| (a - b).abs() < 1e-6);
        // Unity axes: +x right, +y up, +z forward; 1 = 45°
        assert_eq!(center([0.0, 0.0]), [0.0, 0.0, 1.0]);
        let half = std::f32::consts::FRAC_1_SQRT_2;
        assert!(close(center([1.0, 0.0]), [half, 0.0, half]));
        assert!(close(center([-1.0, 0.0]), [-half, 0.0, half]));
        assert!(close(center([0.0, 1.0]), [0.0, half, half]));
        assert!(close(center([0.0, -1.0]), [0.0, -half, half]));
        // 22.5° right and down: tan(22.5°) on each axis before normalising
        let t = (std::f32::consts::PI / 8.0).tan();
        let length = (2.0 * t * t + 1.0).sqrt();
        assert!(close(center([0.5, -0.5]), [t / length, -t / length, 1.0 / length]));
        // Always a unit vector
        for gaze in [[0.3, 0.7], [-1.0, 1.0], [0.01, -0.99]] {
            let v = center(gaze);
            assert!((v.iter().map(|x| x * x).sum::<f32>() - 1.0).abs() < 1e-6, "{gaze:?}");
        }
    }

    #[test]
    fn every_sample_has_the_eyes_and_the_fixed_vrcft_names() {
        let messages = plain([0.1, 0.2, 0.3, 0.4, 0.2, 0.3], [0.375, 0.5]);
        let v2 = |name: &str| format!("/avatar/parameters/FT/v2/{name}");
        let expected = [CENTER_VEC.to_owned(), EYES_CLOSED.to_owned()].into_iter().chain(
            ["EyeLeftX", "EyeLeftY", "EyeRightX", "EyeRightY", "EyeLidLeft", "EyeLidRight", "EyeX", "EyeY"].map(v2),
        );
        assert_eq!(addresses(&messages), expected.collect::<Vec<_>>());
        // One float each (CenterVec three), as sent
        let values: Vec<Vec<f32>> = messages.iter().map(|(_, args)| floats(args)).collect();
        assert_eq!(values[2..], [[0.1], [0.2], [0.3], [0.4], [0.375], [0.5], [0.2], [0.3]]);
        assert_eq!(values[1], [1.0 - 0.4375 / 0.75]);
        // EyesClosedAmount: 0 open (relaxed or widened) to 1 shut, both eyes as one
        let closed = |lids| floats(&plain([0.0; 6], lids)[1].1)[0];
        assert_eq!(closed([0.75, 0.75]), 0.0);
        assert_eq!(closed([1.0, 1.0]), 0.0);
        assert_eq!(closed([0.0, 0.0]), 1.0);
        assert_eq!(closed([0.0, 0.75]), 0.5);
    }

    #[test]
    fn squint_and_brows_go_back_to_0_once_they_stop_and_the_final_set_is_relaxed_eyes_ahead() {
        let camera = CameraValues { squint: [Some(0.4), Some(0.2)], ..CameraValues::default() };
        let mut held = Held::default();
        let with = osc_set([0.0; 6], [0.75; 2], &camera, Some([0.5, 0.3]), &mut held);
        assert_eq!(held, Held { squint: [true, true], brows: true });
        assert!(addresses(&with).contains(&"/avatar/parameters/FT/v2/EyeSquint"));
        assert!(addresses(&with).contains(&"/avatar/parameters/FT/v2/BrowUp"));
        // The left eye's squint stops: 0 for it, not yet for the combined one; the brows stop: 0 for all of them
        let left_gone = CameraValues { squint: [None, Some(0.2)], ..CameraValues::default() };
        let next = osc_set([0.0; 6], [0.75; 2], &left_gone, None, &mut held);
        let zeros: Vec<&str> = next[10..]
            .iter()
            .filter(|(_, args)| args == &[OscType::Float(0.0)])
            .map(|(addr, _)| addr.as_str())
            .collect();
        assert!(zeros.contains(&"/avatar/parameters/FT/v2/EyeSquintLeft"));
        assert!(!zeros.contains(&"/avatar/parameters/FT/v2/EyeSquint"));
        assert!(zeros.contains(&"/avatar/parameters/FT/v2/BrowExpression"));
        assert_eq!(held, Held { squint: [false, true], brows: false });
        // The final set: relaxed open eyes looking ahead, and 0 for the squint still out; then nothing more to release
        let last = osc_final(&mut held);
        let ahead = [0.0, 0.0, 1.0].map(OscType::Float).to_vec();
        let mut expected = vec![(CENTER_VEC.to_owned(), ahead), (EYES_CLOSED.to_owned(), vec![OscType::Float(0.0)])];
        for (name, value) in [("EyeLeftX", 0.0), ("EyeLeftY", 0.0), ("EyeRightX", 0.0), ("EyeRightY", 0.0)]
            .into_iter()
            .chain([("EyeLidLeft", 0.75), ("EyeLidRight", 0.75), ("EyeX", 0.0), ("EyeY", 0.0)])
            .chain([("EyeSquintRight", 0.0), ("EyeSquint", 0.0)])
        {
            expected.push((format!("/avatar/parameters/FT/v2/{name}"), vec![OscType::Float(value)]));
        }
        assert_eq!(last, expected);
        assert_eq!(held, Held::default());
        assert_eq!(osc_final(&mut held), expected[..10]);
    }

    #[test]
    fn max_rate_keeps_every_other_sample_of_90_at_45_with_jitter_and_restarts_after_a_gap() {
        let config = ExtraOutput {
            name: "x".into(),
            enabled: true,
            format: Some(ExtraFormat::Osc),
            host: "127.0.0.1".into(),
            port: 9000,
            bundle: true,
            max_rate: Some(45.0),
            error: None,
        };
        let mut extra = Extra {
            output: Output::new(Target::Fixed { host: "127.0.0.1".into(), port: 9000 }),
            config,
            stream: None,
            duplicate: None,
            sent: VecDeque::new(),
            held: Held::default(),
            next_due: None,
        };
        let start = Instant::now();
        // 90 samples a second for 10 s, each up to 2 ms early or late
        let jitter = [0.0, 1.5, -2.0, 0.5, 2.0, -1.0, -0.5, 1.0];
        let times = (0..900).map(|n| start + Duration::from_secs_f64(1.0 + n as f64 / 90.0 + jitter[n % 8] / 1000.0));
        let sent = times.clone().filter(|time| extra.due(*time)).count();
        assert_eq!(sent, 450);
        // After a second without samples the next one goes, then every other one again
        let later = start + Duration::from_secs(12);
        let after: Vec<bool> = (0..6).map(|n| extra.due(later + Duration::from_secs_f64(n as f64 / 90.0))).collect();
        assert_eq!(after, [true, false, true, false, true, false]);
        // Without max_rate every sample goes
        extra.config.max_rate = None;
        assert!(times.map(|time| extra.due(time)).all(|due| due));
    }

    #[test]
    fn main_follows_the_main_outputs_host_and_auto_the_steam_link_pc() {
        let extra = |host: &str| ExtraOutput {
            name: "x".into(),
            enabled: true,
            format: Some(ExtraFormat::Osc),
            host: host.into(),
            port: 9015,
            bundle: false,
            max_rate: None,
            error: None,
        };
        let fixed = Settings { host: "192.168.0.10".into(), ..Settings::default() };
        let auto = Settings::default();
        let at = |host: &str| Target::Fixed { host: host.into(), port: 9015 };
        assert!(target(&fixed, &extra("main")) == at("192.168.0.10"));
        assert!(target(&auto, &extra("main")) == Target::SteamLink { port: 9015 });
        assert!(target(&fixed, &extra("auto")) == Target::SteamLink { port: 9015 });
        assert!(target(&auto, &extra("10.0.0.2")) == at("10.0.0.2"));
        // Nothing while it is off, can't be used, or sending is paused
        assert!(stream(&fixed, &extra("main")).is_some());
        assert!(stream(&fixed, &ExtraOutput { enabled: false, ..extra("main") }).is_none());
        assert!(stream(&fixed, &ExtraOutput { error: Some("bad".into()), ..extra("main") }).is_none());
        assert!(stream(&Settings { sending: false, ..fixed.clone() }, &extra("main")).is_none());
    }
}
