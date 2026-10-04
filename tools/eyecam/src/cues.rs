//! Spoken-free cues for someone wearing the headset: a protocol of timed steps, each announced by a beep pattern.
//!
//! Protocol file: one step per line, `seconds label` (e.g. `5 widen`); `#` starts a comment.
//! Beeps: lead_in = 1 short (high), normal = 1 short, widen = 2 short, close = 3 short, squint = 1 long (mid),
//! look_up = 1 long (high), look_down = 1 long (low), bright = 2 short (high), dark = 2 short (low),
//! anything else = 1 long (mid); the end of the protocol = 2 long (low).

use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};

#[derive(Clone, Debug, PartialEq)]
pub struct Step {
    pub seconds: f64,
    pub label: String,
}

pub fn parse(text: &str) -> Result<Vec<Step>, String> {
    let mut steps = Vec::new();
    for (n, line) in text.lines().enumerate() {
        let line = line.split('#').next().unwrap().trim();
        if line.is_empty() {
            continue;
        }
        let mut parts = line.split_whitespace();
        let seconds: f64 = parts
            .next()
            .unwrap()
            .parse()
            .ok()
            .filter(|s: &f64| s.is_finite() && *s > 0.0)
            .ok_or_else(|| format!("line {}: expected `seconds label`", n + 1))?;
        let label = parts.collect::<Vec<_>>().join("_");
        if label.is_empty() {
            return Err(format!("line {}: missing label", n + 1));
        }
        steps.push(Step { seconds, label });
    }
    if steps.is_empty() {
        return Err("no steps".into());
    }
    Ok(steps)
}

/// The beep pattern for a label: a name (for the log and the WAV file) and its tones, (Hz, ms) each.
pub fn pattern(label: &str) -> (&'static str, &'static [(f32, u32)]) {
    const SHORT: u32 = 110;
    const LONG: u32 = 650;
    match label {
        "lead_in" => ("1short_high", &[(1400.0, SHORT)]),
        "normal" => ("1short", &[(1000.0, SHORT)]),
        "widen" => ("2short", &[(1000.0, SHORT), (1000.0, SHORT)]),
        "close" => ("3short", &[(1000.0, SHORT), (1000.0, SHORT), (1000.0, SHORT)]),
        "look_up" => ("long_high", &[(1400.0, LONG)]),
        "look_down" => ("long_low", &[(550.0, LONG)]),
        "bright" => ("2short_high", &[(1800.0, SHORT), (1800.0, SHORT)]),
        "dark" => ("2short_low", &[(450.0, SHORT), (450.0, SHORT)]),
        "end" => ("2long_low", &[(550.0, LONG), (550.0, LONG)]),
        _ => ("long_mid", &[(850.0, LONG)]),
    }
}

/// A mono 16-bit 48 kHz WAV of the tones, 110 ms of silence between them, at `volume` (0..1) of full scale.
pub fn wav(tones: &[(f32, u32)], volume: f32) -> Vec<u8> {
    const RATE: u32 = 48000;
    let mut samples: Vec<i16> = Vec::new();
    for (i, &(hz, ms)) in tones.iter().enumerate() {
        if i > 0 {
            samples.extend(std::iter::repeat_n(0, (RATE * 110 / 1000) as usize));
        }
        let n = (RATE * ms / 1000) as usize;
        for k in 0..n {
            // 5 ms fade in and out, so it clicks less.
            let fade = (k.min(n - 1 - k) as f32 / (RATE as f32 * 0.005)).min(1.0);
            let s = (2.0 * std::f32::consts::PI * hz * k as f32 / RATE as f32).sin();
            samples.push((s * fade * volume.clamp(0.0, 1.0) * i16::MAX as f32) as i16);
        }
    }
    let data_len = (samples.len() * 2) as u32;
    let mut out = Vec::with_capacity(44 + data_len as usize);
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&(36 + data_len).to_le_bytes());
    out.extend_from_slice(b"WAVEfmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes()); // PCM
    out.extend_from_slice(&1u16.to_le_bytes()); // mono
    out.extend_from_slice(&RATE.to_le_bytes());
    out.extend_from_slice(&(RATE * 2).to_le_bytes());
    out.extend_from_slice(&2u16.to_le_bytes());
    out.extend_from_slice(&16u16.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data_len.to_le_bytes());
    for s in samples {
        out.extend_from_slice(&s.to_le_bytes());
    }
    out
}

/// Plays WAVs with the first of pw-play, paplay, aplay found on PATH, without waiting for them.
pub struct Player {
    program: Option<PathBuf>,
    dir: PathBuf,
    volume: f32,
    children: Vec<Child>,
}

impl Player {
    /// `dir` is where the WAVs are written.
    pub fn new(dir: &Path, volume: f32, enabled: bool) -> Self {
        let program = enabled
            .then(|| {
                let path = std::env::var_os("PATH").unwrap_or_default();
                ["pw-play", "paplay", "aplay"]
                    .iter()
                    .find_map(|name| std::env::split_paths(&path).map(|d| d.join(name)).find(|p| p.is_file()))
            })
            .flatten();
        Self { program, dir: dir.to_path_buf(), volume, children: Vec::new() }
    }

    pub fn program(&self) -> Option<&Path> {
        self.program.as_deref()
    }

    /// Start playing the pattern for `label`; returns the pattern name.
    pub fn play(&mut self, label: &str) -> &'static str {
        let (name, tones) = pattern(label);
        self.children.retain_mut(|c| !matches!(c.try_wait(), Ok(Some(_))));
        let Some(program) = &self.program else { return name };
        let file = self.dir.join(format!("{name}.wav"));
        if !file.exists() {
            let _ = std::fs::create_dir_all(&self.dir);
            if std::fs::write(&file, wav(tones, self.volume)).is_err() {
                return name;
            }
        }
        let mut cmd = Command::new(program);
        if program.ends_with("aplay") {
            cmd.arg("-q");
        }
        // Over ssh XDG_RUNTIME_DIR may be unset; PipeWire/PulseAudio need it to find the user's session.
        if std::env::var_os("XDG_RUNTIME_DIR").is_none() {
            cmd.env("XDG_RUNTIME_DIR", format!("/run/user/{}", unsafe { libc::getuid() }));
        }
        if let Ok(child) = cmd.arg(&file).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).spawn() {
            self.children.push(child);
        }
        name
    }

    /// Let the last beep finish (at most 2 s).
    pub fn finish(&mut self) {
        for _ in 0..40 {
            self.children.retain_mut(|c| !matches!(c.try_wait(), Ok(Some(_))));
            if self.children.is_empty() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_the_default_protocol() {
        let steps = parse(include_str!("../protocol_widen.txt")).unwrap();
        assert_eq!(steps.first(), Some(&Step { seconds: 3.0, label: "lead_in".into() }));
        assert_eq!(steps.iter().filter(|s| s.label == "widen").count(), 3);
        assert_eq!(steps.iter().map(|s| s.seconds).sum::<f64>(), 83.0);
        assert_eq!(steps.last(), Some(&Step { seconds: 8.0, label: "bright".into() }));
        assert!(parse("abc normal").is_err());
        assert!(parse("5").is_err());
        assert_eq!(parse("2 look up # x").unwrap()[0].label, "look_up");
    }

    #[test]
    fn wav_has_the_right_length() {
        let w = wav(pattern("widen").1, 0.3);
        assert_eq!(&w[..4], b"RIFF");
        assert_eq!(w.len(), 44 + 2 * 48 * (110 + 110 + 110));
    }
}
