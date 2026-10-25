/* navmap_qt —— navimap 的 Qt 前端入口。
 *
 * 用法与 SDL 版 navimap 完全一致：
 *   ./navmap_qt [--graph=products/graph/beijing] [--w=1280] [--h=800]
 *               [--font=<路径.ttf|none>] [--font-size=14]
 *               [--no-autobuild] [--sources=<目录>]
 *               [--pbf-display=<sources目录>[:LRU上限]] [--q=<查询词>]
 *               [--shot=<输出.png>]        ← 渲染一帧存 PNG 后退出
 *                                               （配合 QT_QPA_PLATFORM=offscreen
 *                                                 可无人值守出图）
 *               [--shot-search] [--shot-track]  ← 截图前先展开搜索框 / 轨迹面板
 *
 * 分层：本文件只做参数解析与生命周期编排；窗口/部件在 main_window.cpp，
 * 地图画布在 map_canvas.cpp；C 核心（渲染/算路/搜索）经 window.h 的
 * app_* API 使用，一行不改。
 */
#include <QApplication>
#include <QImage>

#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include "ui/window.h"
#include "map/mapset.h"     /* mapset_enable_pbf_display / mapset_totals */
#include "utils/datum.h"     /* DatumId / DATUM_*：--datum 解析 */
}

#include "main_window.h"
#include "qt_font.h"

