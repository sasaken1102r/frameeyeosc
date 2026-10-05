//! What `eyecam-rec --serve` publishes for the VR panel (status.json) and the control commands it accepts on
//! ctl.sock. The README documents both for the panel; keep them in sync.

pub const DEFAULT_DIR: &str = "/run/user/1000/eyecam";
pub const STATUS_FILE: &str = "status.json";
pub const CTL_FILE: &str = "ctl.sock";
pub const DEFAULT_PROTOCOL: &str = "widen";

/// Why the eye cameras are not locked while they are searched for (status.json `search`).
pub const SEARCH_NOT_WORN: &str = "not_worn";
pub const SEARCH_NO_VIDEO: &str = "no_video";
pub const SEARCH_ONE_EYE: &str = "one_eye";

/// The message that goes with a `search` reason ("" for none: the headset to put on, as before a search).
pub fn search_message(search: &str) -> &'static str {
    match search {
        SEARCH_NO_VIDEO => "目の映像がまだ流れていない（SteamVR の視線トラッキングはオン？）",
        SEARCH_ONE_EYE => "片目しか映っていない（ちゃんとかぶれてる？）",
        _ => "HMD をかぶってね（目の映像を待ってるよ）",
    }
}

/// The last look for the eye video (status.json `search_detail`), kept after a lock: what `ring::discover` saw, and
/// how much of the buffers had changed before it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SearchDetail {
    pub look: crate::ring::Look,
    /// 64 KiB blocks of the camera buffers that changed since the look before (0: nothing is being written).
    pub changed_blocks: usize,
    /// When it looked (Unix seconds).
    pub unix: f64,
}

impl SearchDetail {
    fn to_json(&self) -> String {
        format!(
            "{{\"candidates\":{},\"refresh_hz\":{},\"slots\":{},\"both_eyes\":{},\"stopped_at\":{},\"changed_blocks\":{},\"unix\":{}}}",
            self.look.candidates,
            num(self.look.refresh_hz),
            self.look.slots,
            self.look.both_eyes,
            json_str(self.look.stopped_at),
            self.changed_blocks,
            num(self.unix),
        )
    }
}

/// The last `calib wear` (status.json `last_calib`), read back from its calib_result.json: when, how it ended, and per
/// eye (left, right) how the pupil search went in the normal steps. Numbers it doesn't have are NaN (null).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct LastCalib {
    /// Local time, as calib_result.json has it ("2026-10-05 19:51:03").
    pub time: String,
    pub ok: bool,
    /// "L" / "R": it went through without that eye (it kept its earlier values); "LR": it failed; "".
    pub failed_eye: String,
    /// What eyecam-rec said (Japanese; message_en in the JSON).
    pub message: String,
    /// Normal-step frames with the pupil found, and normal-step frames.
    pub pupil_frames: [f64; 2],
    pub normal_frames: [f64; 2],
    /// The median pupil position in the normal frames (px in the 400x400 frame).
    pub pupil_x: [f64; 2],
    pub pupil_y: [f64; 2],
    /// The pupil search window's left and right edge (the medians over the calibration).
    pub window: [[f64; 2]; 2],
}

