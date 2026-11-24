#!/usr/bin/env python3
"""
地球テクスチャ変換スクリプト（標準ライブラリ + macOS の sips のみ使用）

  正距円筒図法 (横:縦 = 2:1) の画像を、ファームに埋め込む生データへ変換する。

  python3 tools/make_texture.py --day    assets/earth_day.jpg  \
                                --night  assets/earth_night.jpg \
                                --clouds assets/earth_clouds_composite.jpg

  出力:
    data/earth_day.rgb565   1024x512  RGB565 little-endian   (1 MB)
    data/earth_night.gray8  1024x512  8bit 輝度 (都市の灯り)  (512 KB)
    data/earth_cloud.gray8  1024x512  8bit 雲の濃さ          (512 KB)

  雲は、雲入りの合成画像 (Blue Marble "Land_ocean_ice_cloud") と雲なしの昼の画像
  (Blue Marble "Land_shallow_topo") の差から取り出す。どちらも同じ投影・同じ位置合わせ。

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
LAND_GAMMA = 0.65        # 陸地の暗い色を持ち上げる強さ (1 = そのまま、小さいほど明るい)
SEA_GAMMA = 0.90         # 海の補正 (弱め)
LAND_KNEE = 0.50         # これより明るい陸地の色は圧縮する
LAND_HIGHLIGHT = 0.45    # 圧縮の強さ (1 = そのまま、小さいほど砂漠が暗く)
CLOUD_THRESHOLD = 0.30   # これより薄い差は雲とみなさない (大陸が見えるよう薄い雲は捨てる)
CLOUD_OPACITY = 0.80     # 最も濃い雲の不透明度
CLOUD_POLAR_FADE = 0.25  # 緯度 60° 以上の雲の残す割合 (海氷・氷床を雲と誤認するため)
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
    # 森林などの暗い陸地は、同じ光でも砂漠よりずっと暗く見える。
    # 陸地の暗い色を持ち上げ (ガンマ補正)、明るすぎる砂漠は抑える。海は深みを残すため弱く。
    def land_curve(x):
        y = (x / 255) ** LAND_GAMMA
        if y > LAND_KNEE:                      # 明るい所 (砂漠など) は圧縮
            y = LAND_KNEE + (y - LAND_KNEE) * LAND_HIGHLIGHT
        return int(255 * y + 0.5)
    lut_land = [land_curve(x) for x in range(256)]
    lut_sea = [int(255 * (x / 255) ** SEA_GAMMA + 0.5) for x in range(256)]
    buf = bytearray(W * H * 2)
    for i in range(W * H):
        r, g, b = rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]
        t = lut_sea if (b > r + 12 and b >= g) else lut_land
        r, g, b = t[r], t[g], t[b]
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


def write_clouds(day_rgb, comp_rgb):
    """雲入り画像と雲なし画像の「白さ」の差を雲の濃さとする"""
    buf = bytearray(W * H)
    for i in range(W * H):
        o = i * 3
        wc = min(comp_rgb[o], comp_rgb[o + 1], comp_rgb[o + 2])
        ws = min(day_rgb[o], day_rgb[o + 1], day_rgb[o + 2])
        a = (wc - ws) / max(16, 255 - ws)
        a = min(1.0, max(0.0, (a - CLOUD_THRESHOLD) / (1.0 - CLOUD_THRESHOLD)))
        lat = abs(0.5 - (i // W + 0.5) / H) * 180.0
        t = min(1.0, max(0.0, (lat - 58.0) / 12.0))
        polar = 1.0 - (1.0 - CLOUD_POLAR_FADE) * t
        buf[i] = int(a * a * (3 - 2 * a) * CLOUD_OPACITY * polar * 255)
    with open(os.path.join(DATA, "earth_cloud.gray8"), "wb") as f:
        f.write(buf)


def write_zero_clouds():
    with open(os.path.join(DATA, "earth_cloud.gray8"), "wb") as f:
        f.write(bytes(W * H))


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
    ap.add_argument("--clouds", help="雲入りの合成画像 (Land_ocean_ice_cloud)")
    ap.add_argument("--placeholder", action="store_true")
    a = ap.parse_args()
    os.makedirs(DATA, exist_ok=True)
    if a.placeholder:
        d, n = placeholder()
        write_day(d)
        write_night(n)
        write_zero_clouds()
    else:
        if not a.day:
            ap.error("--day か --placeholder を指定してください")
        day = load_rgb(a.day)
        write_day(day)
        if a.clouds:
            write_clouds(day, load_rgb(a.clouds))
        elif not os.path.exists(os.path.join(DATA, "earth_cloud.gray8")):
            write_zero_clouds()
        if a.night:
            write_night(load_rgb(a.night))
        else:
            write_night(bytearray(W * H * 3))
    print("written to", DATA)


if __name__ == "__main__":
    main()
