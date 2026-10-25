#!/usr/bin/env python3
"""浮层透明度的**像素级判据**：某个浮层设成 pct% 时，它在截图上到底透了没有。

为什么不能只判"两张图不一样"：那样"设 75% 结果画成 20%"也会判过。
这里用的是**与 alpha 成正比的精确关系**：

    设 opaque = 该浮层 100%（完全不透明）时的像素 O
      hidden  = 该浮层 0%   （完全隐藏）时的像素 H（= 底图像素）
      mid     = 该浮层 pct% 时的像素 M

    合成是线性的：M = a·O_color + (1−a)·H，其中 a = 元素自身底色 alpha × pct/100
    ⇒ |M − H| / |O − H| = pct / 100    （与元素自身 alpha 是多少无关！）

    所以"75% 真的是 75%"这条可以直接量出来，不必知道该浮层底色的固有 alpha
    （右栏按钮本身是 245 不是 255，用固定配色算就会假红）。

★ 顺带判**逐浮层独立**：改一个浮层的透明度，截图上变化的像素必须**全部落在
  该浮层的矩形内**（矩形外的变化 = 误伤别的浮层，或者根本没按浮层分开设）。

用法:
  python3 scripts/ovl_px.py <100%.bmp> <pct%.bmp> <0%.bmp> \\
      --rect=X,Y,W,H --pct=75 [--tol=0.04] [--strict-outside=1]

退出码 0 = 全部通过；1 = 有不符（每行判据前带 OK / FAIL）。
"""
import struct
import sys


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
        raise SystemExit('只支持 24 位 BMP: %s' % path)
    top_down = h < 0
    h = abs(h)
    stride = (w * 3 + 3) & ~3
    rows = [data[off + y * stride: off + y * stride + w * 3] for y in range(h)]
    # ★ h>0 表示"自下而上"存储（文件第一行是屏幕最后一行）⇒ 必须翻过来，
    #   否则 y 坐标是镜像的（bmpdiff.py 只做同比对，翻不翻不影响差异率，
    #   但这里要按绝对坐标取"标题栏/按钮"的像素，翻错了就全错）。
    if not top_down:
        rows.reverse()
    px = bytearray()
    for row in rows:                      # BMP 存的是 BGR
        for x in range(w):
            px += bytes((row[3 * x + 2], row[3 * x + 1], row[3 * x]))
    return w, h, px


def main():
    argv = sys.argv[1:]
    pos = [arg for arg in argv if not arg.startswith('--')]
    if len(pos) != 3:
        raise SystemExit(__doc__)
    f_opaque, f_mid, f_hidden = pos

    rect = None
    pct = 75.0
    tol = 0.04
    strict_outside = 1
    for arg in argv:
        kv = arg.split('=', 1)
        key = kv[0]
        val = kv[1] if len(kv) > 1 else ''
        if key == '--rect':
            rect = tuple(int(v) for v in val.split(','))
        elif key == '--pct':
            pct = float(val)
        elif key == '--tol':
            tol = float(val)
        elif key == '--strict-outside':
            strict_outside = int(val)
    if not rect or len(rect) != 4:
        raise SystemExit('需要 --rect=X,Y,W,H')
    rx, ry, rw, rh = rect

    w1, h1, po = read_bmp(f_opaque)
    w2, h2, pm = read_bmp(f_mid)
    w3, h3, ph = read_bmp(f_hidden)
    if (w1, h1) != (w2, h2) or (w1, h1) != (w3, h3):
        raise SystemExit('三张图尺寸不一致')

    def at(px, x, y):
        i = 3 * (y * w1 + x)
        return px[i], px[i + 1], px[i + 2]

    fails = 0

    # ---- 取样点：矩形内 |O−H| 最大的那个像素（浮层确实覆盖、且对比明显）----
    best = None
    for y in range(ry, min(ry + rh, h1)):
        for x in range(rx, min(rx + rw, w1)):
            o, h = at(po, x, y), at(ph, x, y)
            d = abs(o[0] - h[0]) + abs(o[1] - h[1]) + abs(o[2] - h[2])
            if best is None or d > best[0]:
                best = (d, x, y)
    if not best or best[0] < 24:
        print('FAIL 矩形 %d,%d,%d,%d 内找不到"100%% 与 0%% 明显不同"的像素 '
              '(浮层没画出来？矩形给错了？)' % (rx, ry, rw, rh))
        return 1
    _, sx, sy = best
    O, M, H = at(po, sx, sy), at(pm, sx, sy), at(ph, sx, sy)

    # ---- 判据 1：pct% 时该点确实处在 O 与 H 之间（透了一点，但没全透）----
    moved = sum(abs(M[c] - H[c]) for c in range(3))
    full = sum(abs(O[c] - H[c]) for c in range(3))
    if moved > 0 and moved < full:
        print('OK   pct=%g 时取样点 (%d,%d) 介于不透明与全透之间：%s → %s → %s'
              % (pct, sx, sy, O, M, H))
    else:
        print('FAIL pct=%g 时取样点 (%d,%d) 不在 O 与 H 之间：O=%s M=%s H=%s'
              % (pct, sx, sy, O, M, H))
        fails += 1

    # ---- 判据 2（核心）：|M−H| / |O−H| == pct/100 ----
    # 取 |O−H| 最大的通道算比值（量纲最大、舍入影响最小）
    ci = max(range(3), key=lambda c: abs(O[c] - H[c]))
    denom = abs(O[ci] - H[ci])
    ratio = abs(M[ci] - H[ci]) / denom if denom else 0.0
    want = pct / 100.0
    ok = abs(ratio - want) <= tol
    print('%s 混合比 |M−H|/|O−H| = %.4f（期望 %.4f ±%.2f，通道 %s）'
          % ('OK  ' if ok else 'FAIL', ratio, want, tol, 'RGB'[ci]))
    if not ok:
        fails += 1

    # ---- 判据 3：逐浮层独立 —— 矩形外一个像素都不许变 ----
    out = 0
    inside = 0
    for y in range(h1):
        for x in range(w1):
            i = 3 * (y * w1 + x)
            if pm[i] != ph[i] or pm[i + 1] != ph[i + 1] or pm[i + 2] != ph[i + 2]:
                if rx <= x < rx + rw and ry <= y < ry + rh:
                    inside += 1
                else:
                    out += 1
    if inside == 0:
        print('FAIL 矩形内没有像素随该浮层的透明度变化（等于没生效）')
        fails += 1
    else:
        print('OK   矩形内随透明度变化的像素 %d 个' % inside)
    if strict_outside:
        if out == 0:
            print('OK   矩形外 0 个像素受影响（改这个浮层没误伤别的浮层）')
        else:
            print('FAIL 矩形外有 %d 个像素跟着变了（透明度没按浮层分开设）' % out)
            fails += 1

    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
