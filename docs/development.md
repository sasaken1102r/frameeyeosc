# Development

[Back to the README](../README.md)

Build and test on the headset (the binary must link against glibc, and the panel against SteamVR's OpenVR library; see `scripts/package.sh`). `scripts/package.sh` builds the Rust programs in a container with an older glibc (Debian bookworm, with podman), so they also start on older SteamOS:

```sh
cargo test --release
cmake -G Ninja -S panel -B panel/build && ninja -C panel/build
panel/build/gaze-fit-test   # the panel's eye fit logic
scripts/package.sh   # builds dist/frameeyeosc-<version>-steamframe-aarch64.tar.gz with both, and dist/SHA256SUMS
```

`vendor/frame-updater/` is a copy of the update checker shared by my Steam Frame apps, and `vendor/frame-apps/` of the list of my other Frame apps on Advanced › Version. Don't edit them here: `scripts/package.sh` stops if either differs from what its `MANIFEST.sha256` records.

`tools/eyecam/` is a copy of eyecam, the eye-camera tool (`eyecam-rec` and `eyecam-grab`), which is developed on its own branch. Don't edit it here either; to update it, check out that directory from the commit to take and commit it with the commit's hash in the message: `git rm -rq tools/eyecam && git checkout <commit> -- tools/eyecam` (the `git rm` drops files the new version no longer has). It has its own `Cargo.toml` and `Cargo.lock` and is not part of the root Cargo build; `scripts/package.sh` builds and tests it separately.

To publish a release, attach both files. The panel's "Update" button refuses releases without `SHA256SUMS` and asks for a manual update instead. The release notes are the version's section of `CHANGELOG.md` (without its heading): it starts with a one-paragraph English summary, then a paragraph starting with `日本語: ` with the Japanese summary, then the list. A panel running an older version (0.7.1 or later) shows the summary while that release is available, the Japanese one on a Japanese panel, without markdown and cut at 300 characters:

```sh
gh release create v0.4.0 --title v0.4.0 --notes-file notes.md
gh release upload v0.4.0 dist/frameeyeosc-0.4.0-steamframe-aarch64.tar.gz dist/SHA256SUMS
```

Each release also adds its section to `CHANGELOG.ja.md`, the short Japanese changelog the panel shows: the same heading as in `CHANGELOG.md`, the `日本語:` paragraph as its summary, then the changes in plain words.

To tune the eye processing against real data, record the eye tracker's raw samples (nothing is sent while recording, so it can run next to the service), then replay the file. The replay prints a few numbers for the current settings next to the same settings with the 0.4.0 steps turned off; settings come from `config.json` and options as usual. Recordings are personal data, so keep them out of the repository.

```sh
~/.local/bin/frameeyeosc --record ~/eyes.csv              # stop with Ctrl+C (or SIGINT / SIGTERM); the file is complete up to the stop
~/.local/bin/frameeyeosc --replay ~/eyes.csv --blink-hold-ms 120 --replay-out ~/processed.csv   # also writes the processed values
```

The panel can make the same recording while you play: "Eye log" on the Advanced tab's "Debug tools" starts it and turns into "Stop 1:23"; the left column shows "Recording 1:23" in red meanwhile. It stops by itself after 60 minutes, and when the panel exits. The files go to `~/.local/share/frameeyeosc/recordings/`: `eyes_YYYY-MM-DD_HH-MM-SS.csv` (about 2.3 MB a minute), the `config.json` in use as `eyes_….config.json`, and the recorder's messages as `eyes_….log`. With the [eye cameras](eyecam.md#eye-cameras) in use, the panel also has eyecam keep its drift log for as long as the recording runs: numbers only, no pictures, one line per eye per second (where it found the pupil, the levels it compares with, the values it worked out, Valve's openness and pitch; about 1.3 MB an hour) in `~/eyecam/drift/eyes_YYYY-MM-DD_HH-MM-SS.eyecam.csv`, named after the recording, and the recording's `.log` says which file it is. Nothing leaves the headset; delete them when you no longer need them.

While eyecam runs, each row of a recording also has the eye cameras' values at the end (`cam_l_*` / `cam_r_*`: whether they were fresh and valid, closed, eyelid, widening, squint, pupil in mm and as dilation, confidence and age, then `cam_calib_state` and `cam_live`; empty while there are none), and last `cam_eyes_anatomical` (`1` on rows with camera values), which says `cam_l_*` is the left eye. The eyecam of 0.7.2 to 0.7.5 called the right eye's camera L, so recordings without that column (with camera columns, from those versions) have `cam_l_*` for the right eye and `cam_r_*` for the left; a replay reads them the other way round, so each reaches the eye it was of. Older recordings and readers keep working. A replay of such a recording mixes them in as frameeyeosc does, and its report adds how often camera values were there and how often an eye went out widened with and without them.
