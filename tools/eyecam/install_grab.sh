#!/bin/bash
# eyecam-grab を「cap_sys_ptrace だけを持つ」ファイルとしてインストールする。一度だけ sudo で実行する。
# これで eyecam-rec --serve が自分で grab を起動でき、毎回の sudo が要らなくなる。
#
#   sudo ./install_grab.sh                      # 隣の eyecam-grab（なければ ./target/release/eyecam-grab）を入れる
#   sudo ./install_grab.sh path/to/eyecam-grab  # ビルドしたものを指定して入れる
#   sudo ./install_grab.sh --uninstall          # 取り除く
#
# 置き場所は /home/.eyecam/eyecam-grab（root 所有 0755、ディレクトリも root 所有 0755）。
# SteamOS では /var が A/B スロットごとの小さなパーティションで、アップデートで切り替わると消えうるため、
# アップデートしても残る /home パーティションの、ユーザーが書き込めない root のディレクトリに置く。
set -euo pipefail

DEST_DIR=/home/.eyecam
DEST="$DEST_DIR/eyecam-grab"

if [ "$(id -u)" -ne 0 ]; then
    echo "sudo で実行してね: sudo $0 $*" >&2
    exit 1
fi

if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$DEST"
    rmdir "$DEST_DIR" 2>/dev/null || true
    echo "取り除いた: $DEST"
    exit 0
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
# 引数なし: このスクリプトの隣の eyecam-grab（~/.local/lib/eyecam に入れたもの）、なければビルドしたもの
if [ -n "${1:-}" ]; then
    SRC="$1"
elif [ -f "$HERE/eyecam-grab" ]; then
    SRC="$HERE/eyecam-grab"
else
    SRC="$HERE/target/release/eyecam-grab"
fi
if [ ! -f "$SRC" ] || [ -L "$SRC" ]; then
    echo "eyecam-grab が見つからない: $SRC" >&2
    exit 1
fi
if [ -L "$DEST_DIR" ] || { [ -e "$DEST_DIR" ] && [ ! -d "$DEST_DIR" ]; }; then
    echo "$DEST_DIR がふつうのディレクトリじゃないので触らない" >&2
    exit 1
fi

echo "入れるファイル: $SRC"
sha256sum "$SRC"

# root 所有・ほかの人は書けないディレクトリに、root 所有・0755 のコピーを置き、能力を付けてから名前を付け替える
# （途中の状態のファイルが $DEST として見えることはない）。
install -d -o root -g root -m 0755 "$DEST_DIR"
tmp="$(mktemp "$DEST_DIR/.eyecam-grab.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
install -o root -g root -m 0755 "$SRC" "$tmp"
setcap cap_sys_ptrace=ep "$tmp"
mv -f "$tmp" "$DEST"
trap - EXIT

caps="$(getcap "$DEST")"
echo "$caps"
case "$caps" in
    *cap_sys_ptrace=ep*) ;;
    *) echo "能力が付いていない（setcap が効いていない）" >&2; exit 1 ;;
esac
ls -l "$DEST"
sha256sum "$DEST"
echo "インストールした。eyecam-rec --serve は次から自動でバッファを取りに行く。"
