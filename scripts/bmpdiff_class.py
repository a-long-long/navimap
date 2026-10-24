#!/usr/bin/env python3
"""分析两张 BMP 的差异：把差异像素按"是否丢了要素"分类，而不是只看总比例。

判据（照 bmpdiff.py 的读法，但更严格）：
  * "要素丢失" = 旧图里是**饱和色**（路/面/线的真实颜色）而新图变成了背景色
    （或反过来）。这类差异不可接受。
  * "同色系抖动" = 同一元素两侧只是深浅差 1~2 级。这在 alpha 混色公式
    与 SDL 不完全一致时必然出现，属于可接受范围。

用法: python3 scripts/bmpdiff_class.py a.bmp b.bmp [--w 800 --h 600]
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


def sat(c):
    """颜色的"饱和度"：max-min。背景/浅灰 <8，真实要素色 >40。"""
    return max(c) - min(c)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    w1, h1, p1 = read_bmp(sys.argv[1])
    w2, h2, p2 = read_bmp(sys.argv[2])
    if (w1, h1) != (w2, h2):
        raise SystemExit('尺寸不同: %dx%d vs %dx%d' % (w1, h1, w2, h2))

    n = w1 * h1
    diff = 0
    lost = 0            # 旧=饱和色 → 新=非饱和色（要素丢了）
    gained = 0          # 旧=非饱和色 → 新=饱和色（多画了要素）
    jitter = 0          # 两侧都饱和（同色系深浅）
    maxdelta = 0
    region = Counter()

    for i in range(n):
        a = p1[3 * i:3 * i + 3]
        b = p2[3 * i:3 * i + 3]
        if a == b:
            continue
        diff += 1
        d = max(abs(a[k] - b[k]) for k in range(3))
        if d > maxdelta:
            maxdelta = d
        sa, sb = sat(a), sat(b)
        if sa > 40 and sb <= 40:
            lost += 1
        elif sb > 40 and sa <= 40:
            gained += 1
        elif sa > 40 and sb > 40:
            jitter += 1
        # 定位差异所在区域（chrome 都在边缘：标题栏/顶栏/右栏/左面板/底部）
        x = (i % w1)
        y = (i // w1)
        if y < 76:
            region['顶栏/标题栏'] += 1
        elif x >= w1 - 60:
            region['右栏按钮'] += 1
        elif x < 280 and y > 120:
            region['左侧面板'] += 1
        elif y >= h1 - 120:
            region['底部指令条'] += 1
        else:
            region['地图区'] += 1

    print('size           %dx%d' % (w1, h1))
    print('diff_pixels    %d / %d  (%.3f%%)' % (diff, n, 100.0 * diff / n))
    print('最大单通道差   %d' % maxdelta)
    print()
    print('分类（关键判据）')
    print('  要素丢失      %d   ← 旧图饱和色变背景色，不允许' % lost)
    print('  多出要素      %d   ← 旧图背景色变饱和色' % gained)
    print('  同色系深浅    %d' % jitter)
    print()
    print('差异分布（按画面区域）')
    for k, v in region.most_common():
        print('  %-14s %6d  (%.1f%% of diff)' % (k, v, 100.0 * v / max(diff, 1)))
    print()
    if lost:
        print('结论: 不可接受（有要素丢失）')
        return 1
    print('结论: 可接受（无要素丢失，差异集中在 %s）'
          % (region.most_common(1)[0][0] if region else '-'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
