#!/usr/bin/env python3
"""浮层透明度的**像素级判据**：某个浮层设成 pct% 时，它在截图上到底透了没有。

规则（2026-10-25 J 先生要求改成"只淡底板、文字保持清晰"）：
    · **底板 / 描边**按 pct 线性半透明；
    · **文字 / 图标**不参与线性淡出 —— pct=0 整个浮层隐藏，pct>0 前景完全不透明。

为什么不能只判"两张图不一样"：那样"设 75% 结果画成 20%"也会判过。
这里用的是**与 alpha 成正比的精确关系**：

    设 opaque = 该浮层 100%（完全不透明）时的像素 O
      hidden  = 该浮层 0%   （完全隐藏）时的像素 H（= 底图像素）
      mid     = 该浮层 pct% 时的像素 M

    合成是线性的：M = a·O_color + (1−a)·H，其中 a = 元素自身底色 alpha × pct/100
    ⇒ |M − H| / |O − H| = pct / 100    （与元素自身 alpha 是多少无关！）

    所以"75% 真的是 75%"这条可以直接量出来，不必知道该浮层底色的固有 alpha
    （右栏按钮本身是 225 不是 255，用固定配色算就会假红）。

★ 两条判据合起来才把"只淡底板"钉死：
    1. **底板**（矩形内像素的中位数）比值 == pct/100
       —— 中位数而不是单点取样：底板像素占多数，单点很容易采到文字/图标上。
    2. **前景**（文字/图标）必须**有**一批像素比值 == 1.0（一点没淡）
       —— 前景像素的比值是 1.0（100% 与 75% 两张图上逐像素相同），
          底板像素是 pct/100。两者混在一起，中位数取底板那一簇，
          前景那一簇用"数量"来判。
    3. **逐浮层独立**：改一个浮层的透明度，截图上变化的像素必须全部落在
       该浮层的矩形内（矩形外的变化 = 误伤别的浮层，或者根本没按浮层分开设）。

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


def median(vals):
    s = sorted(vals)
    n = len(s)
    if n == 0:
        return None
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def main():
    argv = sys.argv[1:]
    pos = [arg for arg in argv if not arg.startswith('--')]
    if len(pos) != 3:
        raise SystemExit(__doc__)
    f_opaque, f_mid, f_hidden = pos

    rect = None
    fgrect = None
    fg_min = 0
    pct = 75.0
    tol = 0.04
    strict_outside = 1
    check_fg = 1
    for arg in argv:
        kv = arg.split('=', 1)
        key = kv[0]
        val = kv[1] if len(kv) > 1 else ''
        if key == '--rect':
            rect = tuple(int(v) for v in val.split(','))
        elif key == '--fgrect':
            fgrect = tuple(int(v) for v in val.split(','))
        elif key == '--fg-min':
            fg_min = int(val)
        elif key == '--pct':
            pct = float(val)
        elif key == '--tol':
            tol = float(val)
        elif key == '--strict-outside':
            strict_outside = int(val)
        elif key == '--fg':
            check_fg = int(val)
    if not rect or len(rect) != 4:
        raise SystemExit('需要 --rect=X,Y,W,H')
    rx, ry, rw, rh = rect
    fx, fy, fw, fh = fgrect if fgrect else rect

    w1, h1, po = read_bmp(f_opaque)
    w2, h2, pm = read_bmp(f_mid)
    w3, h3, ph = read_bmp(f_hidden)
    if (w1, h1) != (w2, h2) or (w1, h1) != (w3, h3):
        raise SystemExit('三张图尺寸不一致')

    def at(px, x, y):
        i = 3 * (y * w1 + x)
        return px[i], px[i + 1], px[i + 2]

    fails = 0

    # ---- 收集矩形内每个像素的"透过比" r = |M−H| / |O−H| ----
    #   底板像素 ⇒ r ≈ pct/100；前景（文字/图标）⇒ r = 1.0（一点没淡）。
    #   只统计"确实变了"的像素（|O−H| 足够大），否则噪声会拉偏中位数。
    ratios = []
    changed = 0
    for y in range(ry, min(ry + rh, h1)):
        for x in range(rx, min(rx + rw, w1)):
            O, M, H = at(po, x, y), at(pm, x, y), at(ph, x, y)
            ci = max(range(3), key=lambda c: abs(O[c] - H[c]))
            denom = abs(O[ci] - H[ci])
            if denom < 8:               # 没变（或变化太小，量不准）
                continue
            changed += 1
            ratios.append(abs(M[ci] - H[ci]) / denom)

    if not ratios:
        print('FAIL 矩形 %d,%d,%d,%d 内找不到"100%% 与 0%% 明显不同"的像素 '
              '(浮层没画出来？矩形给错了？)' % (rx, ry, rw, rh))
        return 1

    want = pct / 100.0

    # ---- 判据 1（核心）：底板像素的中位比值 == pct/100 ----
    med = median(ratios)
    ok = med is not None and abs(med - want) <= tol
    print('%s 底板透过比中位数 |M−H|/|O−H| = %.4f（期望 %.4f ±%.2f，'
          '变化像素 %d 个）'
          % ('OK  ' if ok else 'FAIL', med if med is not None else -1,
             want, tol, changed))
    if not ok:
        fails += 1

    # ---- 判据 2：前景（文字/图标）保持清晰 ----
    #   前景**不参与线性淡出** ⇒ 它在 100% 与 pct% 两张图上应当**逐像素未变**。
    #   口径 a（主判据）：M == O 且 O != H —— 覆盖**纯色**前景
    #     （C 侧 rline 画的图标、Qt 1.8px 宽的矢量笔画）。
    #     实测判别力：C 右栏正向 92 个 / 把前景一起淡掉后 **0 个**。
    #   口径 b（仅在 --fgrect 明确框出前景时启用）：|e75| / |e100| > (1+k)/2，
    #     e = 像素与"0% 时的底图"的差。
    #     —— 覆盖**抗锯齿**前景（14px 中文笔画没有 coverage=1 的纯色核，
    #     口径 a 数不到；实测 Qt 搜索按钮「搜索」二字：正向 111 个 /
    #     文字被淡后 39 个）。
    #     ★ 口径 b 必须框准前景区域才有判别力：不框的话它在底板边缘有 129 个
    #       纯噪声（注入前后 129 vs 130，完全无判别力）。
    fg = 0
    fg_total = 0
    thr = (1.0 + want) / 2.0
    use_b = fgrect is not None
    for y in range(fy, min(fy + fh, h1)):
        for x in range(fx, min(fx + fw, w1)):
            i = 3 * (y * w1 + x)
            O = (po[i], po[i + 1], po[i + 2])
            M = (pm[i], pm[i + 1], pm[i + 2])
            H = (ph[i], ph[i + 1], ph[i + 2])
            if O == H:
                continue                       # 没被浮层覆盖，与前景无关
            fg_total += 1
            if M == O:
                fg += 1                       # (a) 逐像素未变
                continue
            if not use_b:
                continue
            e100 = abs(O[0] - H[0]) + abs(O[1] - H[1]) + abs(O[2] - H[2])
            if e100 < 20:
                continue
            e75 = abs(M[0] - H[0]) + abs(M[1] - H[1]) + abs(M[2] - H[2])
            if e75 / e100 > thr:
                fg += 1                       # (b) 偏离量没按比例缩小
    need = fg_min if fg_min > 0 else 60
    if not check_fg:
        print('SKIP 前景判据（--fg=0：这个浮层在本图里没有前景可验）')
    elif fg >= need:
        print('OK   前景（文字/图标）保持清晰：%d/%d 个像素未被淡（>= %d%s）'
              % (fg, fg_total, need,
                 '，含抗锯齿口径' if use_b else ''))
    else:
        print('FAIL 前景（文字/图标）没有保持清晰：未被淡的像素只有 %d/%d 个'
              '（需要 >= %d）—— 文字/图标被一起淡掉了？' % (fg, fg_total, need))
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
