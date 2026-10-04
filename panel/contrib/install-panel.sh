#!/bin/sh
# Installs frameeyeosc-panel for the current user so it shows up under "Launch program" (+) on the SteamVR
# dashboard: the binary, the .desktop file, icons and the systemd user unit. No sudo.
# Autostart is not enabled here (use "Start with SteamVR" in the panel).
# Usage, from the panel folder after building into build/:  sh contrib/install-panel.sh
set -eu

cd "$(dirname "$0")/.."
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
CONFIG="${XDG_CONFIG_HOME:-$HOME/.config}"
BIN="$HOME/.local/bin/frameeyeosc-panel"
DESKTOP="$DATA/applications/frameeyeosc-panel.desktop"
ICONS="$DATA/icons/hicolor"
UNIT="$CONFIG/systemd/user/frameeyeosc-panel.service"

if [ ! -x build/frameeyeosc-panel ]; then
    echo "build/frameeyeosc-panel is missing. Build it first" >&2
    exit 1
fi

# The binary (a running panel keeps running; the file is only replaced)
install -Dm755 build/frameeyeosc-panel "$BIN"
# Icons (Steam looks in hicolor/<size>/apps and does not find a 256x256-only icon, so install three sizes)
for size in 48 128 256; do
    install -Dm644 "contrib/icons/frameeyeosc-panel-$size.png" "$ICONS/${size}x${size}/apps/frameeyeosc-panel.png"
done
# .desktop with Exec set to the binary's absolute path
mkdir -p "$(dirname "$DESKTOP")"
sed "s|@BINARY@|$BIN|" contrib/frameeyeosc-panel.desktop > "$DESKTOP.tmp"
chmod 644 "$DESKTOP.tmp"
mv "$DESKTOP.tmp" "$DESKTOP"
# The changelogs from the repository, for the version history (where install.sh puts them)
for changelog in CHANGELOG.md CHANGELOG.ja.md; do
    if [ -f "../$changelog" ]; then
        install -Dm644 "../$changelog" "$DATA/frameeyeosc/$changelog"
    fi
done
# The systemd user unit (installed and loaded only; not enabled, not started)
install -Dm644 contrib/frameeyeosc-panel.service "$UNIT"
systemctl --user daemon-reload

if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$DESKTOP"
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$(dirname "$DESKTOP")" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -f "$ICONS/index.theme" ]; then
    gtk-update-icon-cache -q "$ICONS" || true
fi

echo "Installed:"
echo "  $BIN"
echo "  $DESKTOP"
echo "  $ICONS/{48x48,128x128,256x256}/apps/frameeyeosc-panel.png"
echo "  $UNIT (turn autostart on with \"Start with SteamVR\" in the panel)"
