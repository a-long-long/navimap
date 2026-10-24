cmake_minimum_required(VERSION 3.13)
project(navimap C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -g")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -g")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# ---------------------------------------------------------------------------
# LTO（链接期优化）：实测 8 省 1280x800 第二帧 z9 132→125、z11 62→60、
# z13 45→44 ms，四档缩放逐像素 0 差异（PLAN.md §30）。
# ★ -O3 和 -march=native 都实测没有额外收益（z9 仍是 125），只留 LTO。
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
# 依赖（方案2 架构约束：navimap_core 不链 SDL）。
# SDL 只出现在两个适配层：navmap_sdl（window_sdl.c）与 map_shot（自己 include
# SDL.h）。navimap_qt 只链 navimap_core + Qt Widgets。字体已换成 stb_truetype
# （单头文件、零外部库），于是"核心零 SDL"完全可行。SDL2_ttf 已彻底移除。
# ---------------------------------------------------------------------------
macro(nm_require_pc var mod)
    pkg_check_modules(${var} REQUIRED ${mod})
    if(NOT ${var}_FOUND)
        message(FATAL_ERROR
            "${mod} 未找到。请先执行：source ${CMAKE_SOURCE_DIR}/env.sh 再重新 cmake。")
    endif()
endmacro()

nm_require_pc(SDL2       sdl2)
nm_require_pc(ZLIB       zlib)
nm_require_pc(SQLITE3    sqlite3)
nm_require_pc(PROTOC     libprotobuf-c)

# libprotobuf-c.pc 里写的是 prefix=/usr，但头文件只装到了 ~/.local/usr/include，
# 所以 pkg-config 给的 Cflags 是错的。这里自己找一次真实头文件位置：HINTS 优先、
# 默认路径兜底（曾经写成 NO_DEFAULT_PATH 只认 ~/.local，换机器直接 FATAL）。
find_path(PROTOC_REAL_INCLUDE_DIR
    NAMES protobuf-c/protobuf-c.h
    HINTS $ENV{HOME}/.local/usr/include ${PROTOC_INCLUDE_DIRS} /usr/local/include)
if(NOT PROTOC_REAL_INCLUDE_DIR)
    message(FATAL_ERROR "找不到 protobuf-c/protobuf-c.h（pkg-config 的 Cflags 指向的目录不存在）")
endif()
message(STATUS "protobuf-c hdr ${PROTOC_REAL_INCLUDE_DIR}")

# 链接也必须指到 ~/.local 那份 1.3.1（系统 /usr/lib/<arch> 里有一份 2019 年的
# 1.0.0，只有 .so.1 无 .so；头 1.3.1 库 1.0.0 能编过、一跑就废）。HINTS 把
# ~/.local 排前面（顺序即优先级），既钉死本机版本，又不阻断标准安装位置。
# 多架构子目录用 CMAKE_LIBRARY_ARCHITECTURE（CMake 按工具链自动推导）而非写死。
find_library(PROTOC_LIB NAMES protobuf-c libprotobuf-c
    HINTS $ENV{HOME}/.local/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} $ENV{HOME}/.local/usr/lib
          ${PROTOC_LIBRARY_DIRS} /usr/local/lib)
if(NOT PROTOC_LIB)
    message(FATAL_ERROR "找不到 libprotobuf-c.so（本机在 ~/.local/usr/lib/<多架构三元组>，先 source env.sh）")
endif()
message(STATUS "protobuf-c lib ${PROTOC_LIB}")

# protobuf-c 代码生成：Linux 用 protoc-c；Windows/MSYS2 的 protoc-c 实为
# protoc-gen-c 插件，必须走 `protoc --plugin=protoc-gen-c=...`。
if(WIN32)
    find_program(PROTOC        protoc REQUIRED)
    find_program(PROTOC_GEN_C NAMES protoc-gen-c protoc-gen-c.exe REQUIRED)
    message(STATUS "protoc         ${PROTOC}")
    message(STATUS "protoc-gen-c   ${PROTOC_GEN_C}")
