#include "render/chrome.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "render/font.h"
#include "render/navdraw.h"
#include "render/raster.h"
#include "route/maneuver.h"
#include "map/coverage.h"
#include "settings/settings.h"

/* ===================== 前向声明 ===================== */
static int  settings_hit(App *a, int x, int y);
static void chrome_act_settings(App *a);
static void draw_settings_panel(Raster *rs, App *a);

/* ===================== 几何：绘制与命中测试共用 ===================== */

#define TH_TITLE CHROME_TITLE_H   /* 自绘标题栏高度 */
#define TH_TOP   46               /* 顶栏高度 */
#define COL_W    42      /* 右栏按钮宽度 */
#define BTN      40      /* 右栏/按钮边长 */
#define GAP      8

typedef struct {
    int w, h;
    RectI title, wmin, wmax, wclose;
    RectI topbar, menubtn, search, tbrowse, tnav;
    RectI zin, zout, loc, day;
    RectI navtop, nback, nex;
    RectI steps;
    RectI alts;        /* 多方案对比面板（算路后、且多于 1 条可行时出现） */
    RectI scale;
    RectI compass;
    RectI menu;
} CLay;

#define IN(r,x,y) ((x) >= (r).x && (x) < (r).x + (r).w && \
                   (y) >= (r).y && (y) < (r).y + (r).h)

static void layout(App *a, CLay *L)
{
    int w = a->view.w, h = a->view.h;
    L->w = w; L->h = h;

    L->title  = (RectI){ 0, 0, w, TH_TITLE };
    int by = (TH_TITLE - 22) / 2;
    L->wclose = (RectI){ w - 30, by, 22, 22 };
    L->wmax   = (RectI){ w - 58, by, 22, 22 };
    L->wmin   = (RectI){ w - 86, by, 22, 22 };

    L->topbar = (RectI){ 0, TH_TITLE, w, TH_TOP };
    L->menubtn = (RectI){ 8, TH_TITLE + 8, 30, TH_TOP - 16 };
    L->tbrowse = (RectI){ w - 160, TH_TITLE + 10, 76, TH_TOP - 20 };
    L->tnav    = (RectI){ w - 80,  TH_TITLE + 10, 76, TH_TOP - 20 };
    int sw = w - 44 - 168; if (sw < 120) sw = 120;
    L->search = (RectI){ 44, TH_TITLE + 8, sw, TH_TOP - 16 };

    int rx = w - COL_W - 8, ry0 = TH_TITLE + TH_TOP + 12;
    L->zin  = (RectI){ rx, ry0,                  COL_W, BTN };
    L->zout = (RectI){ rx, ry0 + (BTN + GAP),    COL_W, BTN };
    L->loc  = (RectI){ rx, ry0 + 2*(BTN + GAP),  COL_W, BTN };
    L->day  = (RectI){ rx, ry0 + 3*(BTN + GAP),  COL_W, BTN };

    int ny = TH_TITLE + TH_TOP;
    if (a->have_rr) {
        L->navtop = (RectI){ 0, ny, w, 36 };
        L->nback  = (RectI){ 8,  ny + 6, 24, 24 };
        L->nex    = (RectI){ w - 34, ny + 6, 26, 24 };
        ny += 36 + 8;
    } else {
        L->navtop = (RectI){ 0, 0, 0, 0 };
        L->nback  = (RectI){ 0, 0, 0, 0 };
        L->nex    = (RectI){ 0, 0, 0, 0 };
    }

    int ph = h - ny - 70;
    if (ph < 80) ph = 80;
    if (ph > 360) ph = 360;
    L->steps = (RectI){ 12, ny, 250, ph };

    /* 多方案对比面板：放在右下角。只有"可行方案多于 1 条"才值得比（1 条没得选）。
       行高 34，首行是标题 22，加 8 内边距。 */
    int nfeas = 0;
    if (a->have_alts)
        for (int k = 0; k < a->ralts.n; k++)
            if (a->ralts.err[k] == 0) nfeas++;
    if (nfeas > 1) {
        int AW = (a->font ? 236 : 200);
        int AH = 22 + nfeas * 34 + 8;
        L->alts = (RectI){ w - AW - 12, h - AH - 12, AW, AH };
    } else {
        L->alts = (RectI){ 0, 0, 0, 0 };
    }

    L->scale = (RectI){ 12, h - 34, 150, 22 };

    if (a->view.rot != 0.0)
        L->compass = (RectI){ w - 56, TH_TITLE + TH_TOP + 12, 44, 44 };
    else
        L->compass = (RectI){ 0, 0, 0, 0 };

    int mc = chrome_menu_count();
    L->menu = (RectI){ 8, TH_TITLE + 8, 200, mc * 30 + 8 };
}

