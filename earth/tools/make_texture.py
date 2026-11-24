#!/usr/bin/env python3
"""
地球テクスチャ変換スクリプト（標準ライブラリ + macOS の sips のみ使用）

  正距円筒図法 (横:縦 = 2:1) の画像を、ファームに埋め込む生データへ変換する。

  python3 tools/make_texture.py --day   assets/earth_day.jpg  \
                                --night assets/earth_night.jpg

  出力:
    data/earth_day.rgb565   1024x512  RGB565 little-endian   (1 MB)
    data/earth_night.gray8  1024x512  8bit 輝度 (都市の灯り)  (512 KB)

  画像が無い場合は --placeholder で手続き生成の仮テクスチャを作る。
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

W, H = 1024, 512
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")


def load_rgb(path):
    """sips で W x H の BMP に変換して RGB の bytearray (W*H*3) を返す"""
    with tempfile.TemporaryDirectory() as td:
        bmp = os.path.join(td, "t.bmp")
        subprocess.run(
            ["sips", "-s", "format", "bmp", "-z", str(H), str(W), path, "--out", bmp],
            check=True, stdout=subprocess.DEVNULL,
        )
        with open(bmp, "rb") as f:
            b = f.read()
    if b[:2] != b"BM":
        sys.exit("BMP 変換に失敗しました: " + path)
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    if bpp not in (24, 32):
        sys.exit(f"未対応の BMP ({bpp}bpp)")
    bottom_up = h > 0
    h = abs(h)
    if (w, h) != (W, H):
        sys.exit(f"サイズが想定外です: {w}x{h}")
    step = bpp // 8
    stride = (w * step + 3) & ~3
    out = bytearray(W * H * 3)
    for y in range(H):
        sy = (H - 1 - y) if bottom_up else y
        row = off + sy * stride
        for x in range(W):
            p = row + x * step
            o = (y * W + x) * 3
            out[o] = b[p + 2]
            out[o + 1] = b[p + 1]
            out[o + 2] = b[p]
    return out


def write_day(rgb):
    buf = bytearray(W * H * 2)
    for i in range(W * H):
        r, g, b = rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        struct.pack_into("<H", buf, i * 2, v)
    with open(os.path.join(DATA, "earth_day.rgb565"), "wb") as f:
        f.write(buf)


def write_night(rgb):
    buf = bytearray(W * H)
    for i in range(W * H):
        r, g, b = rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]
        buf[i] = (r * 77 + g * 150 + b * 29) >> 8
    with open(os.path.join(DATA, "earth_night.gray8"), "wb") as f:
        f.write(buf)


def placeholder():
    """値ノイズで大陸っぽい模様を作る（実際の地形ではない）"""
    rnd = random.Random(1)
    G = 16
    grid = [[rnd.random() for _ in range(G * 2)] for _ in range(G + 1)]

    def noise(u, v, f):
        x, y = u * 2 * G * f / 4, v * G * f / 4
        x0, y0 = int(x), int(y)
        fx, fy = x - x0, y - y0
        fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
        gx = lambda i: grid[min(y0 + i, G)]
        a = gx(0)[x0 % (2 * G)] * (1 - fx) + gx(0)[(x0 + 1) % (2 * G)] * fx
        c = gx(1)[x0 % (2 * G)] * (1 - fx) + gx(1)[(x0 + 1) % (2 * G)] * fx
        return a * (1 - fy) + c * fy

    day = bytearray(W * H * 3)
    night = bytearray(W * H * 3)
    for y in range(H):
        lat = abs(y / H - 0.5) * 2
        for x in range(W):
            u, v = x / W, y / H
            h = noise(u, v, 4) * 0.55 + noise(u, v, 8) * 0.3 + noise(u, v, 16) * 0.15
            o = (y * W + x) * 3
            if lat > 0.88:
                c = (235, 240, 245)
            elif h > 0.55:
                t = min(1, (h - 0.55) * 5)
                c = (int(60 + 120 * t), int(110 + 40 * t), int(40 + 30 * t))
                if rnd.random() < 0.004:
                    night[o:o + 3] = b"\xff\xff\xff"
            else:
                c = (10, 30 + int(60 * h), 70 + int(80 * h))
            day[o:o + 3] = bytes(c)
    return day, night


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--day")
    ap.add_argument("--night")
    ap.add_argument("--placeholder", action="store_true")
    a = ap.parse_args()
    os.makedirs(DATA, exist_ok=True)
    if a.placeholder:
        d, n = placeholder()
        write_day(d)
        write_night(n)
    else:
        if not a.day:
            ap.error("--day か --placeholder を指定してください")
        write_day(load_rgb(a.day))
        if a.night:
            write_night(load_rgb(a.night))
        else:
            write_night(bytearray(W * H * 3))
    print("written to", DATA)


if __name__ == "__main__":
    main()