else()
    find_program(PROTOC_C NAMES protoc-c REQUIRED)
    if(NOT PROTOC_C)
        message(FATAL_ERROR "protoc-c 未找到（本机在 ~/.local/usr/bin）。请先 source ${CMAKE_SOURCE_DIR}/env.sh")
    endif()
    message(STATUS "protoc-c       ${PROTOC_C}")
endif()

message(STATUS "sdl2          ${SDL2_VERSION}")
message(STATUS "protobuf-c    ${PROTOC_VERSION}")
message(STATUS "protoc-c      ${PROTOC_C}")

# ---------------------------------------------------------------------------
# protobuf-c 代码生成
# ---------------------------------------------------------------------------
set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})
set(PROTO_SRCS ${GEN_DIR}/osmformat.pb-c.c ${GEN_DIR}/fileformat.pb-c.c)
set(PROTO_HDRS ${GEN_DIR}/osmformat.pb-c.h ${GEN_DIR}/fileformat.pb-c.h)
# 生成源由 add_custom_command 产出，必须显式标记 GENERATED，否则 add_library
# 在配置期因文件尚不存在而拒绝创建目标。
set_source_files_properties(${PROTO_SRCS} PROPERTIES GENERATED TRUE)

if(WIN32)
    add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
        COMMAND ${PROTOC} --plugin=protoc-gen-c=${PROTOC_GEN_C} --c_out=${GEN_DIR}
                -I ${CMAKE_SOURCE_DIR}/proto
                ${CMAKE_SOURCE_DIR}/proto/osmformat.proto
                ${CMAKE_SOURCE_DIR}/proto/fileformat.proto
        DEPENDS ${CMAKE_SOURCE_DIR}/proto/osmformat.proto
                ${CMAKE_SOURCE_DIR}/proto/fileformat.proto
        COMMENT "Generating protobuf-c bindings (Windows: protoc + plugin)"
        VERBATIM)
else()
    add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
        COMMAND ${PROTOC_C} --c_out=${GEN_DIR} -I ${CMAKE_SOURCE_DIR}/proto
                ${CMAKE_SOURCE_DIR}/proto/osmformat.proto
                ${CMAKE_SOURCE_DIR}/proto/fileformat.proto
        DEPENDS ${CMAKE_SOURCE_DIR}/proto/osmformat.proto
                ${CMAKE_SOURCE_DIR}/proto/fileformat.proto
        COMMENT "Generating protobuf-c bindings for OSM PBF schema"
        VERBATIM)
endif()

# ---------------------------------------------------------------------------
# 核心库 + 阶段 1 叶子库（ARCH_ASSESSMENT.md §4）
#   navmap_utils（src/utils/*）与 navmap_pbf（src/pbf/*）都是 DAG 上的叶子
#   （零内部依赖），拆库零破环风险。两者都链进 navimap_core 的 PUBLIC 接口，
#   24 个 exe 继续只链 navimap_core（本阶段不改它们的 target_link_libraries）。
# ---------------------------------------------------------------------------
set(UTILS_SRCS
    src/utils/plat.c
    src/utils/mem.c
    src/utils/geo.c
    src/utils/strpool.c)
set(PBF_SRCS
    src/pbf/pbf_stream.c
    src/pbf/pbf_block.c)

set(CORE_SRCS
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
    src/render/chrome.c
    src/render/font.c
    src/render/labels.c
    src/ui/window.c
    ${PROTO_SRCS})

# 叶子库 1：navmap_utils（src/utils/*，零内部依赖）
add_library(navmap_utils STATIC ${UTILS_SRCS})
target_include_directories(navmap_utils PUBLIC ${CMAKE_SOURCE_DIR}/src)
if(UNIX)
    target_link_libraries(navmap_utils PUBLIC m)
endif()
if(WIN32)
    target_link_libraries(navmap_utils PUBLIC psapi)
    # MinGW 的 %z/%ll 格式告警（同核心）
    target_compile_options(navmap_utils PUBLIC -Wno-format)
endif()

