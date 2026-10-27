/* voice_wire.h —— 语音播报的**接线层**（ui 层）
 *
 * ★ 为什么要独立成模块：接线层（取状态 → 编排 → 发声）原先长在 window.c 的
 *   App 里，只被 app_nav_sim_tick() 调用 ⇒ 无头端到端自测**够不着**它，
 *   只能自己复刻一遍逻辑 ⇒ "把窗口的发声循环改坏"这种真 bug 抓不到
 *   （负向对照实测：注入后两个自测全绿 = 判据假绿）。
 *   现在接线函数本身是可调用的生产函数（nav_voice_frame），
 *   自测调**同一份代码** ⇒ 接线断了必然被抓到。
 *
 * ★ 分层：
 *   策略层（该播第几级 / 话术 / 冷却）在 route/voice.c，纯逻辑、零平台依赖；
 *   接线层（这一帧取哪些状态、按什么顺序交给 TTS）在本模块。
 */
#ifndef NAVIMAP_UI_VOICE_WIRE_H
#define NAVIMAP_UI_VOICE_WIRE_H

#include <stdint.h>

#include "route/route.h"
#include "route/maneuver.h"
#include "route/navsim.h"
#include "route/voice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 接线层需要的最小状态集合（= App 里与语音有关的字段）。
 *
 *   vr           播报器状态（可写：层级记忆 / 同文本冷却计时都在里面）
 *   rr / man     当前路线与指令表（只读）
 *   sim          模拟导航状态（只读：剩余里程 / 速度 / 已到达标志）
 *   voice_on     语音总开关（V 键 / Qt『设置』菜单）
 *   paused       模拟是否暂停（偏航重算时为 1）⇒ 暂停时不播**常规**播报
 *   ann_finished "导航结束"收尾语是否已播（可写，保证恰好一次） */
typedef struct {
    VoiceRouter       *vr;
    const RouteResult *rr;
    const ManeuverList *man;
    const NavSim       *sim;
    int                voice_on;
    int                paused;
    uint8_t           *ann_finished;
} NavVoiceCtx;

/* 播报一帧（生产接线函数）。
 *
 * ★ 时间由调用方注入（生产传 nm_now_ms()，自测传合成时钟）
 *   ⇒ 冷却 / 去抖的判据确定可复现。
 * ★ 发声走 nm_tts_speak() ⇒ 无头自测用 NAVIMAP_TTS_DRYRUN
 *   捕获"窗口真正交出去的话"，而不是自己复刻一份收集逻辑。 */
void nav_voice_frame(NavVoiceCtx *c, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
