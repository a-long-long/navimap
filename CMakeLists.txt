cmake_minimum_required(VERSION 3.13)
project(navimap C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -g")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# LTO（链接期优化）：实测 8 省 1280×800 第二帧 z9 132→125、z11 62→60、
# z13 45→44 ms，四档缩放逐像素 0 差异（PLAN.md §30）。
#
# 为什么值：渲染热点里最贵的是 mv_project——它在 map_view.c，而调用点全在
# paint.c / labels.c。没有 LTO 时它跨编译单元无法内联，于是每次调用都要重跑
# 一遍"缓存键比对 + 5 个字段的加载"，微基准里这一项就占投影成本的 13%。
#
# ★ -O3 和 -march=native **都实测没有额外收益**（z9 仍是 125），只留 LTO。
#   代价是链接变慢，换来的是免费的 5%。
# ---------------------------------------------------------------------------
include(CheckIPOSupported)
check_ipo_supported(RESULT NM_IPO_OK OUTPUT NM_IPO_ERR)
if(NM_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    message(STATUS "LTO 已开启（-flto）")
else()
    message(STATUS "LTO 不可用，按普通 -O2 构建：${NM_IPO_ERR}")
endif()

find_package(PkgConfig REQUIRED)

# ---------------------------------------------------------------------------
# 依赖：全部 REQUIRED。
# 原因（血泪）：SDL2_ttf 装在 ~/.local，默认 pkg-config 看不见；上一版用了
# 可选查找，CMake 只打印一行 "SDL2_ttf not found; text rendering is disabled"
# 就静默继续，等到做路名渲染才发现没文字。这里改成缺了就直接失败，
# 并提示先 source env.sh。
# ---------------------------------------------------------------------------
macro(nm_require_pc var mod)
    pkg_check_modules(${var} REQUIRED ${mod})
    if(NOT ${var}_FOUND)
        message(FATAL_ERROR
            "${mod} 未找到。请先执行：source ${CMAKE_SOURCE_DIR}/env.sh 再重新 cmake。")
    endif()
endmacro()

nm_require_pc(SDL2       sdl2)
nm_require_pc(SDL2TTF    SDL2_ttf)
nm_require_pc(ZLIB       zlib)
nm_require_pc(SQLITE3    sqlite3)
nm_require_pc(PROTOC     libprotobuf-c)

# libprotobuf-c.pc 里写的是 prefix=/usr，但头文件只装到了 ~/.local/usr/include，
# 所以 pkg-config 给的 Cflags 是错的（指向不存在的 /usr/include/protobuf-c）。
# 这里自己找一次真实头文件位置，找不到就直接失败，别留到编译期才炸。
find_path(PROTOC_REAL_INCLUDE_DIR
    NAMES protobuf-c/protobuf-c.h
    HINTS $ENV{HOME}/.local/usr/include /usr/local/include /usr/include
    NO_DEFAULT_PATH)
if(NOT PROTOC_REAL_INCLUDE_DIR)
    message(FATAL_ERROR "找不到 protobuf-c/protobuf-c.h（pkg-config 的 Cflags 指向的目录不存在）")
endif()
message(STATUS "protobuf-c hdr ${PROTOC_REAL_INCLUDE_DIR}")

# 同理，链接也必须指到 ~/.local 那份 1.3.1。
# 系统 /usr/lib/aarch64-linux-gnu 里有一份 2019 年的 1.0.0（只有 .so.1，无 .so），
# 头文件是 1.3.1 而库是 1.0.0 的话能编过、一跑就废——所以这里用完整路径钉死。
find_library(PROTOC_LIB NAMES protobuf-c
    HINTS $ENV{HOME}/.local/usr/lib/aarch64-linux-gnu
    NO_DEFAULT_PATH)
if(NOT PROTOC_LIB)
    message(FATAL_ERROR "找不到 libprotobuf-c.so（应在 ~/.local/usr/lib/aarch64-linux-gnu）")
endif()
message(STATUS "protobuf-c lib ${PROTOC_LIB}")

find_library(SDL2TTF_LIB NAMES SDL2_ttf
    HINTS ${SDL2TTF_LIBRARY_DIRS} $ENV{HOME}/.local/lib /usr/local/lib
    NO_DEFAULT_PATH)
if(NOT SDL2TTF_LIB)
    message(FATAL_ERROR "找不到 libSDL2_ttf.so")
endif()
message(STATUS "SDL2_ttf lib ${SDL2TTF_LIB}")

find_program(PROTOC_C protoc-c)
if(NOT PROTOC_C)
    message(FATAL_ERROR "protoc-c 未找到（在 ~/.local/usr/bin）。请先 source ${CMAKE_SOURCE_DIR}/env.sh")
endif()

message(STATUS "sdl2          ${SDL2_VERSION}")
message(STATUS "SDL2_ttf      ${SDL2TTF_VERSION}")
message(STATUS "protobuf-c    ${PROTOC_VERSION}")
message(STATUS "protoc-c      ${PROTOC_C}")

# ---------------------------------------------------------------------------
# protobuf-c 代码生成
# ---------------------------------------------------------------------------
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})
set(PROTO_SRCS ${GEN_DIR}/osmformat.pb-c.c ${GEN_DIR}/fileformat.pb-c.c)
set(PROTO_HDRS ${GEN_DIR}/osmformat.pb-c.h ${GEN_DIR}/fileformat.pb-c.h)

