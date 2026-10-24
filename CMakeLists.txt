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
find_package(Threads REQUIRED)

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
set_source_files_properties(${PROTO_SRCS} ${PROTO_HDRS} PROPERTIES GENERATED TRUE)

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
# 核心库 + 阶段 1 叶子库（ARCH_ASSESSMENT.md §4，字面补全拆分）
#   src/utils 内的 4 个模块拆成独立 STATIC 库：
#     navmap_plat / navmap_geo / navmap_strpool（纯叶子，零内部依赖）
#     navmap_mem（仅依赖 navmap_plat：mem → plat）
#   加 navmap_pbf（src/pbf/*）共 5 个叶子库，全部链进 navimap_core 的 PUBLIC
#   接口；24 个 exe 继续只链 navimap_core（本阶段不改它们的
#   target_link_libraries）。依赖：mem → plat；其余零内部依赖，拆库零破环风险。
# ---------------------------------------------------------------------------
set(PBF_SRCS
    src/pbf/pbf_stream.c
    src/pbf/pbf_block.c
    ${PROTO_SRCS}
    ${PROTO_HDRS})

# 阶段 2：把核心剩余模块提升为独立 STATIC 库（ARCH_ASSESSMENT.md §4 DAG）。
#   map → pbf, utils        route → map, utils
#   search → utils          render → map, route, search, utils（并反向引用 ui，见下）
#   ui → map, render, route, search, utils
# 注意：实测 render/chrome.c 调用 ui/window.c 的 app_nav_*（导航动作），ui/window.c
# 又调用 render 的 paint/navdraw/chrome —— render↔ui 是真实的符号级环（文档 DAG 未体现）。
# 用 LINKER:-start-group 包住全部模块库消解该环（仅 GNU/MinGW ld 需要；MSVC link 自带多轮）。
set(MAP_SRCS
    src/map/road_tags.c
    src/map/binfmt.c
    src/map/graph_build.c
    src/map/pbfmap.c
    src/map/mapset.c
    src/map/autobuild.c
    src/map/coverage.c
    src/map/map_view.c
    src/map/tileidx.c)
set(ROUTE_SRCS
    src/route/route.c
    src/route/lmtable.c
    src/route/revidx.c
    src/route/maneuver.c
    src/route/navsim.c)
set(SEARCH_SRCS
    src/search/search.c)
set(RENDER_SRCS
    src/render/paint.c
    src/render/raster.c
    src/render/navdraw.c
    src/render/chrome.c
    src/render/font.c
    src/render/labels.c)
set(UI_SRCS
    src/ui/window.c)

# 阶段 1 叶子库：把 src/utils 按模块拆成 4 个独立 STATIC 库（字面补全阶段1）。
# 依赖关系：mem → plat；geo / strpool / plat 为纯叶子（零内部依赖）。
# 每个叶子库都 PUBLIC 暴露 ${CMAKE_SOURCE_DIR}/src 作为包含根，使消费者可用
# "utils/xxx.h"；并各自携带平台系统库（m / psapi），保证可独立复用。

# 平台相关链接（每个叶子库各自挂，不再聚合到单个 navmap_utils）：
#   Unix 需要 -lm（geo 等数学函数）；Windows 的 GetProcessMemoryInfo 在 psapi.dll。
if(UNIX)
    set(NM_LEAF_SYS_LIBS m)
elseif(WIN32)
    set(NM_LEAF_SYS_LIBS psapi)
else()
    set(NM_LEAF_SYS_LIBS "")
endif()

