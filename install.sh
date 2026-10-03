#!/usr/bin/env bash
# Install or update frameeyeosc as a user service on a Steam Frame. Run it on the headset as the
# normal user (no sudo needed):
#   ./install.sh                     install or update frameeyeosc, then start it
#   ./install.sh --with-panel        the same, plus the SteamVR dashboard panel
#   ./install.sh --uninstall         remove both, keeping settings and learned calibration
#   ./install.sh --uninstall --purge remove everything, settings included
# Without --with-panel an installed panel is left as it is, so frameeyeosc can be updated on its own.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
bin_dir="$HOME/.local/bin"
config_dir="$config_home/frameeyeosc"
unit_dir="$config_home/systemd/user"
unit="frameeyeosc.service"
panel_unit="frameeyeosc-panel.service"
panel_desktop="$data_home/applications/frameeyeosc-panel.desktop"
icon_dir="$data_home/icons/hicolor"
icon_sizes=(48 128 256)
# The panel's update button runs this script, which reruns install.sh with the options in install-args
share_dir="$data_home/frameeyeosc"
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/frameeyeosc"
install_args="$config_dir/install-args"

with_panel=false
uninstall=false
purge=false
for arg in "$@"; do
    case "$arg" in
        --with-panel) with_panel=true ;;
        --uninstall) uninstall=true ;;
        --purge) purge=true ;;
        *)
            echo "Unknown option: $arg (use --with-panel, --uninstall or --uninstall --purge)" >&2
            exit 2
            ;;
    esac
done

if $uninstall; then
    systemctl --user disable --now "$unit" 2>/dev/null || true
    systemctl --user disable --now "$panel_unit" 2>/dev/null || true
    rm -f "$unit_dir/$unit" "$bin_dir/frameeyeosc"
    rm -f "$unit_dir/$panel_unit" "$bin_dir/frameeyeosc-panel" "$panel_desktop"
    rm -rf "$share_dir" "$cache_dir"
    rm -f "$install_args"
    for size in "${icon_sizes[@]}"; do
        rm -f "$icon_dir/${size}x${size}/apps/frameeyeosc-panel.png"
    done
    systemctl --user daemon-reload
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$(dirname "$panel_desktop")" 2>/dev/null || true
    fi
    if $purge; then
        rm -rf "$config_dir"
        echo "Removed frameeyeosc, the panel, the settings and the learned calibration."
    else
        echo "Removed frameeyeosc and the panel. Settings and calibration are kept in $config_dir (add --purge to delete them)."
    fi
    exit 0
fi
if $purge; then
    echo "--purge only goes with --uninstall" >&2
    exit 2
fi

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "This build is for the Steam Frame (aarch64), but this machine is $(uname -m)." >&2
    exit 1
fi
if $with_panel && [[ ! -x "$here/frameeyeosc-panel" ]]; then
    echo "frameeyeosc-panel is not in $here. Use a release tarball that includes the panel." >&2
    exit 1
fi
if [[ ! -e /dev/shm/eye-server.mmap ]]; then
    echo "Note: /dev/shm/eye-server.mmap does not exist yet. It appears once SteamVR's eye tracking runs; the service retries until then."
fi

install -Dm755 "$here/frameeyeosc" "$bin_dir/frameeyeosc"
install -Dm644 "$here/frameeyeosc.service" "$unit_dir/$unit"
if [[ ! -f "$config_dir/env" ]]; then
    install -Dm644 "$here/frameeyeosc.env.example" "$config_dir/env"
fi
# The updater (the release tarball has it next to install.sh, a repository checkout under vendor/)
updater="$here/frame-update.sh"
[[ -f "$updater" ]] || updater="$here/vendor/frame-updater/frame-update.sh"
if [[ -f "$updater" ]]; then
    install -Dm755 "$updater" "$share_dir/frame-update.sh"
fi
# The changelogs, for the panel's version history (next to install.sh in the tarball and in a checkout)
for changelog in CHANGELOG.md CHANGELOG.ja.md; do
    if [[ -f "$here/$changelog" ]]; then
        install -Dm644 "$here/$changelog" "$share_dir/$changelog"
    else
        rm -f "$share_dir/$changelog"
    fi
done

if $with_panel; then
    # A running panel keeps its old binary until it restarts; the file is only replaced
    install -Dm755 "$here/frameeyeosc-panel" "$bin_dir/frameeyeosc-panel"
    install -Dm644 "$here/frameeyeosc-panel.service" "$unit_dir/$panel_unit"
    # Steam looks for launcher icons in hicolor/<size>/apps and misses a 256x256-only icon
    for size in "${icon_sizes[@]}"; do
        install -Dm644 "$here/icons/frameeyeosc-panel-$size.png" "$icon_dir/${size}x${size}/apps/frameeyeosc-panel.png"
    done
    # Exec needs the absolute path: Steam may start it without ~/.local/bin in PATH
    mkdir -p "$(dirname "$panel_desktop")"
    sed "s|@BINARY@|$bin_dir/frameeyeosc-panel|" "$here/frameeyeosc-panel.desktop" >"$panel_desktop.tmp"
    chmod 644 "$panel_desktop.tmp"
    mv "$panel_desktop.tmp" "$panel_desktop"
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$(dirname "$panel_desktop")" 2>/dev/null || true
    fi
fi

# Options for the next update from the panel. An installed panel is updated along with frameeyeosc,
# whether or not this run had --with-panel
if $with_panel || [[ -x "$bin_dir/frameeyeosc-panel" ]]; then
    printf -- '--with-panel\n' >"$install_args.tmp"
else
    : >"$install_args.tmp"
fi
mv "$install_args.tmp" "$install_args"

systemctl --user daemon-reload
systemctl --user enable "$unit"
systemctl --user restart "$unit"
if $with_panel; then
    # Starts with SteamVR from its next start (the panel's "Start with SteamVR" turns this off).
    # Not started now; a panel already running as the service is restarted to pick up the new binary.
    systemctl --user enable "$panel_unit"
    systemctl --user try-restart "$panel_unit"
fi
sleep 2
systemctl --user --no-pager status "$unit" | head -n 5 || true

cat <<EOF

frameeyeosc is installed and starts together with SteamVR.
  Settings: $config_dir/config.json   (the panel writes it; you can also edit it by hand.
            Changes are picked up within a second, no restart needed)
  Options:  $config_dir/env   (FRAMEEYEOSC_ARGS; these win over config.json.
            Apply with: systemctl --user restart frameeyeosc)
  Logs:     journalctl --user -u frameeyeosc -f
EOF
if $with_panel; then
    cat <<EOF

The panel starts with SteamVR from the next SteamVR start ("Eye" on the dashboard).
To open it now, pick "frameeyeosc panel" under Launch program (+) on the dashboard.
Its "Start with SteamVR" switch turns the autostart off. Logs: journalctl --user -u frameeyeosc-panel -f
EOF
elif [[ -x "$bin_dir/frameeyeosc-panel" ]]; then
    echo
    echo "The panel in $bin_dir was left as it is (run with --with-panel to update it too)."
fi
cat <<EOF

On your PC, turn off Steam Link's own OSC output (SteamVR settings > Steam Link > OSC),
otherwise it drives the avatar's eyes too with unsmoothed data.
EOF
