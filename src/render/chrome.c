#include "render/chrome.h"

#include <math.h>
#include <string.h>

#include "render/font.h"
#include "render/navdraw.h"
#include "route/maneuver.h"
#include "map/coverage.h"

/* ===================== 几何：绘制与命中测试共用 ===================== */

#define TH_TITLE CHROME_TITLE_H   /* 自绘标题栏高度 */
#define TH_TOP   46               /* 顶栏高度 */
#define COL_W    42      /* 右栏按钮宽度 */
#define BTN      40      /* 右栏/按钮边长 */
#define GAP      8

typedef struct {
    int w, h;
    SDL_Rect title, wmin, wmax, wclose;
    SDL_Rect topbar, menubtn, search, tbrowse, tnav;
    SDL_Rect zin, zout, loc, day;
    SDL_Rect navtop, nback, nex;
    SDL_Rect steps;
    SDL_Rect scale;
    SDL_Rect compass;
    SDL_Rect menu;
} CLay;

#define IN(r,x,y) ((x) >= (r).x && (x) < (r).x + (r).w && \
                   (y) >= (r).y && (y) < (r).y + (r).h)

static void layout(App *a, CLay *L)
{
    int w = a->view.w, h = a->view.h;
    L->w = w; L->h = h;

    L->title  = (SDL_Rect){ 0, 0, w, TH_TITLE };
    int by = (TH_TITLE - 22) / 2;
    L->wclose = (SDL_Rect){ w - 30, by, 22, 22 };
    L->wmax   = (SDL_Rect){ w - 58, by, 22, 22 };
    L->wmin   = (SDL_Rect){ w - 86, by, 22, 22 };

    L->topbar = (SDL_Rect){ 0, TH_TITLE, w, TH_TOP };
    L->menubtn = (SDL_Rect){ 8, TH_TITLE + 8, 30, TH_TOP - 16 };
    L->tbrowse = (SDL_Rect){ w - 160, TH_TITLE + 10, 76, TH_TOP - 20 };
    L->tnav    = (SDL_Rect){ w - 80,  TH_TITLE + 10, 76, TH_TOP - 20 };
    int sw = w - 44 - 168; if (sw < 120) sw = 120;
    L->search = (SDL_Rect){ 44, TH_TITLE + 8, sw, TH_TOP - 16 };

    int rx = w - COL_W - 8, ry0 = TH_TITLE + TH_TOP + 12;
    L->zin  = (SDL_Rect){ rx, ry0,                  COL_W, BTN };
    L->zout = (SDL_Rect){ rx, ry0 + (BTN + GAP),    COL_W, BTN };
    L->loc  = (SDL_Rect){ rx, ry0 + 2*(BTN + GAP),  COL_W, BTN };
    L->day  = (SDL_Rect){ rx, ry0 + 3*(BTN + GAP),  COL_W, BTN };

    int ny = TH_TITLE + TH_TOP;
    if (a->have_rr) {
        L->navtop = (SDL_Rect){ 0, ny, w, 36 };
        L->nback  = (SDL_Rect){ 8,  ny + 6, 24, 24 };
        L->nex    = (SDL_Rect){ w - 34, ny + 6, 26, 24 };
        ny += 36 + 8;
    } else {
        L->navtop = (SDL_Rect){ 0, 0, 0, 0 };
        L->nback  = (SDL_Rect){ 0, 0, 0, 0 };
        L->nex    = (SDL_Rect){ 0, 0, 0, 0 };
    }

    int ph = h - ny - 70;
    if (ph < 80) ph = 80;
    if (ph > 360) ph = 360;
    L->steps = (SDL_Rect){ 12, ny, 250, ph };

    L->scale = (SDL_Rect){ 12, h - 34, 150, 22 };

    if (a->view.rot != 0.0)
        L->compass = (SDL_Rect){ w - 56, TH_TITLE + TH_TOP + 12, 44, 44 };
    else
        L->compass = (SDL_Rect){ 0, 0, 0, 0 };

    int mc = chrome_menu_count();
    L->menu = (SDL_Rect){ 8, TH_TITLE + 8, 200, mc * 30 + 8 };
}

/* ===================== 小工具 ===================== */

static void rfill(SDL_Renderer *r, int x, int y, int w, int h,
                  Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_Rect q = { x, y, w, h };
    SDL_RenderFillRect(r, &q);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}
static void rstroke(SDL_Renderer *r, int x, int y, int w, int h,
                    Uint8 cr, Uint8 cg, Uint8 cb, Uint8 ca)
{
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    SDL_Rect q = { x, y, w, h };
    SDL_RenderDrawRect(r, &q);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
}
static void rline(SDL_Renderer *r, int x0, int y0, int x1, int y1,
                  Uint8 cr, Uint8 cg, Uint8 cb)
{
    SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    SDL_RenderDrawLine(r, x0, y0, x1, y1);
}

