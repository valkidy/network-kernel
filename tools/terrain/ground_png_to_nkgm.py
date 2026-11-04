#!/usr/bin/env python3
"""Cook a ground height map exported by unity-terrain-example's GroundMapExporter
(16-bit grayscale PNG, north row first, 1.2 m per sample) into NKGM v1
(game_server/src/terrain/terrain_assets.h): the same samples, rows south first,
little-endian. The runtime never decodes PNG.

    ground_png_to_nkgm.py <in.png> <out.nkgm>
"""
import struct
import sys
import zlib


def read_png16_gray(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")
    pos, idat, ihdr = 8, b"", None
    while pos < len(data):
        (n,) = struct.unpack(">I", data[pos:pos + 4])
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]
        (crc,) = struct.unpack(">I", data[pos + 8 + n:pos + 12 + n])
        if zlib.crc32(kind + body) & 0xFFFFFFFF != crc:
            raise SystemExit(f"{path}: bad CRC in {kind!r}")
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, depth, color, _, _, interlace = ihdr
    if (depth, color, interlace) != (16, 0, 0):
        raise SystemExit(f"{path}: want 16-bit grayscale, non-interlaced (got depth {depth}, color {color})")
    raw, bpp, stride = zlib.decompress(idat), 2, w * 2
    prev, rows, q = bytearray(stride), [], 0
    for _ in range(h):
        f, line = raw[q], bytearray(raw[q + 1:q + 1 + stride])
        q += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([(line[2 * x] << 8) | line[2 * x + 1] for x in range(w)])
        prev = line
    return w, h, rows


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    w, h, rows = read_png16_gray(sys.argv[1])
    out = bytearray(b"NKGM" + struct.pack("<HHIIII", 1, 0, w, h, 1200, 0))
    for row in reversed(rows):  # PNG is north first; NKGM is south first
        out += struct.pack(f"<{w}H", *row)
    open(sys.argv[2], "wb").write(out)


if __name__ == "__main__":
    main()
