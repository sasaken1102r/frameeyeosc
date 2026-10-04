"""L と R を取り違えて録ったセッションを直す（eye_L.raw と eye_R.raw を入れ替え、frames.csv の eye 列も入れ替える）。

    python fix_swap.py rec_2026-10-04_00-41-34

a417dab までの eyecam は、どちらのカメラが L かを画の位置で決めていたので、顔の位置しだいで逆になることがあった
（meta.txt の slot_camera が 1,1,1,1,0,0,0,0 のセッション）。これはそれを直す。
- eye_L.raw と eye_R.raw を名前の付け替えで入れ替える（中身はコピーしない）
- frames.csv の eye 列の L と R を入れ替える（eye_index はそのまま。それぞれのファイルの何枚目かは変わらないため）
- meta.txt の最後に repaired_swap=1 を書く。2 回目は断る（--force で強行）
headers.bin・valve.csv・cues.csv は目に関係ないので触らない。書き換えるファイルは一時ファイル経由で置き換える。
"""

import argparse
import csv
import io
import os
import pathlib


def load_meta(session):
    """meta.txt の key=value を読む（同じ key は後のものが勝つ）。"""
    meta = {}
    for line in (session / "meta.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            meta[key] = value
    return meta


def replace_text(path, text):
    """一時ファイルに書いてから置き換える（ハードリンクされた元ファイルを書き換えないため）。"""
    tmp = path.with_name(path.name + ".fixtmp")
    tmp.write_text(text, encoding="utf-8", newline="")
    os.replace(tmp, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("session", type=pathlib.Path)
    parser.add_argument("--force", action="store_true", help="repaired_swap=1 があっても入れ替える")
    args = parser.parse_args()

    session = args.session
    meta = load_meta(session)
    if meta.get("repaired_swap") == "1" and not args.force:
        raise SystemExit("もう入れ替え済み（repaired_swap=1）。戻したいなら --force")
    frame = int(meta.get("frame_width", 400)) * int(meta.get("frame_height", 400))
    left, right = session / "eye_L.raw", session / "eye_R.raw"
    sizes = {p.name: p.stat().st_size for p in (left, right)}
    if any(size % frame for size in sizes.values()):
        raise SystemExit(f"raw のサイズが {frame} バイトの倍数じゃない: {sizes}")

    # frames.csv を先に読んで、行数が raw と合うか確かめてから触る
    with open(session / "frames.csv", newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        fields = reader.fieldnames
        rows = list(reader)
    counts = {eye: sum(1 for r in rows if r["eye"] == eye) for eye in ("L", "R")}
    if counts["L"] != sizes["eye_L.raw"] // frame or counts["R"] != sizes["eye_R.raw"] // frame:
        raise SystemExit(f"frames.csv の行数 {counts} と raw の枚数 "
                         f"{ {k: v // frame for k, v in sizes.items()} } が合わない。触らずにやめる")
    for r in rows:
        r["eye"] = {"L": "R", "R": "L"}[r["eye"]]
    out = io.StringIO()
    writer = csv.DictWriter(out, fieldnames=fields, lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)

    tmp = session / "eye_L.raw.fixtmp"
    os.replace(left, tmp)
    os.replace(right, left)
    os.replace(tmp, right)
    replace_text(session / "frames.csv", out.getvalue())
    note = ("\n# fix_swap.py\nrepaired_swap=1\n"
            "repaired_swap_note=eye_L.raw and eye_R.raw exchanged, frames.csv eye labels exchanged; "
            "slot_camera and labels above are the original (wrong) assignment\n")
    replace_text(session / "meta.txt", (session / "meta.txt").read_text(encoding="utf-8") + note)
    print(f"{session}: L/R を入れ替えた（L {counts['R']} 枚、R {counts['L']} 枚）")


if __name__ == "__main__":
    main()