/* ===================== 夜间蒙版 ===================== */

void paint_night_tint(SDL_Renderer *r, int w, int h)
{
    rfill(r, 0, 0, w, h, 8, 14, 34, 96);
}

/* ===================== 主菜单条目 ===================== */

static const char *MENU_ITEMS[] = {
    "昼 / 夜 切换",
    "搜索地点",
    "清除路线",
    "缩放到路线",
    "跟随车头",
    "退出",
};
int chrome_menu_count(void) { return (int)(sizeof(MENU_ITEMS)/sizeof(MENU_ITEMS[0])); }
const char *chrome_menu_label(int i)
{
    if (i < 0 || i >= chrome_menu_count()) return "";
    return MENU_ITEMS[i];
}

/* ===================== 绘制：各元素 ===================== */

static void draw_titlebar(SDL_Renderer *r, App *a, const CLay *L)
{
    rfill(r, L->title.x, L->title.y, L->title.w, L->title.h, 28, 36, 56, 255);
    if (a->font) font_draw(a->font, r, 10, L->title.y + 6, "NavMap", 235, 240, 250, 0);
    /* 三个窗口按钮：红/黄/绿 小圆点（mac 风，跨平台通用） */
    const SDL_Rect *bs[3] = { &L->wclose, &L->wmax, &L->wmin };
    Uint8 col[3][3] = { {233,84,72}, {240,192,72}, {76,200,96} };
    for (int i = 0; i < 3; i++) {
        int cx = bs[i]->x + bs[i]->w/2, cy = bs[i]->y + bs[i]->h/2;
        rfill(r, cx-7, cy-7, 14, 14, col[i][0], col[i][1], col[i][2], 255);
    }
}

static void draw_topbar(SDL_Renderer *r, App *a, const CLay *L)
{
    rfill(r, L->topbar.x, L->topbar.y, L->topbar.w, L->topbar.h, 245, 247, 250, 250);
    rstroke(r, L->topbar.x, L->topbar.y + L->topbar.h - 1, L->topbar.w, 1, 200, 206, 214, 255);

    /* 菜单按钮（☰） */
    rfill(r, L->menubtn.x, L->menubtn.y, L->menubtn.w, L->menubtn.h, 60, 110, 220, 255);
    SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
    for (int i = 0; i < 3; i++)
        SDL_RenderDrawLine(r, L->menubtn.x + 7, L->menubtn.y + 9 + i*8,
                              L->menubtn.x + 23, L->menubtn.y + 9 + i*8);

    /* 搜索框（占位；活跃时由 paint_search_box 的大框盖在上面） */
    if (!a->su.active) {
        rfill(r, L->search.x, L->search.y, L->search.w, L->search.h, 255, 255, 255, 255);
        rstroke(r, L->search.x, L->search.y, L->search.w, L->search.h, 180, 188, 200, 255);
        if (a->font)
            font_draw(a->font, r, L->search.x + 10, L->search.y + 8,
                      "搜索地点、道路…", 150, 156, 168, 0);
    }

    /* 模式页签 */
    int navOn = a->have_rr;
    SDL_Rect tabs[2] = { L->tbrowse, L->tnav };
    const char *lab[2] = { "浏览", "导航" };
    int on[2] = { !navOn, navOn };
    for (int i = 0; i < 2; i++) {
        if (on[i]) rfill(r, tabs[i].x, tabs[i].y, tabs[i].w, tabs[i].h, 60, 110, 220, 255);
        else       rstroke(r, tabs[i].x, tabs[i].y, tabs[i].w, tabs[i].h, 150, 160, 178, 255);
        if (a->font)
            font_draw(a->font, r, tabs[i].x + 22, tabs[i].y + 9, lab[i],
                      on[i] ? 255 : 90, on[i] ? 255 : 100, on[i] ? 255 : 120, 0);
    }
}