/* ===================== 小工具 ===================== */

static void rfill(Raster *rs, int x, int y, int w, int h,
                  uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca)
{
    raster_fill_rect(rs, x, y, w, h, cr, cg, cb, ca);
}
static void rstroke(Raster *rs, int x, int y, int w, int h,
                    uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca)
{
    raster_rect(rs, x, y, w, h, cr, cg, cb, ca);
}
static void rline(Raster *rs, int x0, int y0, int x1, int y1,
                  uint8_t cr, uint8_t cg, uint8_t cb)
{
    raster_line_rgba(rs, x0, y0, x1, y1, cr, cg, cb, 255);
}

/* ===================== 夜间蒙版 ===================== */

void paint_night_tint(Raster *rs, int w, int h)
{
    rfill(rs, 0, 0, w, h, 8, 14, 34, 96);
}

/* ===================== 主菜单条目 ===================== */

static const char *MENU_ITEMS[] = {
    "昼 / 夜 切换",
    "搜索地点",
    "清除路线",
    "缩放到路线",
    "跟随车头",
    "设置",
    "退出",
};
int chrome_menu_count(void) { return (int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0])); }
const char *chrome_menu_label(int i)
{
    if (i < 0 || i >= chrome_menu_count()) return "";
    return MENU_ITEMS[i];
}

/* ===================== 绘制：各元素 ===================== */

static void draw_titlebar(Raster *rs, App *a, const CLay *L)
{
    rfill(rs, L->title.x, L->title.y, L->title.w, L->title.h, 28, 36, 56, 255);
    if (a->font) font_draw(a->font, rs, 10, L->title.y + 6, "NavMap", 235, 240, 250, 0);
    /* 三个窗口按钮：红/黄/绿 小圆点（mac 风，跨平台通用） */
    const RectI *bs[3] = { &L->wclose, &L->wmax, &L->wmin };
    uint8_t col[3][3] = { {233,84,72}, {240,192,72}, {76,200,96} };
    for (int i = 0; i < 3; i++) {
        int cx = bs[i]->x + bs[i]->w/2, cy = bs[i]->y + bs[i]->h/2;
        rfill(rs, cx-7, cy-7, 14, 14, col[i][0], col[i][1], col[i][2], 255);
    }
}

static void draw_topbar(Raster *rs, App *a, const CLay *L)
{
    rfill(rs, L->topbar.x, L->topbar.y, L->topbar.w, L->topbar.h, 245, 247, 250, 250);
    rstroke(rs, L->topbar.x, L->topbar.y + L->topbar.h - 1, L->topbar.w, 1, 200, 206, 214, 255);

    /* 菜单按钮（☰） */
    rfill(rs, L->menubtn.x, L->menubtn.y, L->menubtn.w, L->menubtn.h, 60, 110, 220, 255);
    for (int i = 0; i < 3; i++)
        rline(rs, L->menubtn.x + 7, L->menubtn.y + 9 + i*8,
                   L->menubtn.x + 23, L->menubtn.y + 9 + i*8, 255, 255, 255);

    /* 搜索框（占位；活跃时由 paint_search_box 的大框盖在上面） */
    if (!a->su.active) {
        rfill(rs, L->search.x, L->search.y, L->search.w, L->search.h, 255, 255, 255, 255);
        rstroke(rs, L->search.x, L->search.y, L->search.w, L->search.h, 180, 188, 200, 255);
        if (a->font)
            font_draw(a->font, rs, L->search.x + 10, L->search.y + 8,
                      "搜索地点、道路…", 150, 156, 168, 0);
    }

    /* 模式页签 */
    int navOn = a->have_rr;
    RectI tabs[2] = { L->tbrowse, L->tnav };
    const char *lab[2] = { "浏览", "导航" };
    int on[2] = { !navOn, navOn };
    for (int i = 0; i < 2; i++) {
        if (on[i]) rfill(rs, tabs[i].x, tabs[i].y, tabs[i].w, tabs[i].h, 60, 110, 220, 255);
        else       rstroke(rs, tabs[i].x, tabs[i].y, tabs[i].w, tabs[i].h, 150, 160, 178, 255);
        if (a->font)
            font_draw(a->font, rs, tabs[i].x + 22, tabs[i].y + 9, lab[i],
                      on[i] ? 255 : 90, on[i] ? 255 : 100, on[i] ? 255 : 120, 0);
    }
}

