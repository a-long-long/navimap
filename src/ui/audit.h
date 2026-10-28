/* audit.h —— 离屏像素审计（共享模块）
 *
 * 把"像素计数 + 断言 + 日志打印"从离屏通道里抽出来，让
 * navmap_qt --shot-offscreen（C 核心地图缓冲）与 regress.sh 的离屏判据
 * 共用同一份逻辑，保证像素判据逐位一致（保 74/4 回归基线不出偏差）。
 *
 * ★ 计数一律按**字节**比色：C 核心缓冲是 RGB 字节序（p[2]=R, p[1]=G, p[0]=B，
 *   alpha 字节忽略）。offscreen_shot 的 malloc 缓冲与 Qt 画布的
 *   QImage::Format_RGB32 都是这个布局，字节比对能跨两种缓冲一致，
 *   也绕开精确配色相等的脆弱性。
 *
 * 本头自带 ui/window.h（App / PaintStats 全量定义 + app_ovl_a / app_focus_point），
 * 并用 extern "C" 包住，使它能同时被 C 与 C++ 包含。
 *
 * ★ 模块归属（2026-10-28 R2，PLAN.md §53）：本文件原在 render/，因要 App
 * 全量定义而反向引用 ui/window.h ⇒ render→ui 倒置 + 符号级环。现移到 ui/：
 * 审计是**验收通道**的一部分（读 App 状态 + 像素缓冲做断言），不是渲染算法。
 */
#ifndef NM_UI_AUDIT_H
#define NM_UI_AUDIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "ui/window.h"   /* App + PaintStats 全量定义；app_ovl_a / app_focus_point */

/* 各前端按自己的 flag 填好传进来（这些状态前端特有，不在 App 里） */
typedef struct {
    int    have_search;    /* 搜索框已展开（对应 a.su.active） */
    int    have_route;     /* 已算路（对应 a.have_rr） */
    int    have_sim;       /* 模拟导航进行中 */
    double deviate_m;      /* 注入偏移米数（>0 表示要判偏航） */
    int    n_gpx_in;       /* 本次导入的 GPX 文件数（>0 才查轨迹/收藏像素） */
} AuditCtx;

/* 在 C 核心地图缓冲 buf（RGB 字节序，stride 以字节计）上做全套像素计数 + 断言，
 * 打印全部审计日志行（route_pixels / gpx_trk_px / gpx_fav_px /
 * coverage / nomap_banner_px / search_box_px / done_pixels / 各 ASSERT-FAIL / ASSERT-OK）。
 * 返回 0=全部 ASSERT-OK；1=有 ASSERT-FAIL。 */
int app_offscreen_audit(const App *a, const uint8_t *buf, int w, int h, int stride,
                        const PaintStats *st, const AuditCtx *c);

#ifdef __cplusplus
}
#endif

#endif /* NM_UI_AUDIT_H */
