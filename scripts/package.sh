#!/usr/bin/env bash
# Build a release tarball. Run this on the Steam Frame itself: the binary has to link against the
# headset's glibc, because the eye server's shared mutex uses glibc's layout (a static musl build
# would lock it with an incompatible layout). The panel links against SteamVR's libopenvr_api.so,
# which is only on the headset too.
set -euo pipefail
cd "$(dirname "$0")/.."

cargo="${CARGO:-cargo}"
command -v "$cargo" >/dev/null || cargo="$HOME/.cargo/bin/cargo"
version="$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -n 1)"
name="frameeyeosc-$version-steamframe-aarch64"

# The updater copy must be exactly what frame-updater's sync.sh put there
sh vendor/frame-updater/verify.sh

"$cargo" test --release
"$cargo" build --release
cmake -G Ninja -S panel -B panel/build -DCMAKE_BUILD_TYPE=Release
ninja -C panel/build
panel/build/gaze-fit-test
panel/build/gaze-dots-test
panel/build/sounds-test
panel/build/text-test
panel/build/auto-recenter-test
panel/build/recorder-test
panel/build/model-test
panel/build/changelog-test
panel/build/eyecam-test
panel/build/report-test
# eyecam, the eye-camera tool (tools/eyecam): its own crate and lockfile, built into its own target folder.
# Stripped like frameeyeosc (its Cargo.toml is a vendored copy, so the profile is set here)
eyecam_target=tools/eyecam/target
"$cargo" test --release --locked --manifest-path tools/eyecam/Cargo.toml --target-dir "$eyecam_target"
CARGO_PROFILE_RELEASE_STRIP=true "$cargo" build --release --locked --manifest-path tools/eyecam/Cargo.toml \
    --target-dir "$eyecam_target"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/frameeyeosc/icons"
cp target/release/frameeyeosc install.sh contrib/frameeyeosc.service contrib/frameeyeosc.env.example \
    LICENSE THIRD_PARTY_LICENSES.md README.md README.ja.md CHANGELOG.md CHANGELOG.ja.md "$stage/frameeyeosc/"
cp panel/build/frameeyeosc-panel panel/contrib/frameeyeosc-panel.service panel/contrib/frameeyeosc-panel.desktop \
    "$stage/frameeyeosc/"
cp panel/contrib/icons/frameeyeosc-panel-{48,128,256}.png "$stage/frameeyeosc/icons/"
cp vendor/frame-updater/frame-update.sh "$stage/frameeyeosc/"
# install.sh puts these in ~/.local/lib/eyecam (install_grab.sh is run once by the user, with sudo)
mkdir -p "$stage/frameeyeosc/eyecam"
cp "$eyecam_target/release/eyecam-rec" "$eyecam_target/release/eyecam-grab" tools/eyecam/install_grab.sh \
    tools/eyecam/protocol_*.txt tools/eyecam/eyecam.service tools/eyecam/NOTICE "$stage/frameeyeosc/eyecam/"
# eyecam-grab's sha256 for the release notes (users compare it before installing the tool with sudo; not in the
# tarball, whose copy would sit next to the file it vouches for)
echo "eyecam-grab sha256: $(sha256sum "$stage/frameeyeosc/eyecam/eyecam-grab" | cut -d' ' -f1)"
mkdir -p dist
tar -C "$stage" -czf "dist/$name.tar.gz" frameeyeosc
# The panel's update button only installs releases that carry this file (attach it to the release too)
(cd dist && sha256sum "$name.tar.gz" >SHA256SUMS)
echo "dist/$name.tar.gz"
echo "dist/SHA256SUMS"