static void draw_rcol(Raster *rs, App *a, const CLay *L)
{
    RectI bs[4] = { L->zin, L->zout, L->loc, L->day };
    for (int i = 0; i < 4; i++)
        rfill(rs, bs[i].x, bs[i].y, bs[i].w, bs[i].h, 250, 251, 253, 245);
    rstroke(rs, L->zin.x,  L->zin.y,  L->zin.w,  L->zin.h,  150, 158, 172, 255);
    rstroke(rs, L->zout.x, L->zout.y, L->zout.w, L->zout.h, 150, 158, 172, 255);
    rstroke(rs, L->loc.x,  L->loc.y,  L->loc.w,  L->loc.h,  150, 158, 172, 255);
    rstroke(rs, L->day.x,  L->day.y,  L->day.w,  L->day.h,  150, 158, 172, 255);

    /* ＋ / − */
    rline(rs, L->zin.x + 10,  L->zin.y + BTN/2,  L->zin.x + BTN-10, L->zin.y + BTN/2, 40, 50, 70);
    rline(rs, L->zin.x + BTN/2, L->zin.y + 10,  L->zin.x + BTN/2, L->zin.y + BTN-10, 40, 50, 70);
    rline(rs, L->zout.x + 10, L->zout.y + BTN/2, L->zout.x + BTN-10, L->zout.y + BTN/2, 40, 50, 70);
    /* 定位：圆环 + 中心点 */
    int lcx = L->loc.x + BTN/2, lcy = L->loc.y + BTN/2;
    uint32_t loc_c = raster_rgb(rs, 40, 50, 70);
    for (int t = 0; t < 360; t += 12)
        raster_point(rs, (int)(lcx + 11*cos(t*M_PI/180.0)),
                          (int)(lcy + 11*sin(t*M_PI/180.0)), loc_c);
    rfill(rs, lcx-3, lcy-3, 6, 6, 40, 50, 70, 255);
    /* 昼夜：日(黄)/夜(蓝) 圆 */
    if (a->day_night) rfill(rs, L->day.x + BTN/2 - 11, L->day.y + BTN/2 - 11, 22, 22, 60, 90, 200, 255);
    else              rfill(rs, L->day.x + BTN/2 - 11, L->day.y + BTN/2 - 11, 22, 22, 250, 200, 60, 255);
}

static void draw_navtop(Raster *rs, App *a, const CLay *L)
{
    if (!a->have_rr) return;
    rfill(rs, L->navtop.x, L->navtop.y, L->navtop.w, L->navtop.h, 30, 90, 200, 255);
    rfill(rs, L->nback.x, L->nback.y, L->nback.w, L->nback.h, 255, 255, 255, 255);
    rline(rs, L->nback.x + 16, L->nback.y + 4, L->nback.x + 6, L->nback.y + 12, 30, 90, 200);
    rline(rs, L->nback.x + 6, L->nback.y + 12, L->nback.x + 16, L->nback.y + 20, 30, 90, 200);
    rline(rs, L->nex.x + 6, L->nex.y + 6, L->nex.x + 20, L->nex.y + 18, 255, 255, 255);
    rline(rs, L->nex.x + 20, L->nex.y + 6, L->nex.x + 6, L->nex.y + 18, 255, 255, 255);
    if (a->font) {
        char dst[64];
        if (a->pick_b.have)
            snprintf(dst, sizeof(dst), "到 终点 (%.4f, %.4f)", a->pick_b.lat, a->pick_b.lon);
        else
            snprintf(dst, sizeof(dst), "导航中");
        font_draw(a->font, rs, L->nex.x + 34, L->navtop.y + 9, dst, 255, 255, 255, 0);
    }
}

static void draw_navbottom(Raster *rs, App *a, const CLay *L)
{
    (void)L;
    /* 偏航重算中 → 顶部弹出"重新规划中…" */
    if (!(a->sim_on && a->sim_paused && a->nav_busy && a->have_rr)) return;
    if (!a->font) return;
    int bw = 200, bh = 30, bx = (L->w - bw)/2, by = TH_TITLE + TH_TOP + 6;
    rfill(rs, bx, by, bw, bh, 232, 150, 40, 240);
    font_draw(a->font, rs, bx + 30, by + 6, "重新规划中…", 255, 255, 255, 0);
}

