#!/usr/bin/env python3
"""把两张 BMP 的差异可视化：差异像素染成红色叠在新图上，保存 PNG。

用途：bmpdiff.py 给出"多少像素不同"，但说不出"哪里不同、是不是丢了东西"。
这个脚本把差异位置直接画出来（红色 = 不同），一眼就能判断是：
  - 文字重排/抗锯齿差异（零星点）→ 可接受
  - 某个控件整体偏移（成片条带）   → 真 bug，要修
  - 要素整块消失（成片同色）       → 回归

注意：SDL_SaveBMP 写的是**自底向上**的 BMP（height > 0），读的时候要翻。
scripts/bmpdiff.py 里的 read_bmp 不翻（两张图同样不翻 ⇒ 逐像素比对仍然正确，
只是坐标方向与屏幕相反）；这里做可视化时按屏幕方向翻回来。

用法: python3 scripts/bmpdiff_vis.py a.bmp b.bmp out.png [--zoom N]
"""
import struct
import sys
import zlib


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        raise SystemExit('不是 BMP: %s' % path)
    off = struct.unpack_from('<I', data, 10)[0]
    hdr = struct.unpack_from('<I', data, 14)[0]
    if hdr < 40:
        raise SystemExit('不支持的 BMP 头 %d' % hdr)
    w = struct.unpack_from('<i', data, 18)[0]
    h = struct.unpack_from('<i', data, 22)[0]
    if struct.unpack_from('<H', data, 28)[0] != 24:
        raise SystemExit('只支持 24 位 BMP')
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        row = data[off + y * stride: off + y * stride + w * 3]
        rows.append(row)
    if h > 0:
        rows.reverse()                     # bottom-up → 屏幕方向
    px = bytearray()
    for row in rows:
        for x in range(w):
            px += bytes((row[3 * x + 2], row[3 * x + 1], row[3 * x]))
    return w, abs(h), px


def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(tag, payload):
        c = tag + payload
        return struct.pack('>I', len(payload)) + c + struct.pack('>I', zlib.crc32(c))

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 6))
           + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    w1, h1, p1 = read_bmp(sys.argv[1])
    w2, h2, p2 = read_bmp(sys.argv[2])
    out = sys.argv[3]
    zoom = 1
    if '--zoom' in sys.argv:
        zoom = int(sys.argv[sys.argv.index('--zoom') + 1])
    if (w1, h1) != (w2, h2):
        raise SystemExit('尺寸不同: %dx%d vs %dx%d' % (w1, h1, w2, h2))

    n = w1 * h1
    diff = 0
    rgb = bytearray(p2)                    # 以新图为底
    for i in range(n):
        a = p1[3 * i:3 * i + 3]
        b = p2[3 * i:3 * i + 3]
        if a == b:
            continue
        diff += 1
        # 差异像素染红（保留一点原色便于看形状）
        g = (a[1] + b[1]) // 2
        rgb[3 * i + 0] = 255
        rgb[3 * i + 1] = g // 3
        rgb[3 * i + 2] = g // 3

    if zoom > 1:
        W, H = w1 * zoom, h1 * zoom
        big = bytearray()
        for y in range(h1):
            row = bytearray()
            for x in range(w1):
                i = (y * w1 + x) * 3
                row += bytes(rgb[i:i + 3]) * zoom
            for _ in range(zoom):
                big += row
        rgb = big
        w1, h1 = W, H

    write_png(out, w1, h1, rgb)
    print('diff_pixels    %d / %d  (%.3f%%)' % (diff, n, 100.0 * diff / n))
    print('wrote          %s  (%dx%d)' % (out, w1, h1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
