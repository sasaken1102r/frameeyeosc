# Outputs

[Back to the README](../README.md)

## Compared with Steam Link's own OSC

https://github.com/user-attachments/assets/200526ab-4733-4072-8230-8d83ea14653d

SteamVR's Steam Link can send eye tracking over OSC by itself (`LeftEyeX`, `LeftEyeLid`, ...). Compared on the same eye recording (SteamVR 2.18.2):

| | frameeyeosc 0.7.2 | SteamVR's own OSC |
|---|---|---|
| Gaze | Can turn the eyes in a little ("move eyes separately": as if looking about 2 m away) | One gaze for both eyes |
| Gaze smoothing | One Euro smoothing, strength selectable | None (raw values) |
| Eyelids | Each eye's relaxed open lined up; blinks held, left/right synced | Raw openness turned over (in 1/254 steps) |
| Blinks and winks | Yes | Yes |
| Widened eyes | Yes, with the eye cameras (SteamOS 0.4.3 too) | No |
| Pupil size | Yes, with the eye cameras: smaller in bright places, bigger in the dark | No |
| Squints | Yes, with the eye cameras after the optional user calibration | No |
| Brows | Optional since 0.8.0: raised when you widen your eyes | No |
| Setup | Install on the Frame (the eye cameras usually need no sudo; if that fails, a one-time sudo step) | Nothing to install: a SteamVR setting and a VRCFaceTracking module |

The video shows the avatar's eyes in VRChat with the eye cameras in use. See [Eye cameras](eyecam.md#eye-cameras) for the setup.

## Avatar parameters

