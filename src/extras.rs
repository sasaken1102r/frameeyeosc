//! Extra outputs: up to three more destinations for the eye data next to the main output (config.json's
//! extra_outputs, or --extra), each in one of two formats:
//!
//! - `feo1`: the FrameEyeOSC packets of docs/protocol.md, byte for byte the ones the main output's FrameEyeOSC sends
//!   (the same packet, built once per sample: one session and one sequence for every destination). Without eye data a
//!   packet without TRACKING every FEO_IDLE_INTERVAL, as there.
//! - `tracking_eye`: VRChat's own eye tracking input, plain OSC messages (no bundles): /tracking/eye/CenterVec from the
//!   combined gaze as sent (always CenterVec, also with independent_eyes), and with `lids` EyesClosedAmount. When the
//!   eye data stops, one CenterVec 0,0,1 (and EyesClosedAmount 0) and then nothing: silence is its "no data".
//!
//! Each extra goes through the address check (dest) like the main output, and one whose address and port are the main
//! output's, the pupils' (LiveLink mode) or an earlier extra's is refused. Pausing (sending false) pauses them too.
//! When what an extra sends or where changes, or it stops, its old destination gets its final message first (while
//! the eye data runs; without it the last message was one already), as the main output's streams do.

use crate::config::{ExtraFormat, ExtraOutput, Settings};
use crate::osc::Value as OscType;
use crate::status::ExtraStatus;
use crate::{Output, RATE_WINDOW, Target, eyes_closed_amount, gaze_vector, status};
use std::collections::VecDeque;
use std::net::SocketAddr;
use std::time::Instant;

const CENTER_VEC: &str = "/tracking/eye/CenterVec";
const EYES_CLOSED: &str = "/tracking/eye/EyesClosedAmount";

/// What an extra sends and where, while it sends: when this changes, the old destination gets its final message.
#[derive(Clone, PartialEq)]
struct Stream {
    format: ExtraFormat,
    target: Target,
    lids: bool,
}

/// What `extra` sends with these settings, if anything: nothing while it is off or can't be used, or sending is paused.
fn stream(settings: &Settings, extra: &ExtraOutput) -> Option<Stream> {
    let format = extra.usable().filter(|_| settings.sending)?;
    Some(Stream {
        format,
        target: target(settings, extra),
        lids: extra.lids && format == ExtraFormat::TrackingEye,
    })
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

/// How an extra is named in the log: its name, format and whether it carries the eyelids.
fn describe(extra: &ExtraOutput) -> String {
    let format = extra.format.map_or("?", ExtraFormat::name);
    let lids = if extra.lids { ", lids" } else { "" };
    format!("extra output \"{}\" ({format}{lids})", extra.name)
}

/// The tracking_eye messages for a sample: CenterVec for the combined gaze as sent (x, y in -1..1, 1 = 45°; see
/// gaze_vector), and EyesClosedAmount for the VRCFT eyelids as sent when `lids` has them.
pub fn tracking_eye_messages(gaze: [f32; 2], lids: Option<[f32; 2]>) -> Vec<(&'static str, Vec<OscType>)> {
    let mut messages = vec![(CENTER_VEC, gaze_vector(gaze).map(OscType::Float).to_vec())];
    messages.extend(lids.map(|lids| (EYES_CLOSED, vec![OscType::Float(eyes_closed_amount(lids))])));
    messages
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
}

impl Extra {
    /// Whether what it gets goes out: it is on and not refused as a duplicate (its address may still be unknown, or
    /// refused by the address check, and then nothing goes out).
    fn sending(&self) -> bool {
        self.stream.is_some() && self.duplicate.is_none()
    }

    /// Its final message, where it sends now: `feo` (a packet without TRACKING) for feo1, relaxed open eyes looking
    /// ahead for tracking_eye.
    fn send_final(&mut self, feo: &[u8]) {
        let Some(stream) = self.stream.as_ref().filter(|_| self.duplicate.is_none()) else {
            return;
        };
        match stream.format {
            ExtraFormat::Feo1 => self.output.send_datagram(feo),
            ExtraFormat::TrackingEye => {
                let lids = stream.lids.then_some([crate::LID_RELAXED; 2]);
                for (addr, args) in tracking_eye_messages([0.0; 2], lids) {
                    // (never an error: see Output::send)
                    let _ = self.output.send(addr, &args);
                }
            }
        }
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
            ExtraFormat::TrackingEye => Vec::new(),
        };
        let empty = ExtraOutput {
            name: String::new(),
            enabled: false,
            format: None,
            host: String::new(),
            port: 0,
            lids: false,
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
                });
            }
            let extra = &mut self.list[index];
            let starts = new.is_some() && extra.stream != new;
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

    /// Send one sample: `feo` (this sample's FrameEyeOSC packet, if built) to feo1, and the combined gaze and VRCFT
    /// eyelids as sent to tracking_eye.
    pub fn send_sample(&mut self, now: Instant, feo: Option<&[u8]>, gaze: [f32; 2], lids: [f32; 2]) {
        for extra in self.list.iter_mut().filter(|extra| extra.sending()) {
            let Some(stream) = &extra.stream else { continue };
            match stream.format {
                ExtraFormat::Feo1 => match feo {
                    Some(packet) => extra.output.send_datagram(packet),
                    None => continue,
                },
                ExtraFormat::TrackingEye => {
                    for (addr, args) in tracking_eye_messages(gaze, stream.lids.then_some(lids)) {
                        let _ = extra.output.send(addr, &args);
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

    /// While there is no eye data: the packet without TRACKING to each feo1 extra (tracking_eye stays silent).
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
                    lids: config.lids,
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

    #[test]
    fn center_vec_points_where_the_gaze_does() {
        let center = |gaze: [f32; 2]| floats(&tracking_eye_messages(gaze, None)[0].1);
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
    fn eyelids_go_out_only_with_lids() {
        let messages = tracking_eye_messages([0.1, 0.2], None);
        assert_eq!(messages.iter().map(|(addr, _)| *addr).collect::<Vec<_>>(), [CENTER_VEC]);
        let messages = tracking_eye_messages([0.1, 0.2], Some([0.375, 0.375]));
        assert_eq!(messages.iter().map(|(addr, _)| *addr).collect::<Vec<_>>(), [CENTER_VEC, EYES_CLOSED]);
        assert_eq!(floats(&messages[1].1), [0.5]);
        // 0 open (relaxed or widened) to 1 shut, both eyes as one
        let closed = |lids| floats(&tracking_eye_messages([0.0; 2], Some(lids))[1].1)[0];
        assert_eq!(closed([0.75, 0.75]), 0.0);
        assert_eq!(closed([1.0, 1.0]), 0.0);
        assert_eq!(closed([0.0, 0.0]), 1.0);
        assert_eq!(closed([0.0, 0.75]), 0.5);
    }

    #[test]
    fn main_follows_the_main_outputs_host_and_auto_the_steam_link_pc() {
        let extra = |host: &str| ExtraOutput {
            name: "x".into(),
            enabled: true,
            format: Some(ExtraFormat::TrackingEye),
            host: host.into(),
            port: 9015,
            lids: false,
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
