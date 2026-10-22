#!/usr/bin/env python3
"""把 SDL_SaveBMP 出来的 BMP 转成 PNG（纯标准库，不装 Pillow）。

本机没有 PIL，而 Read 工具看不了 BMP，所以离屏渲染的结果要过这一道才能肉眼检查。
用法: python3 scripts/bmp2png.py in.bmp out.png
"""
import struct
import sys
import zlib


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        raise SystemExit('不是 BMP')
    off = struct.unpack_from('<I', data, 10)[0]
    hdr_size = struct.unpack_from('<I', data, 14)[0]
    if hdr_size < 40:
        raise SystemExit('不支持的 BMP 头 %d' % hdr_size)
    w, h = struct.unpack_from('<ii', data, 18)
    planes, bpp = struct.unpack_from('<HH', data, 26)
    compression = struct.unpack_from('<I', data, 30)[0]
    if compression != 0:
        raise SystemExit('只支持 BI_RGB，收到 %d' % compression)
    top_down = h < 0
    h = abs(h)
    if bpp not in (24, 32):
        raise SystemExit('只支持 24/32 bpp，收到 %d' % bpp)

    stride = ((w * bpp + 31) // 32) * 4
    px = bytearray()
    for row in range(h):
        src_row = row if top_down else (h - 1 - row)
        base = off + src_row * stride
        out = bytearray()
        for x in range(w):
            p = base + x * (bpp // 8)
            b, g, r = data[p], data[p + 1], data[p + 2]
            out += bytes((r, g, b))
        px += out
    return w, h, bytes(px)


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('用法: bmp2png.py in.bmp out.png')
    w, h, px = read_bmp(sys.argv[1])
    write_png(sys.argv[2], w, h, px)
    print('wrote %s (%dx%d)' % (sys.argv[2], w, h))