static void draw_rcol(SDL_Renderer *r, App *a, const CLay *L)
{
    SDL_Rect bs[4] = { L->zin, L->zout, L->loc, L->day };
    for (int i = 0; i < 4; i++)
        rfill(r, bs[i].x, bs[i].y, bs[i].w, bs[i].h, 250, 251, 253, 245);
    rstroke(r, L->zin.x,  L->zin.y,  L->zin.w,  L->zin.h,  150, 158, 172, 255);
    rstroke(r, L->zout.x, L->zout.y, L->zout.w, L->zout.h, 150, 158, 172, 255);
    rstroke(r, L->loc.x,  L->loc.y,  L->loc.w,  L->loc.h,  150, 158, 172, 255);
    rstroke(r, L->day.x,  L->day.y,  L->day.w,  L->day.h,  150, 158, 172, 255);

    /* ＋ / − */
    rline(r, L->zin.x + 10,  L->zin.y + BTN/2,  L->zin.x + BTN-10, L->zin.y + BTN/2, 40, 50, 70);
    rline(r, L->zin.x + BTN/2, L->zin.y + 10,  L->zin.x + BTN/2,  L->zin.y + BTN-10, 40, 50, 70);
    rline(r, L->zout.x + 10, L->zout.y + BTN/2, L->zout.x + BTN-10, L->zout.y + BTN/2, 40, 50, 70);
    /* 定位：圆环 + 中心点 */
    SDL_SetRenderDrawColor(r, 40, 50, 70, 255);
    int lcx = L->loc.x + BTN/2, lcy = L->loc.y + BTN/2;
    for (int t = 0; t < 360; t += 12)
        SDL_RenderDrawPoint(r, (int)(lcx + 11*cos(t*M_PI/180.0)),
                               (int)(lcy + 11*sin(t*M_PI/180.0)));
    rfill(r, lcx-3, lcy-3, 6, 6, 40, 50, 70, 255);
    /* 昼夜：日(黄)/夜(蓝) 圆 */
    if (a->day_night) rfill(r, L->day.x + BTN/2 - 11, L->day.y + BTN/2 - 11, 22, 22, 60, 90, 200, 255);
    else              rfill(r, L->day.x + BTN/2 - 11, L->day.y + BTN/2 - 11, 22, 22, 250, 200, 60, 255);
}

static void draw_navtop(SDL_Renderer *r, App *a, const CLay *L)
{
    if (!a->have_rr) return;
    rfill(r, L->navtop.x, L->navtop.y, L->navtop.w, L->navtop.h, 30, 90, 200, 255);
    rfill(r, L->nback.x, L->nback.y, L->nback.w, L->nback.h, 255, 255, 255, 255);
    rline(r, L->nback.x + 16, L->nback.y + 4, L->nback.x + 6, L->nback.y + 12, 30, 90, 200);
    rline(r, L->nback.x + 6, L->nback.y + 12, L->nback.x + 16, L->nback.y + 20, 30, 90, 200);
    rline(r, L->nex.x + 6, L->nex.y + 6, L->nex.x + 20, L->nex.y + 18, 255, 255, 255);
    rline(r, L->nex.x + 20, L->nex.y + 6, L->nex.x + 6, L->nex.y + 18, 255, 255, 255);
    if (a->font) {
        char dst[64];
        if (a->pick_b.have)
            snprintf(dst, sizeof(dst), "到 终点 (%.4f, %.4f)", a->pick_b.lat, a->pick_b.lon);
        else
            snprintf(dst, sizeof(dst), "导航中");
        font_draw(a->font, r, L->nex.x + 34, L->navtop.y + 9, dst, 255, 255, 255, 0);
    }
}

static void draw_navbottom(SDL_Renderer *r, App *a, const CLay *L)
{
    /* 偏航重算中 → 顶部弹出"重新规划中…" */
    if (!(a->sim_on && a->sim_paused && a->nav_busy && a->have_rr)) return;
    if (!a->font) return;
    int bw = 200, bh = 30, bx = (L->w - bw)/2, by = TH_TITLE + TH_TOP + 6;
    rfill(r, bx, by, bw, bh, 232, 150, 40, 240);
    font_draw(a->font, r, bx + 30, by + 6, "重新规划中…", 255, 255, 255, 0);
}

static void draw_steps(SDL_Renderer *r, App *a, const CLay *L)
{
    if (!a->have_rr || a->man.n == 0) return;
    int x = L->steps.x, y = L->steps.y, w = L->steps.w, h = L->steps.h;
    rfill(r, x, y, w, h, 250, 251, 253, 246);
    rstroke(r, x, y, w, h, 150, 158, 172, 255);
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
        if (i & 1) rfill(r, x + 2, ry, w - 4, row_h - 2, 238, 242, 248, 255);
        font_draw(a->font, r, x + 8, ry, t, 36, 42, 56, 0);
    }
}

static void draw_menu(SDL_Renderer *r, App *a, const CLay *L)
{
    if (!a->menu_open) return;
    int x = L->menu.x, y = L->menu.y, w = L->menu.w, h = L->menu.h;
    rfill(r, x, y, w, h, 252, 253, 255, 252);
    rstroke(r, x, y, w, h, 150, 158, 172, 255);
    if (!a->font) return;
    int n = chrome_menu_count();
    for (int i = 0; i < n; i++) {
        int ry = y + 4 + i * 30;
        if (i == a->chrome_menu_item)
            rfill(r, x + 2, ry, w - 4, 28, 214, 228, 250, 255);
        font_draw(a->font, r, x + 14, ry + 6, MENU_ITEMS[i], 40, 46, 60, 0);
    }
}