static void draw_steps(Raster *rs, App *a, const CLay *L)
{
    if (!a->have_rr || a->man.n == 0) return;
    int x = L->steps.x, y = L->steps.y, w = L->steps.w, h = L->steps.h;
    rfill(rs, x, y, w, h, 250, 251, 253, 246);
    rstroke(rs, x, y, w, h, 150, 158, 172, 255);
    if (!a->font) return;
    int row_h = 22, maxrows = (h - 8) / row_h;
    int n = a->man.n; if (n > maxrows) n = maxrows;
    for (int i = 0; i < n; i++) {
        const Maneuver *m = &a->man.v[i];
        if (m->type == MAN_START) continue;
        char t[128];
        maneuver_text(m, t, sizeof(t));
        int ry = y + 4 + i * row_h;
        /* 同类指令用分隔感：奇数行浅底 */
        if (i & 1) rfill(rs, x + 2, ry, w - 4, row_h - 2, 238, 242, 248, 255);
        font_draw(a->font, rs, x + 8, ry, t, 36, 42, 56, 0);
    }
}

/* 多方案对比面板（右下角）。列出每条可行方案：方案名 / 预计时间 / 里程 /
 * 是否收费或走非铺装路，选中方案高亮。点击某行即切到该方案（chrome_act 派发）。 */
static void draw_alts(Raster *rs, App *a, const CLay *L)
{
    if (!a->have_alts || L->alts.w == 0) return;
    int nfeas = 0;
    for (int k = 0; k < a->ralts.n; k++)
        if (a->ralts.err[k] == 0) nfeas++;
    if (nfeas <= 1) return;

    int x = L->alts.x, y = L->alts.y, w = L->alts.w, h = L->alts.h;
    rfill(rs, x, y, w, h, 250, 251, 253, 248);
    rstroke(rs, x, y, w, h, 150, 158, 172, 255);
    if (a->font)
        font_draw(a->font, rs, x + 12, y + 4, "备选方案（点击切换 / 按 1-4）", 40, 46, 60, 0);

    int rowh = 34, ry = y + 22;
    for (int k = 0; k < a->ralts.n; k++) {
        if (a->ralts.err[k] != 0) continue;
        int sel = (k == a->sel_alt);
        if (sel) rfill(rs, x + 2, ry, w - 4, rowh - 2, 214, 228, 250, 255);
        else if (((k) & 1) == 0) rfill(rs, x + 2, ry, w - 4, rowh - 2, 240, 243, 248, 255);
        if (a->font) {
            int cy = ry + 4;
            font_draw(a->font, rs, x + 10, cy, a->ralts.tag[k], sel ? 24 : 36, 42, 56, 0);
            char buf[64];
            snprintf(buf, sizeof(buf), "约 %.0f 分 · %.1f 公里",
                     a->ralts.total_s[k] / 60.0, a->ralts.total_m[k] / 1000.0);
            font_draw(a->font, rs, x + 10, cy + 18, buf, 90, 96, 110, 0);
            /* 特征标记：收费 / 非铺装 */
            int mx = x + w - 12;
            if (a->ralts.unpaved_m[k] > 0) {
                font_draw(a->font, rs, mx - 72, cy + 4, "非铺装", 150, 90, 40, 0);
            }
            if (a->ralts.toll_m[k] > 0) {
                font_draw(a->font, rs, mx - 36, cy + 4, "收费", 180, 60, 50, 0);
            }
        }
        ry += rowh;
    }
}

static void draw_menu(Raster *rs, App *a, const CLay *L)
{
    if (!a->menu_open) return;
    int x = L->menu.x, y = L->menu.y, w = L->menu.w, h = L->menu.h;
    rfill(rs, x, y, w, h, 252, 253, 255, 252);
    rstroke(rs, x, y, w, h, 150, 158, 172, 255);
    if (!a->font) return;
    int n = chrome_menu_count();
    for (int i = 0; i < n; i++) {
        int ry = y + 4 + i * 30;
        if (i == a->chrome_menu_item)
            rfill(rs, x + 2, ry, w - 4, 28, 214, 228, 250, 255);
        font_draw(a->font, rs, x + 14, ry + 6, MENU_ITEMS[i], 40, 46, 60, 0);
    }
}

