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
#
# メッセージは英語と日本語の 2 行で出す（Frame のシステムの言語はいつも英語で、Steam の表示言語からは見分けられないため）。
set -euo pipefail

# say "English" "日本語": 両方を続けて出す（エラーは say_err で標準エラーへ）
say() { printf '%s\n%s\n' "$1" "$2"; }
say_err() { say "$1" "$2" >&2; }

DEST_DIR=/home/.eyecam
DEST="$DEST_DIR/eyecam-grab"
# 前の置き場所（A/B スロットごとの /var）。見つけたら能力を外して消す
OLD_DEST=/var/lib/eyecam/eyecam-grab

if [ "$(id -u)" -ne 0 ]; then
    say_err "Run this with sudo: sudo $0 $*" "sudo で実行してね: sudo $0 $*"
    exit 1
fi

# 置き換える・消す前に、まず能力を外す。ファイルが別の名前（ハードリンク）でも残っていたとき、
# 古いコピーが能力を持ったまま生き残らないようにするため
drop_cap() {
    if [ -f "$1" ] && [ ! -L "$1" ]; then
        setcap -r "$1" 2>/dev/null || true
    fi
}

remove_old() {
    drop_cap "$OLD_DEST"
    rm -f "$OLD_DEST"
    rmdir "$(dirname "$OLD_DEST")" 2>/dev/null || true
}

if [ "${1:-}" = "--uninstall" ]; then
    drop_cap "$DEST"
    rm -f "$DEST"
    rmdir "$DEST_DIR" 2>/dev/null || true
    remove_old
    say "Removed: $DEST" "取り除いた: $DEST"
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
    say_err "eyecam-grab not found: $SRC" "eyecam-grab が見つからない: $SRC"
    exit 1
fi
if [ -L "$DEST_DIR" ] || { [ -e "$DEST_DIR" ] && [ ! -d "$DEST_DIR" ]; }; then
    say_err "$DEST_DIR is not a plain directory, leaving it alone" "$DEST_DIR がふつうのディレクトリじゃないので触らない"
    exit 1
fi

say "Installing: $SRC" "入れるファイル: $SRC"
sha256sum "$SRC"

# root 所有・ほかの人は書けないディレクトリに、root 所有・0755 のコピーを置き、能力を付けてから名前を付け替える
# （途中の状態のファイルが $DEST として見えることはない）。
install -d -o root -g root -m 0755 "$DEST_DIR"
tmp="$(mktemp "$DEST_DIR/.eyecam-grab.XXXXXX")"
trap 'rm -f "$tmp"' EXIT
install -o root -g root -m 0755 "$SRC" "$tmp"
setcap cap_sys_ptrace=ep "$tmp"
drop_cap "$DEST"
mv -f "$tmp" "$DEST"
trap - EXIT
remove_old

caps="$(getcap "$DEST")"
echo "$caps"
case "$caps" in
    *cap_sys_ptrace=ep*) ;;
    *) say_err "The capability is not set (setcap did not take effect)" "能力が付いていない（setcap が効いていない）"; exit 1 ;;
esac
ls -l "$DEST"
sha256sum "$DEST"
say "Installed. You can close this window: the panel picks it up by itself." \
    "インストールした。このウィンドウは閉じていいよ（パネルが自動で使いはじめる）。"
