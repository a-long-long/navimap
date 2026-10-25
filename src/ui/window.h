#ifndef NAVIMAP_WINDOW_H
#define NAVIMAP_WINDOW_H

#include <stdint.h>
#include "utils/plat.h"
#include "map/map_view.h"
#include "map/binfmt.h"
#include "map/mapset.h"
#include "map/autobuild.h"
#include "map/coverage.h"
#include "render/paint.h"
#include "render/font.h"
#include "render/labels.h"
#include "route/route.h"
#include "route/maneuver.h"
#include "route/navsim.h"
#include "search/search.h"

/* 设置持久化（完整设置系统）：前向声明，避免 window.h 直接依赖 settings 头。 */
typedef struct Settings Settings;

/* 选点：用户点下去的位置 + 吸附到路网之后的位置（吸附由后台算路线程做）。 */
typedef struct {
    int    have;
    double lat, lon;        /* 点击位置 */
    int    snapped;
    double slat, slon;      /* 吸附后的位置 */
    double snap_m;          /* 吸附偏移（米） */
    char   prov[64];        /* 落在哪个省 */
} NavPick;

/* 后台算路线程交给主线程的结果包。RouteResultSet 里的指针整体搬过来，
   所以搬完只 free 外壳，不 route_result_free。 */
typedef struct {
    RouteResultSet alts;    /* 多方案（至少 1 条可行） */
    int            err;     /* 0=成功（至少1个可行） -1=不连通 -2=超限 -3=起点/终点附近无路 */
    double         a_snap_m, b_snap_m;
    double         a_lat, a_lon, b_lat, b_lon;
    int            a_prov, b_prov;
} NavResult;

/* 搜索框状态（阶段 7）。
 * text 是"已提交"的文本（输入法最终上屏的那些字），editing 是正在组合的串
 * —— 中文输入必须分这两个：只认 text 的话，拼音阶段一个字都看不见，
 * 会让人以为搜索框没反应。 */
typedef struct {
    int        active;
    char       text[64];
    char       editing[64];
    SearchHit  hits[16];
    int        n_hits, sel;
    uint64_t   elapsed_ms;
} SearchUI;

/* 窗口 + 主循环。
 * Wayland 注意：只用 SDL2 抽象，不碰 X11/GTK 的 API；窗口真实尺寸要
 * PumpEvents 之后才拿得到（本机实测会被窗口管理器夹小），所以按"上限分配、
 * 运行期以实际为准"处理，不假设创建时请求的尺寸就是最终尺寸。
 */