impl LastCalib {
    /// Read a calib_result.json's text: None if it isn't a wear calibration's.
    pub fn from_result(text: &str) -> Option<Self> {
        use crate::json::Json;
        let j = crate::json::parse(text).ok()?;
        if j.get("kind").and_then(Json::str) != Some("wear") {
            return None;
        }
        let ok = matches!(j.get("ok"), Some(Json::Bool(true)));
        let mut c = LastCalib {
            time: j.get("time").and_then(Json::str).unwrap_or("").to_string(),
            ok,
            // (files before failed_eye: only whether it failed is known)
            failed_eye: j.get("failed_eye").and_then(Json::str).unwrap_or(if ok { "" } else { "LR" }).to_string(),
            message: j.get("message").and_then(Json::str).unwrap_or("").to_string(),
            pupil_frames: [f64::NAN; 2],
            normal_frames: [f64::NAN; 2],
            pupil_x: [f64::NAN; 2],
            pupil_y: [f64::NAN; 2],
            window: [[f64::NAN; 2]; 2],
        };
        for (e, name) in ["L", "R"].iter().enumerate() {
            let Some(v) = j.get("values").and_then(|v| v.get(name)) else { continue };
            let n = |x: Option<&Json>| x.and_then(Json::num).unwrap_or(f64::NAN);
            let diag = v.get("diag");
            c.pupil_frames[e] = n(v.get("normal_frames_with_pupil"));
            c.normal_frames[e] = n(diag.and_then(|d| d.get("steps")).and_then(|s| s.get("normal")).and_then(|s| s.get("frames")));
            c.pupil_x[e] = n(diag.and_then(|d| d.get("normal_pupil_x")));
            c.pupil_y[e] = n(diag.and_then(|d| d.get("normal_pupil_y")));
            c.window[e] = [n(diag.and_then(|d| d.get("search_x_min"))), n(diag.and_then(|d| d.get("search_xmax")))];
        }
        Some(c)
    }

    /// The newest wear calibration's under `dir` (`calib_*/calib_result.json`, by name: they are dated).
    pub fn newest_in(dir: &std::path::Path) -> Option<Self> {
        let mut names: Vec<String> = std::fs::read_dir(dir)
            .ok()?
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.starts_with("calib_"))
            .collect();
        names.sort();
        names
            .iter()
            .rev()
            .find_map(|n| Self::from_result(&std::fs::read_to_string(dir.join(n).join("calib_result.json")).ok()?))
    }

    fn to_json(&self) -> String {
        let pair = |v: [f64; 2]| format!("[{},{}]", num_or_null(v[0]), num_or_null(v[1]));
        format!(
            "{{\"time\":{},\"ok\":{},\"failed_eye\":{},\"message\":{},\"message_en\":{},\"pupil_frames\":{},\"normal_frames\":{},\"pupil_x\":{},\"pupil_y\":{},\"window\":[{},{}]}}",
            json_str(&self.time),
            self.ok,
            json_str(&self.failed_eye),
            json_str(&self.message),
            json_str(&crate::message_en::message_en(&self.message)),
            pair(self.pupil_frames),
            pair(self.normal_frames),
            pair(self.pupil_x),
            pair(self.pupil_y),
            pair(self.window[0]),
            pair(self.window[1]),
        )
    }
}

