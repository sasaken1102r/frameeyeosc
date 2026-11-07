# Eye cameras

[Back to the README](../README.md)

The Frame has an infrared camera for each eye. eyecam, a tool that comes with frameeyeosc, reads their video while SteamVR's eye tracking runs and works out from it how open each eye is, whether it is widened or squinted, and the pupil size. frameeyeosc mixes that into what it sends. It is optional: without it, frameeyeosc sends Valve's values as before.

What it adds:

- Widened eyes, also on SteamOS 0.4.3, where Valve's openness can't show them (see [Troubleshooting](troubleshooting.md#troubleshooting)). From relaxed open up, the eyelid comes from the camera, and also where the camera sees the eye widened while Valve's values read it at least two thirds open. Below that, Valve's values, which catch blinks faster, with two exceptions: an eye the camera sees closed goes out closed (Valve's values miss one eye of some blinks; with `camera_close_gate`, on by default, a "closed" in one eye only waits until it is a blink or a wink), and an eye Valve's values read as closing for longer than a blink (0.3 s; longer with a slow camera) while the camera sees it open, as in a squint, takes the camera's eyelid until the two agree again, so a narrowed eye isn't sent closed or pulled shut with the other eye. While the camera sees an eye open, its eyelid can be kept from going lower than `camera_lid_floor` (off by default; 0.25, say), so a hard squint, where the camera's own eyelid nears 0, still shows narrowed. Where Valve's values jump to 1.000 (on SteamOS 0.4.3 that says only that the eye is open) while the camera sees the eye clearly narrowed, the jump is lowered to the camera's eyelid for its first 0.1 s, so an eye held half closed doesn't flick open; an open eye is never lowered this way. The camera's widening fades in over a small band (the camera's first 0.05 above relaxed open; Valve's eyelid 0.5 to 0.6) instead of switching on and off. Widening in one eye only shows at half: while the camera sees both eyes, neither goes further above relaxed open than the mean of the two cameras' widening (real widening is nearly always both eyes). While the camera sees an eye open and Valve's values read it below relaxed open but not closing (SteamOS 0.4.x often reads an open eye at 0.6-0.95, mostly looking down), the eyelid goes no lower than the camera's, up to relaxed open; this only raises, so squints, blinks and winks stay as Valve's values have them. Each eye on its own: an eye the camera can't use right now gets Valve's values.
- Squints (`v2/EyeSquintLeft` / `EyeSquintRight` / `EyeSquint` in VRChat mode, and through VRCFaceTracking in FrameEyeOSC mode), after the optional user calibration below.
- Pupil size (`v2/PupilDilation` and `v2/PupilDiameterLeft` / `PupilDiameterRight` / `PupilDiameter`) in VRChat mode, straight to VRChat in LiveLink mode (see [VRCFaceTracking (LiveLink) mode](outputs.md#vrcfacetracking-livelink-mode)), and through VRCFaceTracking in FrameEyeOSC mode, which carries the squint too (see [VRCFaceTracking (FrameEyeOSC) mode](outputs.md#vrcfacetracking-frameeyeosc-mode)).

Once set up, there is nothing to do each time you put the headset on: it learns your relaxed eyes from about 35 seconds of wearing it with your eyes open, looking ahead as usual, and uses Valve's values until then. Putting the headset back on starts that over.

## Setting it up

https://github.com/user-attachments/assets/516ad2bb-7d03-4690-a3b8-b649ffb19ede

(The panel in the video is in Japanese; it follows your language setting on the Basic tab. The password entry is cut from the video.)

The eye cameras need a one-time setup on the panel's "Eye cameras" tab. It is a checklist that moves on by itself as each step is met:

From 0.9.0 eyecam takes the eye cameras' video from Valve's dsp_service without sudo (a test feature; `source` in [eyecam's README](../tools/eyecam/README.md)). When that works, steps 1 and 2 below don't show and the list starts at 3. They only show when dsp_service hasn't answered for about 15 seconds. Once the setup is done they don't come back: then the tab (and the left column) says dsp_service can't be reached and offers the tool with the same "Open in Konsole" (or passwd first, while SteamOS has no password). The cameras are stopped until dsp_service answers again or the tool is in. If you already installed the tool (eyecam-grab), it keeps being used.

1. Set a password: only if SteamOS has none yet, because step 2 needs it. Steam settings > Developer > Change user password, or "Open passwd in Konsole".
2. Install the tool: "Open in Konsole" opens a Konsole window with `sudo ~/.local/lib/eyecam/install_grab.sh` typed in. Press Enter and type your SteamOS password (over SSH, type the same line). The panel never runs sudo itself. Before you type the password, check the sha256 Konsole shows above the command against the release notes (see [Checking the tool before installing it](#checking-the-tool-before-installing-it)). When an update brings a new version of the tool, the Eye cameras tab (and the left column) asks you to install it again, with the same "Open in Konsole"; the installed one keeps working meanwhile. Only if eyecam marks the installed one as unsafe (older than its safety floor) do the cameras stop until it is installed again, the eyelids going out on Valve's values meanwhile; the notice then says so. Before the setup is done, this step itself comes back as "Install the tool again".
3. Learn your eye movements: with the headset on and both eyes seen, press "Start learning" and move your eyes as the screen says for 18 seconds (close, open normally, open wide, ...; a beep marks each change). If only the widening couldn't be measured, you can carry on with standard values and calibrate again later.

Before the setup begins, the tab first shows what the cameras add and "Get started (about 2 min)", which opens this checklist.

After that, the tab shows what drives the eyelids now, "Calibrate the cameras (18 s)" for when something feels off, and "User calibration (once)" (18 seconds: squint, look up, look down), which squints need. The switch to turn the cameras off is on the Basic tab ("Eye cameras": "Use / Don't use", `camera_lids`). How easily the eyes widen is set on the Eyelids tab ("How easily widening shows"), where "Widening" Off (`widen`) turns widening off altogether while the cameras' closing and squint stay.

Under them, "How the video is taken" says where eyecam takes the video from now, "dsp (no sudo)" or "the tool (eyecam-grab)", and whether the tool is installed:

- Its three choices set eyecam's `source`: "Auto" (the default: the tool when it is installed and works, else dsp), "dsp" and "Tool" (only while the tool is installed). eyecam saves the choice. If it changes where the video comes from, eyecam takes the video again at once, and the cameras' values pause for a few seconds. It can't be changed while recording or calibrating.
- "Install (password once, in Konsole)" opens the same Konsole as step 2 above, with the sha256 to check first. If SteamOS has no password yet (the panel checks, as in step 1), the button opens passwd in Konsole instead and says to set one first. Once the tool is installed, "Remove (password once, in Konsole)" opens Konsole with `sudo ~/.local/lib/eyecam/install_grab.sh --uninstall` typed in. It can only be pressed while the video comes from dsp ("To remove it, switch to dsp first"). The panel never runs sudo itself.
- With "Auto", a tool installed while dsp is in use is used from the next time eyecam takes the video. Choose "Tool" to switch at once.
- The tool is the long-proven way. dsp needs no sudo but is new and relies on Valve's dsp_service. Both depend on Valve internals, so keeping one as a spare helps.

## Pupils on avatars with bit parameters

Some avatars take the pupil dilation not as one float (`v2/PupilDilation`) but packed into a few bool parameters, the way VRCFaceTracking sends them: `v2/PupilDilation1`, `v2/PupilDilation2`, `v2/PupilDilation4` (and `v2/PupilDilation8`). For those, choose how many there are under "How the avatar takes pupils" on the Output tab (`pupil_bits`): with `PupilDilation1`, `2` and `4`, that's 3. frameeyeosc then sends those bits too, worked out as VRCFaceTracking does, next to the float (the avatar ignores the names it doesn't have). Only the bits that changed go out, and all of them once a second. It applies wherever the pupils go to VRChat: in VRChat mode, and in LiveLink mode with "Send pupils straight to VRChat".

It is one value for every avatar. frameeyeosc can't see which avatar you wear or what parameters it has: VRChat only tells that to programs on the PC itself (its OSCQuery listens on 127.0.0.1). So set it for the avatar you use, and back to "Float" for an avatar with a plain float. To find the count, look for `PupilDilation1`, `PupilDilation2`, ... in the avatar's parameters (the avatar's Expression Parameters in Unity, or the parameter list VRCFaceTracking shows for the avatar), and count them.

## Why sudo, and what it installs

SteamOS lets no program take the camera buffers out of the eye tracker, even one running as the same user, unless it has a capability (`CAP_SYS_PTRACE`). So `install_grab.sh` copies one small program, `eyecam-grab`, to `/home/.eyecam/eyecam-grab` and gives that one file this capability. The file and its folder belong to root, so they can't be changed without sudo, and a changed file would lose the capability. They are under `/home` because SteamOS updates keep that partition. No service runs as root, and sudoers isn't changed. `eyecam-rec`, the part that keeps running, runs as you (`eyecam.service`, a user service) and starts `eyecam-grab` when the eye tracker runs. From 0.9.0 it first connects, as you, to Valve's dsp_service (which runs as root) and takes the same buffer read-only from it (a test feature); the tool is only needed when that doesn't work.

What `eyecam-grab` does (about 200 lines, `tools/eyecam/src/bin/eyecam-grab.rs`):

- It finds Valve's eye tracker by its exact program path, running as user 1000 (and stops if there is none or more than one), picks only its eye-camera buffers (`udmabuf`), duplicates those file descriptors, and hands them to eyecam-rec's socket after checking it belongs to user 1000. Then it exits.
- It never stops, attaches to, signals, or reads or writes the memory of the eye tracker. It doesn't read or map the buffers itself, writes no files, and doesn't stay running.
- Programs with a capability run in secure mode (`LD_PRELOAD` and the like have no effect), and it reads neither environment variables nor its own user ID. It doesn't check which program asks: it hands the buffers to whatever listens on `/run/user/1000/eyecam.sock` as user 1000, checking only that the socket and the program on its other end belong to user 1000. That is normally eyecam-rec, but once eyecam-grab is installed, any program running as you can get the eye-camera video through it, just as such a program can already read your files. Programs of other users can't.

eyecam-rec maps the buffers read-only and opens Valve's eye data read-only too. It never runs as root. More, in Japanese: [tools/eyecam/README.md](../tools/eyecam/README.md).

## Checking the tool before installing it

`install_grab.sh` copies `~/.local/lib/eyecam/eyecam-grab`, a folder any program running as you can write to, so before it gets the capability, check that it is the released file: its sha256 must match the `eyecam-grab` sha256 in the release notes on GitHub (the release you installed or updated to).

- The Konsole that "Open in Konsole" opens shows it above the command, before you press Enter and type your password.
- Over SSH, run `sha256sum .local/lib/eyecam/eyecam-grab` before `sudo .local/lib/eyecam/install_grab.sh`.
- `install_grab.sh` prints it again (after `Installing:`), and the installed copy's at the end.

If it differs, don't type the password (close Konsole) and reinstall frameeyeosc from the release. If it was installed already, remove it with `sudo ~/.local/lib/eyecam/install_grab.sh --uninstall` first.

## Turning it off and removing it

- To stop using the cameras, set "Eye cameras" on the Basic tab to "Don't use" (`"camera_lids": false`). frameeyeosc then sends Valve's values only, and tells eyecam-rec to stop working out eye values from the video (`live off`; told again whenever eyecam-rec restarts). The calibration buttons are greyed out meanwhile. eyecam-rec itself keeps running: it holds the camera buffers and writes its status file.
- To stop eyecam completely: `systemctl --user disable --now eyecam`. Updates leave it off: `install.sh` enables eyecam.service only on its first install, and later only restarts it if it is running. `systemctl --user enable --now eyecam` turns it on again.
- To remove the copy that has the capability: `sudo ~/.local/lib/eyecam/install_grab.sh --uninstall`. Run it before `./install.sh --uninstall`, which removes `~/.local/lib/eyecam` (if the copy is still there, it prints the `sudo rm` commands that remove it).
- `./install.sh --uninstall` removes eyecam together with frameeyeosc, and `--purge` also deletes its settings and calibration (`~/.config/eyecam`). The calibration files and developer recordings in `~/eyecam` (see [Privacy](privacy.md#privacy)) are left for you to delete.
