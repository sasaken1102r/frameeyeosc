# frameeyeosc

Sends the Steam Frame's eye tracking (gaze and eye openness) to VRChat over OSC, as VRCFaceTracking-style avatar parameters. It runs on the headset as a background service and works with PC VRChat streamed through Steam Link. It can also send to VRCFaceTracking on the PC, so the eyes can be combined with other trackers.

[日本語版はこちら](README.ja.md)

https://github.com/user-attachments/assets/f8969485-161b-40d4-b9e4-689dee6d1955

This is a fork of [konsti219/frameeyeosc](https://github.com/konsti219/frameeyeosc). The Frame's public APIs only give you a combined gaze direction. konsti219 found that the eye tracker also measures how open each eye is and keeps it in an internal shared-memory object (`/dev/shm/eye-server.mmap`), and that is where this tool reads it from.

## What this fork adds

- It finds your PC on its own by sending to whichever PC Steam Link is streaming from. With the bundled wireless adapter that's the adapter's direct link, so your home network doesn't matter.
- Gaze and eyelids are smoothed with One Euro filters. A small deadzone keeps the eyes still while you fixate, and the gaze is held while your eyes are shut, because the Frame's gaze jumps as the eyes reopen.
- Both eyes share one gaze. On the Frame each eye's gaze wobbles on its own, which makes an avatar's eyes twitch. `--independent-eyes` switches back to per-eye gaze.
- Eyelid values are mapped onto the VRCFT scale (0 closed, 0.75 relaxed, 1 widened). A held-shut eye reads about 0.2 on the Frame, and a relaxed eye wanders between about 0.75 and 0.9.
- Eyelids calibrate themselves. It learns how far each of your eyes opens when relaxed, so if your face or the headset fit makes one eye look more open, the avatar still looks even. Winks still come through.
- It runs as a service that starts with SteamVR and restarts if it stops.
- Settings live in a file that is picked up while running, and an optional panel on the SteamVR dashboard changes them from inside the headset.
- It can send in the format the ETVR Tracking Module for VRCFaceTracking reads (see [VRCFaceTracking (ETVR) mode](#vrcfacetracking-etvr-mode)), or as Live Link Face packets for VRCFaceTracking's LiveLink module, which also carries widened eyes (see [VRCFaceTracking (LiveLink) mode](#vrcfacetracking-livelink-mode)).

## Requirements

- A Steam Frame with Developer Mode on and SSH access (Settings > System > Developer Mode, then set a password under Developer). Choose a strong password: with SSH on, anyone on your network who knows it can log in to the headset.
- PC VRChat streamed with Steam Link, OSC enabled in VRChat (Action Menu > Options > OSC > Enabled).
- An avatar with VRCFaceTracking eye parameters (`FT/v2/EyeLeftX`, `EyeLidLeft`, ...) as floats. Avatars that pack parameters into binary bits are not supported when sending to VRChat directly. frameeyeosc also sends `EyeTrackingActive` as a bool; some avatars declare it as a float and stop tracking on a bool. For those, choose "Float" under "EyeTrackingActive type" on the Output tab (`eye_tracking_active`), or "Off" to not send it at all. Avatars set up for the OSC that SteamVR's Steam Link sends by itself (`LeftEyeX`, `RightEyeLid`, ...) work too: turn on "Steam Link names too" on the Output tab (see [Avatars made for Steam Link's OSC](#avatars-made-for-steam-links-osc)). Avatars without these parameters can follow your eyes through VRChat's own eye tracking input instead (see [Native VRChat eye tracking](#native-vrchat-eye-tracking)).
- For the VRCFaceTracking (ETVR) mode: VRCFaceTracking on the PC with the ETVR Tracking Module. For the LiveLink mode: VRCFaceTracking with the LiveLink module.

## Install

### Easiest: install right inside the Frame (recommended)

No PC needed. In Konsole on the Frame (+ on the bar at the bottom → the list of programs → Konsole), type this command, press Enter, and pick **1** (frameeyeosc) from the menu. It asks whether to install the dashboard panel too.

```sh
curl -fsSL https://frame.sasaken1102s.net | sh
```

- Do this once first: Steam Settings → System → turn on "Enable Developer Mode" (while it's off, Konsole doesn't show up in the + list).
- The other apps (frame-jp-keyboard, frame-mic-tuner, frame-perf-overlay) can be installed from the same menu.
- To update, run the same command and pick the same number again. To uninstall, use `u` in the menu.
- Step-by-step guide and video: https://frame.sasaken1102s.net
- To install without any prompts: `curl -fsSL https://frame.sasaken1102s.net | sh -s -- install eye`

What gets installed and the options are the same as in "Install from a PC" below (it runs `install.sh` for you). After installing, don't forget to turn off Steam Link's OSC output as described below.

### Install from a PC

Download the tarball from the releases page and copy it to the headset, for example from your PC:

```sh
scp frameeyeosc-*-steamframe-aarch64.tar.gz steamos@<headset-ip>:
```

Then on the headset (`ssh steamos@<headset-ip>`):

```sh
tar xzf frameeyeosc-*-steamframe-aarch64.tar.gz
cd frameeyeosc
./install.sh               # frameeyeosc only
./install.sh --with-panel  # frameeyeosc and the dashboard panel
```

No sudo is needed. Everything goes into your home directory (`~/.local/bin`, `~/.config`, `~/.local/share`), so SteamOS updates don't remove it. Run the same command again to update. Without `--with-panel` an installed panel is left as it is.

After that, turn off Steam Link's own OSC output on your PC (SteamVR settings > Steam Link > OSC). Steam Link sends its own unsmoothed eye data to VRChat, and with both running, two sources fight over the avatar's eyes. This is needed in the ETVR and LiveLink modes too, where VRCFaceTracking drives the avatar's eyes.

#### Updating from a version before 0.4.0

Versions before 0.4.0 have no updater, so update to 0.4.0 once by hand: copy and unpack the new tarball as above and run `./install.sh --with-panel` (or `./install.sh` without the panel). Your `~/.config/frameeyeosc/env` and the learned eyelid calibration are kept, and the service restarts on the new version.

Options in `FRAMEEYEOSC_ARGS` in `env` still work as before. But anything set there is locked in the panel ("Locked by command line"). To change it from the panel, remove it from `env`, run `systemctl --user restart frameeyeosc`, and set the value again in the panel.

To remove it: `./install.sh --uninstall` (removes the panel too; add `--purge` to also delete settings and calibration).

#### Updating from the panel (0.4.0 and later)

From 0.4.0 on, the panel's "Update" button does the update. The panel's Advanced page shows the installed version. At start and then at most once a day, the panel asks GitHub whether a newer release exists. That holds while checks succeed: after a failed check it tries again an hour later. "Check now" asks right away. When a newer release exists, the Advanced page shows its summary under the version row (in Japanese on a Japanese panel when the release has one), and "Update" downloads it, checks it against the release's `SHA256SUMS`, and runs its `install.sh` with the options of your last install (kept in `~/.config/frameeyeosc/install-args`). frameeyeosc and the panel restart on the new version. If anything fails before `install.sh` runs, nothing changes; the log is in `~/.cache/frameeyeosc/update.log`. Turn "Check for updates" off to stop the daily check (the "Check now" button still works). The update itself only runs when you press the button.

`SHA256SUMS` is a checksum file from the same release, not a signature. It catches a corrupted or incomplete download. It can't catch a release that was replaced on GitHub, because the checksum would be replaced along with it.

## Panel

`./install.sh --with-panel` adds an "Eye" panel to the SteamVR dashboard. It starts together with SteamVR from the next SteamVR start; to open it right away, pick "frameeyeosc panel" under Launch program (+) on the dashboard.

| Basic | Output |
|---|---|
| ![The Basic tab](docs/images/panel-basic-en_2026-10-01_02-00-00.png) | ![The Output tab](docs/images/panel-output-en_2026-10-01_02-00-00.png) |
| **Gaze** | **Eye fit** |
| ![The Gaze tab](docs/images/panel-gaze-en_2026-10-01_02-00-00.png) | ![The Eye fit tab](docs/images/panel-eyefit-en_2026-10-01_02-00-00.png) |
| **Eyelids** | **Advanced** |
| ![The Eyelids tab](docs/images/panel-lids-en_2026-10-01_02-00-00.png) | ![The Advanced tab](docs/images/panel-advanced-en_2026-10-01_02-00-00.png) |

- The left column always shows what frameeyeosc is doing: sending or paused, where it sends to, messages per second, how many samples a second the eye tracker delivers (marked "low" below 60, with a line under it saying whether frameeyeosc or the Frame was the slow one), both eyelids and the gaze (raw and sent), and a config error if there is one. While the "Track Dominant Eye Only" setting is on, the gaze title line says which eye the Frame tracks ("Frame setting: tracking the right eye only").
- Basic: pause sending, where to send (three cards: VRChat directly, VRCFT (LiveLink), marked recommended, and VRCFT (ETVR), each saying whether wide eyes come through, how others see your eyes, and whether VRCFaceTracking is needed), language (Japanese / English), start with SteamVR, reset all, quit.
- Output: target PC (automatic, fixed to the PC it sends to now, or typed: see below) and port. For VRChat directly also the parameter prefix, the EyeTrackingActive type, whether to send Steam Link's parameter names too and whether to move VRChat's own eyes too; for LiveLink and ETVR what to set up in VRCFaceTracking on the PC instead.
- Gaze: smoothing on or off, light / medium / strong presets and the three filter values, deadzone, holding the gaze while blinking, per-eye gaze, skipping unreliable gaze, removing one-sample glitches.
- Eye fit: one button that fits your gaze and eyelids in about 20 seconds (see [Eye fit](#eye-fit)), fitting straight ahead again, what to fit by itself when you put the headset on ("When put on": nothing, re-center, or re-center + tilt), the result with "Reset", and the values by hand under "Fine-tune".
- Eyelids: auto calibration and its learned values, per-eye scales, the four openness marks drawn over each eye's live openness (blink and open wide to set them), left/right sync, keeping blinks visible (hold time and closing both eyes), eyelid smoothing. Once the eyes are fitted, "Widen" (Off / Less / Normal / More) takes the auto calibration's place, a line says whether an eye widens with the other one, and the marks fold away under "Fine-tune" (3 and 4 greyed there: they are for eyes without a fit). Where a relaxed open eye already reads 1.0 (SteamOS 0.4.3), that line says widening can't come through (see [Troubleshooting](#troubleshooting)).
- Advanced: the version with checking for and installing updates (and the automatic check on or off), diagnostics (showing gaze dots and their distance, recording the eye log: see below), and files and process (file locations, frameeyeosc's PID, options locked by the command line).

The panel writes `config.json` and reads the status file. To pick its default language, it also reads the `language` line of Steam's `~/.steam/registry.vdf` once at startup (read only). For updates it runs `~/.local/share/frameeyeosc/frame-update.sh` (see above). Closing it, quitting it, or not installing it doesn't stop frameeyeosc. While it isn't open on the dashboard it draws nothing. Besides running the update check, the only thing it reads then is the update state file (`~/.cache/frameeyeosc/update-state.json`), about twice a second. The exceptions are an eye fit (it then also reads the status file and shows the dot with the dashboard closed, until the fit is over) and the debug gaze dots while they're switched on (it then listens on their socket and moves the dots about 90 times a second while samples arrive, and reads the status file and checks `config.json` for changes 10 times a second), and "When put on" while the gaze is fitted (it then reads the status file every 0.5 s while the dashboard is closed, to notice the headset being put on). Its "Start with SteamVR" switch enables or disables its systemd user unit (`frameeyeosc-panel.service`). Build notes and debugging options are in [panel/README.md](panel/README.md) (Japanese).

### Setting the target PC by hand

"Auto" sends to the PC Steam Link is streaming from. If that is the wrong PC, press "Enter IP" in the Target PC row on the Output tab. A keypad opens: type the PC's IPv4 address (for example `192.168.1.20`) and press "OK". Leave the port out; it goes in the Port row. The panel writes it to `host` in `config.json`, and the row shows "Manual 192.168.1.20". "Auto" switches back. To use a host name instead of an address, set `host` in `config.json` by hand; the row then shows "Manual" and the name, and "Auto" still switches back.

## Settings

Settings are in `~/.config/frameeyeosc/config.json`. The panel writes it, and you can also edit it by hand. frameeyeosc checks it 10 times a second and applies changes without a restart. Missing keys use the defaults and unknown keys are ignored. If the file is broken or a value is out of range, frameeyeosc keeps the previous settings and reports the error (in the panel and in the status file).

```json
{ "output": "vrchat", "gaze_min_cutoff": 0.3, "lid_sync": 0.6 }
```

| Key | Option | Default | What it does |
|---|---|---|---|
| `sending` | | `true` | `false` pauses sending (in VRChat mode `EyeTrackingActive=false` is sent once, as `eye_tracking_active` says; in LiveLink mode relaxed open eyes looking ahead) |
| `output` | `--output` | `"vrchat"` | `"vrchat"` sends avatar parameters to VRChat, `"etvr"` sends to VRCFaceTracking's ETVR Tracking Module, `"livelink"` sends Live Link Face packets to VRCFaceTracking's LiveLink module |
| `host` | `--target` | `"auto"` | `"auto"` = the PC Steam Link is streaming from, else an IP address or host name without a port |
| `port` | `--port`, `--target` | `null` | `null` = 9000 for `vrchat`, 8889 for `etvr`, 11111 for `livelink` |
| `prefix` | `--prefix` | `"/FT"` | Parameter name prefix; `""` for none. Not used in LiveLink mode |
| `eye_tracking_active` | `--eye-tracking-active` | `"bool"` | How `EyeTrackingActive` is sent in VRChat mode: `"bool"` (true / false), `"float"` (1.0 / 0.0; some avatars need it) or `"off"` (never, not even the one-time "not active" on pausing or losing tracking). ETVR and LiveLink modes never send it |
| `steamlink_params` | `--steamlink-params` | `false` | In VRChat mode, also send the avatar parameters SteamVR's Steam Link sends from its own OSC (`LeftEyeX`, `RightEyeLid`, ...), for avatars made for those; see [Avatars made for Steam Link's OSC](#avatars-made-for-steam-links-osc). Never prefixed. ETVR and LiveLink modes ignore it |
| `native_eyes` | `--native-eyes` | `false` | In VRChat mode, also send VRChat's own eye tracking input (`/tracking/eye/*`), which moves the eyes of avatars without VRCFT parameters (see [Native VRChat eye tracking](#native-vrchat-eye-tracking)); a switch on the Output tab |
| `raw` | `--raw` | `false` | No smoothing, and none of the time-based steps (glitch removal, gaze holding, the quality check, blink hold, holding the sideways gaze far down) |
| `gaze_min_cutoff` | `--gaze-min-cutoff` | `0.3` | Lower = steadier gaze at rest, more lag |
| `gaze_beta` | `--gaze-beta` | `1.5` | Higher = follows fast eye movements with less lag, and settles sooner after one |
| `gaze_d_cutoff` | `--gaze-d-cutoff` | `0.5` | Lower = tracker noise loosens the gaze filter less, and the gaze glides softly into a quick eye movement instead of snapping to it |
| `gaze_deadzone` | `--gaze-deadzone` | `0.005` | Gaze changes smaller than this are ignored (1.0 = 45°); the gaze can stop up to this far short of where the eyes landed |
| `gaze_hold_below` | `--gaze-hold-below` | `0.5` | Hold the gaze while either eye's openness is below this; `0` turns it off |
| `independent_eyes` | `--independent-eyes` | `false` | Send each eye's own gaze instead of the shared one. Looks natural after an eye fit, which fits each eye's sideways gaze too. While the "Track Dominant Eye Only" setting is on, both eyes get the tracked eye's gaze either way |
| `gaze_quality_limit` | `--gaze-quality-limit` | `0` (off) | Optional safety net: ignore an eye's gaze while the Frame's own uncertainty (covariance) for it is above this (for example `0.03`). The other eye moves both, and if both are above it the gaze is held. Eyelids aren't affected. On a well-fitted headset it made no measurable difference, because the uncertainty only rises while the eyes are mostly shut, where `gaze_hold_below` already holds the gaze |
| `despike` | `--no-despike` | `true` | Remove one-sample glitches in gaze and openness (median of 3 samples; everything arrives ~11 ms later) |
| `lid_min_cutoff` / `lid_beta` | `--lid-min-cutoff` / `--lid-beta` | `6.0` / `5.0` | Eyelid smoothing, the same way as for gaze |
| `lid_closed` / `lid_open` / `lid_widen_start` / `lid_wide` | `--lid-closed` ... | `0.30` / `0.80` / `0.92` / `1.00` | How Frame eye openness maps onto closed / relaxed / widened |
| `lid_widen` | `--lid-widen` | `"normal"` | How easily an eye with an eye fit widens: `"off"`, `"low"`, `"normal"` or `"high"` (see [Eye fit](#eye-fit)). Eyes without a fit use `lid_widen_start` / `lid_wide` instead |
| `lid_scale_left` / `lid_scale_right` | `--lid-scale-left` / `--lid-scale-right` | `null` (learned) | Fixed per-eye multiplier instead of the learned one. For an eye with an eye fit, a fine-tune after the fit instead: 0.9 makes that eye read 10% less open (it closes sooner and widens less), `null` = 1.0. The full eye fit and its "Reset" set it back to `null` |
| `lid_calibration` | `--no-lid-calibration` | `true` | Learn eyelid calibration |
| `lid_sync` | `--lid-sync` | `0.4` | Evens out small left/right eyelid differences; larger ones (winks) pass through. `0` turns it off |
| `blink_hold_ms` | `--blink-hold-ms` | `80` | Once an eye is closed, it is sent fully closed for at least this long, so short blinks reach other players. `0` turns it off |
| `blink_sync_below` | `--blink-sync-below` | `0.35` | When one eye is closed and the other is below this (VRCFT scale), both are sent closed. Winks, with the other eye open, pass through. `0` turns it off |
| `gaze_offset_x` / `gaze_offset_y` | `--gaze-offset-x` / `--gaze-offset-y` | `0` / `0` | The gaze that counts as straight ahead, from -0.5 to 0.5 (1.0 = 45°; + is right / up). Set by the eye fit |
| `gaze_gain_x` / `gaze_gain_up` / `gaze_gain_down` | `--gaze-gain-x` / `--gaze-gain-up` / `--gaze-gain-down` | `1.0` | How far the gaze moves from there, sideways, up and down, from 0.5 to 2. Set by the eye fit |
| `gaze_roll_deg` | `--gaze-roll-deg` | `0` | How far the headset sits tilted, in degrees (-20 to 20; positive: looking right reads higher). The tilt is undone around straight ahead, before the gains, for both eyes and the combined gaze. Set by the eye fit |
| `gaze_offset_x_left` / `_right`, `gaze_gain_x_left` / `_right` | `--gaze-offset-x-left` ... | `null` | Each eye's own sideways zero point and gain, used for the per-eye gaze (`independent_eyes`). Set by the eye fit so that each eye points where it really has to at 2 m (they turn in a little); `null` = use `gaze_offset_x` / `gaze_gain_x`. The up/down gaze is shared by both eyes on the Frame, so there is no per-eye one |
| `gaze_down_hold_x_deg` | `--gaze-down-hold-x-deg` | `24` | Looking far down, the Frame's sideways gaze jumps (about 19° to the right). Below this many degrees down, the sideways gaze (both eyes and combined) fades into its value from just before, fully held 10° further down. The angle is the tracker's own, before the zero point and gains. Up and down are not affected. `0` turns it off |
| `gaze_debug_dots` | | `false` | Debug: show a small dot 1 m ahead (`gaze_debug_dots_distance_m`) where the gaze being sent points (one per eye, from each eye, with `independent_eyes`: left cyan, right orange), so you can see what the avatar gets. frameeyeosc then passes every processed sample to the panel over a Unix socket in its status folder; nothing leaves the headset, and nothing is passed while it's off. Hidden during an eye fit |
| `lid_fit_closed_left` ... `lid_fit_down_right` | | `null` | Each eye's Frame openness with the eyes shut, and open while looking up, straight ahead and down (`closed` / `up` / `open` / `down`, `_left` / `_right`). Set by the eye fit; `null` = not fitted. A fitted eye uses these instead of the learned calibration, and doesn't close when you look down; `lid_scale_*` then fine-tunes the result |
| `calibration_reset` | | `0` | Increase it to make the eyelid calibration start over |
| `language` | | Steam's language | Panel language, `"ja"` or `"en"`. Without it, the panel is in Japanese if Steam is set to Japanese and in English otherwise |
| `gaze_debug_dots_distance_m` | | `1.0` | How far ahead the debug gaze dots are (0.3–2.0 m, "Dot distance" on the Advanced tab), the same with the dashboard open or closed. Beyond about 1.2 m the open dashboard hides them. The panel uses it; frameeyeosc ignores it |
| `fit_sounds` | | `true` | The panel plays short sounds during the eye fit. frameeyeosc itself ignores it |
| `auto_recenter` | | `"center"` | What the panel fits by itself once each time the headset is put on (when the gaze is fitted): `"center"` straight ahead only (one dot, 2.5 seconds), `"tilt"` straight ahead and the tilt (straight ahead, up and down, about 7.5 seconds), `"off"` nothing. The button next to "Fit again" runs the same (`"center"` when it is off). frameeyeosc itself ignores it |
| `update_check` | | `true` | The panel looks for a new release on GitHub at start and once a day (an hour later after a failed check). frameeyeosc itself ignores it |

Command-line options win over the file. They go in `~/.config/frameeyeosc/env` (then `systemctl --user restart frameeyeosc`):

```sh
FRAMEEYEOSC_ARGS="--gaze-min-cutoff 0.3 --lid-sync 0.6"
```

Whatever is set there can't be changed from the file, and the panel shows it as "Locked by command line". Run `~/.local/bin/frameeyeosc --help` for all options, including `--config` for another settings file.

## Native VRChat eye tracking

With `"native_eyes": true` (or `--native-eyes`), frameeyeosc in VRChat mode also sends VRChat's own eye tracking input next to the VRCFT parameters. It moves the eyes and eyelids set up under Eye Look in the avatar descriptor, so avatars without VRCFT parameters follow your eyes too, with nothing added to the animator, and an avatar that already has Eye Look set up needs no re-upload. It is off by default; switch it on the Output tab ("VRChat's own eyes too", with "VRChat" as the output).

- `/tracking/eye/CenterVec`: the gaze as sent (smoothed, fitted), as a direction; `/tracking/eye/LeftRightVec` with `independent_eyes`.
- `/tracking/eye/EyesClosedAmount`: both eyelids as sent, averaged into one value (0 open, 1 closed). VRChat takes one value for both eyes and nothing for widening, so a wink closes both eyes halfway and widened eyes are just open. Use an avatar with VRCFT parameters for those.

What it does to an avatar built for VRCFT depends on that avatar's animator (Tracking Control for Eyes & Eyelids). Most of them hand their eyes to animation while `EyeTrackingActive` is true and keep following the VRCFT parameters. An avatar that also has Eyelids set up under Eye Look, though, can close its eyelids twice as far; turn `native_eyes` off for it (the same switch on the Output tab).

Keep SteamVR's own Steam Link OSC off while this is on: it sends the same `/tracking/eye/*` addresses, and the two would fight.

When tracking stops, sending is paused or the output changes, relaxed open eyes looking ahead are sent once; VRChat has no "not active" for this input and returns the eyes to its automatic eye movement after its own timeout.

frameeyeosc sends this only in VRChat mode. In the ETVR and LiveLink modes there is no need: VRCFaceTracking itself sends VRChat's eye tracking input to avatars that have no VRCFT eye parameters.

The avatar needs Eye Look set up in Unity (VRC Avatar Descriptor > Eye Look, with "Enable" pressed). If the eyes follow your gaze but never blink, Eyelids is usually what is missing:

- Eyes: the left and right eye bones under "Transforms", and under "Rotation States" how far the eyes turn for Looking Straight / Up / Down / Left / Right (the preview shows each one).
- Eyelids: "Eyelid Type" set to Blendshapes (or Bones, if the eyelids are moved by bones), the face mesh as "Eyelids Mesh", and the eyes-shut blendshape chosen for "Blink" (often named `vrc.blink`, `blink` or `Eye_Close`).

After changing these the avatar has to be uploaded again.

## Status file

frameeyeosc writes what it is doing to `$XDG_RUNTIME_DIR/frameeyeosc/status.json` (usually `/run/user/1000/frameeyeosc/status.json`) ten times a second: whether it is sending, the destination, messages per second, the eye tracker's samples per second (`tracker_rate`), how well frameeyeosc keeps up with them (`missed_rate`: samples the eye tracker published in the last second that frameeyeosc didn't read; `max_processing_ms`: the longest it took over one sample in the last second; `dropped_rate`: datagrams dropped in the last second because the network was too busy), the latest raw and sent values, the calibration, the settings in effect, which of them are locked by the command line, any config error, why the eye data can't be read if it can't (`source_error`), which eye the Frame tracks alone while the "Track Dominant Eye Only" setting is on (`dominant_eye`: `"left"` or `"right"`; `null` while it is off), whether a relaxed open eye reads 1.0 so widening can't come through (`openness_saturated`), and the latest eye fit measurement. The panel reads it. The folder is readable only by you, lives in memory, and is gone after a reboot. Only the latest values are kept.

## Avatars made for Steam Link's OSC

SteamVR's Steam Link (SteamVR 2.18) sends the Frame's eye tracking to VRChat by itself, under other names than VRCFaceTracking's: `LeftEyeX` instead of `FT/v2/EyeLeftX`, for example. An avatar set up for those doesn't move with frameeyeosc's VRCFaceTracking names. Turn on "Steam Link names too" on the Output tab (`"steamlink_params": true` or `--steamlink-params`) and frameeyeosc sends them as well, after the VRCFaceTracking ones and with each sample. VRChat ignores parameters an avatar doesn't have, so the other set does no harm. Only with "VRChat" as the output.

They carry frameeyeosc's values (smoothed, with the eye fit, blink hold and so on), in Steam Link's conventions as measured on SteamVR 2.18.2:

| Parameter | Type | Value |
|---|---|---|
| `LeftEyeX`, `RightEyeX` | float | The gaze sideways, 1 = 45° right (the same as `EyeLeftX`) |
| `LeftEyeY`, `RightEyeY` | float | The gaze up or down, 1 = 45°, **positive down** (the opposite of `EyeLeftY`, as Steam Link sends it) |
| `LeftEyeLid`, `RightEyeLid` | float | How closed the eye is: 0 open (relaxed open or widened), 1 shut (the opposite way round from `EyeLidLeft`) |
| `LeftEyeLidExpandedSqueeze`, `RightEyeLidExpandedSqueeze` | float | 0.0 while the eye is more than half closed, else 0.8 |
| `LeftEyeSqueezeToggle`, `RightEyeSqueezeToggle` | int | 1 while the eye is more than half closed, else 0 |
| `LeftEyeWidenToggle`, `RightEyeWidenToggle` | int | Always 1, as Steam Link sends it |

- Both eyes get the shared gaze, or each its own with "Move eyes separately" (Steam Link always sends the shared one).
- The names are never prefixed: Steam Link sends them as `/avatar/parameters/LeftEyeX`, whatever `prefix` says.
- Steam Link's `/tracking/eye/...` and `/sl/...` messages (VRChat's own eye tracking) aren't sent.
- Keep Steam Link's own OSC output turned off, as described under Install: with both running, the same parameters come from two sources and fight over the avatar's eyes.

## VRCFaceTracking (ETVR) mode

frameeyeosc can send in the format that the ETVR Tracking Module for VRCFaceTracking reads. VRCFaceTracking then drives the avatar, so the Frame's eyes can be combined with other trackers such as a mouth tracker. The ETVR Tracking Module is a third-party module ([EyeTrackVR/ETVRTrackingModule](https://github.com/EyeTrackVR/ETVRTrackingModule)); frameeyeosc is not part of it.

1. On the PC, install VRCFaceTracking and add the ETVR Tracking Module from its module registry. By default it listens on UDP 8889.
2. Choose "VRCFT (ETVR)" under "Send to" on the panel's Basic tab, or set `"output": "etvr"` (or `--output etvr`). The destination works as usual (the Steam Link PC or a fixed host) on port 8889.

Notes:

- It sends six values: `EyeLeftX`, `EyeLeftY`, `EyeRightX`, `EyeRightY`, `EyeLidLeft`, `EyeLidRight`. `EyeX` / `EyeY` are left out, because receiving them puts the module into a single-eye mode that reads an eyelid value that isn't sent, and the eyelids freeze open.
- The module treats eyelid 1.0 as a relaxed open eye, so widened eyes don't come through in this mode (values stop at 1.0).
- The module smooths the eyelids itself. When you switch in the panel, it offers lighter eyelid smoothing on the frameeyeosc side. Because of that smoothing, a blink held closed for `blink_hold_ms` may not reach fully closed on the avatar; raise it (for example to 120) if short blinks still look half-closed.
- After VRCFaceTracking starts, its window can show "Not Responding" for close to two minutes while the module loads. It isn't broken; wait.
- The PC has to accept UDP 8889. VRCFaceTracking's ModuleProcess usually has an inbound firewall rule already.

## VRCFaceTracking (LiveLink) mode

frameeyeosc can also send Live Link Face packets (the format of Epic's Live Link Face iPhone app) to VRCFaceTracking's LiveLink module. Unlike the ETVR mode, widened eyes come through, so avatars that take their eyelids from VRCFaceTracking, including avatars with binary (bit-packed) parameters, show them too. The LiveLink module comes from the VRCFaceTracking project ([VRCFaceTracking/LiveLinkTrackingModule](https://github.com/VRCFaceTracking/LiveLinkTrackingModule)); frameeyeosc is not part of it.

1. On the PC, install VRCFaceTracking and add the "LiveLink" module from its module registry. Turn off or remove other eye tracking modules (such as the ETVR Tracking Module), so the eyes come from the LiveLink module. It listens on UDP 11111.
2. Choose "VRCFT (LiveLink)" under "Send to" on the panel's Basic tab, or set `"output": "livelink"` (or `--output livelink`). The left column then shows "VRCFT (LiveLink) → …", and the Output tab lists these steps. The destination works as usual (the Steam Link PC or a fixed host) on port 11111.
3. Let UDP 11111 in through Windows Defender Firewall. Without it nothing arrives. Two things trip people up:
   - The LiveLink module runs inside `VRCFaceTracking.ModuleProcess.exe`, not `VRCFaceTracking.exe`, so allowing only VRCFaceTracking isn't enough.
   - With Steam Link's wireless adapter, its network usually shows as "Unidentified network", which Windows treats as Public. A rule for the Private profile only doesn't cover it.

   A rule by port covers both (PowerShell as administrator):

   ```powershell
   New-NetFirewallRule -DisplayName "VRCFT LiveLink (UDP 11111)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 11111 -RemoteAddress LocalSubnet -Profile Private,Public
   ```

Notes:

- Each eye's eyelid, widening and gaze are sent (the ARKit shapes EyeBlink and EyeWide, and the eye's yaw and pitch). VRCFaceTracking's eyelid then comes out the same as in VRChat mode (0 closed, 0.75 relaxed, 1 widened), and so does the gaze. Squint, mouth, brows and head are sent as 0.
- The module doesn't smooth anything, so frameeyeosc's own smoothing settings apply as they are.
- It is sent at up to 50 packets a second (always the newest sample): the module reads one packet every 10-16 ms, and sending every eye sample (90 or more a second) made the eyes lag more and more.
- VRCFaceTracking keeps the last values it got. So when the eye data stops (the headset comes off) or you pause or switch the output, frameeyeosc sends relaxed open eyes looking straight ahead once. While sending is on without eye data, it repeats that twice a second: the module only starts if something arrives within 180 seconds of VRCFaceTracking loading it (if it gave up, reload the module in VRCFaceTracking). Paused, nothing is sent.
- `prefix`, `eye_tracking_active` and `steamlink_params` don't apply: VRCFaceTracking sends the avatar parameters.

## Eye fit

If the avatar's eyes look a little off (looking too far down, or eyelids that close when you look down), the panel's "Eye fit" tab fits them to you. Press "Fit my eyes", then close the dashboard:

1. A dot appears straight ahead, then 15° up, 15° down, 20° left and 20° right, 2.5 seconds each. Keep your head still and follow it with your eyes. The ring around the dot runs down, and the seconds being measured (2, 1) show under it.
2. Then the target says "Close your eyes for 3 s" and counts down 3, 2, 1. Close them at the end of the count and keep them closed for 3 seconds, until the chime. It then says "Open them".

It takes about 20 seconds. Opening the dashboard stops it. Soft sounds mark each step, so you can follow it without watching the panel: a pop when a dot is in place, a pip when it's measured, a low buzz when it's measured again, a tick for each of 3, 2, 1, a chime when you can open your eyes, and a rising chime at the end (two falling tones if it stops). Turn them off with "♪ Sounds" on the tab. A step where the gaze is unsteady (or the eyes aren't shut in the last step) is measured again, up to three times. The result shows on the tab. From then on the one button there says "Fit again", and "Reset" undoes the fit. Straight ahead shifts a little each time the headset is put on, so once the gaze is fitted the panel measures it again by itself: about 3 seconds after you put the headset on (with the dashboard closed), a dot shows straight ahead for 2.5 seconds; look at it. The gains, the tilt and the eyelids stay as they are. The "When put on" row on the tab chooses that ("Re-center", the default), "Re-center + tilt" (the dot, then up and down, 2.5 seconds each, and the tilt measured again) or "Nothing" (`auto_recenter`), and the button next to "Fit again" runs the same by hand (re-center only when it is "Nothing"). Re-centering only is the default because the tilt measured this way scatters by about ±5° from one try to the next (+3.1°, -8.9°, +2.6° and +0.6° within three minutes of one wearing), about as much as it corrects. The values can also be changed by hand under "Fine-tune".

What the fit sets:

- Gaze: where straight ahead is (`gaze_offset_x` / `gaze_offset_y`) and how far the gaze moves sideways, up and down (`gaze_gain_x`, `gaze_gain_up`, `gaze_gain_down`), so that looking 15° up sends 15° up.
- The headset's tilt (`gaze_roll_deg`), from how the move from the down dot to the up one leans: when the headset sits tilted, looking sideways also moves the gaze up or down (at 8°, about 3° for 20° sideways). Between wearings it was seen from +1.1° to +8.4°. The line between the side dots scattered more from fit to fit (three fits in a row: -3.8°, +2.7°, -2.1° from the sides against +2.0°, +2.4°, +1.7° from up/down), so it is only logged next to it. frameeyeosc turns the gaze back by it around straight ahead, before the gains.
- Each eye's sideways gaze, for "Move eyes separately" (`gaze_offset_x_left/right`, `gaze_gain_x_left/right`). The dots are 2 m away, so each eye's true angle to a dot is not the angle from between the eyes: seen from between the eyes a dot straight ahead is at 0°, but the left eye turns about 0.9° right and the right eye about 0.9° left to see it (with a 63 mm distance between the eyes). Each eye is fitted to its own angles, using the distance between the eyes that SteamVR reports (63 mm if it doesn't), so the avatar's eyes turn in naturally.
- Eyelids: each eye's openness with the eyes shut, and open while looking up, straight ahead and down (`lid_fit_*`). The Frame reads an eye as less open when you look down (about 30% less 20° down), so a fitted eye is judged against what is normal for where you look, and doesn't close when you only look down. It is never expected to read more open than straight ahead (fits often measure an eye as more open looking up or down, which hour-long recordings did not bear out: a relaxed eye was then sent half closed hundreds of times an hour), and beyond the 15° up and down dots the reading measured there holds. A fitted eye counts as closed below 30% of the way from its shut to its open reading. The learned calibration is not used for fitted eyes, and the lid marks give way to the fit. If one eye still looks too open or too closed, "Eye scales" on the Eyelids tab fine-tunes it after the fit (`lid_scale_*`: 0.9 = 10% less open); a new full fit starts it over at 1.0. On a recording, the fit cut the times an eye looking down was sent a third closed from 35 to 5, and more blinks were sent fully closed (53 of 60, from 50).
- Widening can't be measured: the Frame's openness rises only about 0.05 when the eyes are opened wide (two fits measured +0.019 / -0.009 and +0.048 / +0.047), and it stops at 1.000. So a fitted eye widens by "Widen" on the Eyelids tab (`lid_widen`), counted from its own expected open reading for where you look: "Less" from 0.10 above it (full at 0.18), "Normal" from 0.07 (full at 0.14), "More" from 0.04 (full at 0.10), "Off" never. A relaxed eye wanders above its usual reading too; over the recordings so far it read visibly widened (40% of the way or more) by accident 2.4% (Less), 6.3% (Normal) or 14.9% (More) of the time. An eye whose straight-ahead reading leaves no room below 1.000 (one user's left eye read 0.945) widens with the other eye; with neither, nothing widens. A widen shows once it has lasted a quarter of a second: just before a blink the Frame's openness often jumps up for a moment (to 1.000 at times), which would otherwise flash wide eyes before the eyes close. Since SteamOS 0.4.3 a relaxed open eye already reads 1.000, so widening can't come through there (see [Troubleshooting](#troubleshooting)).

The dot is fixed to the headset 2 m ahead and only shows while the dashboard is closed. How it works: the panel writes a `gaze_capture` request into `config.json`, frameeyeosc averages the tracker's gaze and each eye's openness for as long as the panel asks (2 seconds skipping the first 0.3 s for a dot, 3 seconds skipping the first 0.5 s with the eyes shut; samples with the eyes shut are skipped except in the last step) and reports the averages in the status file with how many samples came in and at what rate. A dot counts when at least 60% of the samples that came in were usable (at least 12, and never more than 45 are needed), since the tracker's rate varies (90 to 136 a second while streaming, 15 has been seen), and its gaze spread is at most 2.7°. The panel turns them into the settings. Each measurement is logged (`journalctl --user -u frameeyeosc`). With the defaults nothing changes.

## Calibration

Eyelid calibration is automatic. For the first 20 seconds after you put the headset on nothing is learned; after that each eye's relaxed openness is picked up within about 10 seconds and then follows slowly (the last ~10 minutes count most), so a short squint barely moves it. The result is saved every minute to `~/.config/frameeyeosc/calibration` and reused next time. To start over, press Reset in the panel (or increase `calibration_reset`).

## Troubleshooting

- Logs: `journalctl --user -u frameeyeosc -f` (the panel: `journalctl --user -u frameeyeosc-panel -f`)
- `No Steam Link connection found; waiting for one`: Steam Link isn't streaming yet, or set a fixed host.
- `Can't send OSC to ... yet (Network is unreachable)` or `Sending OSC to ... failed (...)`: the network isn't up yet (for example Wi-Fi right after boot) or the PC can't be reached. frameeyeosc keeps running and tries again; `... works again` follows once it can send.
- The left column shows "Eye data" as low (red, below 60 a second; 46 and 15 have been seen instead of 90), or the eye fit fails at the first dot with few samples: the line under the rate says which side was slow. "frameeyeosc can't keep up": frameeyeosc missed samples the eye tracker published, or took too long over one. "The Frame itself sends few": the eye tracker delivered fewer samples than usual, and frameeyeosc read them all. After 10 seconds of a low rate frameeyeosc logs one line with the numbers (`Eye data has been low for 10 s: …` in `journalctl --user -u frameeyeosc`). Please report that line, and whether the PC was streaming over Steam Link at the time.
- `Dropped … datagrams to … the network was too busy to take them at once`: the network (often the Wi-Fi, full with Steam Link's video) couldn't take the messages as fast as they were sent. frameeyeosc never waits for it, so the eye data keeps coming in; those messages are simply not sent. The line comes at most once a minute.
- The log says `Sending OSC to ...` but the avatar doesn't react: check that OSC is enabled in VRChat, then check Windows Firewall. VRChat's own inbound rule is often allowed for the "Public" profile only, so OSC from a "Private" home network gets dropped. Note the rule must be for `VRChat.exe`, not `launch.exe`. A narrow rule that fixes it (PowerShell as administrator):
  ```powershell
  New-NetFirewallRule -DisplayName "VRChat OSC (LAN UDP 9000)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 9000 -RemoteAddress LocalSubnet -Program "C:\Program Files (x86)\Steam\steamapps\common\VRChat\VRChat.exe" -Profile Private,Public
  ```
  The rule covers both profiles because the bundled wireless adapter shows up in Windows as its own network, usually with the "Public" profile, while a home network is often "Private".
- The panel says "Can't read eye data: …": frameeyeosc is running but can't read the eye tracker, and tries again every second (the reason is also logged once, in `journalctl --user -u frameeyeosc`). Right after the headset boots (`… No such file or directory`) this is harmless: the eye tracker isn't up yet. "unsupported eye shared-memory version" means a SteamOS update changed what frameeyeosc reads; see the [Disclaimer](#disclaimer).
- The eyes never widen on SteamOS 0.4.3, and the Eyelids tab says the openness tops out at 1.0: since SteamOS 0.4.3 the Frame reads a relaxed open eye as 1.000, the highest it goes (one user's left eye, both eyes open: a median 0.754 before, 1.000 after, 75-93% of the time), so there is nothing above it to widen by. frameeyeosc tells this from the readings, not from the version (over the last minute with both eyes open, more than half the samples with an eye at 1.000; `openness_saturated` in the [status file](#status-file)), so the note goes away by itself if a later SteamOS changes it (from the next frameeyeosc start, such as after the update's reboot: once on, it stays on while frameeyeosc runs, because the share drifts with where you look). Meanwhile no eyelid is sent above relaxed open, so an eye without a fit (whose 1.000 would land past mark 4) doesn't look wide all the time either; blinks and closing work as before, and "Widen" keeps its setting for when widening can come through again.
- Both eyes look the same way, or one eye's gaze seems to follow the other: the "Track Dominant Eye Only" setting (VR Settings > General, advanced; SteamOS 0.4.3) is on. The Frame then tracks that eye alone and gives both eyes its gaze (the eyelids stay each eye's own). The left column of the panel says so ("Frame setting: tracking the right eye only"); turn the setting off to track both eyes. frameeyeosc only reads this setting, it never changes it.
- Nothing moves while the headset is off your face: expected, the Frame only tracks while worn.
- The panel says "frameeyeosc is not running": check `systemctl --user status frameeyeosc`. Changes made in the panel are still saved and apply once it runs.

## Known issues

- Avatars that use binary (bit-packed) VRCFT parameters are not supported when sending to VRChat directly. In the ETVR and LiveLink modes, the avatar side is up to VRCFaceTracking; use the LiveLink mode for widened eyes.

## Privacy

- frameeyeosc sends gaze and eyelid values only to the destination above (your PC). It has no telemetry and doesn't talk to the internet.
- The panel asks GitHub (`api.github.com`) for the latest release at start and at most once a day (an hour after a failed check), unless "Check for updates" is off. Like any web request, this shows GitHub your IP address. Nothing else is sent, and downloads only come from GitHub.
- On disk it keeps:
  - your settings (`~/.config/frameeyeosc/config.json`) and two numbers, each eye's learned relaxed openness (`~/.config/frameeyeosc/calibration`)
  - from `install.sh`: the update script `~/.local/share/frameeyeosc/frame-update.sh` and your install options `~/.config/frameeyeosc/install-args`
  - from the update check and updates, in `~/.cache/frameeyeosc/`: `update-check.json` (GitHub's last answer), `update-state.json` (progress of the last update), `update.log` (log of the last update), the `update/` work folder (emptied after each run, except for the copy of the update script it keeps), and the `update.lock/` folder while a check or update runs

  - from the panel, in `$XDG_RUNTIME_DIR/frameeyeosc/` (in memory, gone after a reboot): the eye fit's sound files in `sounds/`, written at start

  No eye data is stored, except that each eye fit measurement (an average gaze direction, how much it spread, and each eye's average openness) is logged as one line to the systemd journal, and the eyelid readings of a fit are kept in `config.json`. The latest eye values are in the status file, which is in memory, readable only by you, and overwritten ten times a second; no history is kept.
- The OSC messages are unencrypted, so other devices on the same network could read them.
- With "Show gaze dots" on, the gaze being sent also goes to the panel over a Unix socket (`gaze-dots.sock` in the status folder). It stays on the headset and isn't stored.

## Disclaimer

- Use at your own risk. The changes in this fork were made with Claude Opus 5.5, an AI model. I've tested them with unit tests and on my own Steam Frame, but I can't take responsibility for what happens on yours, so please read the code and check it yourself before you run it. The software comes with no warranty (see [LICENSE](LICENSE)).
- It reads the eye tracker's private, undocumented shared-memory layout (versions 4 and 5; 5 came with SteamOS 0.4.3). A SteamOS update can change that layout. If it does, frameeyeosc can't send eye data until it is updated. It keeps running meanwhile and tries again every second, and the panel shows "Can't read eye data: unsupported eye shared-memory version …".
- It needs no root and doesn't change any SteamOS files or settings. The only thing it writes is a "send me the next sample" flag in the eye tracker's shared memory, and it takes the lock there the same way the tracker's own clients do. The panel only writes frameeyeosc's settings file.
- Reading Valve's undocumented internal data may conflict with the Steam Subscriber Agreement, which restricts reverse engineering. Decide for yourself whether you're comfortable with that before using it.
- This is an unofficial project with no affiliation with or endorsement from Valve Corporation, VRChat Inc., the VRCFaceTracking project or the EyeTrackVR project. Steam, Steam Frame, SteamVR and Steam Link are trademarks of Valve Corporation, and VRChat is a trademark of VRChat Inc. The names are used here only to say what this works with.

## Development

Build and test on the headset (the binary must link against the headset's glibc, and the panel against SteamVR's OpenVR library; see `scripts/package.sh`):

```sh
cargo test --release
cmake -G Ninja -S panel -B panel/build && ninja -C panel/build
panel/build/gaze-fit-test   # the panel's eye fit logic
scripts/package.sh   # builds dist/frameeyeosc-<version>-steamframe-aarch64.tar.gz with both, and dist/SHA256SUMS
```

`vendor/frame-updater/` is a copy of the update checker shared by my Steam Frame apps. Don't edit it here: `scripts/package.sh` stops if it differs from what the copy's `MANIFEST.sha256` records.

To publish a release, attach both files. The panel's "Update" button refuses releases without `SHA256SUMS` and asks for a manual update instead. The release notes are the version's section of `CHANGELOG.md` (without its heading): it starts with a one-paragraph English summary, then a paragraph starting with `日本語: ` with the Japanese summary, then the list. A panel running an older version (0.7.1 or later) shows the summary while that release is available, the Japanese one on a Japanese panel, without markdown and cut at 300 characters:

```sh
gh release create v0.4.0 --title v0.4.0 --notes-file notes.md
gh release upload v0.4.0 dist/frameeyeosc-0.4.0-steamframe-aarch64.tar.gz dist/SHA256SUMS
```

To tune the eye processing against real data, record the eye tracker's raw samples (nothing is sent while recording, so it can run next to the service), then replay the file. The replay prints a few numbers for the current settings next to the same settings with the 0.4.0 steps turned off; settings come from `config.json` and options as usual. Recordings are personal data, so keep them out of the repository.

```sh
frameeyeosc --record ~/eyes.csv              # stop with Ctrl+C (or SIGINT / SIGTERM); the file is complete up to the stop
frameeyeosc --replay ~/eyes.csv --blink-hold-ms 120 --replay-out ~/processed.csv   # also writes the processed values
```

The panel can make the same recording while you play: "Eye log" on the Advanced tab starts it and turns into "Stop 1:23"; the left column shows "Recording 1:23" in red meanwhile. It stops by itself after 60 minutes, and when the panel exits. The files go to `~/.local/share/frameeyeosc/recordings/`: `eyes_YYYY-MM-DD_HH-MM-SS.csv` (about 2.3 MB a minute), the `config.json` in use as `eyes_….config.json`, and the recorder's messages as `eyes_….log`. Nothing leaves the headset; delete them when you no longer need them.

## License

MIT. See [LICENSE](LICENSE); the original work is by konsti219. `vendor/frame-updater/` is not third-party code: it is sasaken1102r's own update checker, shared by their Steam Frame apps and copied here under this repository's MIT license. Licenses of the bundled Rust crates and of the OpenVR SDK header used by the panel are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Changes are listed in [CHANGELOG.md](CHANGELOG.md).

## Thanks

Thanks to konsti219 for frameeyeosc and for finding where the Frame keeps its eyelid data. This fork is built on that work.