/// One snapshot of the daemon's state. Numbers that do not apply are -1 (indices, prox) or 0 (times, counts).
#[derive(Clone, Debug)]
pub struct Status {
    /// waiting_fds | idle | searching | recording | calibrating | error | stopped (written once on a clean exit)
    pub state: &'static str,
    pub message: String,
    /// eyecam-rec holds the camera buffers (eyecam-grab succeeded), whether or not frames are flowing.
    pub has_buffers: bool,
    /// The automatic eyecam-grab: "" (not tried), missing, no_cap, unsafe: ..., waiting_tracker, trying, ok,
    /// failed: ...
    pub auto_grab: String,
    /// The installed eyecam-grab differs from the one shipped beside eyecam-rec (an update brought a new one):
    /// install_grab.sh has to be run again. False when either is missing.
    pub grab_outdated: bool,
    pub locked: bool,
    pub fps: [f64; 2],
    pub step_index: i64,
    pub step_count: usize,
    pub step_label: String,
    pub step_remaining_s: f64,
    pub elapsed_s: f64,
    pub total_s: f64,
    pub session_dir: String,
    pub protocol: String,
    pub prox: f64,
    /// While searching and not locked, why not: SEARCH_NOT_WORN (the proximity sensor says the headset is off,
    /// Valve's eye tracker delivers no samples, and nothing was found), SEARCH_NO_VIDEO (worn by the sensor, or the
    /// sensor unknown, or Valve's eye tracker delivering samples, but no eye video in the buffers), SEARCH_ONE_EYE
    /// (only one camera's video). "" when locked, not searching, or before the first look.
    pub search: &'static str,
    /// Whether the last recording ended early (stop, Ctrl-C, frames gone, ...); its meta.txt says aborted=1.
    pub last_session_aborted: bool,
    /// Bits (as in the live shared memory): 1 wear calibrated this wear, 2 user calibrated, 4 auto baseline ready
    /// (EyeWide works without a calibration).
    pub calib_state: u32,
    pub recalib_suggested: bool,
    /// The auto baseline: "warming" (EyeWide still 0) or "ready".
    pub baseline: &'static str,
    /// Seconds of usable frames (eye open, looking about straight) still needed; 0 when ready.
    pub warmup_remaining_s: f64,
    /// calib.json holds at least one successful wear calibration (the widen step history).
    pub calib_saved: bool,
    /// 0 (strictest) .. 1 (most sensitive), `set widen_sensitivity`.
    pub widen_sensitivity: f64,
    /// Developer mode (`"dev": true` in settings.json, edited by hand): calibrations also keep their eye images.
    pub dev: bool,
    /// A `calib wear` has succeeded at least once (kept in calib.json): the panel's first-time setup is done.
    pub setup_done: bool,
    /// The last `calib wear` of this run: "measured" (widen caught), "default" (not caught: the usual step), or "".
    pub last_calib_widen: &'static str,
    /// The eye whose part of the last `calib wear` failed: "L" / "R" (it went through with the other eye; the failed
    /// one kept its earlier levels), "LR" (both: the calibration failed), or "".
    pub calib_failed_eye: &'static str,
    /// Live processing is on (`live on`, the default).
    pub live: bool,
    /// Live processing time per frame (one eye), ms, averaged over the last second.
    pub live_ms: f64,
    /// Per eye, the share of the last 2 s of frames in which the pupil was found (as the calibration counts them);
    /// NaN (null in the JSON) when live processing is off or the eye's frames stopped.
    pub pupil: [f64; 2],
    /// --prox-min: the proximity reading above which the headset counts as worn.
    pub prox_min: f64,
    /// The last look for the eye video (None before the first).
    pub search_detail: Option<SearchDetail>,
    /// The last `calib wear` (this run's, else the newest saved one; None if there is none).
    pub last_calib: Option<LastCalib>,
    /// The last `error` message (kept after the state moves on; "" for none) and when it came (Unix seconds).
    pub last_error: String,
    pub last_error_unix: f64,
}

impl Default for Status {
    fn default() -> Self {
        Self {
            state: "waiting_fds",
            message: String::new(),
            has_buffers: false,
            auto_grab: String::new(),
            grab_outdated: false,
            locked: false,
            fps: [0.0; 2],
            step_index: -1,
            step_count: 0,
            step_label: String::new(),
            step_remaining_s: 0.0,
            elapsed_s: 0.0,
            total_s: 0.0,
            session_dir: String::new(),
            protocol: String::new(),
            prox: -1.0,
            search: "",
            last_session_aborted: false,
            calib_state: 0,
            recalib_suggested: false,
            baseline: "warming",
            warmup_remaining_s: crate::live::WARMUP_S,
            calib_saved: false,
            widen_sensitivity: crate::live::DEFAULT_WIDEN_SENSITIVITY,
            dev: false,
            setup_done: false,
            last_calib_widen: "",
            calib_failed_eye: "",
            live: false,
            live_ms: 0.0,
            pupil: [f64::NAN; 2],
            prox_min: 20.0,
            search_detail: None,
            last_calib: None,
            last_error: String::new(),
            last_error_unix: 0.0,
        }
    }
}

impl Status {
    /// Clear the per-recording fields (protocol step, times, fps).
    pub fn clear_recording(&mut self) {
        let fresh = Status::default();
        self.fps = fresh.fps;
        self.step_index = fresh.step_index;
        self.step_count = fresh.step_count;
        self.step_label = fresh.step_label;
        self.step_remaining_s = fresh.step_remaining_s;
        self.elapsed_s = fresh.elapsed_s;
        self.total_s = fresh.total_s;
    }