typedef struct {
    /* SDL 相关的窗口/画布/纹理由 window_sdl.c 持有（navimap_qt 不链 SDL，
       它用自己的画布缓冲，二者都通过 app_render_to_buf 调同一套绘制）。 */
    MapView       view;
    MapSet        ms;                     /* 图集：单省或多省（阶段 4） */
    uint64_t      total_nodes, total_edges;
    Font          *font;                  /* 标注字体（可选） */
    int            font_size;             /* 字号（pt） */
    char           font_used[512];        /* 实际使用的字体文件（日志/排查用） */
    char           font_path[512];        /* 生效的字体文件路径（改字号时重开） */
    LabelCache    places;                 /* 地名库（可选，全局一份，跨省通用） */
    int           has_places;
    DedupSet      dd;                     /* 跨省去重（多个省同时盖住视口才启用） */
    int           quit;
    int           need_redraw;
    int           max_class;     /* LOD：当前画到哪一级道路 */
    int           dragging;
    int           last_x, last_y;
    int           down_x, down_y;
    char          status[256];

    /* ---- 导航（阶段 9）----
     * 算路放在后台线程：跨省长途 5 秒级，放主线程会把窗口卡成假死。
     * Router 只被后台线程碰（它不是线程安全的：反向索引是惰性构建的），
     * 主线程只提交请求 + 收割结果，绝不直接调 router_*。 */
    Router       *router;        /* 惰性创建：第一次算路才建（状态表约 30MB） */
    NavPick       pick_a, pick_b;/* 起点 / 终点 */
    RouteResult   rr;             /* 当前主路线（= 选中方案，指针从 ralts 搬来） */
    int           have_rr;
    RouteResultSet ralts;        /* 多方案集合（含已选中的那一条，指针已被搬走） */
    int           have_alts;     /* 是否已有多方案可对比 */
    int           sel_alt;       /* 选中的方案下标（0..n-1） */
    ManeuverList  man;
    NmMutex      *nav_mu;
    int           nav_busy;      /* 后台在算（nav_mu 保护） */
    int           nav_pending;   /* 排队中的新请求（nav_mu 保护） */
    NavPick       req_a, req_b;
    NavResult    *nav_done;      /* 后台产出，主线程收割 */
    int           nav_done_flag;
    char          nav_msg[192];  /* 面板顶行："计算中…" / "无可行路线" 等 */
    int           show_panel;
    int           nav_stop;      /* 退出：后台线程干完手头这单就收工（nav_mu 保护） */
    uint64_t      nav_t0;        /* 本单开始时刻（标题栏显示"计算中 x.x s"） */

    /* ---- 模拟导航（阶段 9 导航层）----
     * sim 只在主线程里动：每帧 advance + locate，结果就是"车头在哪、还剩多远"。
     * sim_paused 用于"偏航重算期间先别走"，sim_on 才是总开关。 */
    NavSim        sim;
    int           sim_on;        /* 模拟导航开着 */
    int           sim_paused;    /* 暂时不走（重算中 / 已到达） */
    int           sim_follow;    /* 视口跟着车头 */
    int           sim_speed_idx; /* 倍速档（SPEEDS[] 下标） */
    uint64_t      sim_tick;      /* 上一帧的时刻，用来算真实 dt */

    /* ---- 无地图区域提示 ----
     * "关注点"（模拟导航中=车头位置，否则=视口中心）落到没有数据的地方时，
     * 顶部弹一条横幅告诉用户去哪下载。cov_lat/lon 是上次检查的位置：
     * 细判要查瓦片索引，位置没实质移动就不重算。 */
    int           cov_state;      /* COV_* ，来自 mapset_coverage */
    int           cov_checked;
    double        cov_lat, cov_lon;
    char          cov_hint[1024]; /* 完整提示（含下载来源），位置变化时打印一次 */
    Font         *font_big;      /* 指令条的大字号（font_size * 2） */

    /* ---- 搜索（阶段 7）----
     * sdb 是独立句柄，不和 LabelCache 共用：标注查询可能正拿着语句在遍历，
     * 搜索插进来复用同一个连接会把游标踩乱（sqlite3 连接不是并发安全的）。 */
    SearchDb     *sdb;
    SearchUI      su;

    /* ---- nav-desktop 风格界面状态（chrome.c 绘制 + 命中测试）----
     * 这些是纯 UI 状态，与导航/渲染算法无关；改它们不影响算路结果。 */
    int           day_night;      /* 0 昼 / 1 夜（夜间半透明蒙版） */
    int           mode;           /* 顶栏模式页签：0 浏览 / 1 导航 */
    int           datum;          /* 坐标系（大地基准）：0 WGS84 / 1 CGCS2000 / 2 北京54 / 3 西安80 */
    int           menu_open;      /* 主菜单浮层是否展开 */
    int           chrome_menu_item;/* 菜单命中项下标（chrome_hit 写入） */
    int           win_drag;       /* 自绘标题栏拖拽中 */
    int           win_drag_x, win_drag_y;  /* 拖拽时鼠标相对窗口的偏移 */
    int           rotating;       /* Ctrl+拖动 旋转地图中 */
    /* Qt 前端开关：置 1 时 paint_frame 跳过 SDL 自绘 chrome/搜索框/面板/
     * 指令条（这些由 Qt 真控件承担），但地图渲染、结果图钉、夜间蒙版保留。 */
    int           qt_ui;

    /* ---- 设置持久化（完整设置系统）----
     * cfg 在 app_init 载入、app_shutdown 写回；show_poi/show_labels 是本次新增的
     * 显示开关（受设置面板控制）；settings_* 是 chrome 设置面板的命中结果，
     * 由 chrome.c 写入、window.c 的 chrome_act 消费。 */
    int           show_poi;        /* POI 图标开关 */
    int           show_labels;     /* 地名标注开关 */
    int           settings_open;   /* 设置面板是否展开（仅 SDL 自绘 chrome） */
    int           settings_row;    /* 面板命中行：>=0 schema 下标，-1 关闭，-2 重置全部 */
    int           settings_btn;    /* 命中按钮：0=左/− 1=右/+ 2=行体（切换） */
    Settings     *cfg;             /* 配置文件（NULL=未启用） */

    /* ---- 语音播报（离线 TTS）----
     * voice_on 受设置面板 / V 键控制；ann_tier 是 per-maneuver 的"已播报层级"位图，
     * 避免同一指令重复播报；ann_arrive 标记到达播报是否已发。 */
    int           voice_on;        /* 语音播报开关 */
    uint8_t      *ann_tier;        /* 每条 maneuver 的已播报层级位图（1=接近 2=立即） */
    int           ann_cap;         /* ann_tier 容量（= man.n） */
    uint8_t       ann_arrive;      /* 到达播报已发 */
} App;

