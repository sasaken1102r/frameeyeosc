//! What `eyecam-rec --serve` publishes for the VR panel (status.json) and the control commands it accepts on
//! ctl.sock. The README documents both for the panel; keep them in sync.

pub const DEFAULT_DIR: &str = "/run/user/1000/eyecam";
pub const STATUS_FILE: &str = "status.json";
pub const CTL_FILE: &str = "ctl.sock";
pub const DEFAULT_PROTOCOL: &str = "widen";

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
    /// A `calib wear` has succeeded at least once (kept in calib.json): the panel's first-time setup is done.
    pub setup_done: bool,
    /// The last `calib wear` of this run: "measured" (widen caught), "default" (not caught: the usual step), or "".
    pub last_calib_widen: &'static str,
    /// Live processing is on (`live on`, the default).
    pub live: bool,
    /// Live processing time per frame (one eye), ms, averaged over the last second.
    pub live_ms: f64,
}

impl Default for Status {
    fn default() -> Self {
        Self {
            state: "waiting_fds",
            message: String::new(),
            has_buffers: false,
            auto_grab: String::new(),
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
            last_session_aborted: false,
            calib_state: 0,
            recalib_suggested: false,
            baseline: "warming",
            warmup_remaining_s: crate::live::WARMUP_S,
            calib_saved: false,
            widen_sensitivity: crate::live::DEFAULT_WIDEN_SENSITIVITY,
            setup_done: false,
            last_calib_widen: "",
            live: false,
            live_ms: 0.0,
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

    /// One line of JSON. `updated_unix` (wall clock seconds) lets a reader tell a live daemon from a stale file.
    pub fn to_json(&self, updated_unix: f64, pid: u32) -> String {
        format!(
            "{{\"version\":1,\"state\":{},\"message\":{},\"message_en\":{},\"has_buffers\":{},\"auto_grab\":{},\"locked\":{},\"fps_l\":{},\"fps_r\":{},\
\"step_index\":{},\"step_count\":{},\"step_label\":{},\"step_remaining_s\":{},\"elapsed_s\":{},\"total_s\":{},\
\"session_dir\":{},\"protocol\":{},\"prox\":{},\"last_session_aborted\":{},\"calib_state\":{},\
\"recalib_suggested\":{},\"baseline\":{},\"warmup_remaining_s\":{},\"calib_saved\":{},\"widen_sensitivity\":{},\"setup_done\":{},\"last_calib_widen\":{},\"live\":{},\"live_ms\":{},\"pid\":{pid},\"updated_unix\":{}}}",
            json_str(self.state),
            json_str(&self.message),
            json_str(&crate::message_en::message_en(&self.message)),
            self.has_buffers,
            json_str(&self.auto_grab),
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
            self.last_session_aborted,
            self.calib_state,
            self.recalib_suggested,
            json_str(self.baseline),
            num(self.warmup_remaining_s),
            self.calib_saved,
            num(self.widen_sensitivity),
            self.setup_done,
            json_str(self.last_calib_widen),
            self.live,
            num(self.live_ms),
            num(updated_unix),
        )
    }
}

fn num(v: f64) -> String {
    if v.is_finite() { format!("{:.3}", v) } else { "-1".into() }
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
    fn json_is_one_escaped_line() {
        let status = Status { message: "録画中 \"x\"\n".into(), prox: f64::NAN, ..Status::default() };
        let json = status.to_json(1.5, 7);
        assert!(!json.contains('\n'));
        assert!(json.starts_with("{\"version\":1,\"state\":\"waiting_fds\",\"message\":\"録画中 \\\"x\\\"\\n\",\"message_en\":\"録画中 \\\"x\\\"\\n\",\"has_buffers\":false,\"auto_grab\":\"\","));
        assert!(json.contains("\"prox\":-1,") && json.contains("\"step_index\":-1,"));
        let status = Status { message: "録画中".into(), ..Status::default() };
        assert!(status.to_json(1.5, 7).contains("\"message\":\"録画中\",\"message_en\":\"Recording\","));
        assert!(json.ends_with("\"updated_unix\":1.500}"));
    }
}
