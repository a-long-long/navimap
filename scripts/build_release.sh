#!/usr/bin/env bash
#
# Release 构建 + 发布包部署（Windows / MSYS2）。
#
# 产出：dist/navimap-release/  —— 一个自包含、可直接双击 start.bat 运行的目录：
#   navmap_qt.exe (Release)
#   Qt6 DLL + 平台插件 (qwindows / qoffscreen)
#   第三方 DLL (protobuf-c / sqlite3 / zlib / libgcc / libstdc++ / winpthread)
#   fonts/  (CJK 字体，exe 同级的 fonts/ 或 ../fonts/)
#   start.bat / README.txt
#
# 用法：
#   ./scripts/build_release.sh          # 干净重建 + 部署
#   ./scripts/build_release.sh --no-rebuild   # 只重新部署已有 build_rel/
#
# 依赖路径（本机）：
#   MSYS2 UCRT64 工具链 : /d/msys64/ucrt64/bin
#   Qt6 (mingw_64)      : /d/Qt/6.11.2/mingw_64
# 换机器请改下面的 MSYS_BIN / QT_BIN / QT_PREFIX。
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build_rel"
DIST="$ROOT/dist/navimap-release"

MSYS_BIN=/d/msys64/ucrt64/bin
QT_BIN=/d/Qt/6.11.2/mingw_64/bin
QT_PREFIX=D:/Qt/6.11.2/mingw_64
# ★ 干净 PATH：只含 ucrt64，绝不放 Qt bin（编译期 Qt DLL 会让 gcc 静默失败）。
CLEAN_PATH="$MSYS_BIN:/usr/bin:/c/Windows/system32:/c/Windows"

NO_REBUILD=0
for a in "$@"; do
    case "$a" in
        --no-rebuild) NO_REBUILD=1 ;;
        *) echo "未知参数: $a" >&2; exit 2 ;;
    esac
done

# ---- 1) 配置 + 编译 Release -------------------------------------------
if [ "$NO_REBUILD" = 0 ]; then
    echo "=== 配置 Release ($BUILD) ==="
    rm -rf "$BUILD"
    env PATH="$CLEAN_PATH" \
        "$MSYS_BIN/cmake.exe" -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT_PREFIX"
    echo "=== 编译 Release (-j4) ==="
    env PATH="$CLEAN_PATH" \
        "$MSYS_BIN/cmake.exe" --build "$BUILD" -j4
fi

# ---- 2) 部署发布包 -----------------------------------------------------
echo "=== 部署到 $DIST ==="
rm -rf "$DIST"
mkdir -p "$DIST"
cp "$BUILD/navmap_qt.exe" "$DIST/"
cp -r "$ROOT/fonts" "$DIST/fonts"

# ---- 3) Qt 依赖收集 ----------------------------------------------------
echo "=== windeployqt --release ==="
"$QT_BIN/windeployqt.exe" --release "$DIST/navmap_qt.exe"
# 附带上 qoffscreen 插件，便于无头出图（--shot-offscreen）自检。
if [ -f "$QT_BIN/../plugins/platforms/qoffscreen.dll" ]; then
    cp "$QT_BIN/../plugins/platforms/qoffscreen.dll" "$DIST/platforms/" 2>/dev/null || true
fi

# ---- 4) 非 Qt 第三方 DLL（windeployqt 不认识）-------------------------
echo "=== 补第三方 DLL ==="
for dll in libprotobuf-c-1.dll libsqlite3-0.dll zlib1.dll \
           libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll; do
    if [ -f "$MSYS_BIN/$dll" ]; then
        cp "$MSYS_BIN/$dll" "$DIST/"
        echo "  + $dll"
    else
        echo "  缺失: $dll (检查 $MSYS_BIN)"
    fi
done

# ---- 5) 启动脚本 + 说明 ------------------------------------------------
cat > "$DIST/start.bat" <<'EOF'
@echo off
cd /d "%~dp0"
navmap_qt.exe --graph=E:/navimap/products/graph/beijing
EOF

cat > "$DIST/README.txt" <<'EOF'
navmap_qt Release 版
====================

【运行】双击 start.bat，自动加载本机已有的北京地图
（E:/navimap/products/graph/beijing，约 370MB，由 sync_maps.sh 生成）。
或自己指定地图目录：  navmap_qt.exe --graph=<地图目录>

【操作】视图菜单勾选「等高线」或按 O 键开关等高线；
鼠标拖拽平移、滚轮缩放；右上角搜索框搜地点（需 places_auto.db 地名库）。

【自带依赖】Qt6 / 字体(fonts/) / protobuf-c / sqlite3 / zlib 均已随包，无需另装。

【已知限制】SRTM DEM 实时下载依赖网络；本机到 AWS S3 带宽极慢，
完整瓦片下载不现实，可用预置 srtm_cache/ 走「缺失才下载」去重。
EOF

echo
echo "=== 发布包已生成: $DIST ==="
ls "$DIST"