add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND ${PROTOC_C} --c_out=${GEN_DIR} -I ${CMAKE_SOURCE_DIR}/proto
            ${CMAKE_SOURCE_DIR}/proto/osmformat.proto
            ${CMAKE_SOURCE_DIR}/proto/fileformat.proto
    DEPENDS ${CMAKE_SOURCE_DIR}/proto/osmformat.proto
            ${CMAKE_SOURCE_DIR}/proto/fileformat.proto
    COMMENT "Generating protobuf-c bindings for OSM PBF schema"
    VERBATIM)

# ---------------------------------------------------------------------------
# 核心库
# ---------------------------------------------------------------------------
set(CORE_SRCS
    src/utils/mem.c
    src/utils/geo.c
    src/utils/strpool.c
    src/pbf/pbf_stream.c
    src/pbf/pbf_block.c
    src/map/road_tags.c
    src/map/binfmt.c
    src/map/graph_build.c
    src/map/pbfmap.c
    src/map/mapset.c
    src/map/autobuild.c
    src/map/coverage.c
    src/map/map_view.c
    src/map/tileidx.c
    src/route/route.c
    src/route/lmtable.c
    src/route/revidx.c
    src/route/maneuver.c
    src/route/navsim.c
    src/search/search.c
    src/render/paint.c
    src/render/raster.c
    src/render/navdraw.c
    src/render/font.c
    src/render/labels.c
    src/ui/window.c
    ${PROTO_SRCS})

add_library(navimap_core STATIC ${CORE_SRCS})

target_include_directories(navimap_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${GEN_DIR}
    ${SDL2_INCLUDE_DIRS}
    ${SDL2TTF_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS}
    ${PROTOC_REAL_INCLUDE_DIR})

target_link_directories(navimap_core PUBLIC
    ${SDL2_LIBRARY_DIRS}
    ${SDL2TTF_LIBRARY_DIRS}
    ${SQLITE3_LIBRARY_DIRS})