An avatar with VRCFaceTracking eye parameters (`FT/v2/EyeLeftX`, `EyeLidLeft`, ...) as floats. Avatars that pack parameters into binary bits are not supported when sending to VRChat directly, except for the eye cameras' pupils (see [Pupils on avatars with bit parameters](eyecam.md#pupils-on-avatars-with-bit-parameters)). frameeyeosc also sends `EyeTrackingActive` as a bool; some avatars declare it as a float and stop tracking on a bool. For those, choose "Float" under "EyeTrackingActive type" on the Output tab (`eye_tracking_active`), or "Off" to not send it at all. Avatars set up for the OSC that SteamVR's Steam Link sends by itself (`LeftEyeX`, `RightEyeLid`, ...) work too: turn on "Steam Link names too" on the Output tab (see [Avatars made for Steam Link's OSC](outputs.md#avatars-made-for-steam-links-osc)). Avatars without these parameters can follow your eyes through VRChat's own eye tracking input instead (see [Native VRChat eye tracking](outputs.md#native-vrchat-eye-tracking)).

## Native VRChat eye tracking

With `"native_eyes": true` (or `--native-eyes`), frameeyeosc in VRChat mode also sends VRChat's own eye tracking input next to the VRCFT parameters. It moves the eyes and eyelids set up under Eye Look in the avatar descriptor, so avatars without VRCFT parameters follow your eyes too, with nothing added to the animator, and an avatar that already has Eye Look set up needs no re-upload. It is off by default; switch it on the Output tab ("VRChat's own eyes too", with "VRChat" as the output).

- `/tracking/eye/CenterVec`: the gaze as sent (smoothed, fitted), as a direction; `/tracking/eye/LeftRightVec` with `independent_eyes`.
- `/tracking/eye/EyesClosedAmount`: both eyelids as sent, averaged into one value (0 open, 1 closed). VRChat takes one value for both eyes and nothing for widening, so a wink closes both eyes halfway and widened eyes are just open. Use an avatar with VRCFT parameters for those.

What it does to an avatar built for VRCFT depends on that avatar's animator (Tracking Control for Eyes & Eyelids). Most of them hand their eyes to animation while `EyeTrackingActive` is true and keep following the VRCFT parameters. An avatar that also has Eyelids set up under Eye Look, though, can close its eyelids twice as far; turn `native_eyes` off for it (the same switch on the Output tab).

Keep SteamVR's own Steam Link OSC off while this is on: it sends the same `/tracking/eye/*` addresses, and the two would fight.

When tracking stops, sending is paused or the output changes, relaxed open eyes looking ahead are sent once; VRChat has no "not active" for this input and returns the eyes to its automatic eye movement after its own timeout.

frameeyeosc sends this only in VRChat mode. In the LiveLink and FrameEyeOSC modes there is no need: VRCFaceTracking itself sends VRChat's eye tracking input to avatars that have no VRCFT eye parameters.

The avatar needs Eye Look set up in Unity (VRC Avatar Descriptor > Eye Look, with "Enable" pressed). If the eyes follow your gaze but never blink, Eyelids is usually what is missing:

- Eyes: the left and right eye bones under "Transforms", and under "Rotation States" how far the eyes turn for Looking Straight / Up / Down / Left / Right (the preview shows each one).
- Eyelids: "Eyelid Type" set to Blendshapes (or Bones, if the eyelids are moved by bones), the face mesh as "Eyelids Mesh", and the eyes-shut blendshape chosen for "Blink" (often named `vrc.blink`, `blink` or `Eye_Close`).

After changing these the avatar has to be uploaded again.

## VRChat Face Tracking

With `"vrc_face_tracking": true` (or `--vrc-face-tracking`), frameeyeosc in VRChat mode also sends VRChat's Face Tracking input, `/tracking/face/v1`: one blob of 102 bytes, one for each Unified Expressions tracking key. It drives the blendshapes and eye bones an avatar maps in its descriptor's Face Tracking section, with no VRCFT parameters and no VRCFaceTracking on the PC, and unlike `native_eyes` it carries each eye on its own: winks, widening, squints and pupils. It is off by default; switch it on the Output tab ("Send to Face Tracking too", with "VRChat" as the output). VRChat's Face Tracking is new (VRChat's beta docs, October 2026), so it needs a VRChat and an SDK that have it.

What goes out, per eye:

- `EyeClosed` from the eyelid as sent (1 shut, 0 from relaxed open up), and `EyeWide` from the part above relaxed open.
- `EyeLookOut` / `In` / `Up` / `Down` from the gaze as sent (each eye's own with "Move eyes separately", else the shared one), 1.0 = 45°: the same values as `v2/EyeLeftX` / `Y`.
- With the eye cameras: `EyeSquint`, and the pupil as `EyeDilation` (wider than usual) or `EyeConstrict` (narrower).
- With `brow_from_widen`: `BrowInnerUp` and `BrowOuterUp`.

Every other key goes out as 0. When tracking stops, sending is paused or the output changes, a blob with every key 0 (eyes relaxed open, looking ahead) is sent once.

Setting the avatar up in Unity (VRC Avatar Descriptor > Face Tracking, with "Enable" pressed):

- Add the face mesh under the Face Tracking meshes.
- Map only the keys frameeyeosc sends. "Auto-Fill" with `eye` as the "Auto-Fill Filter" fills the eye keys; check what it picked (it can also take names like `EyePupil_...`). Keys left at None touch nothing, so the zeros for the mouth and the rest change nothing: a mouth moved by gestures or other animator layers keeps moving.
- Eyelids: "Eyelid Type" Face Tracking.
- Gaze: "Eye Gaze Mode" Bone Rotation turns the eye bones by the EyeLook keys, 1.0 reaching the Rotation States under Eye Look. Both adds the eyeLook blendshapes on top of the bones, which can turn the eyes too far.
- Pupils need "Use All Tracking Keys". (In SDK 3.10.6-beta.2 that checkbox sometimes doesn't toggle; the Inspector's Debug mode can change it.)
- Brows on a Perfect Sync (ARKit) face: both `BrowInnerUpLeft` and `BrowInnerUpRight` to `browInnerUp` (VRChat takes the larger of the two), and `BrowOuterUpLeft` / `Right` to `browOuterUpLeft` / `Right`.
- Avatar Optimizer: Trace and Optimize with "Optimize BlendShape" removes the blendshapes only the Face Tracking section uses. VRChat's log then says `Avatar Face Tracking Blendshape '...' not found on any mesh`, and they don't move; turning that option off brings them back.

After changing these the avatar has to be uploaded again.

On such an avatar the blob wins over `/tracking/eye/*`, so `native_eyes` can stay on for avatars without the section. Each blob replaces the whole Face Tracking state, though: another program sending `/tracking/face/v1` (another face tracker) and frameeyeosc take turns on every key the avatar maps, so send it from only one of them.

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

- Both eyes get the shared gaze, or turned in a little with "Move eyes separately" (Steam Link always sends the shared one).
- The names are never prefixed: Steam Link sends them as `/avatar/parameters/LeftEyeX`, whatever `prefix` says.
- Steam Link's `/tracking/eye/...` and `/sl/...` messages (VRChat's own eye tracking) aren't sent.
- Keep Steam Link's own OSC output turned off, as described under Install: with both running, the same parameters come from two sources and fight over the avatar's eyes.

## VRCFaceTracking (LiveLink) mode

frameeyeosc can also send Live Link Face packets (the format of Epic's Live Link Face iPhone app) to VRCFaceTracking's LiveLink module. Widened eyes come through, so avatars that take their eyelids from VRCFaceTracking, including avatars with binary (bit-packed) parameters, show them too. The LiveLink module comes from the VRCFaceTracking project ([VRCFaceTracking/LiveLinkTrackingModule](https://github.com/VRCFaceTracking/LiveLinkTrackingModule)); frameeyeosc is not part of it.

1. On the PC, install VRCFaceTracking and add the "LiveLink" module from its module registry. Turn off or remove other eye tracking modules (such as FrameEyeOSC), so the eyes come from the LiveLink module. It listens on UDP 11111.
2. Choose "VRCFT (LiveLink)" under "Send to" on the panel's Basic tab, or set `"output": "livelink"` (or `--output livelink`). The left column then shows "VRCFT (LiveLink) → …", and the Output tab lists these steps. The destination works as usual (the Steam Link PC or a fixed host) on port 11111.
3. Let UDP 11111 in through Windows Defender Firewall. Without it nothing arrives. Two things trip people up:
   - The LiveLink module runs inside `VRCFaceTracking.ModuleProcess.exe`, not `VRCFaceTracking.exe`, so allowing only VRCFaceTracking isn't enough.
   - With Steam Link's wireless adapter, its network usually shows as "Unidentified network", which Windows treats as Public. A rule for the Private profile only doesn't cover it.

   A rule by port covers both (PowerShell as administrator):

   ```powershell
   New-NetFirewallRule -DisplayName "VRCFT LiveLink (UDP 11111)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 11111 -RemoteAddress LocalSubnet -Profile Private,Public
   ```

Notes:

- Each eye's eyelid, widening and gaze are sent (the ARKit shapes EyeBlink and EyeWide, and the eye's yaw and pitch). VRCFaceTracking's eyelid then comes out the same as in VRChat mode (0 closed, 0.75 relaxed, 1 widened), and so does the gaze. Squint, mouth and head are sent as 0, and so are the brows unless "Widen raises brows" is on (`brow_from_widen`; then BrowInnerUp and BrowOuterUpLeft / Right). With the eye cameras, the eyelid and widening include theirs.
- The LiveLink module carries no pupils. With the eye cameras, frameeyeosc sends their pupil size straight to VRChat on the same PC (port 9000, the same `v2/Pupil…` parameters as in VRChat mode, with `prefix`, up to 50 times a second); turn it off with "Send pupils straight to VRChat" on the Output tab (`pupils_to_vrchat`).
- The module doesn't smooth anything, so frameeyeosc's own smoothing settings apply as they are.
- It is sent at up to 50 packets a second (always the newest sample): the module reads one packet every 10-16 ms, and sending every eye sample (90 or more a second) made the eyes lag more and more.
- VRCFaceTracking keeps the last values it got. So when the eye data stops (the headset comes off) or you pause or switch the output, frameeyeosc sends relaxed open eyes looking straight ahead once. While sending is on without eye data, it repeats that twice a second: the module only starts if something arrives within 180 seconds of VRCFaceTracking loading it (if it gave up, reload the module in VRCFaceTracking). Paused, nothing is sent.
- `eye_tracking_active` and `steamlink_params` don't apply, and `prefix` only to the pupils above: VRCFaceTracking sends the avatar parameters.

## VRCFaceTracking (FrameEyeOSC) mode

FrameEyeOSC is a VRCFaceTracking module made for frameeyeosc (its own repository, VRCFT-FrameEyeOSC), a separate download for the PC. Like the LiveLink mode, widened eyes come through; on top of that it carries the eye cameras' squint and pupil size, so VRCFaceTracking sends the pupils too, in whatever form each avatar takes them (a float or bit-packed), and there is no "How the avatar takes pupils" to choose per avatar.

https://github.com/user-attachments/assets/56033262-057f-4dde-a511-cee9349f6a3e

1. On the PC, download the module's zip (`FrameEyeOSC_<version>.zip`) from [its releases page](https://github.com/sasaken1102r/VRCFT-FrameEyeOSC/releases). In VRCFaceTracking, open the Module Registry, press "Install from .zip" and pick the zip (the module isn't in the registry's list yet). Turn off or remove other eye tracking modules (such as LiveLink). It listens on UDP 11112. (Without the installer: unzip it, copy `FrameEyeOSC.dll` into `%APPDATA%\VRCFaceTracking\CustomLibs` and restart VRCFaceTracking.)
2. Choose "VRCFT (FrameEyeOSC)" under "Send to" on the panel's Basic tab, or set `"output": "frameeyeosc"` (or `--output frameeyeosc`). The left column then shows "VRCFT (FrameEyeOSC) → …", and the Output tab lists these steps. The destination works as usual (the Steam Link PC or a fixed host) on port 11112.
3. Let UDP 11112 in through Windows Defender Firewall, as for LiveLink above (the module runs inside `VRCFaceTracking.ModuleProcess.exe` too):

   ```powershell
   New-NetFirewallRule -DisplayName "VRCFT FrameEyeOSC (UDP 11112)" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 11112 -RemoteAddress LocalSubnet -Profile Private,Public
   ```

Notes:

- Each sample goes out as one small UDP packet (76 bytes; the layout is in [docs/protocol.md](protocol.md)): per eye the gaze, the eyelid (0 closed, 0.75 relaxed, 1 widened, as in VRChat mode), and with the eye cameras the squint and the pupil (in mm and as a dilation). VRCFaceTracking's eyelids, gaze, squint and pupil parameters then come out the same as in VRChat mode.
- Every eye sample is sent (90 or more a second): the module takes in all that arrived and keeps the newest, so it doesn't fall behind.
- No pupils go straight to VRChat in this mode, so `pupils_to_vrchat` and `pupil_bits` don't apply, and neither do `prefix`, `eye_tracking_active` and `steamlink_params`.
- The module doesn't smooth anything, so frameeyeosc's own smoothing settings apply as they are.
- While sending is on without eye data (the headset off), frameeyeosc tells the module so twice a second; the module then shows relaxed open eyes looking straight ahead. It does the same when nothing arrives for a second (frameeyeosc paused or stopped). When the eye data stops, or you pause or switch the output, frameeyeosc tells it at once.

## Other apps

Next to the main output (VRChat, LiveLink or FrameEyeOSC), frameeyeosc can send the same eye data to up to 3 more apps: a second VRCFaceTracking with the FrameEyeOSC module, a relay, a tool that reads VRChat's eye input (for foveated rendering, say), or an app that reads VRCFaceTracking's parameter names. Each is an entry of `extra_outputs` in the settings file:

```json
{
  "extra_outputs": [
    { "name": "Bridge", "format": "feo1", "host": "192.168.0.10", "port": 11113 },
    { "name": "Gaze", "format": "osc", "host": "main", "port": 9015 }
  ]
}
```

| Key | Default | What it is |
|---|---|---|
| `name` | `"App 1"`, `"App 2"`, ... | Only for the panel and the log: up to 16 printable ASCII characters |
| `enabled` | `true` | `false` keeps the entry but sends nothing |
| `format` | (needed) | `"feo1"` or `"osc"` (below) |
| `host` | (needed) | `"main"`: the main output's host (the Steam Link PC while that is `"auto"`); `"auto"`: the Steam Link PC; or an IP address or host name, without a port |
| `port` | (needed) | 1 to 65535 |
| `bundle` | `false` | `osc` only: each sample's messages in OSC bundles (time tag "immediately", up to 1472 bytes each, so one or two UDP packets a sample) instead of one packet per message. For a receiver that is slow per packet, such as VRChat for Windows run on the Frame itself |
| `max_rate` | `null` | `osc` only: at most this many samples a second (1 to 1000; the eye tracker gives about 90), skipping the rest; `null` sends every sample. The last message when the eye data stops always goes |

The two formats:

- `feo1`: FrameEyeOSC packets ([docs/protocol.md](protocol.md)), byte for byte the ones the FrameEyeOSC mode sends, whatever the main output is: gaze, eyelids, and with the eye cameras squint and pupil size, and the brows with `brow_from_widen`. Each packet is made once and goes to every such destination (and to the main output in FrameEyeOSC mode), with one sequence number. Without eye data it sends "not tracking" twice a second, as the FrameEyeOSC mode does.
- `osc`: plain OSC messages (one per UDP packet, or bundled with `bundle`), for apps that read VRChat's eye input or VRCFaceTracking's parameter names. With every sample, VRChat's eye input: `/tracking/eye/CenterVec` (the shared gaze as a direction; also with "Move eyes separately") and `/tracking/eye/EyesClosedAmount` (both eyelids as one value, 0 open, 1 closed). Then VRCFaceTracking's v2 avatar parameters with the values VRChat mode sends, always under `/avatar/parameters/FT/v2/` whatever `prefix` says: `EyeLeftX`/`Y`, `EyeRightX`/`Y`, `EyeLidLeft`/`Right` and `EyeX`/`Y` always, and the squint and pupils (with the eye cameras) and the brows (with `brow_from_widen`) only while there are some. Never `EyeTrackingActive`, the pupil bits, Steam Link's names or the face tracking blob. When the eye data stops it sends relaxed open eyes looking ahead once (with the squint and brows back to 0), then nothing until the data comes back. The exact list is in [docs/protocol.md](protocol.md#other-formats-osc); later versions may add addresses, never change the ones there are.

The gaze is the one the main output sends: after the eye fit and the gaze scale (1.0 = 45°).

- At most 3 are used; entries after the third send nothing.
- An entry with a mistake, an unknown format or a key this frameeyeosc doesn't know (one written by a newer frameeyeosc, say) sends nothing, and the error names it (`extra_outputs entry 2: …`, in the panel and the status file); the rest of the file is used. An unknown key stops the entry on purpose: a newer key might limit what goes out, and an older frameeyeosc that skipped it would send more than you chose. `bundle` or `max_rate` on a `feo1` entry stops it too (`false` and `null` are fine). The panel doesn't set them; changing an entry to `feo1` there removes them.
- One on the same address and port as the main output is refused ("same as main"), as is one on those of the pupils sent straight to VRChat ("same as the pupils") or of an earlier one ("duplicate").
- `lan_only` applies to each of them, as to the main output (see [Settings](settings.md#settings)).
- Pausing (`sending`) pauses them too. When one is turned off or moved, or its format changes, while the eye data runs, the old destination gets its last message first ("not tracking", or eyes looking ahead).
- Don't point an `osc` destination at a VRChat that already gets your eyes from the main output, whether directly or through VRCFaceTracking (LiveLink or FrameEyeOSC): it moves the avatar too (and VRChat's own eyes), and the two fight. One on the port where Steam Link's own eye OSC arrives likewise means two senders on one port: turn one of them off. Likewise, don't feed a relay from a `feo1` destination while the relay also passes it on to the VRCFaceTracking the main output sends to.
- Each one's start, stop and refusal is logged with its address and format (`journalctl --user -u frameeyeosc`), and the status file lists them (`extras`).

On the command line, `--extra FORMAT@HOST:PORT` (up to 3 times) replaces the file's whole list, named "App 1", "App 2", ..., and the list can't be changed from the file then. `--no-extras` sends to no other app, whatever the file says:

```sh
FRAMEEYEOSC_ARGS="--extra feo1@192.168.0.10:11113 --extra osc@main:9015"
```
