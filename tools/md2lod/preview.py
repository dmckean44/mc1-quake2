#!/usr/bin/env python3
"""Render an MD2 model from two pak files side by side to compare simplification."""

import argparse
import struct
import zlib

import numpy as np

from md2lod import Md2, read_pak


def read_pcx(data):
    xmin, ymin, xmax, ymax = struct.unpack_from("<4H", data, 4)
    width, height = xmax - xmin + 1, ymax - ymin + 1
    bytes_per_line = struct.unpack_from("<H", data, 66)[0]
    out = bytearray()
    i = 128
    while len(out) < bytes_per_line * height:
        b = data[i]
        i += 1
        if b >= 0xC0:
            out += bytes([data[i]]) * (b & 0x3F)
            i += 1
        else:
            out.append(b)
    pixels = np.frombuffer(bytes(out[:bytes_per_line * height]), np.uint8).reshape(height, bytes_per_line)
    palette = np.frombuffer(data[-768:], np.uint8).reshape(256, 3)
    return pixels[:, :width], palette


def render(md2, frame, skin, size, yaw):
    pos = md2.positions()[frame]
    c, s = np.cos(yaw), np.sin(yaw)
    x = pos[:, 0] * c - pos[:, 1] * s
    y = pos[:, 0] * s + pos[:, 1] * c
    z = pos[:, 2]
    extent = max(np.ptp(x), np.ptp(z)) * 1.1
    sx = (x - (x.max() + x.min()) / 2) / extent * size + size / 2
    sy = size / 2 - (z - (z.max() + z.min()) / 2) / extent * size
    depth = y
    image = np.full((size, size), 0, np.uint8)
    zbuf = np.full((size, size), np.inf)
    st = md2.st.astype(np.float64)
    for (a, b, cc), (ta, tb, tc) in zip(md2.tri_xyz, md2.tri_st):
        px = np.array([sx[a], sx[b], sx[cc]])
        py = np.array([sy[a], sy[b], sy[cc]])
        x0, x1 = int(max(px.min(), 0)), int(min(px.max() + 1, size))
        y0, y1 = int(max(py.min(), 0)), int(min(py.max() + 1, size))
        if x0 >= x1 or y0 >= y1:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y0, y1) + 0.5)
        det = (py[1] - py[2]) * (px[0] - px[2]) + (px[2] - px[1]) * (py[0] - py[2])
        if abs(det) < 1e-9:
            continue
        w0 = ((py[1] - py[2]) * (gx - px[2]) + (px[2] - px[1]) * (gy - py[2])) / det
        w1 = ((py[2] - py[0]) * (gx - px[2]) + (px[0] - px[2]) * (gy - py[2])) / det
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        d = w0 * depth[a] + w1 * depth[b] + w2 * depth[cc]
        u = w0 * st[ta, 0] + w1 * st[tb, 0] + w2 * st[tc, 0]
        v = w0 * st[ta, 1] + w1 * st[tb, 1] + w2 * st[tc, 1]
        sub = zbuf[y0:y1, x0:x1]
        hit = inside & (d < sub)
        sub[hit] = d[hit]
        u = np.clip(u.astype(int), 0, skin.shape[1] - 1)
        v = np.clip(v.astype(int), 0, skin.shape[0] - 1)
        image[y0:y1, x0:x1][hit] = skin[v[hit], u[hit]]
    return image


def write_png(path, rgb):
    h, w, _ = rgb.shape
    raw = b"".join(b"\0" + rgb[y].tobytes() for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original")
    parser.add_argument("simplified")
    parser.add_argument("model")
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("--frames", default="0")
    parser.add_argument("--size", type=int, default=256)
    args = parser.parse_args()

    base = read_pak(args.original)
    small = read_pak(args.simplified)
    _, palette = read_pcx(base["pics/colormap.pcx"])
    a, b = Md2(base[args.model]), Md2(small[args.model])
    skin_name = a.skins[0].split(b"\0")[0].decode()
    skin, _ = read_pcx(base[skin_name])
    rows = []
    for frame in (int(f) for f in args.frames.split(",")):
        tiles = []
        for yaw in (0.6, 2.4):
            tiles += [render(a, frame, skin, args.size, yaw), render(b, frame, skin, args.size, yaw)]
        rows.append(np.concatenate(tiles, axis=1))
    write_png(args.output, palette[np.concatenate(rows, axis=0)])


if __name__ == "__main__":
    main()
