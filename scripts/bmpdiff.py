#!/usr/bin/env python3
"""逐像素比对两张同尺寸 BMP（比如优化前/后同一帧），给出差异率与分颜色统计。

为什么要这个脚本：本机没有 PIL/ImageMagick，而渲染优化经常是"线条还是那几条
线，但深浅/位置差一两个像素"，肉眼一看不出来、肉眼一看又觉得不一样。要判断
"这是重组一类样式，还是要素丢了"，必须落到数字。

用法: python3 scripts/bmpdiff.py a.bmp b.bmp
输出: 总差异像素占比 + 差异最集中的若干种 (旧色→新色) 转移。

读法：
  * 差异 <0.5% 且转移多是"同色系深浅互转" → 像素抖动，可接受
  * 出现"路/面的颜色 → 背景色"的转移占比很高 → 要素真的丢了，不可接受
"""
import struct
import sys
from collections import Counter


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        raise SystemExit('不是 BMP: %s' % path)
    off = struct.unpack_from('<I', data, 10)[0]
    hdr_size = struct.unpack_from('<I', data, 14)[0]
    if hdr_size < 40:
        raise SystemExit('不支持的 BMP 头 %d' % hdr_size)
    w = struct.unpack_from('<i', data, 18)[0]
    h = struct.unpack_from('<i', data, 22)[0]
    if struct.unpack_from('<H', data, 28)[0] != 24:
        raise SystemExit('只支持 24 位 BMP')
    top_down = h < 0
    h = abs(h)
    stride = (w * 3 + 3) & ~3
    rows = []
    for y in range(h):
        row = data[off + y * stride: off + y * stride + w * 3]
        rows.append(row)
    if top_down:
        rows.reverse()
    px = bytearray()
    for row in rows:                      # BMP 存的是 BGR
        for x in range(w):
            px += bytes((row[3 * x + 2], row[3 * x + 1], row[3 * x]))
    return w, h, px


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    w1, h1, p1 = read_bmp(sys.argv[1])
    w2, h2, p2 = read_bmp(sys.argv[2])
    if (w1, h1) != (w2, h2):
        raise SystemExit('尺寸不同: %dx%d vs %dx%d' % (w1, h1, w2, h2))

    diff = 0
    trans = Counter()
    n = w1 * h1
    for i in range(n):
        a = p1[3 * i:3 * i + 3]
        b = p2[3 * i:3 * i + 3]
        if a != b:
            diff += 1
            trans[(tuple(a), tuple(b))] += 1

    print('size           %dx%d' % (w1, h1))
    print('diff_pixels    %d / %d  (%.3f%%)' % (diff, n, 100.0 * diff / n))
    print('top转移 旧RGB -> 新RGB (占比按差异像素)')
    for (a, b), c in trans.most_common(8):
        print('   %-16s -> %-16s  %7d  %5.1f%%' % (
            '%d,%d,%d' % a, '%d,%d,%d' % b, c, 100.0 * c / max(diff, 1)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