static void draw_nomap(Raster *rs, App *a, const CLay *L)
{
    (void)L;
    if (a->ms.n != 0) return;     /* 仅图集完全没加载时显示 */
    int w = 360, h = 96, x = (L->w - w)/2, y = (L->h - h)/2;
    rfill(rs, x, y, w, h, 252, 253, 255, 250);
    rstroke(rs, x, y, w, h, 230, 120, 90, 255);
    if (!a->font) return;
    font_draw(a->font, rs, x + 20, y + 16, "未加载地图数据", 200, 70, 50, 0);
    font_draw(a->font, rs, x + 20, y + 44,
              "把 *.osm.pbf 放进 sources/ 后运行", 90, 96, 110, 0);
    font_draw(a->font, rs, x + 20, y + 66,
              "scripts/sync_maps.sh", 90, 96, 110, 0);
}

static void draw_scale(Raster *rs, App *a, const CLay *L)
{
    /* 每像素米数（赤道周长 × cos(lat) / 像素/世界）。
       scale = 256 * 2^zoom 像素/世界；世界赤道周长 ≈ 40075016.686 m。 */
    double mpp = (40075016.686 * cos(a->view.lat * M_PI / 180.0)) /
                 (256.0 * pow(2.0, a->view.zoom));
    /* 取一个漂亮的整距离，使标尺约 110 px。任意数量级都成立：
       先定 10 的幂，再在 1/2/5 里挑一个不超过目标的倍数。 */
    double target = 110.0 * mpp;          /* 约 110px 对应的米数 */
    if (target <= 0) return;
    double step = pow(10.0, floor(log10(target)));
    double rem = target / step;
    double unit = (rem >= 5.0) ? 5.0 : (rem >= 2.0) ? 2.0 : 1.0;
    double dist = unit * step;            /* 最终标尺长度（米） */
    int px = (int)(dist / mpp);
    if (px < 8 || px > L->w - 40) return;
    int x = L->scale.x, y = L->scale.y;
    /* 标尺：一条底线 + 两端竖线 */
    rline(rs, x, y + 12, x + px, y + 12, 30, 36, 48);
    rline(rs, x, y + 6, x, y + 18, 30, 36, 48);
    rline(rs, x + px, y + 6, x + px, y + 18, 30, 36, 48);
    if (a->font) {
        char buf[32];
        if (dist >= 1000.0) snprintf(buf, sizeof(buf), "%.1f 公里", dist/1000.0);
        else                snprintf(buf, sizeof(buf), "%.0f 米", dist);
        font_draw(a->font, rs, x, y - 2, buf, 30, 36, 48, 0);
    }
}

static void draw_compass(Raster *rs, App *a, const CLay *L)
{
    if (a->view.rot == 0.0) return;
    int cx = L->compass.x + L->compass.w/2;
    int cy = L->compass.y + L->compass.h/2;
    int R = 18;
    /* 外圈 */
    uint32_t cc = raster_rgb(rs, 70, 80, 100);
    for (int t = 0; t < 360; t += 8)
        raster_point(rs, (int)(cx + R*cos(t*M_PI/180.0)),
                          (int)(cy + R*sin(t*M_PI/180.0)), cc);
    /* N 指针：指向屏幕角 rot（顺时针） */
    double ang = a->view.rot * M_PI / 180.0;
    int nx = (int)(cx + R * sin(ang));
    int ny = (int)(cy - R * cos(ang));
    rline(rs, cx, cy, nx, ny, 210, 60, 50);
    if (a->font) font_draw(a->font, rs, nx - 4, ny - 12, "N", 210, 60, 50, 0);
}

void paint_chrome(Raster *rs, App *a)
{
    if (!rs) return;
    CLay L; layout(a, &L);

    draw_titlebar(rs, a, &L);
    draw_topbar(rs, a, &L);
    draw_rcol(rs, a, &L);
    draw_navtop(rs, a, &L);
    draw_navbottom(rs, a, &L);
    draw_steps(rs, a, &L);
    draw_alts(rs, a, &L);
    draw_menu(rs, a, &L);
    draw_settings_panel(rs, a);
    draw_nomap(rs, a, &L);
    draw_scale(rs, a, &L);
    draw_compass(rs, a, &L);
}

/* ===================== 命中测试 ===================== */

