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
     *   但**终点收尾语不受暂停影响** —— 官方 stopNavigation(arrived) 的那句
     *   "您已到达目的地附近，导航结束" 是在**到达**这一刻说的，
     *   而本项目到达时会把 sim_paused 置 1（模拟停下）⇒
     *   ★★ 若把收尾语也挡在 paused 后面，这句话**永远播不出来**
     *     （实测真 bug：GUI 里从来没响过这句）。
     *   ⇒ 收尾语单独判，不受 paused 影响。 */
    if (!c->paused) {
        for (int i = 0; i < n; i++)
            nm_tts_speak(vf.text[i]);
    }

    /* 5) 终点收尾语（官方 NavigationDialogs.stopNavigation 的 arrived 分支）。
     *    ★ 官方这句属于**界面**层，不在 VoiceRouter 里 ⇒ 单独在这里播，
     *      用 ann_finished 保证只播一次（与 vr.arrived_said 是两件事）。 */
    if (c->sim->arrived && !*c->ann_finished) {
        *c->ann_finished = 1;
        nm_tts_speak("您已到达目的地附近，导航结束");
    }
}
