#!/bin/bash
# NavMap 构建/运行环境。构建前先 source 本文件：
#     source /home/along/navimap/env.sh
#
# 为什么需要它：SDL2_ttf / protobuf-c / protoc-c 是手工装进 ~/.local 的，
# 默认 pkg-config 与 PATH 都看不见它们，CMake 会"静默降级"（只打印 not found
# 就把文字渲染关掉），等你做到路名那一步才发现没有文字。

export PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig:$HOME/.local/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/local/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
# libprotoc.so.17 / libprotobuf-c.so 也在 ~/.local/usr/lib/aarch64-linux-gnu
export LD_LIBRARY_PATH="$HOME/.local/usr/lib/aarch64-linux-gnu:$HOME/.local/lib:/usr/local/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$HOME/.local/usr/bin:$PATH"

# 排错用：三条都必须有输出
if [ "${NAVMAP_ENV_QUIET:-0}" != "1" ]; then
    printf 'sdl2        %s\n' "$(pkg-config --modversion sdl2 2>&1)"
    printf 'SDL2_ttf    %s\n' "$(pkg-config --modversion SDL2_ttf 2>&1)"
    printf 'libprotobuf-c %s\n' "$(pkg-config --modversion libprotobuf-c 2>&1)"
    printf 'protoc-c    %s\n' "$(command -v protoc-c || echo 'NOT FOUND')"
fi