target_link_libraries(navimap_core PUBLIC
    ${SDL2_LIBRARIES}
    ${SDL2TTF_LIB}
    ${ZLIB_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${PROTOC_LIB}
    m)

# ---------------------------------------------------------------------------
# 命令行工具（GUI 无法无头验证，逻辑一律做成可断言的 CLI）
# ---------------------------------------------------------------------------
add_executable(pbf_info tools/pbf_info.c)
target_link_libraries(pbf_info PRIVATE navimap_core)

# PBF 按需解码显示层的等价性校验（"只留 PBF"方案的门禁）
add_executable(pbfmap_check tools/pbfmap_check.c)
target_link_libraries(pbfmap_check PRIVATE navimap_core)

# via=way 转向限制形态探针（调研用，决定展开方案前先看数据长什么样）
add_executable(restr_probe tools/restr_probe.c)
target_link_libraries(restr_probe PRIVATE navimap_core)

add_executable(graph_extract tools/graph_extract.c)
target_link_libraries(graph_extract PRIVATE navimap_core)

add_executable(graph_check tools/graph_check.c)
target_link_libraries(graph_check PRIVATE navimap_core)

add_executable(tag_stats tools/tag_stats.c)
target_link_libraries(tag_stats PRIVATE navimap_core)

# 主程序（GUI，Wayland 下由人确认）
add_executable(navimap src/main.c)
target_link_libraries(navimap PRIVATE navimap_core)

# 离屏渲染：无人值守验证渲染结果（本机没有 xvfb-run，只能这么验）
add_executable(map_shot tools/map_shot.c)
target_link_libraries(map_shot PRIVATE navimap_core)

# 跨省接边验证：同一坐标在多个省里 OSM id 是否一致（阶段 8 坐标键路由的前提）
add_executable(tile_xcheck tools/tile_xcheck.c)
target_link_libraries(tile_xcheck PRIVATE navimap_core)

# 瓦片下标 查表版 vs 精确版 的逐点等价性验收（--expect-diff 是对照组）
add_executable(tile_selftest tools/tile_selftest.c)
target_link_libraries(tile_selftest PRIVATE navimap_core)

# 优先队列自验收（--expect-fail 是负向对照：坏实现必须被抓出）
add_executable(heap_selftest tools/heap_selftest.c)
target_link_libraries(heap_selftest PRIVATE navimap_core)

# 路线计算（跨省 A*，坐标键接边）
add_executable(route_cli tools/route_cli.c)
target_link_libraries(route_cli PRIVATE navimap_core)

# 地标距离：一对多 Dijkstra（PLAN.md §48，A* 地标下界的第一块砖）
add_executable(lmdist_cli tools/lmdist_cli.c)
target_link_libraries(lmdist_cli PRIVATE navimap_core)

# 地标下界表 lm.bin 的构建（网格聚合，PLAN.md §49.6）
add_executable(lmbuild_cli tools/lmbuild_cli.c)
target_link_libraries(lmbuild_cli PRIVATE navimap_core)

# 反向入边索引 rev.bin 的生成与逐元素自验收（--verify）
add_executable(revbuild_cli tools/revbuild_cli.c)
target_link_libraries(revbuild_cli PRIVATE navimap_core)

# 模拟定位 / 偏航 / 剩余 ETA 的无头验收（阶段 9 导航层）
add_executable(navsim_cli tools/navsim_cli.c)
target_link_libraries(navsim_cli PRIVATE navimap_core)

# via=way 两段转向限制的 A/B 对照验收（阶段 8 补）
add_executable(restr_ab tools/restr_ab.c)
target_link_libraries(restr_ab PRIVATE navimap_core)

# 地名 / POI 搜索的无头验收（阶段 7）
add_executable(search_cli tools/search_cli.c)
target_link_libraries(search_cli PRIVATE navimap_core)

# 地名 / POI 库生成器（2026-09-23 补）
# 以前只有一份旧工具留下的存量 places_auto.db：新下载的 PBF 永远补不上地名，
# 因为 place=* / amenity=* 都挂在 node 上，而 graph_build 只留 ways。
add_executable(places_build tools/places_build.c)
target_link_libraries(places_build PRIVATE navimap_core)

# 无地图区域检测（"这里有没有地图"+"去哪下载"），GUI 提示条的无头验收
add_executable(coverage_cli tools/coverage_cli.c)
target_link_libraries(coverage_cli PRIVATE navimap_core)
