/* voice_wire.c —— 语音播报的接线层（取状态 → 编排 → 发声）
 *
 * ★ 这个文件就是"窗口每帧到底播了什么"的**唯一实现**：
 *   src/ui/window.c 的 app_nav_sim_tick() 调它；
 *   tools/voice_e2e.c 的端到端自测也调它（同一份代码，不是副本）。
 *   ⇒ 接线层坏掉（发声循环断了、收尾语被吞了）自测必然变红。
 */
#include "ui/voice_wire.h"

#include <stdio.h>
#include <string.h>

#include "utils/plat.h"

void nav_voice_frame(NavVoiceCtx *c, uint64_t now_ms)
{
    if (!c || !c->vr || !c->rr || !c->man || !c->sim || !c->ann_finished) return;
    if (!c->voice_on) return;

    /* 1) 到达播报：官方 arrival() —— 剩余里程 < 200 m 时播"前方即将到达目的地"。
     *    真正停在终点时（sim.arrived）另播一句（官方 nav-desktop
     *    NavigationDialogs.java:71 的 "您已到达目的地附近，导航结束"）。 */
    double remain = navsim_remaining_m(c->sim);

    /* 2) 当前速度：官方用 speedMps（GPS 车速）。本项目的模拟车按**当前段限速**走，
     *    所以 navsim_speed_ms 就是等价物（也让"提前量随速度放大"有真实输入）。 */
    double speed_ms = navsim_speed_ms(c->sim);

    /* 3) 当前指令下标与距离（官方 State.index / State.distToTurn） */
    double to_go = 0.0;
    int mi = navsim_next_maneuver(c->sim, c->man, &to_go, NULL);

    /* 4) 一帧的播报决策（顺序照官方 VoiceRouter.update）。
     *    ★ 编排本身在 route/voice.c 的 voice_tick() 里（speedLimit →
     *      arrival → alarm → turn），这里只负责取状态 + 发声。 */
    VoiceFrame vf;
    int n = voice_tick(c->vr, c->rr, c->man, mi, to_go, speed_ms, remain,
                       c->sim->arrived, now_ms, &vf);

    /* ★★ 暂停时不播常规播报（偏航重算期间不该出声）。
     *   但**到达那一帧是例外**：到达时会把 sim_paused 置 1（模拟停下），
     *   而这一帧恰恰是收尾语 + 可能尚未播出的到达提示该说的时候
     *   ⇒ 到达帧不被 paused 抑制（官方 stopNavigation(arrived) 的那句
     *   是在"到达"这一刻说的，与"暂停"无关）。
     *   ★ 这一条是"先播报后置位"那个真 bug 的**唯一裁决点**：
     *     window.c 即便把 sim_paused 在到达前就置 1（旧版 bug 形状），
     *     nav_voice_frame 照样把到达帧的播报放出来 ⇒
     *     调用方顺序不再是 load-bearing，判据只需盖这里（e2e A8 + 负向 #9/#10）。 */
    int suppress = c->paused && !c->sim->arrived;
    if (!suppress) {
        for (int i = 0; i < n; i++)
            nm_tts_speak(vf.text[i]);
    }

    /* 5) 终点收尾语（官方 NavigationDialogs.stopNavigation 的 arrived 分支）。
     *    ★ 永远不受 paused 影响（见上，到达帧例外）。
     *      用 ann_finished 保证只播一次（与 vr.arrived_said 是两件事）。 */
    if (c->sim->arrived && !*c->ann_finished) {
        *c->ann_finished = 1;
        nm_tts_speak("您已到达目的地附近，导航结束");
    }
}
