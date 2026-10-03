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

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/frameeyeosc/icons"
cp target/release/frameeyeosc install.sh contrib/frameeyeosc.service contrib/frameeyeosc.env.example \
    LICENSE THIRD_PARTY_LICENSES.md README.md README.ja.md CHANGELOG.md CHANGELOG.ja.md "$stage/frameeyeosc/"
cp panel/build/frameeyeosc-panel panel/contrib/frameeyeosc-panel.service panel/contrib/frameeyeosc-panel.desktop \
    "$stage/frameeyeosc/"
cp panel/contrib/icons/frameeyeosc-panel-{48,128,256}.png "$stage/frameeyeosc/icons/"
cp vendor/frame-updater/frame-update.sh "$stage/frameeyeosc/"
mkdir -p dist
tar -C "$stage" -czf "dist/$name.tar.gz" frameeyeosc
# The panel's update button only installs releases that carry this file (attach it to the release too)
(cd dist && sha256sum "$name.tar.gz" >SHA256SUMS)
echo "dist/$name.tar.gz"
echo "dist/SHA256SUMS"
