#!/bin/bash
# 语音播报负向对照：注入 bug ⇒ 必须被抓到（判据不能是"假绿"）
#
# 用法：./scripts/voice_neg.sh
# 逐个注入 bug，重建，跑 voice_selftest + voice_e2e，
# 两者**至少有一个**必须失败（fail-closed）。
#
# ★ 为什么必须做：判据写成"!= 0"这种形状时，逻辑写反也能通过
#   （记忆里已踩过 8 项假 FAIL 的坑）。每个注入都必须实测能红。
set -u
cd "$(dirname "$0")/.." || exit 1

SRC=src/route/voice.c
WIN=src/ui/window.c
WIRE=src/ui/voice_wire.c
BAK=/tmp/voice_neg_bak
mkdir -p "$BAK"
cp "$SRC" "$BAK/voice.c"
cp "$WIN" "$BAK/window.c"
cp "$WIRE" "$BAK/voice_wire.c"

BUILD=build
pass=0; fail=0

restore() {
    cp "$BAK/voice.c" "$SRC"
    cp "$BAK/window.c" "$WIN"
    cp "$BAK/voice_wire.c" "$WIRE"
}

# 跑一次：返回 0 = 两个自测都过（说明注入没被抓到 = FAIL）
run_case() {
    cmake --build "$BUILD" --target voice_selftest voice_e2e >/dev/null 2>&1
    local r1=0 r2=0
    ./build/voice_selftest.exe >/dev/null 2>&1 || r1=1
    ./build/voice_e2e.exe    >/dev/null 2>&1 || r2=1
    echo "$((r1 | r2))"
}

# inject <名字> <文件> <python 替换表达式>
inject() {
    local name="$1" file="$2" expr="$3"
    restore
    python - "$file" "$expr" <<'PY'
import io, sys
path, expr = sys.argv[1], sys.argv[2]
old, new = expr.split('|||')
s = io.open(path, encoding='utf-8').read()
if old not in s:
    print("INJECT-PATTERN-NOT-FOUND")
    sys.exit(3)
s = s.replace(old, new, 1)
io.open(path, 'w', encoding='utf-8', newline='').write(s)
PY
    if [ $? -ne 0 ]; then
        echo "  [$name] 注入失败（模式没找到）"; fail=$((fail+1)); return
    fi
    local r
    r=$(run_case)
    if [ "$r" = "1" ]; then
        echo "  [$name] ✔ 被抓到（自测失败）"
        pass=$((pass+1))
    else
        echo "  [$name] ✘ 没被抓到 —— 判据是假绿！"
        fail=$((fail+1))
    fi
    restore
}

echo "=== 语音播报负向对照 ==="

# ---- 1) 告警实例号退回"扫描计数器"（本次修掉的那个真 bug） ----
inject "inst 用扫描计数器（位置相关）" "$SRC" \
  'tunnel_inst = (int)i;|||tunnel_inst = 0;'

# ---- 2) 同文本冷却只对转向生效（官方 say() 是唯一出口） ----
# ★ 锚点必须是**告警函数**里的 say_dup（voice_router_maybe_say_alarm）。
#   上一版锚在"5) 每帧编排"注释上，唯一匹配到的是 voice_router_maybe_say_arrive()
#   的 say_dup —— 那里的 arrived_said 先置位 ⇒ 永远命中不了 ⇒ 注入零影响
#   ⇒ 两个自测都过 ⇒ **判据假绿**（实测踩过）。
#   告警函数里的 say_dup 可观测：3.5c 第 2 次调用（inst=1, t=5000）距上次播
#   4000 ms > 告警自身 3000 ms 错开 ⇒ 只可能被 15 s 同文本冷却挡住。
inject "告警不过同文本冷却" "$SRC" "    /* 官方 say() 是所有播报的出口 ⇒ 同文本冷却对告警也生效。 */
    if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
    return 1;|||    /* 官方 say() 是所有播报的出口 ⇒ 同文本冷却对告警也生效。 */
    return 1;"

