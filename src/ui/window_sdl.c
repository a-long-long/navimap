/* window_sdl.c —— SDL 窗口前端（**唯一**直接碰 SDL 的界面文件）。
 *
 * 为什么单独一个文件：navimap_core 必须做到"零 SDL"（Qt 前端不链 SDL），
 * 而窗口/事件/离屏画布这些天然属于 SDL。把它们从 window.c 里搬到这里，
 * 边界就清楚了：
 *   - window.c       平台中立的业务层：状态、算路调度、绘制编排（paint_frame）
 *   - window_sdl.c   SDL 适配层：窗口、事件循环、离屏画布、贴图上传
 *   - ui_qt/         Qt 适配层：真控件 + 自持 QImage 画布
 *
 * 三者共用同一套绘制（app_render_to_buf → paint_frame），不重复渲染代码。
 *
 * 离屏画布为什么还要 surface：面/路/线直接写像素，需要一块能拿到 pixels 的
 * 内存，而 SDL 的窗口渲染器不暴露它的 surface。所以统一成：先在**离屏
 * surface** 上画完（整帧像素），再把整张 surface 上传成纹理贴到窗口。
 * 代价是每帧多一次 1280×800×4 的上传（实测 ~2.6 ms），换来的是窗口版与
 * map_shot 走**同一套** paint_frame，不再有两份渲染代码。
 *
 * ★ 像素格式一律从 surface 的 format 里读（Rmask/Gmask/Bmask/Amask），
 *   不写死：写死就会出现"某台机器上红蓝互换"这种只在别人机器上复现的 bug。
 */
#include "ui/window.h"

#include "render/chrome.h"
#include "render/paint.h"
#include "map/map_view.h"
#include "utils/plat.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* ---- SDL 侧的私有状态 ----
 * 这些不进 App：App 已经是平台中立的（Qt 前端不链 SDL，App 里放 SDL 句柄
 * 就等于把 SDL 头文件拖回核心）。SDL 窗口版全局只有一份，所以放文件静态。 */
static SDL_Window   *g_win     = NULL;
static SDL_Renderer *g_win_ren = NULL;
static SDL_Texture  *g_tex     = NULL;
static SDL_Surface  *g_surf    = NULL;   /* 离屏像素缓冲 */

/* ---- 离屏画布 ----
 * 尺寸变了（resize）就重建 surface + 纹理。纹理格式由 surface 的 mask 推导，
 * 保证 UpdateTexture 的源格式与纹理格式一致（否则上传后颜色错位）。 */
static int sdl_ensure_canvas(int w, int h)
{
    if (g_surf && g_surf->w == w && g_surf->h == h && g_tex) return 0;
    if (w <= 0 || h <= 0) return -1;

    if (g_tex) { SDL_DestroyTexture(g_tex); g_tex = NULL; }
    if (g_surf) { SDL_FreeSurface(g_surf); g_surf = NULL; }

    g_surf = SDL_CreateRGBSurface(0, w, h, 32, 0, 0, 0, 0);
    if (!g_surf) return -1;
    if (g_win_ren) {
        Uint32 fmt = SDL_MasksToPixelFormatEnum(g_surf->format->BitsPerPixel,
                                                g_surf->format->Rmask,
                                                g_surf->format->Gmask,
                                                g_surf->format->Bmask,
                                                g_surf->format->Amask);
        g_tex = SDL_CreateTexture(g_win_ren, fmt,
                                  SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!g_tex) { SDL_FreeSurface(g_surf); g_surf = NULL; return -1; }
    }
    return 0;
}

/* 窗口的最小化/最大化是 **SDL 专属动作**（Qt 有自己的窗口按钮，根本不会
 * 命中 chrome 的自绘标题栏），所以只能在这里做。
 *
 * ★ 为什么不在 chrome_act 里做：chrome.c 是平台中立的业务层，a->win 已随
 *   核心去 SDL 化移除；把 SDL_MinimizeWindow 放回去就等于把 SDL 拖回核心。
 *   chrome_hit 仍然返回 CH_WIN_MIN/CH_WIN_MAX（命中判定是纯几何，与平台
 *   无关），由本层在派发前截住。 */
static int sdl_window_action(int id)
{
    if (!g_win) return 0;
    if (id == CH_WIN_MIN) { SDL_MinimizeWindow(g_win); return 1; }
    if (id == CH_WIN_MAX) { SDL_MaximizeWindow(g_win); return 1; }
    return 0;
}