# 叶子库 2：navmap_pbf（src/pbf/*，零内部依赖）
#   PBF 解码依赖 zlib（解压）与 protobuf-c（schema），生成头在 GEN_DIR。
add_library(navmap_pbf STATIC ${PBF_SRCS})
target_include_directories(navmap_pbf PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS}
    ${PROTOC_REAL_INCLUDE_DIR})
target_link_directories(navmap_pbf PUBLIC ${PROTOC_LIBRARY_DIRS})
target_link_libraries(navmap_pbf PUBLIC ${ZLIB_LIBRARIES} ${PROTOC_LIB})
if(WIN32)
    target_compile_options(navmap_pbf PUBLIC -Wno-format)
endif()

# 核心库：去掉 utils/pbf 后剩余的模块。
add_library(navmap_core STATIC ${CORE_SRCS})

# ★ 核心不含 SDL：连 include 路径都不给。理由见上面"依赖"一节的架构约束。
#   谁需要 SDL（窗口版 / 离屏出图）自己去链 navimap_sdl 或直接链 SDL2。
target_include_directories(navmap_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS}
    ${PROTOC_REAL_INCLUDE_DIR})

target_link_directories(navmap_core PUBLIC
    ${SQLITE3_LIBRARY_DIRS}
    ${PROTOC_LIBRARY_DIRS})