static void draw_nomap(SDL_Renderer *r, App *a, const CLay *L)
{
    if (a->ms.n != 0) return;     /* 仅图集完全没加载时显示 */
    int w = 360, h = 96, x = (L->w - w)/2, y = (L->h - h)/2;
    rfill(r, x, y, w, h, 252, 253, 255, 250);
    rstroke(r, x, y, w, h, 230, 120, 90, 255);
    if (!a->font) return;
    font_draw(a->font, r, x + 20, y + 16, "未加载地图数据", 200, 70, 50, 0);
    font_draw(a->font, r, x + 20, y + 44,
              "把 *.osm.pbf 放进 sources/ 后运行", 90, 96, 110, 0);
    font_draw(a->font, r, x + 20, y + 66,
              "scripts/sync_maps.sh", 90, 96, 110, 0);
}

static void draw_scale(SDL_Renderer *r, App *a, const CLay *L)
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
    rline(r, x, y + 12, x + px, y + 12, 30, 36, 48);
    rline(r, x, y + 6, x, y + 18, 30, 36, 48);
    rline(r, x + px, y + 6, x + px, y + 18, 30, 36, 48);
    if (a->font) {
        char buf[32];
        if (dist >= 1000.0) snprintf(buf, sizeof(buf), "%.1f 公里", dist/1000.0);
        else                snprintf(buf, sizeof(buf), "%.0f 米", dist);
        font_draw(a->font, r, x, y - 2, buf, 30, 36, 48, 0);
    }
}

static void draw_compass(SDL_Renderer *r, App *a, const CLay *L)
{
    if (a->view.rot == 0.0) return;
    int cx = L->compass.x + L->compass.w/2;
    int cy = L->compass.y + L->compass.h/2;
    int R = 18;
    /* 外圈 */
    SDL_SetRenderDrawColor(r, 70, 80, 100, 255);
    for (int t = 0; t < 360; t += 8)
        SDL_RenderDrawPoint(r, (int)(cx + R*cos(t*M_PI/180.0)),
                               (int)(cy + R*sin(t*M_PI/180.0)));
    /* N 指针：指向屏幕角 rot（顺时针） */
    double ang = a->view.rot * M_PI / 180.0;
    int nx = (int)(cx + R * sin(ang));
    int ny = (int)(cy - R * cos(ang));
    rline(r, cx, cy, nx, ny, 210, 60, 50);
    if (a->font) font_draw(a->font, r, nx - 4, ny - 12, "N", 210, 60, 50, 0);
}

void paint_chrome(SDL_Renderer *r, App *a)
{
    if (!r) return;
    CLay L; layout(a, &L);

    draw_titlebar(r, a, &L);
    draw_topbar(r, a, &L);
    draw_rcol(r, a, &L);
    draw_navtop(r, a, &L);
    draw_navbottom(r, a, &L);
    draw_steps(r, a, &L);
    draw_menu(r, a, &L);
    draw_nomap(r, a, &L);
    draw_scale(r, a, &L);
    draw_compass(r, a, &L);
}

/* ===================== 命中测试 ===================== */

int chrome_hit(App *a, int x, int y)
{
    CLay L; layout(a, &L);

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

    return CH_NONE;
}

/* ===================== 动作派发 ===================== */

void chrome_act(App *a, int id, int x, int y)
{
    (void)x; (void)y;
    switch (id) {
    case CH_WIN_MIN:
        if (a->win) SDL_MinimizeWindow(a->win);
        break;
    case CH_WIN_MAX:
        if (a->win) SDL_MaximizeWindow(a->win);
        break;
    case CH_WIN_CLOSE:
        a->quit = 1;
        break;
    case CH_MENU_OPEN:
        a->menu_open = !a->menu_open;
        a->need_redraw = 1;
        break;
    case CH_MODE_BROWSE:
        a->mode = 0; a->need_redraw = 1;
        break;
    case CH_MODE_NAV:
        a->mode = 1; a->need_redraw = 1;
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
        a->day_night = !a->day_night; a->need_redraw = 1;
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
    case CH_MENU_ITEM:
        switch (a->chrome_menu_item) {
        case 0: a->day_night = !a->day_night; break;
        case 1: app_search_open(a); break;
        case 2: app_nav_clear(a); break;
        case 3: app_nav_fit(a); break;
        case 4: app_nav_sim_follow(a); break;
        case 5: a->quit = 1; break;
        }
        a->menu_open = 0;
        a->need_redraw = 1;
        break;
    default:
        break;
    }
}
