# frameeyeosc

Sends the Steam Frame's eye tracking (gaze and eye openness) to VRChat over OSC, as VRCFaceTracking-style avatar parameters. It runs on the headset as a background service and works with PC VRChat streamed through Steam Link. It can also send to VRCFaceTracking on the PC, so the eyes can be combined with other trackers.

[日本語版はこちら](README.ja.md)

https://github.com/user-attachments/assets/f8969485-161b-40d4-b9e4-689dee6d1955

This is a fork of [konsti219/frameeyeosc](https://github.com/konsti219/frameeyeosc). The Frame's public APIs only give you a combined gaze direction. konsti219 found that the eye tracker also measures how open each eye is and keeps it in an internal shared-memory object (`/dev/shm/eye-server.mmap`), and that is where this tool reads it from.

The way the eye-camera tool finds the eye camera frames is ported from Curtis English's [FrameEyeCameraFeed](https://github.com/Curtis-VL/FrameEyeCameraFeed) (MIT); thank you.

## What it does

- Finds the PC that Steam Link streams to and sends your eye data there
- Smooths gaze and eyelids so your avatar's eyes don't twitch
- Eyelids calibrate themselves as you use it, and blinks and winks come through
- Sends to VRChat directly or to VRCFaceTracking (the LiveLink module or the dedicated FrameEyeOSC module)
- Optionally uses the Frame's eye cameras to send widened eyes, squints and pupil size too
- A panel on the SteamVR dashboard lets you change settings with the headset on

For the details see [What it does, in detail](docs/features.md); for a comparison with Steam Link's own OSC see [Outputs](docs/outputs.md#compared-with-steam-links-own-osc).

## Requirements

- A Steam Frame with Developer Mode on and SSH access (Settings > System > Developer Mode, then set a password under Developer). Choose a strong password: with SSH on, anyone on your network who knows it can log in to the headset.
- PC VRChat streamed with Steam Link, OSC enabled in VRChat (Action Menu > Options > OSC > Enabled).
- An avatar with VRCFaceTracking eye parameters (`FT/v2/EyeLeftX`, `EyeLidLeft`, ...) as floats (details in [Avatar parameters](docs/outputs.md#avatar-parameters))
- For the VRCFaceTracking (LiveLink) mode: VRCFaceTracking on the PC with the LiveLink module. For the FrameEyeOSC mode: VRCFaceTracking with the FrameEyeOSC module.

## Install

### Easiest: install right inside the Frame (recommended)

No PC needed. In Konsole on the Frame (+ on the bar at the bottom → the list of programs → Konsole), type this command, press Enter, and pick **1** (frameeyeosc) from the menu. It asks whether to install the dashboard panel too.

```sh
curl -fsSL https://frame.sasaken1102s.net | sh
```

https://github.com/user-attachments/assets/d9c8fe37-759f-4609-8b6e-02300b1e7cf4

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

Where things go, and the eye-camera tool eyecam that comes with it: see [Where things go](docs/update.md#where-things-go).

After that, turn off Steam Link's own OSC output on your PC (SteamVR settings > Steam Link > OSC). Steam Link sends its own unsmoothed eye data to VRChat, and with both running, two sources fight over the avatar's eyes. This is needed in the LiveLink and FrameEyeOSC modes too, where VRCFaceTracking drives the avatar's eyes.

## Getting started

1. On the dashboard, pick "frameeyeosc panel" under Launch program (+) (from then on it starts with SteamVR)
2. On the "Basic" tab, pick where to send: VRChat directly, VRCFT (LiveLink) or VRCFT (FrameEyeOSC). For VRCFT, the "Output" tab lists what to set up on the PC
3. If your avatar's eyes look off, press "Fit my eyes" on the "Eye fit" tab and follow the dots with your eyes (about 20 s; see [Eye fit](docs/eye-fit.md#eye-fit))
4. To also send widened eyes and squints, set up the eye cameras once on the "Eye cameras" tab (about 2 minutes; see [Eye cameras](docs/eyecam.md))

Every tab of the panel is described in [Panel](docs/panel.md), and the settings file and options in [Settings and status file](docs/settings.md).

## Updating and going back

- Press "Update" under Advanced › Version in the panel. Running the install command again updates too
- To get test versions, turn on "Get test versions" on the same page
- How to go back to an earlier release: [Going back to an earlier release](docs/update.md#going-back-to-an-earlier-release)

## Troubleshooting

- When asking for help, press "Open" on the "Diagnostics" row under Advanced › Having trouble and send a screenshot of that page
- Logs: `journalctl --user -u frameeyeosc -f` (the panel: `journalctl --user -u frameeyeosc-panel -f`)
- If your avatar's eyes don't move, check that Steam Link's own OSC output is off on the PC, that OSC is enabled in VRChat, and that Windows Firewall isn't blocking it
- If "Eye data" in the left column is red (low), "Set 90" next to it may bring it back (SteamOS 0.5.7)

More in [Troubleshooting](docs/troubleshooting.md).

## Privacy

- Eye data only goes to the destinations you choose. There is no telemetry
- The panel only goes online to check for updates (GitHub and frame.sasaken1102s.net; turn "Check for updates" off to stop it) and when you press the module's address on the "Output" tab
- Eye-camera video is only kept on the Frame when you record it yourself ("Eye recording", "Record for squint training") or in developer mode. It is never sent anywhere
- What it sends is plain, unencrypted UDP, meant for your home network

Details in [Privacy](docs/privacy.md).

## Known issues

- Avatars that use binary (bit-packed) VRCFT parameters are not supported when sending to VRChat directly, except for the eye cameras' pupils (`pupil_bits`). In the LiveLink and FrameEyeOSC modes, the avatar side is up to VRCFaceTracking.

## Disclaimer

- Use at your own risk. The changes in this fork were made with Claude Opus 5.5, an AI model. I've tested them with unit tests and on my own Steam Frame, but I can't take responsibility for what happens on yours, so please read the code and check it yourself before you run it. The software comes with no warranty (see [LICENSE](LICENSE)).
- It reads the eye tracker's private, undocumented shared-memory layout (versions 4 and 5; 5 came with SteamOS 0.4.3). A SteamOS update can change that layout. If it does, frameeyeosc can't send eye data until it is updated. It keeps running meanwhile and tries again every second, and the panel shows "Can't read eye data: unsupported eye shared-memory version …".
- frameeyeosc needs no root and doesn't change any SteamOS files or settings. The only thing it writes is a "send me the next sample" flag in the eye tracker's shared memory, and it takes the lock there the same way the tracker's own clients do. The panel only writes frameeyeosc's settings file and its own records (calibration and eye fit records, eye logs, squint-training recordings) and sends commands to eyecam-rec. It deletes only calibration and eye fit records older than the newest 10 and what you press Delete for: the eye logs under Advanced > Data (never the one being recorded) and the squint-training recordings. The eye-camera recordings and calibration eye images under Data are deleted by eyecam-rec when the panel asks it to (the calibration results stay). For the eye cameras' setup it also opens Konsole and checks whether SteamOS has a password (`steamos-passwd --has-password`, which only reads). The optional eye cameras usually connect, as you, to Valve's dsp_service and take its buffer read-only (writing nothing to it). Only when that doesn't work do they need one sudo step, which you run yourself: it puts one root-owned program with one capability in `/home/.eyecam` (see [Eye cameras](docs/eyecam.md#eye-cameras)).
- eyecam reads the eye cameras' video from buffers the eye tracker uses internally, which are undocumented too. A SteamOS update can stop that; frameeyeosc then sends Valve's values.
- Reading Valve's undocumented internal data may conflict with the Steam Subscriber Agreement, which restricts reverse engineering. Decide for yourself whether you're comfortable with that before using it.
- This is an unofficial project with no affiliation with or endorsement from Valve Corporation, VRChat Inc., the VRCFaceTracking project or the EyeTrackVR project. Steam, Steam Frame, SteamVR and Steam Link are trademarks of Valve Corporation, and VRChat is a trademark of VRChat Inc. The names are used here only to say what this works with.

## More

- [What it does, in detail](docs/features.md)
- [Installing, updating and going back](docs/update.md)
- [Panel](docs/panel.md)
- [Outputs](docs/outputs.md) (VRChat, VRCFaceTracking, other apps)
- [Eye fit and calibration](docs/eye-fit.md)
- [Eye cameras](docs/eyecam.md)
- [Settings and status file](docs/settings.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Privacy](docs/privacy.md)
- [Development](docs/development.md)

## License

MIT. See [LICENSE](LICENSE); the original work is by konsti219. `vendor/frame-updater/` is not third-party code: it is sasaken1102r's own update checker, shared by their Steam Frame apps and copied here under this repository's MIT license. `tools/eyecam/` is sasaken1102r's own too, under the same license; it ports parts of Curtis English's FrameEyeCameraFeed (MIT, see [tools/eyecam/NOTICE](tools/eyecam/NOTICE)). Licenses of the bundled Rust crates, of the OpenVR SDK header used by the panel and FrameEyeCameraFeed's notice are in [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md). Changes are listed in [CHANGELOG.md](CHANGELOG.md).

## Thanks

Thanks to konsti219 for frameeyeosc and for finding where the Frame keeps its eyelid data. This fork is built on that work. Thanks also to Curtis English, whose FrameEyeCameraFeed showed how to find the eye cameras' frames; eyecam builds on it.