int main(int argc, char **argv)
{
    const char *graph = "products/graph/beijing";
    int w = 1280, h = 800;
    const char *font = NULL;          /* NULL = 默认字体候选链 */
    int font_size = 0;
    const char *q = NULL;             /* 启动时搜一次并跳到第一条结果 */
    const char *pbf_display = NULL;
    int pbf_lru = 3;
    const char *shot = NULL;          /* --shot=path：出一张图就退出 */
    int grid = 0;                     /* --grid：开局打开坐标网叠加层 */
    int ui_test = 0;                  /* --ui-test：跑无头 UI 自检后退出 */
    int shot_search = 0;              /* --shot-search：截图前先展开搜索框 */
    int shot_track = 0;               /* --shot-track：截图前先展开轨迹/收藏面板 */
    const char *datum = NULL;         /* --datum=wgs84|cgcs2000|bj54|xa80：坐标系（命令行优先） */
    const char *datum_grid = NULL;    /* --datum-grid=<file>：图幅改正量格网（入口②） */
    const char *gpx_in[8] = {0};      /* --gpx=<file>：启动即导入（可多次，最多 8 个） */
    int n_gpx_in = 0;
    const char *gpx_out = NULL;       /* --export-gpx=<file>：退出时导出全部轨迹+收藏 */

    AutoBuildOpt ab;
    memset(&ab, 0, sizeof(ab));
    ab.enable = 1;

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--graph=", 8) == 0) graph = argv[i] + 8;
        else if (strncmp(argv[i], "--w=", 4) == 0) w = atoi(argv[i] + 4);
        else if (strncmp(argv[i], "--h=", 4) == 0) h = atoi(argv[i] + 4);
        else if (strncmp(argv[i], "--font=", 7) == 0) font = argv[i] + 7;
        else if (strncmp(argv[i], "--font-size=", 12) == 0)
            font_size = atoi(argv[i] + 12);
        else if (strncmp(argv[i], "--q=", 4) == 0) q = argv[i] + 4;
        else if (strcmp(argv[i], "--no-autobuild") == 0) ab.enable = 0;
        else if (strncmp(argv[i], "--sources=", 10) == 0) ab.src_dir = argv[i] + 10;
        else if (strncmp(argv[i], "--shot=", 7) == 0) shot = argv[i] + 7;
        else if (strcmp(argv[i], "--grid") == 0) grid = 1;
        else if (strncmp(argv[i], "--datum=", 8) == 0) datum = argv[i] + 8;
        else if (strncmp(argv[i], "--datum-grid=", 13) == 0) datum_grid = argv[i] + 13;
        else if (strncmp(argv[i], "--gpx=", 6) == 0) {
            if (n_gpx_in < (int)(sizeof(gpx_in) / sizeof(gpx_in[0])))
                gpx_in[n_gpx_in++] = argv[i] + 6;
            else fprintf(stderr, "提示: --gpx 最多 %d 个，忽略 %s\n",
                         (int)(sizeof(gpx_in) / sizeof(gpx_in[0])), argv[i] + 6);
        }
        else if (strncmp(argv[i], "--export-gpx=", 13) == 0) gpx_out = argv[i] + 13;
        else if (strcmp(argv[i], "--ui-test") == 0) ui_test = 1;
        else if (strcmp(argv[i], "--shot-search") == 0) shot_search = 1;
        else if (strcmp(argv[i], "--shot-track") == 0) shot_track = 1;
        else if (strncmp(argv[i], "--pbf-display=", 14) == 0) {
            pbf_display = argv[i] + 14;
            const char *colon = strrchr(pbf_display, ':');
            if (colon && colon[1] >= '0' && colon[1] <= '9') {
                pbf_lru = atoi(colon + 1);
                size_t dlen = (size_t)(colon - pbf_display);
                static char dirbuf[4096];
                if (dlen < sizeof(dirbuf)) {
                    memcpy(dirbuf, pbf_display, dlen);
                    dirbuf[dlen] = '\0';
                    pbf_display = dirbuf;
                }
            }
        }
        else { fprintf(stderr, "未知参数 %s\n", argv[i]); return 2; }
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("NavMap"));

    /* ★ Qt 控件的中文字体：Qt 自己画控件文字，走 Qt 的字体系统；地图上的文字
     *   是 C 侧 stb_truetype 画的（两套互不相干）。不显式给 Qt 中文字体的话，
     *   没装中文字体的机器上控件文字全是方框（tofu），而地图文字却正常。
     *   这里把 C 侧自带的字体（含 DroidSansFallback = CJK）注册进 Qt 字体库，
     *   并设成"中英回退链"。详见 qt_font.cpp。 */
    qt_setup_fonts(font_size > 0 ? font_size : 0);

    /* ★ Qt 前端**不初始化也不链接 SDL**：画布缓冲自己管（见 map_canvas.cpp），
     * 窗口/事件全是 Qt 的。
     * 以前这里必须 SDL_Init(SDL_INIT_VIDEO|SDL_INIT_NOPARACHUTE) 并设
     * SDL_HINT_NO_SIGNAL_HANDLERS——因为 SDL 默认会给 SIGINT/SIGTERM 装
     * "parachute"处理器，把退出请求塞进 SDL 事件队列等 SDL_PollEvent 消费，
     * 而 Qt 应用不轮询 SDL 事件，信号就被整个吞掉（实测 timeout 8 发 TERM
     * 进程纹丝不动，只能 SIGKILL）。现在不碰 SDL，这个坑整个消失：
     * Qt 的信号处理是 Qt 自己的，与 SDL 无关。 */

    /* C 核心生命周期：app_init 装图集/字体/搜索库；qt_ui=1 让 paint_frame
     * 跳过自绘 chrome（由 Qt 控件承担）。 */
    App a;
    if (app_init(&a, graph, w, h, font, font_size, &ab) != 0) return 1;
    a.qt_ui = 1;

    /* 坐标系：命令行优先于设置文件（app_init 已按 settings.ini 设过 a.datum）。 */
    if (datum) {
        int did = DATUM_WGS84;
        if (!strcmp(datum, "cgcs2000") || !strcmp(datum, "cgcs2000+") ||
            !strcmp(datum, "2000"))
            did = DATUM_CGCS2000;
        else if (!strcmp(datum, "bj54") || !strcmp(datum, "beijing1954") ||
                 !strcmp(datum, "北京54") || !strcmp(datum, "北京1954"))
            did = DATUM_BJ54;
        else if (!strcmp(datum, "xa80") || !strcmp(datum, "xian1980") ||
                 !strcmp(datum, "西安80") || !strcmp(datum, "西安1980"))
            did = DATUM_XA80;
        a.datum = did;
    }

    /* 图幅改正量格网（入口②）：拿到测绘部门的 1:5万 改正量后转成文本即可直接挂上，
     * 不用改代码。格式见 utils/datum.h 的 datum_grid_load。 */
    if (datum_grid) {
        int rc = datum_grid_load((DatumId)a.datum, datum_grid);
        if (rc != 0) {
            fprintf(stderr, "提示: 改正量格网加载失败（%s，错误码 %d），按无格网继续\n",
                    datum_grid, rc);
        } else {
            fprintf(stderr, "已挂改正量格网: %s -> %s\n", datum_grid, datum_short((DatumId)a.datum));
        }
    }

    if (pbf_display) {
        if (mapset_enable_pbf_display(&a.ms, pbf_display, pbf_lru) != 0) {
            fprintf(stderr, "提示: PBF 显示层不可用（目录 %s），改用 nmbin 产物\n",
                    pbf_display);
        } else {
            printf("显示层: 从 PBF 运行时解码（%s，LRU %d 省）\n", pbf_display,
                   pbf_lru);
        }
    }
    /* GPX 导入（启动即入）：导入的轨迹/收藏同时写 tracks.db，下次开机还在。 */
    for (int i = 0; i < n_gpx_in; i++) {
        int n = app_gpx_import(&a, gpx_in[i]);
        if (n < 0)
            fprintf(stderr, "提示: GPX %s 导入失败（%d）\n", gpx_in[i], n);
        else
            printf("GPX: %s -> %d 条轨迹（含 wpt 转收藏）\n", gpx_in[i], n);
    }
    if (gpx_out) {
        if (app_gpx_export(&a, gpx_out) == 0)
            printf("GPX: 已导出全部轨迹/收藏 -> %s\n", gpx_out);
        else
            fprintf(stderr, "提示: GPX 导出失败（%s）\n", gpx_out);
    }
    if (q) {
        int n = app_search_query(&a, q);
        printf("搜索「%s」%d 条（%llu ms）\n", q, n,
               (unsigned long long)a.su.elapsed_ms);
        if (n > 0) {
            a.view.lat = a.su.hits[0].lat;
            a.view.lon = a.su.hits[0].lon;
            if (a.view.zoom < 14.0) a.view.zoom = 15.0;
        }
    }

    uint64_t tn, te;
    mapset_totals(&a.ms, &tn, &te);
    printf("图: %s  %d 省  节点 %llu  边 %llu\n", graph, a.ms.n,
           (unsigned long long)tn, (unsigned long long)te);
    printf("字体: %s  %d pt\n",
           a.font_used[0] ? a.font_used : "(无)", a.font_size);

    fflush(stdout);
    MainWindow win(&a);
    fflush(stdout);

    if (ui_test) {
        /* 无头 UI 自检：断言"搜索按钮 → 点击 → 输入框出现 → Esc 收起"
           这条交互链，以及所有按钮都有悬停提示。
           ★ 必须先把窗口 show() 出来：控件的 isVisible() 依赖祖先可见，
             否则"初始可见 / 点击后可见"这两条都测不准。 */
        win.resize(w, h);
        win.show();
        QApplication::processEvents();
        int rc = win.runUiSelfTest();
        app_shutdown(&a);
        return rc;
    }

    if (shot) {
        /* 无人值守：渲染一帧抓全窗口（含 chrome 控件）存 PNG */
        int shot_fail = 0;
        if (grid) win.setShowGrid(true);
        if (shot_search) win.expandSearchForShot();   /* 展开搜索框，便于看展开态 */
        if (shot_track) win.openTrackPanelForShot();  /* 展开轨迹/收藏面板 */
        win.resize(w, h);
        win.show();
        QApplication::processEvents();   /* 让布局/首帧完成 */
        for (int i = 0; i < 3; i++) QApplication::processEvents();
        /* ★ 截图不许撒谎：面板要求展开就必须真的贴在右栏左边。
         *   （踩过：layoutOverlays 用 isVisible() 判断显隐，而 openTrackPanelForShot
         *     跑在 win.show() 之前 ⇒ 分支被跳过 ⇒ 面板停在 (0,0)，截图上看不见。）
         *   这条断言把"图里有没有面板"变成退出码，而不是靠人眼。 */
        if (shot_track) {
            const QSize cs = win.canvasSize();
            const QPoint pp = win.trackPanelPos();
            const QSize ps = win.trackPanelSize();
            const QPoint rp = win.rightOverlayPos();
            if (!win.trackPanelVisible() || ps.width() <= 0 ||
                cs.width() <= 0 || pp.x() <= cs.width() / 2 ||
                pp.x() + ps.width() > rp.x() + 1) {
                printf("ASSERT-FAIL    轨迹面板没贴到右栏左边（pos=%d,%d %dx%d 画布 %dx%d 右栏 %d）\n",
                       pp.x(), pp.y(), ps.width(), ps.height(),
                       cs.width(), cs.height(), rp.x());
                shot_fail = 1;
            } else {
                printf("track_panel    %d,%d %dx%d（画布 %dx%d，右栏 x=%d）\n",
                       pp.x(), pp.y(), ps.width(), ps.height(),
                       cs.width(), cs.height(), rp.x());
            }
        }
        /* 画布右下角坐标框 / 左下角路线信息都由 paintEvent 自动绘制，
           不再需要像原先那样显式刷新状态栏（--shot 不启动主循环 ⇒ 状态栏空白）。 */
        QImage img = win.grab().toImage();
        if (!img.save(QString::fromUtf8(shot))) {
            fprintf(stderr, "保存截图失败: %s\n", shot);
        } else {
            printf("截图已保存: %s\n", shot);
        }
        app_shutdown(&a);
        return shot_fail;
    }

    if (q && a.su.n_hits > 0) a.su.active = 1;   /* 启动查询的结果显示图钉 */

    win.show();
    win.start();
    int rc = app.exec();
    app_shutdown(&a);
    return rc;
}
