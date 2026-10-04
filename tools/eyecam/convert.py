"""eyecam のセッションを PC で見られる形にする（PNG の間引き書き出し / MP4）。

    pip install numpy pillow
    python convert.py rec_2026-10-03_15-00-00                # 30 フレームごとに PNG
    python convert.py rec_2026-10-03_15-00-00 --every 1      # 全フレーム PNG
    python convert.py rec_2026-10-03_15-00-00 --mp4          # eye_L.mp4 / eye_R.mp4（ffmpeg が PATH に要る）
    python convert.py rec_2026-10-03_15-00-00 --dump         # lock_dump.bin を画像化（枠合わせの確認用）

raw は無加工（右目は上下逆さま）で保存されているので、見る用にだけ右目を上下反転する（180° 回転ではない）。
"""

import argparse
import csv
import pathlib
import shutil
import subprocess

import numpy as np
from PIL import Image


def load_meta(session):
    """meta.txt の key=value を読む（同じ key は後のものが勝つ）。"""
    meta = {}
    for line in (session / "meta.txt").read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.startswith("#"):
            key, value = line.split("=", 1)
            meta[key] = value
    return meta


def frames(session, eye, width, height):
    """eye_L.raw / eye_R.raw をフレームの配列として開く（メモリマップ）。"""
    path = session / f"eye_{eye}.raw"
    if not path.exists() or path.stat().st_size == 0:
        return np.zeros((0, height, width), np.uint8)
    data = np.memmap(path, dtype=np.uint8, mode="r")
    return data[: data.size // (width * height) * width * height].reshape(-1, height, width)


def for_view(frame, eye):
    """見る用: パディング列を落とし、右目は上下反転する。"""
    frame = frame[:, :400]
    return np.flipud(frame) if eye == "R" else frame


def measured_fps(session, eye):
    """frames.csv の t_raw から実測のフレームレートを出す。"""
    with open(session / "frames.csv", newline="") as f:
        times = [float(row["t_raw"]) for row in csv.DictReader(f) if row["eye"] == eye]
    if len(times) < 2 or times[-1] <= times[0]:
        return 60.0
    return (len(times) - 1) / (times[-1] - times[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("session", type=pathlib.Path)
    parser.add_argument("--every", type=int, default=30, help="PNG を何フレームごとに書くか")
    parser.add_argument("--mp4", action="store_true", help="PNG の代わりに MP4 を作る")
    parser.add_argument("--dump", action="store_true", help="lock_dump.bin を 512 幅の画像にする")
    args = parser.parse_args()

    session = args.session
    meta = load_meta(session)
    if meta.get("framing") != "per_slot":
        print("注意: 09fbf3d までの eyecam で録ったセッション。スロット 0-3 の目（ふつうは L）は 64 列ずれて保存されていて、"
              "左端 64 列が欠け、右端 64 列はパディング（真っ黒）になっている。復元はできない。README の「古いセッション」参照")
    width, height = int(meta.get("frame_width", 400)), int(meta.get("frame_height", 400))

    if args.dump:
        dump = np.fromfile(session / "lock_dump.bin", dtype=np.uint8)
        rows = dump.size // 512
        Image.fromarray(dump[: rows * 512].reshape(rows, 512)).save(session / "lock_dump.png")
        print(f"{session / 'lock_dump.png'}: 512 x {rows}, offset {meta.get('lock_dump_offset')}")
        return

    for eye in ("L", "R"):
        video = frames(session, eye, width, height)
        if len(video) == 0:
            print(f"eye {eye}: no frames")
            continue
        if args.mp4:
            ffmpeg = shutil.which("ffmpeg")
            if not ffmpeg:
                raise SystemExit("ffmpeg が見つからない（PATH に入れてね）")
            fps = measured_fps(session, eye)
            out = session / f"eye_{eye}.mp4"
            cmd = [ffmpeg, "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "gray", "-s", "400x400",
                   "-r", f"{fps:.3f}", "-i", "-", "-pix_fmt", "yuv420p", "-crf", "18", str(out)]
            with subprocess.Popen(cmd, stdin=subprocess.PIPE) as proc:
                for frame in video:
                    proc.stdin.write(np.ascontiguousarray(for_view(frame, eye)).tobytes())
                proc.stdin.close()
            print(f"{out}: {len(video)} frames at {fps:.1f} fps")
        else:
            out_dir = session / f"png_{eye}"
            out_dir.mkdir(exist_ok=True)
            for i in range(0, len(video), args.every):
                Image.fromarray(for_view(video[i], eye)).save(out_dir / f"{i:06d}.png")
            print(f"{out_dir}: {len(range(0, len(video), args.every))} of {len(video)} frames")


if __name__ == "__main__":
    main()