int chrome_hit(App *a, int x, int y)
{
    CLay L; layout(a, &L);

    /* 设置面板优先：打开时所有点击归面板处理，吞掉地图平移 */
    if (a->settings_open) return settings_hit(a, x, y);

    if (IN(L.wclose, x, y)) return CH_WIN_CLOSE;
    if (IN(L.wmax,   x, y)) return CH_WIN_MAX;
    if (IN(L.wmin,   x, y)) return CH_WIN_MIN;

    if (a->menu_open && IN(L.menu, x, y)) {
        int idx = (y - L.menu.y - 4) / 30;
        if (idx < 0 || idx >= chrome_menu_count()) idx = 0;
        a->chrome_menu_item = idx;
        return CH_MENU_ITEM;
    }

    if (IN(L.menubtn, x, y)) return CH_MENU_OPEN;
    if (IN(L.tbrowse, x, y)) return CH_MODE_BROWSE;
    if (IN(L.tnav,    x, y)) return CH_MODE_NAV;
    if (IN(L.search,  x, y)) return CH_SEARCH;
    if (IN(L.zin,     x, y)) return CH_ZOOM_IN;
    if (IN(L.zout,    x, y)) return CH_ZOOM_OUT;
    if (IN(L.loc,     x, y)) return CH_LOCATE;
    if (IN(L.day,     x, y)) return CH_DAYNIGHT;
    if (IN(L.nback,   x, y)) return CH_NAV_BACK;
    if (IN(L.nex,     x, y)) return CH_NAV_EXIT;
    if (IN(L.compass, x, y)) return CH_COMPASS;

    /* 多方案对比面板：命中某行 → 返回 CH_ALT_BASE + 实际方案下标 k */
    if (a->have_alts && IN(L.alts, x, y)) {
        int row = (y - L.alts.y - 22) / 34;
        if (row >= 0) {
            int cnt = 0;
            for (int k = 0; k < a->ralts.n; k++) {
                if (a->ralts.err[k] != 0) continue;
                if (cnt == row) return CH_ALT_BASE + k;
                cnt++;
            }
        }
        return CH_NONE;
    }

    return CH_NONE;
}

/* ===================== 动作派发 ===================== */

/* 注：CH_WIN_MIN / CH_WIN_MAX 依赖 SDL 窗口句柄（a->win 已随核心去 SDL 化移除），
 * 改由 window_sdl.c 的事件循环直接处理，这里不再管。 */
void chrome_act(App *a, int id, int x, int y)
{
    (void)x; (void)y;
    switch (id) {
    case CH_WIN_CLOSE:
        a->quit = 1;
        break;
    case CH_MENU_OPEN:
        a->menu_open = !a->menu_open;
        a->need_redraw = 1;
        break;
    case CH_MODE_BROWSE:
        a->mode = 0; app_persist_settings(a);
        break;
    case CH_MODE_NAV:
        a->mode = 1; app_persist_settings(a);
        break;
    case CH_SEARCH:
        app_search_open(a);
        break;
    case CH_ZOOM_IN:
        mv_zoom_at(&a->view, 0.5, a->view.w/2, a->view.h/2);
        mv_clamp_zoom(&a->view, 3.0, 19.0);
        a->need_redraw = 1;
        break;
    case CH_ZOOM_OUT:
        mv_zoom_at(&a->view, -0.5, a->view.w/2, a->view.h/2);
        mv_clamp_zoom(&a->view, 3.0, 19.0);
        a->need_redraw = 1;
        break;
    case CH_LOCATE: {
        if (a->sim_on) { a->view.lat = a->sim.car_lat; a->view.lon = a->sim.car_lon; }
        else if (a->pick_a.have) { a->view.lat = a->pick_a.lat; a->view.lon = a->pick_a.lon; }
        a->need_redraw = 1;
        break;
    }
    case CH_DAYNIGHT:
        a->day_night = !a->day_night; app_persist_settings(a);
        break;
    case CH_NAV_BACK:
        app_nav_clear(a);
        break;
    case CH_NAV_EXIT:
        if (a->sim_on) app_nav_sim_toggle(a);
        app_nav_clear(a);
        break;
    case CH_COMPASS:
        a->view.rot = 0.0; a->need_redraw = 1;
        break;
    default:
        /* 多方案对比：CH_ALT_BASE + k → 切到第 k 条 */
        if (id >= CH_ALT_BASE && id < CH_ALT_BASE + ROUTE_ALT_K) {
            app_nav_select_alt(a, id - CH_ALT_BASE);
        }
        break;
    case CH_MENU_ITEM:
        switch (a->chrome_menu_item) {
        case 0: a->day_night = !a->day_night; app_persist_settings(a); break;
        case 1: app_search_open(a); break;
        case 2: app_nav_clear(a); break;
        case 3: app_nav_fit(a); break;
        case 4: app_nav_sim_follow(a); break;
        case 5: a->settings_open = 1; break;
        case 6: a->quit = 1; break;
        }
        a->menu_open = 0;
        a->need_redraw = 1;
        break;
    case CH_SETTINGS:
        chrome_act_settings(a);
        break;
    }
}