    /// Keep the message of an `error` as `last_error` when it starts or changes. `before` is the message if the state
    /// was already `error` before this update, else None.
    pub fn note_error(&mut self, before: Option<&str>, now_unix: f64) {
        if self.state == "error" && before != Some(self.message.as_str()) {
            self.last_error = self.message.clone();
            self.last_error_unix = now_unix;
        }
    }

    /// One line of JSON. `updated_unix` (wall clock seconds) lets a reader tell a live daemon from a stale file.
    pub fn to_json(&self, updated_unix: f64, pid: u32) -> String {
        format!(
            "{{\"version\":1,\"state\":{},\"message\":{},\"message_en\":{},\"has_buffers\":{},\"auto_grab\":{},\"grab_outdated\":{},\"locked\":{},\"fps_l\":{},\"fps_r\":{},\
\"step_index\":{},\"step_count\":{},\"step_label\":{},\"step_remaining_s\":{},\"elapsed_s\":{},\"total_s\":{},\
\"session_dir\":{},\"protocol\":{},\"prox\":{},\"search\":{},\"last_session_aborted\":{},\"calib_state\":{},\
\"recalib_suggested\":{},\"baseline\":{},\"warmup_remaining_s\":{},\"calib_saved\":{},\"widen_sensitivity\":{},\"dev\":{},\"setup_done\":{},\"last_calib_widen\":{},\"calib_failed_eye\":{},\"live\":{},\"live_ms\":{},\"pupil_l\":{},\"pupil_r\":{},\"prox_min\":{},\"search_detail\":{},\"last_calib\":{},\"last_error\":{},\"last_error_en\":{},\"last_error_unix\":{},\"pid\":{pid},\"updated_unix\":{}}}",
            json_str(self.state),
            json_str(&self.message),
            json_str(&crate::message_en::message_en(&self.message)),
            self.has_buffers,
            json_str(&self.auto_grab),
            self.grab_outdated,
            self.locked,
            num(self.fps[0]),
            num(self.fps[1]),
            self.step_index,
            self.step_count,
            json_str(&self.step_label),
            num(self.step_remaining_s),
            num(self.elapsed_s),
            num(self.total_s),
            json_str(&self.session_dir),
            json_str(&self.protocol),
            num(self.prox),
            json_str(self.search),
            self.last_session_aborted,
            self.calib_state,
            self.recalib_suggested,
            json_str(self.baseline),
            num(self.warmup_remaining_s),
            self.calib_saved,
            num(self.widen_sensitivity),
            self.dev,
            self.setup_done,
            json_str(self.last_calib_widen),
            json_str(self.calib_failed_eye),
            self.live,
            num(self.live_ms),
            num_or_null(self.pupil[0]),
            num_or_null(self.pupil[1]),
            num(self.prox_min),
            self.search_detail.as_ref().map_or("null".into(), SearchDetail::to_json),
            self.last_calib.as_ref().map_or("null".into(), LastCalib::to_json),
            json_str(&self.last_error),
            json_str(&crate::message_en::message_en(&self.last_error)),
            num(self.last_error_unix),
            num(updated_unix),
        )
    }
}

fn num(v: f64) -> String {
    if v.is_finite() { format!("{:.3}", v) } else { "-1".into() }
}

fn num_or_null(v: f64) -> String {
    if v.is_finite() { format!("{:.3}", v) } else { "null".into() }
}

