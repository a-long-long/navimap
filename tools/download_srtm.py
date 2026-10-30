#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""download_srtm.py —— 下载 SRTM DEM 瓦片（.hgt）到本地目录，供 skedaddle_gen_contours 自动生成等高线。

只依赖 Python 标准库（urllib + gzip），**无需 GDAL / curl / 任何第三方包**。
脚本会在 skedaddle_gen_contours --download 时被自动调用；也可单独手动运行，预拉取某区域的 DEM。

SRTM 瓦片按 1°×1° 划分，文件名形如 N<lat>E<lon>.hgt（如 N37E114.hgt）：
  - 北纬 N、南纬 S；东经 E、西经 W
  - 纬度 2 位、经度 3 位（N37 / S05 / E114 / W074）

默认从 AWS Open Data 的 skadi 数据集下载 .hgt.gz 并就地解压：
    https://s3.amazonaws.com/elevation-tiles-prod/skadi/N37/N37E114.hgt.gz
（skadi 在美/日等覆盖 SRTM1 30m，其余地区 SRTM3 90m；两种尺寸 skedaddle_gen_contours 都能读。）

部分网络（如中国大陆）直连 S3 不稳，可用 --mirror 指向可达镜像/代理，例如：
    --mirror=https://elevation-tiles-prod.s3.amazonaws.com/skadi
或自建/第三方镜像（保持 <mirror>/N37/N37E114.hgt.gz 的目录结构即可）。

用法：
  download_srtm.py --bbox=lat0,lon0,lat1,lon1 --out=DIR [--mirror=URL] [--keep-gz]

退出码：正常完成（含部分失败）返回 0；参数错误返回 2。单瓦片失败不致命，会继续下一个。
"""
import argparse
import gzip
import os
import sys
import time
import urllib.request
from math import floor

# 默认镜像与同数据的备用主机（默认镜像不可达时自动尝试，显式 --mirror 则不回退）。
DEFAULT_MIRROR = "https://s3.amazonaws.com/elevation-tiles-prod/skadi"
ALT_MIRROR     = "https://elevation-tiles-prod.s3.amazonaws.com/skadi"

UA = "navimap-srtm/1.0 (+https://github.com/osm/planet)"
HTTP_TIMEOUT = 90  # 秒，单瓦片下载超时


def parse_bbox(s):
    parts = [x.strip() for x in s.split(",")]
    if len(parts) != 4:
        raise ValueError("bbox 须为 lat0,lon0,lat1,lon1")
    v = [float(x) for x in parts]
    if v[0] > v[2]:
        v[0], v[2] = v[2], v[0]
    if v[1] > v[3]:
        v[1], v[3] = v[3], v[1]
    return v[0], v[1], v[2], v[3]


def tile_name(tlat, tlon):
    ns = "N" if tlat >= 0 else "S"
    ew = "E" if tlon >= 0 else "W"
    return "%s%02d%s%03d" % (ns, abs(tlat), ew, abs(tlon))


def tiles_for_bbox(lat0, lon0, lat1, lon1):
    out = []
    for tlat in range(int(floor(lat0)), int(floor(lat1)) + 1):
        for tlon in range(int(floor(lon0)), int(floor(lon1)) + 1):
            out.append((tlat, tlon, tile_name(tlat, tlon)))
    return out


def try_download(url, dest):
    """下载 url 到 dest；成功返回 True，失败打印原因返回 False。"""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
            data = r.read()
        with open(dest, "wb") as f:
            f.write(data)
        return True
    except Exception as e:  # noqa: BLE001 - 任何网络/IO 错误都降级，不中断整体
        sys.stderr.write("    ✗ %s\n      %s\n" % (url, e))
        return False


def main(argv):
    ap = argparse.ArgumentParser(
        description="下载 SRTM DEM 瓦片（.hgt）到本地目录")
    ap.add_argument("--bbox", required=True,
                    help="lat0,lon0,lat1,lon1（度，自动归一化为西南-东北）")
    ap.add_argument("--out", required=True, help="瓦片落盘目录（.hgt 文件）")
    ap.add_argument("--mirror", default=DEFAULT_MIRROR,
                    help="镜像基址，须含 <mirror>/N37/N37E114.hgt.gz 结构")
    ap.add_argument("--keep-gz", action="store_true",
                    help="保留下载的 .hgt.gz，不删除")
    args = ap.parse_args(argv[1:])

    try:
        lat0, lon0, lat1, lon1 = parse_bbox(args.bbox)
    except ValueError as e:
        sys.stderr.write("bbox 解析失败：%s\n" % e)
        return 2

    os.makedirs(args.out, exist_ok=True)
    tiles = tiles_for_bbox(lat0, lon0, lat1, lon1)
    sys.stderr.write("bbox=(%.4f,%.4f)-(%.4f,%.4f) → %d 张 1° 瓦片\n"
                     % (lat0, lon0, lat1, lon1, len(tiles)))

    mirrors = [args.mirror]
    if args.mirror == DEFAULT_MIRROR and ALT_MIRROR != args.mirror:
        mirrors.append(ALT_MIRROR)

    n_ok = n_skip = n_fail = 0
    t0 = time.time()
    for tlat, tlon, name in tiles:
        hgt = os.path.join(args.out, name + ".hgt")
        if os.path.exists(hgt) and os.path.getsize(hgt) > 0:
            n_skip += 1
            continue
        gz = os.path.join(args.out, name + ".hgt.gz")
        ok = False
        for m in mirrors:
            base = m.rstrip("/")
            # 目录按纬度带分：N37E114 → N37/
            url = "%s/%s/%s.hgt.gz" % (base, name[:3], name)
            sys.stderr.write("  ↓ %s ... " % url)
            if try_download(url, gz):
                try:
                    with gzip.open(gz, "rb") as f_in, open(hgt, "wb") as f_out:
                        f_out.write(f_in.read())
                    if not args.keep_gz:
                        try:
                            os.remove(gz)
                        except OSError:
                            pass
                    ok = True
                    sys.stderr.write("ok\n")
                    break
                except Exception as e:  # noqa: BLE001
                    sys.stderr.write("解压失败: %s\n" % e)
                    try:
                        os.remove(hgt)
                    except OSError:
                        pass
        if ok:
            n_ok += 1
        else:
            n_fail += 1

    dt = time.time() - t0
    sys.stderr.write("DEM 下载完成：新增 %d，已存在跳过 %d，失败 %d（%.1fs）\n"
                     % (n_ok, n_skip, n_fail, dt))
    if n_fail:
        sys.stderr.write("提示：失败的瓦片多位于海洋/无数据区，或被网络拦截；"
                         "可换 --mirror 或手动放置 .hgt。\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
