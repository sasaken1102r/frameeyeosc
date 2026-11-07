# What it does, in detail

[Back to the README](../README.md)

- It finds your PC on its own by sending to whichever PC Steam Link is streaming from. With the bundled wireless adapter that's the adapter's direct link, so your home network doesn't matter.
- Gaze and eyelids are smoothed with One Euro filters. A small deadzone keeps the eyes still while you fixate, and the gaze is held while your eyes are shut, because the Frame's gaze jumps as the eyes reopen.
- Both eyes share one gaze. On the Frame each eye's gaze wobbles on its own, which makes an avatar's eyes twitch. `--independent-eyes` turns the eyes in a little toward each other instead (as if looking about 2 m away), each keeping its own up/down; the Frame's own per-eye sideways gaze is too unsteady to send as it is.
- Eyelid values are mapped onto the VRCFT scale (0 closed, 0.75 relaxed, 1 widened). A held-shut eye reads about 0.2 on the Frame, and a relaxed eye wanders between about 0.75 and 0.9.
- Eyelids calibrate themselves. It learns how far each of your eyes opens when relaxed, so if your face or the headset fit makes one eye look more open, the avatar still looks even. Winks still come through.
- It runs as a service that starts with SteamVR and restarts if it stops.
- Settings live in a file that is picked up while running, and an optional panel on the SteamVR dashboard changes them from inside the headset.
- It can also send Live Link Face packets for VRCFaceTracking's LiveLink module, which also carries widened eyes (see [VRCFaceTracking (LiveLink) mode](outputs.md#vrcfacetracking-livelink-mode)), or to FrameEyeOSC, a VRCFaceTracking module made for it, which also carries squints and pupils (see [VRCFaceTracking (FrameEyeOSC) mode](outputs.md#vrcfacetracking-frameeyeosc-mode)).
- Optionally it also uses the Frame's eye cameras, through eyecam, a tool that comes with it: widened eyes come through again on SteamOS 0.4.3, and squints and pupil size are sent too. It takes a one-time setup on the panel (see [Eye cameras](eyecam.md#eye-cameras)). An optional, experimental "Half-shut check (AI)" keeps an open eye from reading as half shut, for example while looking to the side.
- Optionally the avatar's brows go up when you widen your eyes ("Widen raises brows", off by default; the cameras don't see brows, so the brows follow the widening). It works on avatars with brow parameters.