# 叶子库 1：navmap_plat（src/utils/plat.c，纯叶子）
add_library(navmap_plat STATIC src/utils/plat.c)
target_include_directories(navmap_plat PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(navmap_plat PUBLIC ${NM_LEAF_SYS_LIBS})
if(WIN32)
    target_compile_options(navmap_plat PUBLIC -Wno-format)
endif()

# 叶子库 2：navmap_geo（src/utils/geo.c，纯叶子）
add_library(navmap_geo STATIC src/utils/geo.c)
target_include_directories(navmap_geo PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(navmap_geo PUBLIC ${NM_LEAF_SYS_LIBS})
if(WIN32)
    target_compile_options(navmap_geo PUBLIC -Wno-format)
endif()

# 叶子库 3：navmap_strpool（src/utils/strpool.c，纯叶子）
add_library(navmap_strpool STATIC src/utils/strpool.c)
target_include_directories(navmap_strpool PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(navmap_strpool PUBLIC ${NM_LEAF_SYS_LIBS})
if(WIN32)
    target_compile_options(navmap_strpool PUBLIC -Wno-format)
endif()

# 叶子库 4：navmap_mem（src/utils/mem.c，依赖 plat）
add_library(navmap_mem STATIC src/utils/mem.c)
target_include_directories(navmap_mem PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(navmap_mem PUBLIC navmap_plat ${NM_LEAF_SYS_LIBS})
if(WIN32)
    target_compile_options(navmap_mem PUBLIC -Wno-format)
endif()

# 叶子库 5：navmap_pbf（src/pbf/*，零内部依赖）
#   PBF 解码依赖 zlib（解压）与 protobuf-c（schema），生成头在 GEN_DIR。
add_library(navmap_pbf STATIC ${PBF_SRCS})
target_include_directories(navmap_pbf PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS}
    ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_pbf PUBLIC ${ZLIB_LIBRARIES} ${PROTOC_LIB})
if(WIN32)
    target_compile_options(navmap_pbf PUBLIC -Wno-format)
endif()

# 阶段 2 模块库（DAG 顺序，见上方 MODULE_SRCS 注释；聚合层 navimap_core 见下方）。
# 每个模块库 PUBLIC 暴露 ${CMAKE_SOURCE_DIR}/src 含入根（消费者可用 "map/xxx.h"），
# 并链入自身的模块依赖。第三方库（z/sqlite3/protoc/m）统一由下方 navimap_core 聚合层
# 承载，模块库本身只表达"模块↔模块"依赖，便于独立复用。

# navmap_map：pbf + 全部叶子库
add_library(navmap_map STATIC ${MAP_SRCS})
target_include_directories(navmap_map PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_map PUBLIC
    navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem)

# navmap_search：仅叶子库（search→utils）。search.c 真正调用 sqlite3_*，
# 故把 sqlite3 收进本模块库，保证 search 系列工具（search_cli/places_build）
# 只链 navmap_search 也能解析数据库符号（模块可独立复用，不必拖整库）。
add_library(navmap_search STATIC ${SEARCH_SRCS})
target_include_directories(navmap_search PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_search PUBLIC
    navmap_geo navmap_strpool navmap_plat navmap_mem
    ${SQLITE3_LIBRARIES})

# navmap_route：map + 叶子库
add_library(navmap_route STATIC ${ROUTE_SRCS})
target_include_directories(navmap_route PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_route PUBLIC
    navmap_map navmap_geo navmap_strpool navmap_plat navmap_mem)

# navmap_render：map + route + search + 叶子库
#   （render 反向引用 ui 的 app_nav_*，由下方 start-group 消解；此处不显式声明 ui 依赖，
#    以避免 CMake 报循环依赖警告，同时保留 DAG 可读性）
add_library(navmap_render STATIC ${RENDER_SRCS})
target_include_directories(navmap_render PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_render PUBLIC
    navmap_map navmap_route navmap_search
    navmap_geo navmap_strpool navmap_plat navmap_mem)

# navmap_ui：render + route + search + map + 叶子库
add_library(navmap_ui STATIC ${UI_SRCS})
target_include_directories(navmap_ui PUBLIC
    ${CMAKE_SOURCE_DIR}/src ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS} ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS} ${PROTOC_REAL_INCLUDE_DIR})
target_link_libraries(navmap_ui PUBLIC
    navmap_render navmap_route navmap_search navmap_map
    navmap_geo navmap_strpool navmap_plat navmap_mem)

# navimap_core：聚合层（方案2 架构边界的对外门面）。
#   ★ 仅含一个无副作用的锚点符号（src/core_anchor.c），不含业务 .c；
#     把 5 个模块库 + 5 个叶子库 + 第三方库聚合成一个链接面，
#     使 24 个 exe / navmap_sdl / navmap_qt 等无需改动即可继续链接 navimap_core。
#   ★ 核心零 SDL：聚合层连 SDL 的 include 都不给；SDL 只在 navmap_sdl / map_shot。
add_library(navimap_core STATIC src/core_anchor.c)

# ★ 核心不含 SDL：连 include 路径都不给。理由见上面"依赖"一节的架构约束。
#   谁需要 SDL（窗口版 / 离屏出图）自己去链 navimap_sdl 或直接链 SDL2。
target_include_directories(navimap_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${GEN_DIR}
    ${ZLIB_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${PROTOC_INCLUDE_DIRS}
    ${PROTOC_REAL_INCLUDE_DIR})


# 平台相关链接：libm 只有类 Unix 有；Windows 的 GetProcessMemoryInfo 在 psapi.dll。
# 注意：必须并入同一次 target_link_libraries 调用。本机 CMake 3.22.1 对"同一目标的
# 第二次 target_link_libraries（且包在 if() 内）"会误报 target not built by this project，
# 故用配置期变量在这里一次性决定平台库，再在下方单次调用里展开。
if(UNIX)
    set(NAVIMAP_CORE_SYS_LIBS m)
elseif(WIN32)
    set(NAVIMAP_CORE_SYS_LIBS psapi)
else()
    set(NAVIMAP_CORE_SYS_LIBS "")
endif()

# ★ render↔ui 是真实的符号级环（见 MODULE_SRCS 注释），必须用 --start-group 包住
#   全部项目库，让链接器多轮扫描消解环。MSVC 的 link.exe 自带多轮，无需该选项。
if(MSVC)
    target_link_libraries(navimap_core PUBLIC
        navmap_map navmap_route navmap_search navmap_render navmap_ui
        navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem
        ${ZLIB_LIBRARIES} ${SQLITE3_LIBRARIES} ${PROTOC_LIB} ${NAVIMAP_CORE_SYS_LIBS})
else()
    target_link_libraries(navimap_core PUBLIC
        -Wl,--start-group
        navmap_map navmap_route navmap_search navmap_render navmap_ui
        navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem
        ${ZLIB_LIBRARIES} ${SQLITE3_LIBRARIES} ${PROTOC_LIB} ${NAVIMAP_CORE_SYS_LIBS}
        -Wl,--end-group)
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

# 阶段 2：同样的 Windows 编译选项/宏也要套到五个模块库上（navimap_core 已不含业务 .c，
# 真正的编译单元在模块库里）。叶子库（navmap_pbf/geo/...）已在各自定义处挂过 -Wno-format。
set(NM_ALL_MOD_LIBS
    navmap_map navmap_route navmap_search navmap_render navmap_ui navmap_core)
if(MSVC)
    foreach(t ${NM_ALL_MOD_LIBS})
        target_compile_definitions(${t} PUBLIC
            _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
    endforeach()
endif()
if(WIN32)
    foreach(t ${NM_ALL_MOD_LIBS})
        target_compile_definitions(${t} PUBLIC SDL_MAIN_HANDLED)
        target_compile_options(${t} PUBLIC -Wno-format)
    endforeach()
endif()

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
# 阶段 2（续）：24 个 exe 改为只链所需模块库。
#   每个工具只链它真正用到的模块库（map/route/search/render/ui 的子集），
#   叶子库（pbf/geo/strpool/plat/mem）与第三方库（z/sqlite3/protoc/m）统一在
#   nm_link_tool 内补齐。因为 render↔ui 是真实符号级环，所有项目库必须包在
#   -Wl,--start-group … -Wl,--end-group 里让链接器多轮扫描消解（MSVC link 自带
#   多轮，走 else 分支纯列清单）。
#   这样每个 exe 的链接面被裁剪到最小，且依赖方向被链接器强制（偷偷反向依赖会
#   立刻 undefined-reference），符合 ARCH_ASSESSMENT.md §4 阶段2 的验收目标。
# ---------------------------------------------------------------------------
function(nm_link_tool tgt)
    if(MSVC)
        target_link_libraries(${tgt} PRIVATE
            ${ARGN}
            navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem
            ${ZLIB_LIBRARIES} ${SQLITE3_LIBRARIES} ${PROTOC_LIB} ${NM_LEAF_SYS_LIBS})
    else()
        target_link_libraries(${tgt} PRIVATE
            -Wl,--start-group
            ${ARGN}
            navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem
            ${ZLIB_LIBRARIES} ${SQLITE3_LIBRARIES} ${PROTOC_LIB} ${NM_LEAF_SYS_LIBS}
            -Wl,--end-group)
    endif()
endfunction()

# ---------------------------------------------------------------------------
# 命令行工具（GUI 无法无头验证，逻辑一律做成可断言的 CLI）
# ---------------------------------------------------------------------------
add_executable(pbf_info tools/pbf_info.c)
nm_link_tool(pbf_info)


# PBF 按需解码显示层的等价性校验（"只留 PBF"方案的门禁）
add_executable(pbfmap_check tools/pbfmap_check.c)
nm_link_tool(pbfmap_check navmap_map navmap_route navmap_search navmap_render navmap_ui)

# via=way 转向限制形态探针（调研用）
add_executable(restr_probe tools/restr_probe.c)
nm_link_tool(restr_probe)

add_executable(graph_extract tools/graph_extract.c)
nm_link_tool(graph_extract navmap_map navmap_route navmap_search navmap_render navmap_ui)

add_executable(graph_check tools/graph_check.c)
nm_link_tool(graph_check navmap_map navmap_route navmap_search navmap_render navmap_ui)

add_executable(tag_stats tools/tag_stats.c)
nm_link_tool(tag_stats)

# ---------------------------------------------------------------------------
# SDL 适配层：唯一直接依赖 SDL 的库。只有 SDL 窗口版（navmap）链它。
# ---------------------------------------------------------------------------
add_library(navmap_sdl STATIC src/ui/window_sdl.c)
target_include_directories(navmap_sdl PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${SDL2_INCLUDE_DIRS})
target_link_directories(navmap_sdl PUBLIC ${SDL2_LIBRARY_DIRS})
target_link_libraries(navmap_sdl PUBLIC
    -Wl,--start-group
    navmap_map navmap_route navmap_search navmap_render navmap_ui
    navmap_pbf navmap_geo navmap_strpool navmap_plat navmap_mem
    ${ZLIB_LIBRARIES} ${SQLITE3_LIBRARIES} ${PROTOC_LIB} ${NAVIMAP_CORE_SYS_LIBS}
    -Wl,--end-group
    ${SDL2_LIBRARIES} Threads::Threads)

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
    target_link_libraries(navmap_qt PRIVATE navimap_core ${QT}::Widgets Threads::Threads)
    message(STATUS "Qt 前端: 启用 (${QT} ${${QT}Widgets_VERSION})")
else()
    message(STATUS "Qt 前端: 未找到 Qt6/Qt5 Widgets，跳过 navmap_qt")
endif()

# 离屏渲染：自己用 SDL 存 BMP（SDL_SaveBMP），直接链 SDL2，但不链 navimap_sdl。
add_executable(map_shot tools/map_shot.c)
target_include_directories(map_shot PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(map_shot PRIVATE ${SDL2_LIBRARY_DIRS})
nm_link_tool(map_shot navmap_map navmap_route navmap_search navmap_render navmap_ui)
target_link_libraries(map_shot PRIVATE ${SDL2_LIBRARIES} Threads::Threads)

# 光栅 alpha 混色与 SDL 软渲染器的逐像素等价性（去 SDL 化的门禁）。
add_executable(blend_equiv tools/blend_equiv.c)
target_include_directories(blend_equiv PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(blend_equiv PRIVATE ${SDL2_LIBRARY_DIRS})
nm_link_tool(blend_equiv navmap_map navmap_route navmap_search navmap_render navmap_ui)
target_link_libraries(blend_equiv PRIVATE ${SDL2_LIBRARIES} Threads::Threads)

# 线段裁剪与 SDL_IntersectRectAndLine 的等价性（去 SDL 化的门禁）。
add_executable(clip_equiv tools/clip_equiv.c)
target_include_directories(clip_equiv PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(clip_equiv PRIVATE ${SDL2_LIBRARY_DIRS})
nm_link_tool(clip_equiv navmap_map navmap_route navmap_search navmap_render navmap_ui)
target_link_libraries(clip_equiv PRIVATE ${SDL2_LIBRARIES} Threads::Threads)

# 架构门禁：核心与 Qt 前端零 SDL 符号（正反双向）。
add_executable(core_no_sdl tools/core_no_sdl.c)
nm_link_tool(core_no_sdl)

# 跨省接边验证（阶段 8 坐标键路由的前提）
add_executable(tile_xcheck tools/tile_xcheck.c)
nm_link_tool(tile_xcheck navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 瓦片下标 查表版 vs 精确版 的逐点等价性验收
add_executable(tile_selftest tools/tile_selftest.c)
nm_link_tool(tile_selftest navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 优先队列自验收（--expect-fail 是负向对照）
add_executable(heap_selftest tools/heap_selftest.c)
nm_link_tool(heap_selftest navmap_route)

# 路线计算（跨省 A*，坐标键接边）
add_executable(route_cli tools/route_cli.c)
nm_link_tool(route_cli navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 地标距离：一对多 Dijkstra（PLAN.md §48）
add_executable(lmdist_cli tools/lmdist_cli.c)
nm_link_tool(lmdist_cli navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 地标下界表 lm.bin 的构建（网格聚合，PLAN.md §49.6）
add_executable(lmbuild_cli tools/lmbuild_cli.c)
nm_link_tool(lmbuild_cli navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 反向入边索引 rev.bin 的生成与逐元素自验收（--verify）
add_executable(revbuild_cli tools/revbuild_cli.c)
nm_link_tool(revbuild_cli navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 模拟定位 / 偏航 / 剩余 ETA 的无头验收（阶段 9 导航层）
add_executable(navsim_cli tools/navsim_cli.c)
nm_link_tool(navsim_cli navmap_map navmap_route navmap_search navmap_render navmap_ui)

# via=way 两段转向限制的 A/B 对照验收（阶段 8 补）
add_executable(restr_ab tools/restr_ab.c)
nm_link_tool(restr_ab navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 地名 / POI 搜索的无头验收（阶段 7）
add_executable(search_cli tools/search_cli.c)
nm_link_tool(search_cli navmap_search)

# 地名 / POI 库生成器（2026-09-23 补）
add_executable(places_build tools/places_build.c)
nm_link_tool(places_build navmap_search)

# 无地图区域检测（GUI 提示条的无头验收）
add_executable(coverage_cli tools/coverage_cli.c)
nm_link_tool(coverage_cli navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 平台抽象层（utils/plat.h）的全接口自验收（正反双向）。
add_executable(plat_selftest tools/plat_selftest.c)
nm_link_tool(plat_selftest)

# 字体契约验收：内置 Noto 字体集是否真能加载并出字形。
add_executable(font_selftest tools/font_selftest.c)
nm_link_tool(font_selftest navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 字体诊断探针：中文变方框（tofu）时定位是"没有字形"还是"有字形但画不出"。
add_executable(font_probe tools/font_probe.c)
nm_link_tool(font_probe navmap_map navmap_route navmap_search navmap_render navmap_ui)

# 字体链的中文覆盖断言：地图文字（stb_truetype）与 Qt 控件文字共用这条候选链。
add_executable(font_cjk_check tools/font_cjk_check.c)
nm_link_tool(font_cjk_check navmap_map navmap_route navmap_search navmap_render navmap_ui)

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