# ---- 3) 播报顺序：turn 放到最前（官方是 turn 在最后） ----
inject "转向放最前（违反官方顺序）" "$SRC" \
  '    /* ---- 1) 限速（官方 speedLimit）----|||    /* ---- 0) 转向（提前，违反官方顺序）---- */
    if (ml && ml->n > 0 && man_idx >= 0 && man_idx < ml->n &&
        to_go_m >= 0.0) {
        if (voice_router_maybe_say(vr, man_idx, ml, to_go_m, speed_ms,
                                   buf, sizeof buf, now_ms))
            frame_push(out, buf);
    }

    /* ---- 1) 限速（官方 speedLimit）----'

# ---- 4) 沿路告警扫描基准用"剩余里程"而不是"已走里程" ----
inject "告警基准用剩余里程（口径反了）" "$SRC" \
  '        double along = rr->total_m - remain_m;|||        double along = remain_m;'

# ---- 5) 层级判定顺序反了（先查最弱的一级） ----
inject "层级判定先查最弱" "$SRC" \
  '    if (to_go_m <= now_lead(lead[VOICE_TURN_NOW], speed_ms))
        return VOICE_TURN_NOW;
    if (to_go_m <= scaled_lead(lead[VOICE_TURN_IN], speed_ms))
        return VOICE_TURN_IN;
    if (to_go_m <= scaled_lead(lead[VOICE_PREPARE], speed_ms))
        return VOICE_PREPARE;|||    if (to_go_m <= scaled_lead(lead[VOICE_PREPARE], speed_ms))
        return VOICE_PREPARE;
    if (to_go_m <= scaled_lead(lead[VOICE_TURN_IN], speed_ms))
        return VOICE_TURN_IN;
    if (to_go_m <= now_lead(lead[VOICE_TURN_NOW], speed_ms))
        return VOICE_TURN_NOW;'

# ---- 6) 提前量不按官方 (int) 截断 ----
inject "提前量不截断（浮点）" "$SRC" \
  '    out_lead[VOICE_PREPARE]      = (double)(int)(v * VOICE_PREPARE_SEC);|||    out_lead[VOICE_PREPARE]      = (v * VOICE_PREPARE_SEC);'

# ---- 7) 隧道太短不丢弃（官方 MIN_TUNNEL_M） ----
inject "短隧道不丢弃" "$SRC" \
  '            if (tunnel_len >= VOICE_ALARM_MIN_TUNNEL_M) {|||            if (tunnel_len > 0) {'

# ---- 8) 接线层不发声（接线断了） ----
# ★ 目标从 window.c 迁到 src/ui/voice_wire.c：接线的**唯一实现**已经搬过去了
#   （window.c 只是薄封装）。上一版注入 window.c 的发声循环 ⇒ 现在的 window.c
#   里已经没有那个模式了（会报 INJECT-PATTERN-NOT-FOUND）。
#   注入写法用 `if (n < 0)` 而不是 `i < 0`：后者会让 n 变成"未使用变量"
#   ⇒ -Werror 直接编译失败 ⇒ 跑的还是旧 exe ⇒ 判据假绿（编译失败不等于抓到）。
inject "接线：窗口不播报" "$WIRE" "    if (!c->paused) {
        for (int i = 0; i < n; i++)
            nm_tts_speak(vf.text[i]);
    }|||    if (!c->paused) {
        for (int i = 0; i < n; i++)
            if (n < 0) nm_tts_speak(vf.text[i]);
    }"

# ---- 9) 收尾语被暂停挡掉（就是那个真 bug 的形状） ----
#   把"您已到达目的地附近，导航结束"挪进 paused 保护里 ⇒ A8 判据必须变红。
inject "收尾语被暂停挡掉" "$WIRE" "    if (!c->paused) {
        for (int i = 0; i < n; i++)
            nm_tts_speak(vf.text[i]);
    }|||    if (!c->paused) {
        for (int i = 0; i < n; i++)
            nm_tts_speak(vf.text[i]);
        if (c->sim->arrived && !*c->ann_finished) {
            *c->ann_finished = 1;
            nm_tts_speak(\"您已到达目的地附近，导航结束\");
        }
    }"

echo "=== 结果：通过 $pass / 失败 $fail ==="
restore
cmake --build "$BUILD" --target voice_selftest voice_e2e >/dev/null 2>&1
if [ "$fail" -ne 0 ]; then exit 1; fi
exit 0