/* ===================== 设置面板（完整设置系统） ===================== */

/* 面板整体矩形 */
static void settings_panel_rect(const App *a, RectI *panel)
{
    int W = 380;
    int H = 40 + SETTINGS_SCHEMA_N * 34 + 52;
    panel->x = (a->view.w - W) / 2;
    panel->y = (a->view.h - H) / 2;
    panel->w = W; panel->h = H;
}

/* 第 i 行矩形（0..N-1） */
static void settings_row_rect(const RectI *panel, int i, RectI *row)
{
    row->x = panel->x + 12;
    row->y = panel->y + 40 + i * 34;
    row->w = panel->w - 24;
    row->h = 28;
}

/* 行内控件矩形：bool 返回两等宽格（左=关/右=开）；int 返回 减/加 两格。
 * 绘制与命中测试共用，保证一致。第三个返回值含义：l=左(0)/减，r=右(1)/加。 */
static void settings_control_rects(const RectI *row, int is_bool, RectI *l, RectI *r)
{
    int vx = row->x + row->w - 120, vy = row->y + 2, vw = 108, vh = 24;
    if (is_bool) {
        l->x = vx;         l->y = vy; l->w = vw / 2; l->h = vh;
        r->x = vx + vw/2;  r->y = vy; r->w = vw / 2; r->h = vh;
    } else {
        l->x = vx;          l->y = vy; l->w = 28; l->h = vh;
        r->x = vx + vw - 28; r->y = vy; r->w = 28; r->h = vh;
    }
}

/* 底部按钮：重置全部（左）/ 关闭（右） */
static void settings_footer_rects(const RectI *panel, RectI *reset, RectI *close)
{
    int fy = panel->y + panel->h - 40;
    reset->x = panel->x + 12;  reset->y = fy;
    reset->w = (panel->w - 36) / 2; reset->h = 30;
    close->x = reset->x + reset->w + 12; close->y = fy;
    close->w = reset->w; close->h = 30;
}

static void draw_settings_panel(Raster *rs, App *a)
{
    if (!a->settings_open || !a->cfg) return;
    RectI panel; settings_panel_rect(a, &panel);
    rfill(rs, panel.x, panel.y, panel.w, panel.h, 250, 251, 253, 252);
    rstroke(rs, panel.x, panel.y, panel.w, panel.h, 120, 128, 144, 255);
    if (!a->font) return;
    font_draw(a->font, rs, panel.x + 14, panel.y + 12, "设置", 30, 36, 48, 0);

    for (int i = 0; i < SETTINGS_SCHEMA_N; i++) {
        const SettingMeta *m = &SETTINGS_SCHEMA[i];
        RectI row; settings_row_rect(&panel, i, &row);
        int ry = row.y + 6;
        font_draw(a->font, rs, row.x + 4, ry, m->label, 40, 46, 60, 0);

        RectI l, r; settings_control_rects(&row, m->type == ST_BOOL, &l, &r);
        if (m->type == ST_BOOL) {
            int on = settings_get_bool(a->cfg, m->key, (int)m->def);
            rfill(rs, l.x, l.y, l.w, l.h, on ? 200 : 96, on ? 205 : 150, on ? 215 : 160, 255);
            rfill(rs, r.x, r.y, r.w, r.h, on ? 96 : 200, on ? 150 : 205, on ? 160 : 215, 255);
            font_draw(a->font, rs, l.x + l.w/2 - 8,  l.y + 5, "关", on?140:255, on?140:255, on?140:255, 0);
            font_draw(a->font, rs, r.x + r.w/2 - 8,  r.y + 5, "开", on?255:140, on?255:140, on?255:140, 0);
        } else {
            int cur = settings_get_int(a->cfg, m->key, (int)m->def);
            rfill(rs, l.x, l.y, l.w, l.h, 150, 160, 175, 255);
            rfill(rs, r.x, r.y, r.w, r.h, 150, 160, 175, 255);
            font_draw(a->font, rs, l.x + 9, l.y + 4, "-", 40, 40, 40, 0);
            font_draw(a->font, rs, r.x + 8, r.y + 4, "+", 40, 40, 40, 0);
            char val[64];
            if (m->opts) snprintf(val, sizeof(val), "%s",
                                  m->opts + (cur ? (int)strlen(m->opts) + 1 : 0));
            else         snprintf(val, sizeof(val), "%d", cur);
            font_draw(a->font, rs, l.x + l.w + 8, ry, val, 40, 46, 60, 0);
        }
    }

    RectI reset, close; settings_footer_rects(&panel, &reset, &close);
    rfill(rs, reset.x, reset.y, reset.w, reset.h, 220, 90, 80, 255);
    rfill(rs, close.x, close.y, close.w, close.h, 90, 130, 210, 255);
    font_draw(a->font, rs, reset.x + reset.w/2 - 24, reset.y + 7, "重置全部", 255, 255, 255, 0);
    font_draw(a->font, rs, close.x + close.w/2 - 16, close.y + 7, "关闭",    255, 255, 255, 0);
}

