#!/bin/bash
# eyecam-rec を ~/.local/lib/eyecam に入れて、systemd --user で常駐させる（sudo は要らない）。
#
#   ./install_user.sh                 # ./target/release の eyecam-rec を入れて、起動まで
#   ./install_user.sh --bin DIR       # DIR/eyecam-rec と DIR/eyecam-grab を入れる（install_grab.sh も一緒に置く）
#   ./install_user.sh --no-enable     # ファイルとユニットを置くだけ（起動しない）
#   ./install_user.sh --uninstall     # 止めて取り除く
#   --prefix DIR / --unit-dir DIR     # 置き場所を変える（試すとき用）
#
# 更新も同じコマンド。動いている eyecam-rec のファイルは名前の付け替えで置き換え、そのあと再起動する。
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$HOME/.local/lib/eyecam"
UNIT_DIR="$HOME/.config/systemd/user"
BIN_DIR="$here/target/release"
ENABLE=1
UNINSTALL=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --unit-dir) UNIT_DIR="$2"; shift 2 ;;
        --bin) BIN_DIR="$2"; shift 2 ;;
        --no-enable) ENABLE=0; shift ;;
        --uninstall) UNINSTALL=1; shift ;;
        *) echo "知らないオプション: $1" >&2; exit 1 ;;
    esac
done

if [ "$(id -u)" -eq 0 ]; then
    echo "これは sudo なしで実行してね（ユーザーの systemd に入れる）" >&2
    exit 1
fi

if [ "$UNINSTALL" -eq 1 ]; then
    if [ "$ENABLE" -eq 1 ]; then
        systemctl --user disable --now eyecam.service 2>/dev/null || true
    fi
    rm -f "$UNIT_DIR/eyecam.service"
    # 入れたファイルだけを消す（--prefix を間違えても、ほかのものは消さない）
    rm -f "$PREFIX"/eyecam-rec "$PREFIX"/eyecam-grab "$PREFIX"/install_grab.sh "$PREFIX"/protocol_*.txt
    rmdir "$PREFIX" 2>/dev/null || echo "残したもの（自分で入れたファイル）: $PREFIX" >&2
    [ "$ENABLE" -eq 1 ] && systemctl --user daemon-reload
    echo "取り除いた: $PREFIX, $UNIT_DIR/eyecam.service"
    exit 0
fi

[ -x "$BIN_DIR/eyecam-rec" ] || { echo "見つからない: $BIN_DIR/eyecam-rec" >&2; exit 1; }
mkdir -p "$PREFIX" "$UNIT_DIR"
for b in eyecam-rec eyecam-grab; do
    if [ -f "$BIN_DIR/$b" ]; then
        install -m 0755 "$BIN_DIR/$b" "$PREFIX/.$b.new"
        mv -f "$PREFIX/.$b.new" "$PREFIX/$b"
    fi
done
install -m 0644 "$here"/protocol_*.txt "$PREFIX/"
# sudo で一度だけ叩く install_grab.sh も隣に置く（引数なしで、ここの eyecam-grab を入れる）
install -m 0755 "$here/install_grab.sh" "$PREFIX/.install_grab.sh.new"
mv -f "$PREFIX/.install_grab.sh.new" "$PREFIX/install_grab.sh"
sed "s#@PREFIX@#$PREFIX#g" "$here/eyecam.service" > "$UNIT_DIR/eyecam.service"
sha256sum "$PREFIX/eyecam-rec"
echo "置いた: $PREFIX, $UNIT_DIR/eyecam.service"

if [ "$ENABLE" -eq 1 ]; then
    systemctl --user daemon-reload
    systemctl --user enable eyecam.service
    systemctl --user restart eyecam.service
    systemctl --user --no-pager status eyecam.service | head -5
    echo "ログ: journalctl --user -u eyecam -f"
fi