fn json_str(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CalibKind {
    /// Every wear: close, normal, widen.
    Wear,
    /// Once per user: squint, look up, look down.
    User,
}

#[derive(Debug, PartialEq)]
pub enum Command {
    /// Start a recording with this protocol name (protocol_<name>.txt).
    Start(String),
    Stop,
    Status,
    Calib(CalibKind),
    Live(bool),
    /// `set widen_sensitivity <0..1>`: answered by the control thread itself, in every state.
    SetWidenSensitivity(f64),
}

/// Parse one control line. Protocol names are 1-32 of [A-Za-z0-9_-], so they can never name a path.
pub fn parse_command(line: &str) -> Result<Command, String> {
    let mut words = line.split_whitespace();
    let command = match (words.next(), words.next()) {
        (Some("start"), None) => Command::Start(DEFAULT_PROTOCOL.into()),
        (Some("start"), Some(name)) => {
            let ok = (1..=32).contains(&name.len())
                && name.chars().all(|c| c.is_ascii_alphanumeric() || c == '_' || c == '-');
            if !ok {
                return Err("プロトコル名に使えない文字がある".into());
            }
            Command::Start(name.into())
        }
        (Some("stop"), None) => Command::Stop,
        (Some("status"), None) => Command::Status,
        (Some("calib"), Some("wear")) => Command::Calib(CalibKind::Wear),
        (Some("calib"), Some("user")) => Command::Calib(CalibKind::User),
        (Some("live"), Some("on")) => Command::Live(true),
        (Some("live"), Some("off")) => Command::Live(false),
        (Some("set"), Some("widen_sensitivity")) => {
            let v = words.next().and_then(|w| w.parse::<f64>().ok()).filter(|v| (0.0..=1.0).contains(v));
            Command::SetWidenSensitivity(v.ok_or("widen_sensitivity は 0 から 1 の数で")?)
        }
        _ => return Err("知らないコマンド".into()),
    };
    if words.next().is_some() {
        return Err("知らないコマンド".into());
    }
    Ok(command)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_commands_and_refuses_paths() {
        assert_eq!(parse_command("start"), Ok(Command::Start("widen".into())));
        assert_eq!(parse_command(" start pupil \n"), Ok(Command::Start("pupil".into())));
        assert_eq!(parse_command("stop"), Ok(Command::Stop));
        assert_eq!(parse_command("status"), Ok(Command::Status));
        assert_eq!(parse_command("calib wear"), Ok(Command::Calib(CalibKind::Wear)));
        assert_eq!(parse_command("calib user"), Ok(Command::Calib(CalibKind::User)));
        assert_eq!(parse_command("live off"), Ok(Command::Live(false)));
        assert!(parse_command("calib").is_err() && parse_command("live maybe").is_err());
        assert_eq!(parse_command("set widen_sensitivity 0.25"), Ok(Command::SetWidenSensitivity(0.25)));
        for bad in ["set widen_sensitivity", "set widen_sensitivity 1.5", "set widen_sensitivity x", "set widen_sensitivity 0.5 1", "set foo 1"] {
            assert!(parse_command(bad).is_err(), "{bad}");
        }
        for bad in ["start ../x", "start a/b", "start .", "start a b", "stop now", "", "rm"] {
            assert!(parse_command(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn every_search_reason_has_a_message_in_both_languages() {
        for reason in ["", SEARCH_NOT_WORN, SEARCH_NO_VIDEO, SEARCH_ONE_EYE] {
            let ja = search_message(reason);
            assert_ne!(crate::message_en::message_en(ja), ja, "{reason}");
        }
        assert_eq!(search_message(SEARCH_NOT_WORN), search_message(""));
    }

    #[test]
    fn json_is_one_escaped_line() {
        let status = Status { message: "録画中 \"x\"\n".into(), prox: f64::NAN, ..Status::default() };
        let json = status.to_json(1.5, 7);
        assert!(!json.contains('\n'));
        assert!(json.starts_with("{\"version\":1,\"state\":\"waiting_fds\",\"message\":\"録画中 \\\"x\\\"\\n\",\"message_en\":\"録画中 \\\"x\\\"\\n\",\"has_buffers\":false,\"auto_grab\":\"\","));
        assert!(json.contains("\"prox\":-1,\"search\":\"\",") && json.contains("\"step_index\":-1,"));
        let searching = Status { search: SEARCH_NO_VIDEO, prox: 12.5, ..Status::default() }.to_json(1.5, 7);
        assert!(searching.contains("\"prox\":12.500,\"search\":\"no_video\",") && crate::json::parse(&searching).is_ok());
        let status = Status { message: "録画中".into(), ..Status::default() };
        assert!(status.to_json(1.5, 7).contains("\"message\":\"録画中\",\"message_en\":\"Recording\","));
        assert!(json.ends_with("\"updated_unix\":1.500}"));
        assert!(json.contains("\"widen_sensitivity\":0.500,\"dev\":false,\"setup_done\":false,"));
        assert!(Status { dev: true, ..Status::default() }.to_json(1.5, 7).contains(",\"dev\":true,"));
        assert!(json.contains("\"last_calib_widen\":\"\",\"calib_failed_eye\":\"\","));
        assert!(json.contains(",\"pupil_l\":null,\"pupil_r\":null,"), "{json}");
        let seen = Status { pupil: [0.95, f64::NAN], ..Status::default() }.to_json(1.5, 7);
        assert!(seen.contains(",\"pupil_l\":0.950,\"pupil_r\":null,") && crate::json::parse(&seen).is_ok(), "{seen}");
        assert!(Status { calib_failed_eye: "R", ..Status::default() }.to_json(1.5, 7).contains(",\"calib_failed_eye\":\"R\","));
        assert!(
            json.contains(",\"prox_min\":20.000,\"search_detail\":null,\"last_calib\":null,\"last_error\":\"\",\"last_error_en\":\"\",\"last_error_unix\":0.000,"),
            "{json}"
        );
    }

    #[test]
    fn search_detail_says_what_the_last_look_saw() {
        let look = crate::ring::Look { candidates: 8, refresh_hz: 65.0, slots: 8, both_eyes: true, stopped_at: "" };
        let status = Status { search_detail: Some(SearchDetail { look, changed_blocks: 412, unix: 2.0 }), ..Status::default() };
        let json = status.to_json(1.5, 7);
        assert!(
            json.contains(",\"search_detail\":{\"candidates\":8,\"refresh_hz\":65.000,\"slots\":8,\"both_eyes\":true,\"stopped_at\":\"\",\"changed_blocks\":412,\"unix\":2.000},"),
            "{json}"
        );
        let parsed = crate::json::parse(&json).unwrap();
        assert_eq!(parsed.get("search_detail").and_then(|d| d.get("candidates")).and_then(crate::json::Json::num), Some(8.0));
        let stopped = crate::ring::Look { stopped_at: crate::ring::STOP_NO_CANDIDATES, ..crate::ring::Look::default() };
        let json = Status { search_detail: Some(SearchDetail { look: stopped, changed_blocks: 0, unix: 2.0 }), ..Status::default() }.to_json(1.5, 7);
        assert!(json.contains("\"stopped_at\":\"no_candidates\",\"changed_blocks\":0,"), "{json}");
    }

    /// A calib_result.json as livesvc writes it (a wear calibration that went through without the right eye).
    const RESULT: &str = r#"{
  "kind": "wear",
  "time": "2026-10-05 19:51:03",
  "ok": true,
  "failed_eye": "R",
  "message": "校正できた（右目は瞳がうまく見えなかったので、前の値を使うよ）[0/486、90 必要]",
  "values": {"L": {"r_px": 40, "normal_frames_with_pupil": 486, "diag": {"fps": 90, "search_x_min": 186, "search_xmax": 346, "normal_pupil_x": 240.5, "normal_pupil_y": 201, "normal_contrast": 30, "steps": {"normal": {"frames": 486, "pupil": 486}}}},
             "R": {"r_px": null, "normal_frames_with_pupil": 0, "diag": {"fps": 90, "search_x_min": 180, "search_xmax": 340, "normal_pupil_x": null, "normal_pupil_y": null, "normal_contrast": 12, "steps": {"normal": {"frames": 486, "pupil": 0}}}},
             "thresholds": {}},
  "params": null
}
"#;

    #[test]
    fn last_calib_is_read_back_from_calib_result() {
        let c = LastCalib::from_result(RESULT).unwrap();
        assert_eq!((c.time.as_str(), c.ok, c.failed_eye.as_str()), ("2026-10-05 19:51:03", true, "R"));
        assert_eq!(c.pupil_frames, [486.0, 0.0]);
        assert_eq!(c.normal_frames, [486.0, 486.0]);
        assert_eq!((c.pupil_x[0], c.pupil_y[0]), (240.5, 201.0));
        assert!(c.pupil_x[1].is_nan() && c.pupil_y[1].is_nan());
        assert_eq!(c.window, [[186.0, 346.0], [180.0, 340.0]]);
        let json = Status { last_calib: Some(c), ..Status::default() }.to_json(1.5, 7);
        assert!(json.contains(",\"last_calib\":{\"time\":\"2026-10-05 19:51:03\",\"ok\":true,\"failed_eye\":\"R\",\"message\":\"校正できた（"), "{json}");
        assert!(json.contains("\"message_en\":\"Calibrated (Right eye: couldn't see the pupil well, using its previous values) [0/486, 90 needed]\""), "{json}");
        assert!(json.contains("\"pupil_frames\":[486.000,0.000],\"normal_frames\":[486.000,486.000],\"pupil_x\":[240.500,null],\"pupil_y\":[201.000,null],\"window\":[[186.000,346.000],[180.000,340.000]]}"), "{json}");
        assert!(crate::json::parse(&json).is_ok());
        // Only a wear calibration's; an older file without failed_eye says only whether it failed
        assert!(LastCalib::from_result(&RESULT.replace("\"wear\"", "\"user\"")).is_none());
        let old = LastCalib::from_result(&RESULT.replace("\"failed_eye\": \"R\",", "").replace("\"ok\": true", "\"ok\": false")).unwrap();
        assert_eq!((old.ok, old.failed_eye.as_str()), (false, "LR"));
        assert!(LastCalib::from_result("not json").is_none());
    }

    #[test]
    fn the_newest_saved_wear_calibration_is_found() {
        let dir = std::env::temp_dir().join(format!("eyecam-lastcalib-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        for (name, text) in [
            ("calib_2026-10-05_19-51-03", RESULT.to_string()),
            ("calib_2026-10-05_20-10-00", RESULT.replace("\"wear\"", "\"user\"")),
            ("calib_2026-10-04_08-00-00", RESULT.replace("19:51:03", "08:00:00")),
            ("rec_2026-10-06_00-00-00", RESULT.replace("19:51:03", "00:00:00")),
        ] {
            std::fs::create_dir_all(dir.join(name)).unwrap();
            std::fs::write(dir.join(name).join("calib_result.json"), text).unwrap();
        }
        assert_eq!(LastCalib::newest_in(&dir).map(|c| c.time), Some("2026-10-05 19:51:03".to_string()));
        assert!(LastCalib::newest_in(&dir.join("missing")).is_none());
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn last_error_keeps_the_error_after_the_state_moves_on() {
        let why = "右目を閉じたのが検出できなかった（もう一度、しっかり閉じてね）";
        let mut s = Status::default();
        s.note_error(None, 1.0);
        assert_eq!((s.last_error.as_str(), s.last_error_unix), ("", 0.0));
        s.state = "error";
        s.message = why.into();
        s.note_error(None, 2.0);
        assert_eq!(s.last_error_unix, 2.0);
        // Still the same error: its time stays
        s.note_error(Some(why), 3.0);
        assert_eq!(s.last_error_unix, 2.0);
        // Back to idle: kept, in both languages
        s.state = "idle";
        s.message = "待機中".into();
        s.note_error(Some(why), 4.0);
        assert_eq!(s.last_error, why);
        let json = s.to_json(5.0, 7);
        assert!(
            json.contains("\"last_error_en\":\"Right eye: couldn't detect the eye closing (try again and close it firmly)\",\"last_error_unix\":2.000,"),
            "{json}"
        );
        // The same error again later: a new time
        s.state = "error";
        s.message = why.into();
        s.note_error(None, 6.0);
        assert_eq!(s.last_error_unix, 6.0);
    }
}