target_link_libraries(navmap_core PUBLIC
    navmap_utils
    navmap_pbf
    ${ZLIB_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${PROTOC_LIB})

# 平台相关链接：libm 只有类 Unix 有；Windows 的 GetProcessMemoryInfo 在 psapi.dll。
if(UNIX)
    target_link_libraries(navimap_core PUBLIC m)
endif()
if(WIN32)
    target_link_libraries(navimap_core PUBLIC psapi)
endif()

# Windows（MSVC）需要这两个宏：_CRT_SECURE_NO_WARNINGS 关掉 POSIX 名字的"不安全"
# 警告（fopen/snprintf 我们是故意用的），_USE_MATH_DEFINES 放 M_PI。
# SDL_MAIN_HANDLED：SDL.h 默认会把 main 改写成 SDL_main，要求链 SDL2main；本程序
# 入口就是自己的 main()，不需要那层包装。
if(MSVC)
    target_compile_definitions(navimap_core PUBLIC
        _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
endif()
if(WIN32)
    target_compile_definitions(navimap_core PUBLIC SDL_MAIN_HANDLED)
    # MinGW-w64 的 printf 格式检查把 %z / %ll 当成非法（ms_printf 原型），但这些
    # 是合法 C99，在 glibc/Linux 下零警告。此处仅抑制该工具链误报，不改变语义。
    target_compile_options(navimap_core PUBLIC -Wno-format)

    # ★★ Windows(MSYS2) 必须走编译启动器，否则 gcc 会静默失败。
    # 现象：ninja 在 Windows 上通过 cmd.exe 执行编译命令，cmd.exe 里 gcc/g++ 退出码
    # 1 且 stderr 一个字都没有——看起来像"代码编译不过"，其实代码完全没问题。
    # 原因：cmd.exe 里没有 MSYS 运行时，MSYS 的路径转换层不生效，gcc 派生 cc1plus
    # 时传下去的 PATH 是 Windows 形式，子进程拿不到正确环境 ⇒ 启动即死、且不报错。
    # 解决：scripts/build_launcher.bat 把每次编译转到 MSYS2 的 sh 里跑，并由
    # scripts/cc_retry.sh 处理 Defender 实时防护可能误杀 cc1plus（stderr 为空 ⇒ 重试）。
    # 详见 scripts/build_win.sh 顶部的完整证据链。
    #
    # ★ 为什么用 target 属性而不是 CMAKE_C_COMPILER_LAUNCHER：
    #   后者是初始化变量，只在首次 configure 时生效 ⇒ 已有 build/ 的机器改了也不会
    #   生效（实测：设了但 ninja 生成的命令里根本没有它）。target 属性在生成阶段
    #   生效，对已有 cache 同样管用。
    set(NM_COMPILER_LAUNCHER "${CMAKE_SOURCE_DIR}/scripts/build_launcher.bat")
    # 若编译器本身已是包装脚本（cc_wrap/cxx_wrap），再套 build_launcher 会把 .bat
    # 嵌进 sh 里执行而失败（双包装）。此时启动器冗余，直接关掉。
    if(CMAKE_C_COMPILER MATCHES "cc_wrap" OR CMAKE_CXX_COMPILER MATCHES "cxx_wrap")
        set(NM_COMPILER_LAUNCHER "")
    endif()
    if(EXISTS "${NM_COMPILER_LAUNCHER}")
        message(STATUS "编译启动器  ${NM_COMPILER_LAUNCHER}（绕开 cmd.exe 下的 gcc 静默失败）")
    else()
        message(WARNING "找不到编译启动器 ${NM_COMPILER_LAUNCHER}；"
                        "在 Windows 上 gcc 可能静默失败（退出码 1、无输出）")
    endif()
endif()

# ---------------------------------------------------------------------------
# 命令行工具（GUI 无法无头验证，逻辑一律做成可断言的 CLI）
# ---------------------------------------------------------------------------
add_executable(pbf_info tools/pbf_info.c)
target_link_libraries(pbf_info PRIVATE navimap_core)

# PBF 按需解码显示层的等价性校验（"只留 PBF"方案的门禁）
add_executable(pbfmap_check tools/pbfmap_check.c)
target_link_libraries(pbfmap_check PRIVATE navimap_core)

# via=way 转向限制形态探针（调研用）
add_executable(restr_probe tools/restr_probe.c)
target_link_libraries(restr_probe PRIVATE navimap_core)

add_executable(graph_extract tools/graph_extract.c)
target_link_libraries(graph_extract PRIVATE navimap_core)

add_executable(graph_check tools/graph_check.c)
target_link_libraries(graph_check PRIVATE navimap_core)

add_executable(tag_stats tools/tag_stats.c)
target_link_libraries(tag_stats PRIVATE navimap_core)

# ---------------------------------------------------------------------------
# SDL 适配层：唯一直接依赖 SDL 的库。只有 SDL 窗口版（navmap）链它。
# ---------------------------------------------------------------------------
add_library(navmap_sdl STATIC src/ui/window_sdl.c)
target_include_directories(navmap_sdl PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${SDL2_INCLUDE_DIRS})
target_link_directories(navmap_sdl PUBLIC ${SDL2_LIBRARY_DIRS})
target_link_libraries(navmap_sdl PUBLIC navimap_core ${SDL2_LIBRARIES})

# 主程序（GUI）
add_executable(navmap src/main.c)
target_link_libraries(navmap PRIVATE navmap_sdl)

# Qt 前端（可选）：找到 Qt6 或 Qt5 的 Widgets 就多编一个 navmap_qt。
#   同一份源码同时兼容 Qt5/Qt6：无 Q_OBJECT/信号槽，故无需 moc；事件坐标统一用
#   position()/angleDelta()。配置时给 -DCMAKE_PREFIX_PATH=<Qt前缀> 指定版本，
#   找不到则跳过本目标，不影响其余全部产物。
set(QT "")
find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6Widgets_FOUND)
    set(QT Qt6)
else()
    find_package(Qt5 COMPONENTS Widgets QUIET)
    if(Qt5Widgets_FOUND)
        set(QT Qt5)
    endif()
endif()
if(QT)
    add_executable(navmap_qt
        src/ui_qt/qt_main.cpp
        src/ui_qt/map_canvas.cpp
        src/ui_qt/qt_font.cpp
        src/ui_qt/qt_icons.cpp
        src/ui_qt/main_window.cpp)
    target_link_libraries(navmap_qt PRIVATE navimap_core ${QT}::Widgets)
    message(STATUS "Qt 前端: 启用 (${QT} ${${QT}Widgets_VERSION})")
else()
    message(STATUS "Qt 前端: 未找到 Qt6/Qt5 Widgets，跳过 navmap_qt")
endif()

# 离屏渲染：自己用 SDL 存 BMP（SDL_SaveBMP），直接链 SDL2，但不链 navimap_sdl。
add_executable(map_shot tools/map_shot.c)
target_include_directories(map_shot PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(map_shot PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(map_shot PRIVATE navimap_core ${SDL2_LIBRARIES})

# 光栅 alpha 混色与 SDL 软渲染器的逐像素等价性（去 SDL 化的门禁）。
add_executable(blend_equiv tools/blend_equiv.c)
target_include_directories(blend_equiv PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(blend_equiv PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(blend_equiv PRIVATE navimap_core ${SDL2_LIBRARIES})

# 线段裁剪与 SDL_IntersectRectAndLine 的等价性（去 SDL 化的门禁）。
add_executable(clip_equiv tools/clip_equiv.c)
target_include_directories(clip_equiv PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(clip_equiv PRIVATE ${SDL2_LIBRARY_DIRS})
target_link_libraries(clip_equiv PRIVATE navimap_core ${SDL2_LIBRARIES})

# 架构门禁：核心与 Qt 前端零 SDL 符号（正反双向）。
add_executable(core_no_sdl tools/core_no_sdl.c)
target_link_libraries(core_no_sdl PRIVATE navimap_core)

# 跨省接边验证（阶段 8 坐标键路由的前提）
add_executable(tile_xcheck tools/tile_xcheck.c)
target_link_libraries(tile_xcheck PRIVATE navimap_core)

# 瓦片下标 查表版 vs 精确版 的逐点等价性验收
add_executable(tile_selftest tools/tile_selftest.c)
target_link_libraries(tile_selftest PRIVATE navimap_core)

# 优先队列自验收（--expect-fail 是负向对照）
add_executable(heap_selftest tools/heap_selftest.c)
target_link_libraries(heap_selftest PRIVATE navimap_core)

# 路线计算（跨省 A*，坐标键接边）
add_executable(route_cli tools/route_cli.c)
target_link_libraries(route_cli PRIVATE navimap_core)

# 地标距离：一对多 Dijkstra（PLAN.md §48）
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
add_executable(places_build tools/places_build.c)
target_link_libraries(places_build PRIVATE navimap_core)

# 无地图区域检测（GUI 提示条的无头验收）
add_executable(coverage_cli tools/coverage_cli.c)
target_link_libraries(coverage_cli PRIVATE navimap_core)

# 平台抽象层（utils/plat.h）的全接口自验收（正反双向）。
add_executable(plat_selftest tools/plat_selftest.c)
target_link_libraries(plat_selftest PRIVATE navimap_core)

# 字体契约验收：内置 Noto 字体集是否真能加载并出字形。
add_executable(font_selftest tools/font_selftest.c)
target_link_libraries(font_selftest PRIVATE navimap_core)

# 字体诊断探针：中文变方框（tofu）时定位是"没有字形"还是"有字形但画不出"。
add_executable(font_probe tools/font_probe.c)
target_link_libraries(font_probe PRIVATE navimap_core)

# 字体链的中文覆盖断言：地图文字（stb_truetype）与 Qt 控件文字共用这条候选链。
add_executable(font_cjk_check tools/font_cjk_check.c)
target_link_libraries(font_cjk_check PRIVATE navimap_core)

# ---------------------------------------------------------------------------
# Windows(MSYS2)：给所有目标挂编译启动器。
# 必须在所有 target 都定义完之后做（target 属性只能在 target 存在时设置）。
# 用 get_property 拿全部 target 名，逐个设 C/CXX_COMPILER_LAUNCHER。
# ---------------------------------------------------------------------------
if(WIN32 AND NM_COMPILER_LAUNCHER AND EXISTS "${NM_COMPILER_LAUNCHER}")
    get_property(NM_ALL_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t ${NM_ALL_TARGETS})
        set_target_properties(${t} PROPERTIES
            C_COMPILER_LAUNCHER   "${NM_COMPILER_LAUNCHER}"
            CXX_COMPILER_LAUNCHER "${NM_COMPILER_LAUNCHER}")
    endforeach()
    list(LENGTH NM_ALL_TARGETS NM_NT)
    message(STATUS "编译启动器已挂到全部 ${NM_NT} 个目标")
endif()