/* font_path：NULL = 用默认字体（宋体优先的候选链）；"none" 或 "" = 不加载字体
 *             （纯图形，无文字标注）；否则为 .ttf/.ttc 路径。
 * font_size：<=0 时用默认字号 14。
 * ab：启动自愈选项（源 pbf 在、产物没了就现场重建）。NULL = 开启并自动探测
 *     sources/ 与 graph_extract；只想关掉就传 &{ .enable = 0 }。 */
int  app_init(App *a, const char *graph_dir, int w, int h,
              const char *font_path, int font_size, const AutoBuildOpt *ab);

/* "关注点"：模拟导航开着时是车头（人跟着车走），否则是视口中心。
 * 无地图区域提示按它判断，无头验收的判据也必须按它——两处各写一遍的话，
 * 出现"视口有图、车头无图"这种混合情况时就会一边弹提示一边判失败。 */
void app_focus_point(const App *a, double *lat, double *lon);

/* 主循环。★ 实现在 window_sdl.c（**不在** navimap_core 里）：SDL 窗口版专用，
   链 SDL 的目标（navmap / map_shot）才编入。Qt 前端有自己的主循环，不调它。 */
void app_run(App *a);
void app_shutdown(App *a);

/* 设置持久化：app_apply_settings 把配置(cfg)→App 字段；app_persist_settings
 * 把 App→配置并写盘。面板编辑 / 键位 / 菜单切换后调用后者即时落盘。 */
void app_apply_settings(App *a);
void app_persist_settings(App *a);

/* 状态栏文本（窗口标题栏用）。实现留在 window.c（Qt 前端也可用）。 */
void update_status(App *a, double frame_ms);

/* 运行期改字号（返回实际生效的字号）。字号是 TTF_Font 的属性，改它必须
 * 重建 Font —— 旧字形纹理全部作废，这里一并释放。字体被关闭时返回 0。 */
int  app_set_font_size(App *a, int size);

/* 无窗口渲染：把一帧画进调用方持有的像素缓冲（供 map_shot 无人值守验证、
   Qt 前端自持画布都走这条；SDL 窗口版在 window_sdl.c 内部也用它）。
   px 为 w×h 的 32bpp 缓冲，stride 以"像素"计；rmask/gmask/bmask/amask 描述
   打包格式（例如 QImage::Format_RGB32 在 x86 小端为 0x00FF0000/0x0000FF00/
   0x000000FF/0xFF000000）。 */
int  app_render_to_buf(App *a, void *px, int w, int h, size_t stride,
                       uint32_t rmask, uint32_t gmask, uint32_t bmask,
                       uint32_t amask, PaintStats *st);

/* ---- 导航入口（阶段 9）----
 * 全部在主线程调用：只改状态、提交请求、收割结果。真正的 router_* 调用
 * 都在后台线程里。 */
void app_nav_click(App *a, double lat, double lon);   /* 左键点一下：先起点后终点 */
void app_nav_clear(App *a);                           /* 右键 / c 键 */
void app_nav_fit(App *a);                             /* f 键：缩放到整条路线 */
void app_nav_toggle_panel(App *a);                    /* p 键 */
void app_nav_poll(App *a);                            /* 主循环每帧调用，收割结果 */
void app_nav_select_alt(App *a, int k);              /* 切换多方案中的第 k 条（0..n-1） */

/* ---- 模拟导航 ---- */
void app_nav_sim_toggle(App *a);      /* g 键：开始 / 停止 */
void app_nav_sim_speed(App *a, int dir);  /* . 加速 / , 减速 */
void app_nav_sim_follow(App *a);      /* t 键：视口是否跟着车头 */
void app_nav_sim_deviate(App *a);     /* n 键：注入一次偏航（触发重算） */
void app_nav_sim_tick(App *a);        /* 主循环每帧调用：按真实 dt 推进车头 */

/* ---- 搜索入口（阶段 7）----
 * 查询本身是毫秒级（实测均值 1 ms），直接在主线程做，不必开线程。 */
void app_search_open(App *a);                 /* / 或 s 键 */
void app_search_close(App *a);
void app_search_input(App *a, const char *utf8);   /* IME 上屏文本 */
void app_search_editing(App *a, const char *utf8); /* IME 组合串（只显示） */
void app_search_backspace(App *a);
void app_search_move(App *a, int dir);        /* ↑↓ 选择 */
void app_search_commit(App *a);               /* Enter：定位 + 设为起/终点 */
/* 同步搜一次（map_shot --q / main --q 用，没有窗口也行）。返回命中条数。 */
int  app_search_query(App *a, const char *q);

/* 同步算一次路线（供 map_shot 无人值守验证：没有窗口、没有主循环）。
   返回 0 成功。 */
int  app_nav_plan_sync(App *a, double alat, double alon,
                       double blat, double blon);

#endif
