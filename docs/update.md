# Installing, updating and going back

[Back to the README](../README.md)

## Where things go

No sudo is needed. Everything goes into your home directory (`~/.local/bin`, `~/.local/lib/eyecam`, `~/.config`, `~/.local/share`), so SteamOS updates don't remove it. Run the same command again to update. Without `--with-panel` an installed panel is left as it is.

The release also installs eyecam, the eye-camera tool, to `~/.local/lib/eyecam` and, on its first install, enables and starts it as a user service (`eyecam.service`). Updates restart it only if it is running, and leave it off if you turned it off (see [Turning it off and removing it](eyecam.md#turning-it-off-and-removing-it)). Until you set up the eye cameras on the panel (see [Eye cameras](eyecam.md#eye-cameras)) it only waits, and nothing changes. That setup is the only step that may ask for sudo (usually it doesn't), once, and you run it yourself; `install.sh` never does.

## Updating from a version before 0.4.0

Versions before 0.4.0 have no updater, so update to 0.4.0 once by hand: copy and unpack the new tarball as above and run `./install.sh --with-panel` (or `./install.sh` without the panel). Your `~/.config/frameeyeosc/env` and the learned eyelid calibration are kept, and the service restarts on the new version.

Options in `FRAMEEYEOSC_ARGS` in `env` still work as before. But anything set there is locked in the panel ("Locked by command line"). To change it from the panel, remove it from `env`, run `systemctl --user restart frameeyeosc`, and set the value again in the panel.

To remove it: `./install.sh --uninstall` (removes the panel too; add `--purge` to also delete settings and calibration).

## Updating from the panel (0.4.0 and later)

From 0.4.0 on, the panel's "Update" button does the update. The panel's Advanced tab shows the installed version on its "Version" page. At start and then every hour, the panel asks GitHub whether a newer release exists. "Check now" asks right away. When a newer release exists, the "Version" page shows its summary in a box under the version (in Japanese on a Japanese panel when the release has one), and the left column shows a notice that opens that page, and "Update" downloads it, checks it against the release's `SHA256SUMS`, and runs its `install.sh` with the options of your last install (kept in `~/.config/frameeyeosc/install-args`). frameeyeosc and the panel restart on the new version. If anything fails before `install.sh` runs, nothing changes; the log is in `~/.cache/frameeyeosc/update.log`. Turn "Check for updates" off to stop the hourly check (the "Check now" button still works). The update itself only runs when you press the button.

### Test versions

Before a new version comes out for everyone, a test version (such as `0.7.11-beta.1`) may come out for the people who offered to try it. To get them, turn on "Get test versions" on the "Version" page (it's off by default; the setting is kept in `~/.local/state/frameeyeosc/panel.json`). The update checks then also offer test versions, marked "Test version"; the update itself works as above. While it's off, a small "Test version available" badge beside it tells you when a test version newer than yours exists; nothing is installed unless you turn it on and press "Update".

To go back, turn "Get test versions" off. Your test version stays installed until you choose otherwise: when the newest regular version is older than your test version, the "Version" page says "You can go back to the regular version …" with a "Go back" button. It asks first, then installs that regular version the same way as an update (no notice in the left column). If you don't go back, the next regular version (`0.7.11` after `0.7.11-beta.2`) is offered as usual. Test versions up to 0.8.1-beta.1 don't have "Go back" yet; to return right away from those, install the regular version by hand from its release page as described above.

`SHA256SUMS` is a checksum file from the same release, not a signature. It catches a corrupted or incomplete download. It can't catch a release that was replaced on GitHub, because the checksum would be replaced along with it.

## Going back to an earlier release

0.10 took out two things earlier releases have: the ETVR mode (sending to VRCFaceTracking's ETVR Tracking Module; the LiveLink and FrameEyeOSC modes carry widened eyes too) and the eyes as up to 0.7.5 ("v1", `eye_behavior` 1, which 0.7.6 to 0.9.x could switch to on the panel's Advanced › Version page). Settings from before keep working: `"output": "etvr"` (or `--output etvr`) sends to VRChat instead, and a `port` of 8889 set with it in the settings file goes back to VRChat's 9000 (a port given on the command line stays as given); `eye_behavior` (or `--eye-behavior`) is still read and does nothing. frameeyeosc logs one line for `"etvr"` and for an `eye_behavior` of 1, and the panel writes the new values into the settings file. When you move from ETVR to the LiveLink or FrameEyeOSC mode, turn off the ETVR Tracking Module in VRCFaceTracking.

If you need either of them, install the last 0.9 release again (0.9.1 below; if Releases has a newer 0.9.x, use its number instead):

1. From GitHub's [Releases](https://github.com/sasaken1102r/frameeyeosc/releases/tag/v0.9.1), download that release's `frameeyeosc-0.9.1-steamframe-aarch64.tar.gz` and `SHA256SUMS` onto the Frame
2. In Konsole, keep a copy of your settings first
   ```sh
   cp -a ~/.config/frameeyeosc ~/frameeyeosc-config-0.10
   cp -a ~/.config/eyecam ~/eyecam-config-0.10
   ```
3. Unpack and install (the tar.gz holds a `frameeyeosc` folder)
   ```sh
   sha256sum -c SHA256SUMS
   tar xzf frameeyeosc-0.9.1-steamframe-aarch64.tar.gz
   cd frameeyeosc && ./install.sh --with-panel
   ```
4. The 0.10 panel has already written the new values into the settings file, so pick ETVR, or v1 (Advanced › Version), again on that release's panel

Note, only when you go back further, to 0.7.x or earlier: 0.7.6 fixed the eye cameras' left and right being swapped and rewrote `~/.config/eyecam/calib.json` once for it. Going back to 0.7.5 or earlier reads that calibration the wrong way round: calibrate again, or copy the `calib.json.pre-anatomical` 0.7.6 left back to `calib.json`. 0.7.6 also gives both eyes one sideways zero point in the eye fit (earlier releases use it as it is). 0.7.7 set the headset's tilt to 0 and the sideways and down ranges to 1 once; earlier releases use them as they are (fit again there if you want them measured).

To update again later, use the panel's update button or the usual install command.
