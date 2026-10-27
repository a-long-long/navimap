#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""统计离屏 BMP 中的等高线棕色像素，做像素级断言。"""
import struct, sys

def read_bmp(path):
    with open(path, "rb") as f:
        d = f.read()
    assert d[:2] == b"BM", "not BMP"
    off = struct.unpack("<I", d[10:14])[0]
    w = struct.unpack("<i", d[18:22])[0]
    h = struct.unpack("<i", d[22:26])[0]
    # 24-bit: 每行 w*3，4 字节对齐，自下而上
    stride = (w * 3 + 3) & ~3
    px = bytearray(w * h * 3)
    for y in range(h):
        src = off + (h - 1 - y) * stride          # 自下而上 → 翻转
        row = d[src:src + w * 3]
        for x in range(w):
            b, g, r = row[x*3], row[x*3+1], row[x*3+2]
            i = (y * w + x) * 3
            px[i], px[i+1], px[i+2] = r, g, b      # 存成 RGB
    return w, h, px

def near(c, t, tol=14):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))

TARGETS = [(150, 105, 70), (195, 170, 145)]
# 宽松棕色判据：排除奶油底色(244,241,234)，保留所有棕调
def is_brown(r, g, b):
    return (r > b + 30) and (g > b + 10) and (r < 235) and (b < 175)

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        "D:/Users/Administrator/AppData/Local/Temp/shot_beijing.bmp"
    w, h, px = read_bmp(path)
    total = w * h
    exact = [0, 0]
    brown = 0
    # 取样：等距网格 4x4 降采样以加快
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            i = (y * w + x) * 3
            r, g, b = px[i], px[i+1], px[i+2]
            for k, t in enumerate(TARGETS):
                if near((r, g, b), t, 14):
                    exact[k] += 1
            if is_brown(r, g, b):
                brown += 1
    sampled = (h // 2 + 1) * (w // 2 + 1)
    print(f"图像 {w}x{h}  采样像素 {sampled}")
    print(f"计曲线色(150,105,70)±14 命中: {exact[0]}")
    print(f"首曲线色(195,170,145)±14 命中: {exact[1]}")
    print(f"宽松棕色像素(降采样): {brown}  ({100.0*brown/sampled:.3f}% 画面)")
    # 结论：只要两类等高线任一有命中即算渲染成功
    ok = (exact[0] + exact[1]) > 0
    print("结论:", "等高线已渲染 ✓" if ok else "未检测到等高线像素 ✗")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
