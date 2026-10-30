# 第 11 课 · 导航层与语音

> 📖 配套读源码讲义：[`docs/teaching/11_navvoice.md`](../../docs/teaching/11_navvoice.md)（读源码式讲解，讲清楚“为什么这么写”；与本科小程序同号对应）


**累加内容**：从"一条路线"到"可播报的导航"的最后一公里。
- `Maneuver`：把路线折线转成"左转/右转/直行"指令序列（算相邻段方位角差）。
- `VoiceRouter`：★ 只决定"说什么"（纯逻辑），发声在 `nm_tts_speak`（本课用桩打印）。
- `voice_wire`：★ 接线层，唯一裁决点；生产和端到端自测共用，避免判据假绿。

**运行**：
```bash
gcc -std=c11 -O2 -o tut main.c && ./tut
```

**你会看到**：stdout 打印 Maneuver 序列（每个顶点的里程 + 转向类别），以及 VoiceRouter 经 `voice_wire`
接线后逐条播报的文本（"前方 X 米左转"…"到达目的地"）。

**关键概念（项目里一致）**：
1. **分层**：`voice.c` 只定"说什么"，发声在 `plat.c` 的 `nm_tts_speak`；接线**只有一份**在
   `voice_wire.c`。接线若长在 `App` 里，无头自测够不着只能复刻 ⇒ 判据必假绿。
2. **顺序耦合**：到达那一刻既要置 `sim_paused=1` 又要播收尾语 ⇒ 先播报后置位；收尾语单独判、
   不受 paused 挡（否则永远播不出来）。
3. **冷却**：官方 `say()` 是所有播报唯一出口，15 秒同文本冷却对限速/到达/告警/转向一视同仁。

**生产最终形态**：`src/route/maneuver.c`（转向指令）、`src/route/voice.c`（`VoiceRouter` 纯逻辑）、
`src/ui/voice_wire.c`（`nav_voice_frame` 唯一接线点）、`tools/voice_e2e.c`（端到端断言真实发声）。

**练习**：把 `R_LAT/R_LON` 改成一条"先直行再右转再左转"的路线，看 Maneuver 与播报如何随之变化。
