#!/usr/bin/env bash
# Build a release tarball. Run this on the Steam Frame itself: the binary has to link against glibc,
# because the eye server's shared mutex uses glibc's layout (a static musl build would lock it with an
# incompatible layout). The panel links against SteamVR's libopenvr_api.so, which is only on the
# headset too.
set -euo pipefail
cd "$(dirname "$0")/.."

cargo="${CARGO:-cargo}"
command -v "$cargo" >/dev/null || cargo="$HOME/.cargo/bin/cargo"

# The Rust programs are built in a container with an older glibc (Debian bookworm, 2.36): built against
# the Frame's own glibc they need that SteamOS or newer (0.10.0, built on SteamOS 0.5.7 with glibc 2.43,
# didn't start on older SteamOS). FRAMEEYEOSC_BUILD_IMAGE= (empty) builds on the Frame itself instead.
# (pinned by digest: the tag is rebuilt when Debian updates)
image="${FRAMEEYEOSC_BUILD_IMAGE-docker.io/library/rust:1.98.1-slim-bookworm@sha256:3ebe98baef5911f8461cf81ba05644374a3c5ef1f8a33db3977bd4175f5ff1fc}"
# Its own target folders: cargo doesn't see which glibc a program was linked against, so it would keep a
# program the Frame's own cargo built in target/
rust_target="target${image:+/container}"
eyecam_target="tools/eyecam/target${image:+/container}"
# The newest glibc, libstdc++ and C++ ABI versions a shipped program may need (the panel, built on the
# Frame, needs glibc 2.38, GLIBCXX 3.4.30 and CXXABI 1.3.9, as since 0.9.1)
max_glibc=2.38
max_glibcxx=3.4.30
max_cxxabi=1.3.9

# Runs cargo with the given arguments, in the build container unless FRAMEEYEOSC_BUILD_IMAGE is empty
rust_cargo() {
    if [ -z "$image" ]; then
        "$cargo" "$@"
        return
    fi
    # As the Frame's user, not the container's root (the tests check file owners and permissions), with
    # the Frame's cargo folder for the downloaded crates
    podman run --rm --userns=keep-id -v "$PWD":/src:Z -v "$HOME/.cargo":/cargo -e CARGO_HOME=/cargo -w /src \
        ${CARGO_PROFILE_RELEASE_STRIP:+-e CARGO_PROFILE_RELEASE_STRIP="$CARGO_PROFILE_RELEASE_STRIP"} \
        "$image" cargo "$@"
}

# Stops when one of the given programs needs a glibc (GLIBC_), libstdc++ (GLIBCXX_) or C++ ABI (CXXABI_)
# version newer than max_glibc, max_glibcxx or max_cxxabi
check_glibc() {
    local f prefix max need
    for f in "$@"; do
        for prefix in GLIBC GLIBCXX CXXABI; do
            case "$prefix" in
                GLIBC) max="$max_glibc" ;;
                GLIBCXX) max="$max_glibcxx" ;;
                CXXABI) max="$max_cxxabi" ;;
            esac
            need="$(objdump -T "$f" | grep -o "${prefix}_[0-9.]*" | sed "s/^${prefix}_//" | sort -uV | tail -n 1)"
            [ -n "$need" ] || continue
            if [ "$(printf '%s\n%s\n' "$need" "$max" | sort -V | tail -n 1)" != "$max" ]; then
                echo "$f needs ${prefix}_$need, newer than $max: it won't start on older SteamOS" >&2
                exit 1
            fi
            echo "$f: ${prefix}_$need"
        done
    done
}

version="$(sed -n 's/^version = "\(.*\)"/\1/p' Cargo.toml | head -n 1)"
name="frameeyeosc-$version-steamframe-aarch64"

# The updater copy must be exactly what frame-updater's sync.sh put there
sh vendor/frame-updater/verify.sh
# ...and the other Frame apps' list (built into the panel) what frame-apps' sync.sh put there
sh vendor/frame-apps/verify.sh

rust_cargo test --release --locked --target-dir "$rust_target"
rust_cargo build --release --locked --target-dir "$rust_target"
cmake -G Ninja -S panel -B panel/build -DCMAKE_BUILD_TYPE=Release
ninja -C panel/build
# Every panel test CMakeLists.txt registers (add_test), so a new one can't be left out here
ctest --test-dir panel/build --output-on-failure
# eyecam, the eye-camera tool (tools/eyecam): its own crate and lockfile, built into its own target folder.
# Stripped like frameeyeosc (its Cargo.toml is a vendored copy, so the profile is set here)
rust_cargo test --release --locked --manifest-path tools/eyecam/Cargo.toml --target-dir "$eyecam_target"
CARGO_PROFILE_RELEASE_STRIP=true rust_cargo build --release --locked --manifest-path tools/eyecam/Cargo.toml \
    --target-dir "$eyecam_target"

check_glibc "$rust_target/release/frameeyeosc" panel/build/frameeyeosc-panel "$eyecam_target/release/eyecam-rec" \
    "$eyecam_target/release/eyecam-grab"

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/frameeyeosc/icons"
cp "$rust_target/release/frameeyeosc" install.sh contrib/frameeyeosc.service contrib/frameeyeosc.env.example \
    LICENSE THIRD_PARTY_LICENSES.md README.md README.ja.md CHANGELOG.md CHANGELOG.ja.md "$stage/frameeyeosc/"
cp panel/build/frameeyeosc-panel panel/contrib/frameeyeosc-panel.service panel/contrib/frameeyeosc-panel.desktop \
    "$stage/frameeyeosc/"
"${STRIP:-strip}" "$stage/frameeyeosc/frameeyeosc-panel"
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