/* 设置面板命中：settings_open 时优先处理面板，吞掉地图平移。 */
static int settings_hit(App *a, int x, int y)
{
    if (!a->settings_open) return CH_NONE;
    RectI panel; settings_panel_rect(a, &panel);
    if (!IN(panel, x, y)) {
        /* 点在面板外：关闭面板并吞掉这次点击（不当成地图操作） */
        a->settings_row = -1; a->settings_btn = 0;
        return CH_SETTINGS;
    }
    RectI reset, close; settings_footer_rects(&panel, &reset, &close);
    if (IN(reset, x, y)) { a->settings_row = -2; a->settings_btn = 0; return CH_SETTINGS; }
    if (IN(close, x, y)) { a->settings_row = -1; a->settings_btn = 0; return CH_SETTINGS; }
    for (int i = 0; i < SETTINGS_SCHEMA_N; i++) {
        RectI row; settings_row_rect(&panel, i, &row);
        if (IN(row, x, y)) {
            RectI l, r; settings_control_rects(&row, SETTINGS_SCHEMA[i].type == ST_BOOL, &l, &r);
            a->settings_row = i;
            if (IN(l, x, y))      a->settings_btn = 0;
            else if (IN(r, x, y)) a->settings_btn = 1;
            else                  a->settings_btn = 2;
            return CH_SETTINGS;
        }
    }
    a->settings_row = -1; a->settings_btn = 0;   /* 面板内空白处：吞掉 */
    return CH_SETTINGS;
}

/* 处理一次设置面板点击（由 chrome_act 派发） */
void chrome_act_settings(App *a)
{
    if (!a->cfg) { a->settings_open = 0; a->need_redraw = 1; return; }
    if (a->settings_row >= 0 && a->settings_row < SETTINGS_SCHEMA_N) {
        const SettingMeta *m = &SETTINGS_SCHEMA[a->settings_row];
        if (m->type == ST_BOOL) {
            int on = settings_get_bool(a->cfg, m->key, (int)m->def);
            int v = (a->settings_btn == 0) ? 0
                  : (a->settings_btn == 1) ? 1
                  : (on ? 0 : 1);
            settings_set_bool(a->cfg, m->key, v);
        } else { /* ST_INT */
            double cur = settings_get_dbl(a->cfg, m->key, m->def);
            if (a->settings_btn == 0)      cur -= m->step;
            else if (a->settings_btn == 1) cur += m->step;
            else { /* 行体点击：枚举切换（wrap）/ 否则 +step */
                if (m->opts) cur = (cur >= m->max) ? m->min : cur + m->step;
                else         cur += m->step;
            }
            if (cur < m->min) cur = m->min;
            if (cur > m->max) cur = m->max;
            settings_set_int(a->cfg, m->key, (int)(cur + 0.5));
        }
        app_apply_settings(a);
        settings_save(a->cfg);
        a->need_redraw = 1;
    } else if (a->settings_row == -2) {
        settings_reset_all(a->cfg);
        app_apply_settings(a);
        settings_save(a->cfg);
        a->need_redraw = 1;
    } else { /* -1 或越界：关闭面板 */
        a->settings_open = 0;
        a->need_redraw = 1;
    }
}