/* 把一帧画进离屏 surface。像素格式全部取自 surface 自己的 format。 */
static int sdl_render_frame(App *a, PaintStats *st)
{
    if (!g_surf) return -1;
    return app_render_to_buf(a, g_surf->pixels, g_surf->w, g_surf->h,
                              (size_t)(g_surf->pitch / 4),
                              g_surf->format->Rmask, g_surf->format->Gmask,
                              g_surf->format->Bmask, g_surf->format->Amask,
                              st);
}

void app_run(App *a)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init 失败: %s\n", SDL_GetError());
        return;
    }
    g_win = SDL_CreateWindow("NavMap",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             a->view.w, a->view.h,
                             SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE |
                             SDL_WINDOW_BORDERLESS);
    if (!g_win) { fprintf(stderr, "建窗口失败: %s\n", SDL_GetError()); SDL_Quit(); return; }
    g_win_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_win_ren) { fprintf(stderr, "建窗口渲染器失败: %s\n", SDL_GetError()); SDL_Quit(); return; }
    if (sdl_ensure_canvas(a->view.w, a->view.h) != 0) {
        fprintf(stderr, "建离屏画布失败: %s\n", SDL_GetError()); SDL_Quit(); return;
    }

    /* 输入法开关跟着搜索框走：SDL 只在 StartTextInput 之后才发 TEXTINPUT。
     * 跟踪状态变化来开关（开/关搜索框在核心层，那里没有 SDL）。 */
    int text_input_on = 0;

    SDL_Event e;
    while (!a->quit) {
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: a->quit = 1; break;
            case SDL_TEXTINPUT:
                /* 输入法最终上屏的文本（中文一次一个字或一串） */
                app_search_input(a, e.text.text);
                break;
            case SDL_TEXTEDITING:
                /* 正在组合的串（拼音阶段）：只显示，不参与查询 */
                app_search_editing(a, e.edit.text);
                break;
            case SDL_KEYDOWN:
                /* ★ 搜索框打开时，按键优先归搜索：否则打字会顺手触发
                   g/t/n/c/f 这些地图快捷键（'s' 既是搜索又是别的就更乱）。 */
                if (a->su.active) {
                    SDL_Keycode k = e.key.keysym.sym;
                    if (k == SDLK_ESCAPE)               app_search_close(a);
                    else if (k == SDLK_RETURN || k == SDLK_KP_ENTER)
                                                        app_search_commit(a);
                    else if (k == SDLK_BACKSPACE)       app_search_backspace(a);
                    else if (k == SDLK_DOWN)            app_search_move(a, 1);
                    else if (k == SDLK_UP)              app_search_move(a, -1);
                    break;
                }
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    if (a->menu_open) { a->menu_open = 0; a->need_redraw = 1; }
                    else if (a->su.active) app_search_close(a);
                    else if (a->settings_open) { a->settings_open = 0; a->need_redraw = 1; }
                    else a->quit = 1;
                }
                else if (e.key.keysym.sym == SDLK_q) a->quit = 1;
                else if (e.key.keysym.sym == SDLK_d) {
                    a->day_night = !a->day_night; app_persist_settings(a);
                }
                else if (e.key.keysym.sym == SDLK_EQUALS || e.key.keysym.sym == SDLK_KP_PLUS)
                    mv_zoom_at(&a->view, 0.5, a->view.w / 2, a->view.h / 2), a->need_redraw = 1;
                else if (e.key.keysym.sym == SDLK_MINUS || e.key.keysym.sym == SDLK_KP_MINUS)
                    mv_zoom_at(&a->view, -0.5, a->view.w / 2, a->view.h / 2), a->need_redraw = 1;
                else if (a->have_alts && e.key.keysym.sym >= SDLK_1 &&
                         e.key.keysym.sym <= SDLK_4) {
                    /* 有路线时：1-4 直接切到对应方案 */
                    int k = (int)(e.key.keysym.sym - SDLK_1);
                    if (k < a->ralts.n && a->ralts.err[k] == 0)
                        app_nav_select_alt(a, k);
                }
                else if (e.key.keysym.sym >= SDLK_0 && e.key.keysym.sym <= SDLK_4) {
                    a->max_class = e.key.keysym.sym - SDLK_0;
                    a->need_redraw = 1;
                }
                /* 字号：[ 减小 / ] 增大（+/- 已给缩放用了） */
                else if (e.key.keysym.sym == SDLK_LEFTBRACKET) {
                    app_set_font_size(a, a->font_size - 1);
                    app_persist_settings(a);
                    a->need_redraw = 1;
                }                 else if (e.key.keysym.sym == SDLK_RIGHTBRACKET) {
                    app_set_font_size(a, a->font_size + 1);
                    app_persist_settings(a);
                    a->need_redraw = 1;
                }
                /* 导航：c 清除 / f 缩放到路线 / p 开关面板 */
                else if (e.key.keysym.sym == SDLK_c) app_nav_clear(a);
                else if (e.key.keysym.sym == SDLK_f) app_nav_fit(a);
                else if (e.key.keysym.sym == SDLK_p) app_nav_toggle_panel(a);
                /* 搜索：/ 或 s 打开搜索框 */
                else if (e.key.keysym.sym == SDLK_SLASH ||
                         e.key.keysym.sym == SDLK_s) app_search_open(a);
                /* 模拟导航：g 开关 / t 跟随 / n 注入偏航 / . , 倍速 */
                else if (e.key.keysym.sym == SDLK_g) app_nav_sim_toggle(a);
                else if (e.key.keysym.sym == SDLK_t) app_nav_sim_follow(a);
                else if (e.key.keysym.sym == SDLK_n) app_nav_sim_deviate(a);
                else if (e.key.keysym.sym == SDLK_PERIOD) app_nav_sim_speed(a, 1);
                else if (e.key.keysym.sym == SDLK_COMMA)  app_nav_sim_speed(a, -1);
                else if (e.key.keysym.sym == SDLK_v) {      /* 语音播报开关 */
                    a->voice_on = !a->voice_on;
                    app_persist_settings(a);
                    snprintf(a->nav_msg, sizeof(a->nav_msg),
                             a->voice_on ? "语音播报：开" : "语音播报：关");
                    a->need_redraw = 1;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    int mx = e.button.x, my = e.button.y;
                    int id = chrome_hit(a, mx, my);
                    /* 自绘标题栏：落在窗口按钮以外就拖拽整个窗口 */
                    if (my < CHROME_TITLE_H && id != CH_WIN_MIN &&
                        id != CH_WIN_MAX && id != CH_WIN_CLOSE) {
                        a->win_drag = 1;
                        int wx, wy;
                        SDL_GetWindowPosition(g_win, &wx, &wy);
                        a->win_drag_x = mx - wx;
                        a->win_drag_y = my - wy;
                        a->dragging = 0;
                    } else if ((SDL_GetModState() & KMOD_CTRL) && id == CH_NONE) {
                        /* Ctrl+拖动 = 旋转地图（指北针可一键回正北） */
                        a->rotating = 1;
                        a->dragging = 0;
                        a->last_x = mx; a->last_y = my;
                    } else if (id != CH_NONE) {
                        /* 命中 chrome 元素：交给 UP 当点击处理，不拖动地图 */
                        a->dragging = 0;
                    } else {
                        a->dragging = 1;
                        a->last_x = a->down_x = mx;
                        a->last_y = a->down_y = my;
                    }
                } else if (e.button.button == SDL_BUTTON_RIGHT) {
                    app_nav_clear(a);          /* 右键：清掉起终点和路线 */
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    int mx = e.button.x, my = e.button.y;
                    int was_drag = a->win_drag || a->rotating;
                    a->win_drag = 0; a->rotating = 0; a->dragging = 0;
                    int id = chrome_hit(a, mx, my);
                    if (sdl_window_action(id)) break;   /* 最小化/最大化：SDL 专属 */
                    if (id != CH_NONE) {
                        chrome_act(a, id, mx, my);
                        break;
                    }
                    /* 菜单打开时点菜单外任意处：先关菜单，不当成选点 */
                    if (a->menu_open) { a->menu_open = 0; a->need_redraw = 1; break; }
                    /* 拖动和点击用同一个左键：位移在 4 像素以内才算"点",
                       否则每次拖完地图都会顺手设一个点。 */
                    if (!was_drag &&
                        abs(mx - a->down_x) + abs(my - a->down_y) <= 4) {
                        double lat, lon;
                        mv_unproject(&a->view, mx, my, &lat, &lon);
                        app_nav_click(a, lat, lon);
                    }
                }
                break;
            case SDL_MOUSEMOTION: {
                int mx = e.motion.x, my = e.motion.y;
                if (a->win_drag) {
                    SDL_SetWindowPosition(g_win, mx - a->win_drag_x,
                                          my - a->win_drag_y);
                } else if (a->rotating) {
                    a->view.rot += (mx - a->last_x) * 0.4;
                    while (a->view.rot > 180.0)  a->view.rot -= 360.0;
                    while (a->view.rot < -180.0) a->view.rot += 360.0;
                    a->need_redraw = 1;
                } else if (a->dragging) {
                    mv_pan_pixels(&a->view, mx - a->last_x, my - a->last_y);
                    a->need_redraw = 1;
                }
                /* 光标位置始终记着：滚轮缩放拿它当锚点，否则鼠标没动过时
                   会锚在 (0,0)，画面一下就飞走。★ 必须在算完位移之后再更新。 */
                a->last_x = mx; a->last_y = my;
                break;
            }
            case SDL_MOUSEWHEEL:
                /* 2.26+ 的 wheel 事件自带鼠标位置；老版本退回 last_x/last_y */
                mv_zoom_at(&a->view, e.wheel.y > 0 ? 0.35 : -0.35,
                           e.wheel.mouseX ? e.wheel.mouseX : a->last_x,
                           e.wheel.mouseY ? e.wheel.mouseY : a->last_y);
                mv_clamp_zoom(&a->view, 3.0, 19.0);
                a->need_redraw = 1;
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_RESIZED ||
                    e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    a->view.w = e.window.data1;
                    a->view.h = e.window.data2;
                    a->need_redraw = 1;
                }
                break;
            default: break;
            }
        }

        /* 输入法开关跟着搜索框状态走（核心层没有 SDL，只能在这层同步） */
        if (a->su.active && !text_input_on) { SDL_StartTextInput(); text_input_on = 1; }
        else if (!a->su.active && text_input_on) { SDL_StopTextInput(); text_input_on = 0; }

        /* 收割后台算路的结果（它会置 need_redraw） */
        app_nav_poll(a);
        /* 模拟导航推进（内部只在 sim_on 且未暂停时动，开销可忽略） */
        app_nav_sim_tick(a);

        if (!a->need_redraw) {
            /* 算路跑几秒，期间没有重绘，标题栏会一直显示旧数字，看着像卡死。
               这里只改标题，不重画（重画要几百毫秒，反而更卡）。 */
            if (a->nav_busy) {
                char tmp[300];
                snprintf(tmp, sizeof(tmp), "算路中… %.1f 秒（%s）",
                         (double)(nm_now_ms() - a->nav_t0) / 1000.0, a->status);
                SDL_SetWindowTitle(g_win, tmp);
            }
            nm_sleep_ms(8);
            continue;
        }
        a->need_redraw = 0;

        uint64_t t0 = nm_now_ms();
        PaintStats st;
        /* 窗口模式与离屏同一套代码路径：画在离屏 surface 上，再贴到窗口。
           尺寸变了（resize）先重建画布。 */
        if (sdl_ensure_canvas(a->view.w, a->view.h) != 0) { a->quit = 1; break; }
        if (sdl_render_frame(a, &st) != 0) { a->quit = 1; break; }
        if (g_tex) {
            SDL_UpdateTexture(g_tex, NULL, g_surf->pixels, g_surf->pitch);
            SDL_RenderCopy(g_win_ren, g_tex, NULL, NULL);
        }
        SDL_RenderPresent(g_win_ren);
        double ms = (double)(nm_now_ms() - t0);

        update_status(a, ms);
        SDL_SetWindowTitle(g_win, a->status);
        fprintf(stderr, "%s  扫边 %llu 画边 %llu\n", a->status,
                (unsigned long long)st.visited, (unsigned long long)st.drawn);
    }

    /* 收尾：窗口/纹理/渲染器/surface 都是本文件建的，本文件负责拆。
       （原先这些句柄挂在 App 上，App 已经去 SDL 化，改为本地持有。） */
    if (g_tex)     { SDL_DestroyTexture(g_tex); g_tex = NULL; }
    if (g_surf)    { SDL_FreeSurface(g_surf); g_surf = NULL; }
    if (g_win_ren) { SDL_DestroyRenderer(g_win_ren); g_win_ren = NULL; }
    if (g_win)     { SDL_DestroyWindow(g_win); g_win = NULL; }
    SDL_Quit();
}
