/* voice.c —— 语音播报策略（抄写自 OsmAnd，2026-10-25）
 *
 * 对应源文件见 voice.h 的头部清单。本文件是**纯逻辑**：不碰 SDL、不碰线程、
 * 不发声，只回答两个问题："现在该播第几级？" 和 "这句话怎么说？"
 * 发声由 utils/plat.c 的 nm_tts_speak 负责。
 *
 * 为什么值得抄：原来的播报是"固定 300m/60m 两级 + 一句直白文案"，
 * 高速上 300 m 只剩 9 秒（来不及），连续两个路口会连着刷屏。
 * OsmAnd 的"速度 × 秒数"模型把这两件事都解决了。
 */
#include "route/voice.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/road_tags.h"

/* ---------------------------------------------------------------------------
 * 1) 分级提前量（抄 AnnounceTimeDistances.java）
 * ------------------------------------------------------------------------- */

/* 官方 isDistanceLess()：dist <= max(leadDist, currentSpeed/DEFAULT*leadDist)
 * —— 车速比默认快，提前量按比例放大。官方还加 voicePromptDelayTimeSec
 * （蓝牙链路延迟），本项目没有蓝牙通道，跳过。 */
static double scaled_lead(double base_lead, double speed_ms)
{
    double v = speed_ms > 0.0 ? speed_ms : VOICE_DEFAULT_SPEED_MS;
    double scaled = v / VOICE_DEFAULT_SPEED_MS * base_lead;
    return scaled > base_lead ? scaled : base_lead;
}

/* 官方 TURN_NOW 的特殊规则：低速时**缩小**（越慢越晚说"现在"），
 * 高速时放大。官方原码：
 *   if (currentSpeed < DEFAULT_SPEED)
 *       return dist <= max(POSITIONING_TOLERANCE, cur/DEFAULT*TURN_NOW_DISTANCE);
 *   return isDistanceLess(currentSpeed, dist, TURN_NOW_DISTANCE);
 * —— 低速走"缩小"分支（还额外被 12 m 定位误差兜底），
 *   高速走 isDistanceLess（只放大不缩小）。这里照抄。 */
static double now_lead(double base_now, double speed_ms)
{
    double v = speed_ms > 0.0 ? speed_ms : VOICE_DEFAULT_SPEED_MS;
    if (v < VOICE_DEFAULT_SPEED_MS) {
        double small = v / VOICE_DEFAULT_SPEED_MS * base_now;
        return small > VOICE_POS_TOLERANCE_M ? small : VOICE_POS_TOLERANCE_M;
    }
    return scaled_lead(base_now, v);
}

void voice_lead_distances(double default_speed_ms, double out_lead[4])
{
    double v = default_speed_ms > 0.0 ? default_speed_ms : VOICE_DEFAULT_SPEED_MS;

    /* ★ 官方是**整数截断**（(int) 转换），本模块照抄。
     *   官方源码注释里的数字是"四舍五入"过的，两者可能差 1：
     *     PREPARE   官方码 (int)(12.5*115)   = 1437，官方注释写 1438
     *     TURN_IN   官方码 (int)(12.5*22)    = 275， 官方注释写 275（一致）
     *     TURN_NOW  官方码 (int)(12.5*6.708) = 83，   官方注释写 83 （一致）
     *   自行车/步行档同样对得上（码 318/127，注 319/128）⇒ 确认是
     *   "码截断、注四舍五入"。本模块以**码**为准（实际生效的就是码）。 */

    /* 官方 TURN_NOW = speed * min(sqrt(speed*3.6), 8)，再被
     * POSITIONING_TOLERANCE(12m) 兜底。
     * 官方 car 档（DEFAULT_SPEED=12.5 m/s）：(int)83.85 = 83，与官方注释一致。 */
    double now_sec = VOICE_TURN_NOW_SEC;
    if (v > 0.0) {
        double s = sqrt(v * 3.6);
        if (s > 8.0) s = 8.0;
        now_sec = s;
    }
    double now_m = v * now_sec;
    if (now_m < VOICE_POS_TOLERANCE_M) now_m = VOICE_POS_TOLERANCE_M;

    out_lead[VOICE_PREPARE_LONG] = (double)(int)(v * VOICE_PREPARE_LONG_SEC);
    out_lead[VOICE_PREPARE]      = (double)(int)(v * VOICE_PREPARE_SEC);
    out_lead[VOICE_TURN_IN]      = (double)(int)(v * VOICE_TURN_IN_SEC);
    out_lead[VOICE_TURN_NOW]     = (double)(int)now_m;
}

int voice_announce_state(double to_go_m, double speed_ms)
{
    if (to_go_m < 0.0) return VOICE_NONE;

    /* ★ 基准阈值按**默认速度**算（官方就是这么做的：阈值是"车型档位"的属性，
     *   不是当前车速的属性），当前速度只用来做等比缩放。 */
    double lead[4];
    voice_lead_distances(VOICE_DEFAULT_SPEED_MS, lead);

    /* ★ 判定顺序 = 从最紧急往回查，命中即返回**最紧急**的那一级。
     *   官方 nav-desktop VoiceRouter.update() 是
     *       for (i) if (dist <= th[i]) stage = i;   // 后面的覆盖前面的
     *       if (stage > lastStage) say(...)
     *   —— th 数组是"由远到近"排的，所以最后覆盖到的那个就是**最紧急**的一级。
     *   官方 Android getImminentTurnStatus() 同理（先 TURN_NOW 再 PREPARE）。
     *   ★★ 反过来查（先查 PREPARE_LONG）会有真 bug：高速时 PREPARE_LONG 的
     *      圈（3750 m × 速度比例）远大于 PREPARE 的圈（1437 m × 速度比例），
     *      先查它就会让 2000 m 处返回 PREPARE_LONG（更弱的一级），
     *      ⇒ 中间那级永远播不出来。
     *   官方还有一条 isTurnStateNotPassed()：某一级越过下界就不再播，
     *   本模块靠 last_state 单调递增等价实现（见 voice_router_maybe_say）。 */
    if (to_go_m <= now_lead(lead[VOICE_TURN_NOW], speed_ms))
        return VOICE_TURN_NOW;
    if (to_go_m <= scaled_lead(lead[VOICE_TURN_IN], speed_ms))
        return VOICE_TURN_IN;
    if (to_go_m <= scaled_lead(lead[VOICE_PREPARE], speed_ms))
        return VOICE_PREPARE;
    /* PREPARE_LONG 只在高速时播（官方 issue 1411：speed >= 30 m/s）。 */
    if (speed_ms >= VOICE_FAST_SPEED_MS &&
        to_go_m <= scaled_lead(lead[VOICE_PREPARE_LONG], speed_ms))
        return VOICE_PREPARE_LONG;

    return VOICE_NONE;
}

int voice_is_now(double to_go_m, double speed_ms)
{
    return voice_announce_state(to_go_m, speed_ms) == VOICE_TURN_NOW;
}

/* ---------------------------------------------------------------------------
 * 2) 话术模板（抄 VoiceRouter.turnPhrase / action / Alarm.phrase）
 * ------------------------------------------------------------------------- */

/* 转向动作短语。
 * ★ 词表抄 OsmAnd 官方**中文桌面导航**实现
 *   （nav-desktop/src/main/osmand/nav/route/VoiceRouter.java 的 action()）：
 *     C（直行）    ⇒ "沿当前道路直行"
 *     TL           ⇒ "左转"
 *     TSLL（稍左） ⇒ "稍向左转"
 *     TSHL（急左） ⇒ "急向左转"
 *     TR           ⇒ "右转"
 *     TSLR（稍右） ⇒ "稍向右转"
 *     TSHR（急右） ⇒ "急向右转"
 *     KL           ⇒ "靠左行驶"
 *     KR           ⇒ "靠右行驶"
 *     TU/TRU       ⇒ "掉头"
 *     OFFR         ⇒ "驶离主路"
 *     RNDB/RNLB    ⇒ "进入环岛"（+ "，从第 N 个出口驶出"）
 *   本项目 MAN_* 与官方 TurnType 的对应（按**转向语义**对应，不是按名字）：
 *     MAN_STRAIGHT     → C
 *     MAN_LEFT         → TL      MAN_SLIGHT_LEFT  → TSLL
 *     MAN_SHARP_LEFT   → TSHL    MAN_RIGHT        → TR
 *     MAN_SLIGHT_RIGHT → TSLR    MAN_SHARP_RIGHT  → TSHR
 *     MAN_UTURN        → TU      MAN_ROUNDABOUT   → RNDB
 *   ★ 本项目 maneuver.c 按**转角**分类：小于 TURN_NORMAL ⇒ SLIGHT_*、
 *     TURN_NORMAL~TURN_SHARP ⇒ SHARP_* ⇒ 语义上就是"稍转/急转"，
 *     与官方 TSLL/TSHL 一一对应。
 *     （面板文案 maneuver_kind_text 把 SLIGHT_* 写成"靠左/靠右"，那是
 *      车道选择的意思，与转角分类对不上；语音按官方转向语义走。）
 *   ★ 官方 action() 的环岛分支自带出口号：
 *       "进入环岛，从第 N 个出口驶出"  —— 这里照抄。 */
const char *voice_action_text(int man_type, int exit_no)
{
    const char *act;
    switch (man_type) {
    case MAN_STRAIGHT:     act = "沿当前道路直行"; break;
    case MAN_LEFT:         act = "左转";           break;
    case MAN_SLIGHT_LEFT:  act = "稍向左转";       break;
    case MAN_SHARP_LEFT:   act = "急向左转";       break;
    case MAN_RIGHT:        act = "右转";           break;
    case MAN_SLIGHT_RIGHT: act = "稍向右转";       break;
    case MAN_SHARP_RIGHT:  act = "急向右转";       break;
    case MAN_UTURN:        act = "掉头";           break;
    case MAN_ROUNDABOUT:   act = "进入环岛";       break;
    default:               act = "按路线行驶";     break;
    }
    /* 环岛带出口号时用静态缓冲（官方 action() 也是这么返回拼接串的）。
     * ★ 调用方必须**立即**用掉返回值：下一次调用会覆盖。 */
    static char rb[64];
    if (man_type == MAN_ROUNDABOUT && exit_no > 0) {
        snprintf(rb, sizeof rb, "进入环岛，从第 %d 个出口驶出", exit_no);
        return rb;
    }
    return act;
}

/* 距离口语化。
 * ★ 抄 OsmAnd 官方中文实现 nav-desktop/src/main/osmand/nav/core/Units.spoken()
 *   （它直接转给 Units.dist()，公制分支 metersText）：
 *     |d| < 10 m    ⇒ "%.1f 米"（"5.0 米"）
 *     |d| < 1000 m  ⇒ 整十米（"240 米"）
 *     |d| < 100 km  ⇒ 一位小数公里（"1.5 公里"）
 *     else          ⇒ 整数公里（"120 公里"）
 *   ★ 为什么不用官方 Android 中文语音包（OsmAnd-resources/voice/zh/zh_tts.js）
 *     那套 6 档分段（<17 整数米 / <100 十米 / <1000 五十米 / <1500 "大约 1 公里"…）：
 *     那套是 **Android 端 TTS 引擎**的语音包脚本，依赖 JS 运行时 + 预录 ogg，
 *     与本项目的 Windows SAPI + 桌面端不同源；而 nav-desktop 这套是**同形态**
 *     的官方实现（纯 Java、桌面导航、中文），可直接照抄。
 *   ★ 官方 dist() 的零值分支是 `"0 " + shortUnit()`，公制 shortUnit = "公里"
 *     ⇒ "0 公里"。照抄。
 *   与 maneuver.c 的 fmt_distance 不同：语音要"240 米"而不是"237 米"，
 *     免得 TTS 念出一串拗口数字。 */
void voice_spoken_distance(double meters, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    double a = meters < 0.0 ? -meters : meters;
    if (a < 1e-6) {
        snprintf(buf, n, "0 公里");        /* 官方 dist() 的零值分支 */
    } else if (a < 10.0) {
        snprintf(buf, n, "%.1f 米", meters);
    } else if (a < 1000.0) {
        /* 官方：((int)(Math.round(signed / 10.0) * 10)) + " 米" */
        snprintf(buf, n, "%.0f 米", (double)((long)((meters / 10.0) + 0.5) * 10));
    } else if (a < 100000.0) {
        snprintf(buf, n, "%.1f 公里", meters / 1000.0);
    } else {
        snprintf(buf, n, "%.0f 公里", meters / 1000.0);
    }
}

int voice_turn_phrase(const Maneuver *m, double to_go_m, int now,
                       char *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    buf[0] = '\0';
    if (!m) return 0;

    char d[32];
    voice_spoken_distance(to_go_m, d, sizeof d);

    /* 起点：官方桌面实现没有单独的起点句式；本项目沿用"沿 X 出发"
     * （与 maneuver_text 的面板文案一致）。 */
    if (m->type == MAN_START) {
        if (m->road[0]) return snprintf(buf, n, "沿%s出发", m->road);
        return snprintf(buf, n, "从起点出发");
    }

    /* 到达：官方 VoiceRouter.turnPhrase() 的目的地分支
     *   （m.turnType < 0）是 "前方" + voiceDist(distToTurn) + "到达目的地"，
     *   **没有** "现在" 变体（距离始终带上）。照抄。 */
    if (m->type == MAN_ARRIVE) {
        return snprintf(buf, n, "前方%s到达目的地", d);
    }

    /* 转向：官方 turnPhrase()
     *   远： "前方" + voiceDist + act
     *   近： "现在" + act
     *   环岛的出口号已经含在 action() 里（见 voice_action_text）。 */
    const char *act = voice_action_text(m->type, m->exit_no);
    int k;
    if (now) k = snprintf(buf, n, "现在%s", act);
    else     k = snprintf(buf, n, "前方%s%s", d, act);

    /* 路名：官方用 "，进入" + 路名，并排除 "目的地" 这个占位路名。 */
    if (m->road[0] && strcmp(m->road, "目的地") != 0) {
        size_t used = (size_t)(k > 0 ? k : 0);
        if (used < n) snprintf(buf + used, n - used, "，进入%s", m->road);
    }
    return (int)strlen(buf);
}

/* 告警短名表。
 * ★ 抄官方 nav-desktop/src/main/osmand/nav/model/AlarmType.java 的 say 列
 *   （每个告警一条完整中文句）与 Alarm.phrase() 的拼装：
 *     SPEED_CAMERA        ⇒ "前方有测速摄像头"
 *     SPEED_LIMIT         ⇒ "前方限速"
 *     BORDER_CONTROL      ⇒ "前方有边防检查站"
 *     RAILWAY             ⇒ "前方有铁路道口，注意观察"
 *     TRAFFIC_CALMING     ⇒ "前方有减速带"
 *     TOLL_BOOTH          ⇒ "前方有收费站"
 *     STOP                ⇒ "前方需要停车让行"
 *     PEDESTRIAN          ⇒ "前方有人行横道，注意行人"
 *     TUNNEL              ⇒ "进入隧道"（官方 Alarm.phrase 特例，不带"前方"）
 *   ★ 一处**有意的**偏离：官方远处分支是
 *       "前方 " + spoken(dist) + type.say
 *     而 type.say 自带"前方" ⇒ 拼成"前方 500 米前方有测速摄像头"（重复）。
 *     本模块把 say 拆成"整句"和"去前缀核心短语"两列：
 *       近处（<=60 m）用整句 ⇒ 与官方 AlarmType.say **逐字相同**；
 *       远处用"前方 X 米" + 核心短语 ⇒ 只去掉重复的那个"前方"。
 *   ★ 桥梁（BRIDGE）是本项目扩展：路网有 EF_BRIDGE 标志位，
 *     但官方 AlarmType 里没有对应项。 */
static const struct {
    const char *say;      /* 近处播报的整句（= 官方 AlarmType.say） */
    const char *core;     /* 去掉"前方"前缀的核心短语（远处拼距离用） */
} g_alarm[VOICE_ALARM_COUNT] = {
    [VOICE_ALARM_NONE]         = { "",             "" },
    [VOICE_ALARM_TUNNEL]       = { "进入隧道",     "进入隧道" },
    [VOICE_ALARM_TOLL]         = { "前方有收费站", "有收费站" },
    [VOICE_ALARM_BRIDGE]       = { "前方有桥梁",   "有桥梁" },
    [VOICE_ALARM_SPEED_CAMERA] = { "前方有测速摄像头", "有测速摄像头" },
    [VOICE_ALARM_STOP]         = { "前方需要停车让行", "需要停车让行" },
    [VOICE_ALARM_RAILWAY]      = { "前方有铁路道口，注意观察", "有铁路道口，注意观察" },
    [VOICE_ALARM_PEDESTRIAN]   = { "前方有人行横道，注意行人", "有人行横道，注意行人" },
};

int voice_alarm_phrase(int alarm, double dist, double tunnel_len_m,
                        char *buf, size_t n)
{
    if (!buf || n == 0) return 0;
    buf[0] = '\0';
    if (alarm <= VOICE_ALARM_NONE || alarm >= VOICE_ALARM_COUNT) return 0;

    char d[32];
    voice_spoken_distance(dist, d, sizeof d);

    /* 隧道是区间型：官方 Alarm.phrase() 额外报全长
     *   "，全长 " + (len >= 1000 ? Units.dist(len) : round(len/50)*50 + " 米")
     *   （近处不报距离前缀，只报"进入隧道"）。照抄。 */
    if (alarm == VOICE_ALARM_TUNNEL) {
        char len[32];
        if (tunnel_len_m >= 1000.0) {
            voice_spoken_distance(tunnel_len_m, len, sizeof len);
        } else {
            snprintf(len, sizeof len, "%.0f 米",
                     (double)((long)((tunnel_len_m / 50.0) + 0.5) * 50));
        }
        if (dist <= 60.0) return snprintf(buf, n, "进入隧道，全长 %s", len);
        return snprintf(buf, n, "前方%s进入隧道，全长 %s", d, len);
    }

    /* 其余告警：近处整句，远处"前方 X 米" + 核心短语（见上表注释）。 */
    if (dist <= 60.0) return snprintf(buf, n, "%s", g_alarm[alarm].say);
    return snprintf(buf, n, "前方%s%s", d, g_alarm[alarm].core);
}

/* ---------------------------------------------------------------------------
 * 3) 沿路告警扫描（抄 Alarms.java + Navigator.findAlarm + Alarms.priority）
 *
 * 本项目的数据源是**边标志位**（EF_TUNNEL / EF_TOLL / EF_BRIDGE），
 * 官方是**点标注**（speed_camera / toll_booth…）。骨架照抄：
 *   · 官方 Alarms.fromSegments：连续 tunnel 段合并成一条区间告警，
 *     长度 < MIN_TUNNEL_M(60m) 就丢掉（避免地下通道刷屏）。
 *   · 官方 Navigator.findAlarm：遍历全部告警，用 Alarms.priority() 打分，
 *     取最优的一个（分数小者优；同分取近的）。
 *   · 官方 Alarms.priority(a, timeSec, dist)：
 *       dist > 1500            ⇒ 不提示
 *       timeSec < 6 || dist < 75  ⇒ 该类优先级
 *       摄像头类再放宽到 timeSec<15 || dist<150
 *       收费站再放宽到 timeSec<30 || dist<500
 *       timeSec < 7 || dist < 100 ⇒ 优先级 + MAXIMUM 的 10
 *       都不满足                 ⇒ 不提示
 * ------------------------------------------------------------------------- */

/* 官方 AlarmType 的优先级（Android AlarmInfoType 与 nav-desktop AlarmType 同值）。 */
static int alarm_type_priority(int type)
{
    switch (type) {
    case VOICE_ALARM_SPEED_CAMERA: return 1;
    case VOICE_ALARM_TOLL:         return VOICE_ALARM_PRIO_TOLL;
    case VOICE_ALARM_STOP:         return 7;
    case VOICE_ALARM_RAILWAY:      return 4;
    case VOICE_ALARM_PEDESTRIAN:   return 8;
    case VOICE_ALARM_TUNNEL:       return VOICE_ALARM_PRIO_TUNNEL;
    case VOICE_ALARM_BRIDGE:       return VOICE_ALARM_PRIO_BRIDGE;
    default:                       return VOICE_ALARM_PRIO_MAXIMUM;
    }
}

/* 官方 Alarms.priority(a, timeSec, dist)：返回越小越该提示，
 * VOICE_ALARM_PRIO_MAXIMUM+10(=20) 以上视为"不提示"（官方用 Integer.MAX_VALUE）。 */
static int alarm_priority(int type, double time_sec, double dist_m)
{
    if (dist_m > VOICE_ALARM_MAX_DIST_M) return 999;   /* 官方 Integer.MAX_VALUE */

    if (time_sec < 6.0 || dist_m < 75.0) return alarm_type_priority(type);

    /* 摄像头类：官方再放宽一档 */
    if (type == VOICE_ALARM_SPEED_CAMERA &&
        (time_sec < 15.0 || dist_m < 150.0))
        return alarm_type_priority(type);

    /* 收费站：官方再放宽一档 */
    if (type == VOICE_ALARM_TOLL &&
        (time_sec < 30.0 || dist_m < 500.0))
        return alarm_type_priority(type);

    /* 最后一档：官方用 type.priority + MAXIMUM.priority */
    if (time_sec < 7.0 || dist_m < 100.0)
        return alarm_type_priority(type) + VOICE_ALARM_PRIO_MAXIMUM;

    return 999;
}

/* 一个待考虑的告警实例（扫描期内的中间量）。 */
typedef struct {
    int    type;
    double dist_m;      /* 距当前位置 */
    double len_m;       /* 区间型总长（隧道） */
    double end_m;       /* 区间型终点里程（点状 = 起点） */
    int    inst;        /* 实例号（见下） */
} AlarmCand;

/* 追加一个候选。内存不够就返回 -1（调用方放弃本次扫描，不崩）。 */
static int alarm_push(AlarmCand **cand, size_t *n, size_t *cap,
                      int type, double dist, double len, double end_m, int inst)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        AlarmCand *tmp = (AlarmCand *)realloc(*cand, nc * sizeof *tmp);
        if (!tmp) return -1;
        *cand = tmp;
        *cap = nc;
    }
    (*cand)[*n].type   = type;
    (*cand)[*n].dist_m = dist;
    (*cand)[*n].len_m  = len;
    (*cand)[*n].end_m  = end_m;
    (*cand)[*n].inst   = inst;
    (*n)++;
    return 0;
}

int voice_scan_alarm(const RouteResult *rr, double from_m, double speed_ms,
                      VoiceAlarm *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!rr || !rr->ok || rr->n < 2) return 0;
    if (!rr->edge_flags || !rr->seg_m) return 0;

    uint32_t nseg = rr->n - 1;
    double speed = speed_ms > 0.5 ? speed_ms : 12.0;   /* 官方 Navigator 的兜底 12 m/s */

    /* 沿里程扫：官方 Alarms.fromSegments 就是按段序累加里程。
     * 官方用的是**点标注**在段内的位置；本项目只有**整段**的边标志位
     * ⇒ 告警点取"该段起点"，区间型（隧道）取整段合并后的区间。
     *
     * ★★ 实例号 inst 必须**由位置派生**（这里是段下标），不能是"本次扫描
     *   见到的第几个候选"：
     *   官方用 IdentityHashMap<Alarm,Integer>，key 是 Alarm **对象身份**
     *   —— 那批 Alarm 是 Alarms.fromSegments 在**构建路线时一次性**生成的，
     *   之后每次 findAlarm 都遍历同一批对象 ⇒ 身份**与当前位置无关**、恒定。
     *   早期版本用"扫描计数器"当 inst ⇒ 被"已过去 20 m"过滤掉的候选不占号，
     *   于是**同一段隧道在两次扫描里拿到不同的 inst** ⇒ 已播级别记到了
     *   别的告警头上 ⇒ 该播的第二次告警被静默吞掉（实测踩过）。
     *   段下标是位置恒定的标识 ⇒ 与官方对象身份等价。 */
    double acc = 0.0;              /* 当前段起点的里程 */

    /* 候选表：一条路线上的告警不会太多（隧道/收费站/桥梁各若干段），
     * 但为了不写死上限，用动态数组。 */
    AlarmCand *cand = NULL;
    size_t n_cand = 0, cap_cand = 0;

    /* 隧道是区间型：连续 tunnel 段合并成一条（官方 fromSegments 同款）。
     * 官方还有 MIN_TUNNEL_M：太短的隧道不提示（避免地下通道刷屏）。 */
    double tunnel_start = -1.0;
    double tunnel_len  = 0.0;
    int    tunnel_inst  = 0;       /* 隧道实例号 = 其**首段**的段下标 */

    /* 官方 Navigator.findAlarm 的两条过滤：
     *   · 区间型（隧道）已经在里面 ⇒ st.inTunnel = true，跳过（不重复播）
     *   · 点状告警已经过去 20m 以上 ⇒ 跳过
     * 本模块照抄。 */
    for (uint32_t i = 0; i < nseg; i++) {
        double seg_len = (double)rr->seg_m[i];
        uint8_t fl = rr->edge_flags[i];

        /* ---- 隧道区间：连续段合并 ---- */
        if (fl & EF_TUNNEL) {
            if (tunnel_start < 0.0) {
                tunnel_start = acc;
                tunnel_len = 0.0;
                /* ★ 实例号 = 首段的段下标（位置恒定，见上面的说明）。
                 *   合并后的隧道区间可能横跨多段，但它的"身份"由**首段**
                 *   决定 ⇒ 无论从哪个位置扫，得到的 inst 都一样。 */
                tunnel_inst = (int)i;
            }
            tunnel_len += seg_len;
        } else if (tunnel_start >= 0.0) {
            /* 隧道结束：官方 tooShortForPrompt 过滤 */
            if (tunnel_len >= VOICE_ALARM_MIN_TUNNEL_M) {
                double d = tunnel_start - from_m;
                double end_m = tunnel_start + tunnel_len;
                /* 已在隧道里 ⇒ 跳过（官方 findAlarm 的区间型分支） */
                if (!(from_m >= tunnel_start && from_m <= end_m) && d > -20.0) {
                    if (d < 0.0) d = 0.0;
                    if (alarm_push(&cand, &n_cand, &cap_cand, VOICE_ALARM_TUNNEL,
                                   d, tunnel_len, end_m, tunnel_inst) != 0) {
                        free(cand); return 0;
                    }
                }
            }
            tunnel_start = -1.0;
            tunnel_len = 0.0;
        }

        /* ---- 点状告警：收费站 / 桥梁（取段起点）---- */
        for (int t = 0; t < 2; t++) {
            int type = (t == 0) ? VOICE_ALARM_TOLL : VOICE_ALARM_BRIDGE;
            uint16_t bit = (t == 0) ? EF_TOLL : EF_BRIDGE;
            if (!(fl & bit)) continue;
            double d = acc - from_m;
            if (d < -20.0) continue;             /* 官方：已经过去 */
            if (d < 0.0) d = 0.0;
            if (alarm_push(&cand, &n_cand, &cap_cand, type, d, 0.0, acc,
                           (int)i) != 0) {
                free(cand); return 0;
            }
        }

        acc += seg_len;
    }

    /* 路线末尾还开着隧道（隧道一直到最后） */
    if (tunnel_start >= 0.0 && tunnel_len >= VOICE_ALARM_MIN_TUNNEL_M) {
        double d = tunnel_start - from_m;
        double end_m = tunnel_start + tunnel_len;
        if (!(from_m >= tunnel_start && from_m <= end_m) && d > -20.0) {
            if (d < 0.0) d = 0.0;
            if (alarm_push(&cand, &n_cand, &cap_cand, VOICE_ALARM_TUNNEL,
                           d, tunnel_len, end_m, tunnel_inst) != 0) {
                free(cand); return 0;
            }
        }
    }

    /* 选最优：官方 Navigator.findAlarm —— priority 小者优，同 priority 取近的。
     * 官方遍历的是**全部**告警；这里同样遍历全部候选（数量很小）。 */
    int best = -1;
    int best_prio = 999;
    for (size_t k = 0; k < n_cand; k++) {
        double d = cand[k].dist_m;
        double t = d / speed;
        int pr = alarm_priority(cand[k].type, t, d);
        if (pr >= 999) continue;                 /* 官方：不提示 */
        if (best < 0 || pr < best_prio ||
            (pr == best_prio && d < cand[best].dist_m)) {
            best = k;
            best_prio = pr;
        }
    }

    int found = 0;
    if (best >= 0) {
        out->type   = cand[best].type;
        out->dist_m = cand[best].dist_m;
        out->len_m  = cand[best].len_m;
        out->inst   = cand[best].inst;
        found = 1;
    }
    free(cand);
    return found;
}

/* ---------------------------------------------------------------------------
 * 4) 播报节奏（抄 nav-desktop VoiceRouter 的状态与冷却）
 * ------------------------------------------------------------------------- */

void voice_router_reset(VoiceRouter *vr)
{
    if (!vr) return;
    memset(vr, 0, sizeof *vr);
    vr->last_man_idx = -1;
    vr->last_state   = -1;
    vr->last_limit_kmh = -1;
    for (int i = 0; i < VOICE_ALARM_SLOTS; i++) vr->alarm_slot[i].type = -1;
}

/* 同文本冷却：官方 say() 里 "t.equals(lastText) && now-lastTextAt < 15000 ⇒ 丢弃"。 */
static int say_dup(VoiceRouter *vr, const char *text, uint64_t now)
{
    if (vr->last_text[0] && strcmp(vr->last_text, text) == 0 &&
        now - vr->last_text_ms < VOICE_COOLDOWN_SAME_TEXT_MS)
        return 1;
    size_t used = strlen(text);
    if (used >= sizeof vr->last_text) used = sizeof vr->last_text - 1;
    memcpy(vr->last_text, text, used);
    vr->last_text[used] = '\0';
    vr->last_text_ms = now;
    return 0;
}

/* 官方 VoiceRouter.turnPhrase() 的路口合并判据用的 distToNext：
 *   official finishManeuvers(): mvs[i].distToNext = mvs[i+1].distFromStart - mvs[i].distFromStart
 *   ⇒ distToNext[i] = **第 i 条到第 i+1 条**的距离。
 *   官方 VoiceRouter 用的是 st.after.distToNext（= 第 i+1 条到第 i+2 条），
 *   而官方 NavTop 面板用的也是 st.after.distToNext —— 两处一致，
 *   但官方自测 VoiceRouterProbe 设的却是"下一条路口离当前多远"（nx.distToNext=80
 *   表示"与下一路口过近"）—— 与 VoiceRouter 的读法差一条。
 *   ★ 本项目照**官方代码**（VoiceRouter.java:148）实现：合并判据看
 *   "当前这条到下一条的距离"（ml->v[i+1] 相对 ml->v[i] 的里程差）。
 *   本项目 Maneuver.dist_m 的语义是"距**上一条**指令的路程"（maneuver.c 里
 *   acc 累加后清零），正好与官方 distFromStart 链同构 ⇒ 官方 distToNext[i]
 *   等于本项目 dist_m[i+1] - dist_m[i]… 但 dist_m 各自从 0 起算，
 *   所以直接用"下一条的起始里程 - 当前条的起始里程"。
 *   这里用 ManeuverList 的累加口径重算一条 distToNext 链。 */

/* 累加出第 idx 条指令的起始里程（官方 distFromStart 的等价量）。
 *
 * ★ 本项目 Maneuver.dist_m 的语义是"距**上一条**指令的路程"（maneuver.c：
 *   acc 逐段累加、push 时写入、随即清零），且 v[0]（起点）的 dist_m = 0。
 *   ⇒ 第 idx 条的起始里程 = sum(dist_m[1..idx])，**不是** sum(dist_m[0..idx])。
 *   这个口径与 navsim_next_maneuver 的 cum 累加完全一致
 *   （那里也是 cum += v[i].dist_m 后与 here 比较）——两边必须同口径，
 *   否则"距下一条转向"与"距下一条告警"会各算各的。
 *   自测里 navsim 侧的判据（maneuver_text 的距离）能反过来校验这条。 */
static double maneuver_start_m(const ManeuverList *ml, int idx)
{
    double acc = 0.0;
    for (int i = 1; i <= idx && i < ml->n; i++) acc += ml->v[i].dist_m;
    return acc;
}

/* 官方 distToNext 链：返回第 idx 条到第 idx+1 条的距离；没有下一条 ⇒ -1。
 *
 * ★★★ 这里有一处**官方的 off-by-one**，必须说清楚（否则抄出来的行为是错的）：
 *
 *   生产方（nav-desktop RouteEngine.finishManeuvers:257 与
 *   mergeAndLinkManeuvers:555）：
 *       mvs.get(i).distToNext = mvs.get(i + 1).distFromStart - mvs.get(i).distFromStart
 *   ⇒ 严格定义是 "第 i 条到第 i+1 条的距离"。
 *
 *   消费方（VoiceRouter.java:148 与 ui/panel/NavTop.java:149）**都**读
 *   `st.after.distToNext`，而 `st.after` = maneuvers[idx+1]：
 *       - VoiceRouter：判"相邻路口过近 ⇒ 合并播报『然后紧接着<after 的动作>』"
 *       - NavTop    ：显示 "然后 {dist} 米 后 {after 的转向}"
 *   两处都把 `after.distToNext` 当成"**当前这条到下一条**的距离"来用。
 *
 *   官方自己的自测（test/osmand/nav/VoiceRouterProbe.java:119）也是这个意图：
 *       nx.distToNext = 80;   // 与下一路口过近 ⇒ 应合并
 *       （nx 就是 after ⇒ 它把 after.distToNext 当"到下一路口的距离"）
 *
 *   ⇒ 生产方与消费方差一条索引。**按字面照抄生产方**会让"然后紧接着右转"
 *     在"下一条的**再下一条**离下一条很近"时才触发，而播出来的却是 after
 *     的动作 ⇒ 语义对不上。按**消费方意图**（当前→下一条的距离）实现，
 *     与官方面板显示、官方自测断言、以及播报语句本身的语义三者一致。
 *   本函数返回的正是"第 idx 条到第 idx+1 条的距离"，
 *   ⇒ 判据应取 idx = **当前**那条（不是 after 那条）。
 *
 *   本项目 Maneuver.dist_m 的语义是"距**上一条**指令的路程"（maneuver.c 里
 *   acc 累加后清零），与官方 distFromStart 链同构 ⇒ 官方 distToNext[i]
 *   等于本项目 start_m(i+1) - start_m(i)，可以直接复用。 */
static double maneuver_dist_to_next(const ManeuverList *ml, int idx)
{
    if (!ml || idx < 0 || idx + 1 >= ml->n) return -1.0;
    double a = maneuver_start_m(ml, idx);
    double b = maneuver_start_m(ml, idx + 1);
    return b - a;
}

int voice_router_maybe_say(VoiceRouter *vr, int man_idx, const ManeuverList *ml,
                            double to_go_m, double speed_ms,
                            char *out_text, size_t n, uint64_t now_ms)
{
    if (!vr || !out_text || n == 0) return 0;
    out_text[0] = '\0';

    /* 换了指令 ⇒ 层级重置（官方 st.index != lastIdx ⇒ lastStage = -1）。 */
    if (man_idx != vr->last_man_idx) {
        vr->last_man_idx = man_idx;
        vr->last_state   = -1;
    }

    const Maneuver *man = (ml && man_idx >= 0 && man_idx < ml->n) ? &ml->v[man_idx] : NULL;

    int st = voice_announce_state(to_go_m, speed_ms);
    if (st == VOICE_NONE) return 0;
    /* 同一指令只播更紧急的层级（官方 stage > lastStage 才 say）。
     * ★ 这条同时实现了官方的 isTurnStateNotPassed()：层级只会变紧急，
     *   不会退回 ⇒ 越过下界的旧层级不会被补播。 */
    if (st <= vr->last_state) return 0;
    vr->last_state = st;

    int now = (st == VOICE_TURN_NOW);
    voice_turn_phrase(man, to_go_m, now, out_text, n);
    if (!out_text[0]) { vr->last_state = -1; return 0; }

    /* 相邻路口合并（官方 turnPhrase() 末尾）：
     *   if (after != null && after.distToNext >= 0 && after.distToNext < 150
     *       && after.turnType >= 0)
     *       sb.append("，然后紧接着").append(action(after.turnType, ...));
     * ★ 官方读的是 st.after.distToNext。官方的 distToNext 生产方/消费方差一条
     *   索引（见 maneuver_dist_to_next 的注释），按消费方意图（官方面板与官方
     *   自测都按"当前→下一条"读）⇒ 这里取 **当前**那条的 distToNext。 */
    if (ml && man_idx + 1 < ml->n) {
        const Maneuver *after = &ml->v[man_idx + 1];
        double dtn = maneuver_dist_to_next(ml, man_idx);
        if (dtn >= 0.0 && dtn < VOICE_MERGE_M && after->type != MAN_ARRIVE) {
            /* ★ action_text 返回的可能是静态缓冲 ⇒ 立刻拷进本地数组，
             *   否则再调一次就把上一句的缓冲覆盖了。 */
            char nact[64];
            snprintf(nact, sizeof nact, "%s",
                     voice_action_text(after->type, after->exit_no));
            size_t used = strlen(out_text);
            if (used < n) {
                snprintf(out_text + used, n - used, "，然后紧接着%s", nact);
            }
        }
    }

    if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
    return 1;
}

int voice_router_maybe_say_limit(VoiceRouter *vr, int limit_kmh, int cur_kmh,
                                  char *out_text, size_t n, uint64_t now_ms)
{
    if (!vr || !out_text || n == 0) return 0;
    out_text[0] = '\0';

    /* 官方 speedLimit() 的两段：
     *   1) 限速变化播报（首次不播、变化 >= 10、冷却 60s）
     *   2) 超速播报（官方 st.overSpeed：车速 > 限速 + 10/3.6 m/s，冷却 45s）
     * ★ 本项目**没有超速数据源**（navsim 只给"当前段按限速走"，没有独立的
     *   实测车速），所以第 2 段在本项目里恒不触发——但保留它是为了
     *   官方面貌完整 + 将来接上真实车速就能用；判据里也留了入口。 */
    if (limit_kmh > 0) {
        if (vr->last_limit_kmh < 0) {
            /* 官方：首次不播（一上路就说话太吵）。 */
            vr->last_limit_kmh = limit_kmh;
        } else {
            int diff = limit_kmh - vr->last_limit_kmh;
            if (diff < 0) diff = -diff;
            if (diff >= 10 &&
                (!vr->has_limit_said ||
                 now_ms - vr->last_limit_ms >= VOICE_COOLDOWN_LIMIT_MS)) {
                vr->last_limit_kmh = limit_kmh;
                vr->last_limit_ms  = now_ms;
                vr->has_limit_said = 1;
                snprintf(out_text, n, "前方限速 %d 公里每小时", limit_kmh);
                /* 官方 say() 是**所有**播报的出口 ⇒ 同文本冷却对限速也生效。
                 * ★ 官方这里是"先记状态再 say"：即使 say() 把文本丢了，
                 *   lastLimitKmh 也已经更新 ⇒ 这条限速变化就算说过了，
                 *   不会因为文本重复而稍后再补一句。这里照抄。 */
                if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
                return 1;
            }
        }
    } else {
        vr->last_limit_kmh = -1;
    }

    /* 超速：官方 "您已超速，当前限速 N 公里每小时" */
    if (limit_kmh > 0 && cur_kmh > limit_kmh + 10) {
        if (!vr->has_over_said ||
            now_ms - vr->last_over_ms >= VOICE_COOLDOWN_OVER_MS) {
            vr->last_over_ms  = now_ms;
            vr->has_over_said = 1;
            snprintf(out_text, n, "您已超速，当前限速 %d 公里每小时", limit_kmh);
            if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
            return 1;
        }
    }
    return 0;
}

/* 告警实例的级别表：官方用 IdentityHashMap<Alarm,Integer>（按对象身份记），
 * 这里用 (type, inst) 记，等效。返回槽位下标；找不到就找一个空槽/最老的槽。 */
static int alarm_slot_find(VoiceRouter *vr, int type, int inst)
{
    int free_slot = -1;
    int oldest = -1;
    for (int i = 0; i < VOICE_ALARM_SLOTS; i++) {
        if (vr->alarm_slot[i].type == (int16_t)type && vr->alarm_slot[i].inst == (int32_t)inst)
            return i;
        if (vr->alarm_slot[i].type < 0 && free_slot < 0) free_slot = i;
        if (oldest < 0 || vr->alarm_slot[i].stage < vr->alarm_slot[oldest].stage) oldest = i;
    }
    if (free_slot >= 0) return free_slot;
    /* 全满 ⇒ 复用"已播到最高级"的那个槽（它不会再被用到） */
    for (int i = 0; i < VOICE_ALARM_SLOTS; i++)
        if (vr->alarm_slot[i].stage >= 2) return i;
    return oldest;
}

int voice_router_maybe_say_alarm(VoiceRouter *vr, const VoiceAlarm *al,
                                  char *out_text, size_t n, uint64_t now_ms)
{
    if (!vr || !out_text || n == 0) return 0;
    out_text[0] = '\0';
    if (!al || al->type <= VOICE_ALARM_NONE || al->type >= VOICE_ALARM_COUNT) return 0;

    /* 官方：每个告警最多两级 —— 进范围报一次（远），60 m 内再补一句（近）。 */
    int stage = (al->dist_m <= 60.0) ? 2 : 1;

    int slot = alarm_slot_find(vr, al->type, al->inst);
    if (vr->alarm_slot[slot].type == (int16_t)al->type &&
        vr->alarm_slot[slot].inst == (int32_t)al->inst &&
        stage <= vr->alarm_slot[slot].stage)
        return 0;

    /* 与转向播报错开，避免抢话（官方 lastAlarmSay 的 3000 ms）。 */
    if (vr->has_alarm_said &&
        now_ms - vr->last_alarm_ms < VOICE_COOLDOWN_ALARM_MS) return 0;

    voice_alarm_phrase(al->type, al->dist_m, al->len_m, out_text, n);
    if (!out_text[0]) return 0;

    /* ★ 官方顺序：alarmStage.put(a, stage) 在 say() **之前**
     *   ⇒ 即使 say() 把文本当重复丢了，这一级的"已播"也已经记上，
     *   不会稍后再补一句。这里照抄（先把状态记上，再过 say_dup）。 */
    vr->alarm_slot[slot].type  = (int16_t)al->type;
    vr->alarm_slot[slot].inst  = (int32_t)al->inst;
    vr->alarm_slot[slot].stage = (uint8_t)stage;
    vr->last_alarm_ms = now_ms;
    vr->has_alarm_said = 1;
    /* 官方 say() 是所有播报的出口 ⇒ 同文本冷却对告警也生效。 */
    if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
    return 1;
}

int voice_router_maybe_say_arrive(VoiceRouter *vr, double dist_remaining,
                                   char *out_text, size_t n, uint64_t now_ms)
{
    if (!vr || !out_text || n == 0) return 0;
    out_text[0] = '\0';
    if (vr->arrived_said) return 0;
    if (dist_remaining < 0.0 || dist_remaining > VOICE_ARRIVE_M) return 0;

    if (vr->has_arrive_said &&
        now_ms - vr->last_arrive_ms < VOICE_COOLDOWN_ARRIVE_MS) return 0;

    vr->arrived_said = 1;
    vr->last_arrive_ms = now_ms;
    vr->has_arrive_said = 1;
    /* 官方 arrival()：distRemaining < 200 时播 "前方即将到达目的地"。 */
    snprintf(out_text, n, "前方即将到达目的地");
    /* 官方 say() 是所有播报的出口 ⇒ 同文本冷却对到达播报也生效。
     * ★ 官方顺序：arrivedSaid = true 先置位，再 say() ⇒ 即使文本被去抖丢掉，
     *   这一帧的"已播"也记上了（不会稍后再补一句）。这里照抄。 */
    if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
    return 1;
}

/* ---------------------------------------------------------------------------
 * 5) 每帧编排（抄 nav-desktop VoiceRouter.update()）
 *
 * ★ 官方 update() 的顺序（VoiceRouter.java:73-77）：
 *       speedLimit(st, speedMps, now);
 *       arrival(st, now);
 *       alarm(st, now);
 *       …转向…
 *   严格照抄这个顺序。
 *
 * ★ 为什么"编排"也要下沉到 voice.c（而不是留在 window.c）：
 *   官方的"该播什么"是**一个**方法里的顺序决策，不是四个各自独立的 if。
 *   顺序会影响**同文本冷却**的结果：官方的 say() 是所有播报的唯一出口，
 *   同文本冷却对限速/到达/告警/转向一视同仁。
 *   ★ 本模块之前把 say_dup() 只做在转向那条路上 ⇒ 等于只抄了官方行为的一
 *     部分：限速/告警/到达的重复文本不会被去抖。已修（见上方三处 say_dup）。
 *   把编排放在这里还有个好处：**接线后的行为**能在无头 CLI 里端到端断言
 *   （tools/voice_e2e.c 走的就是这条生产路径，不是副本）。 */

/* 追加一句到帧缓冲。 */
static void frame_push(VoiceFrame *f, const char *text)
{
    if (!f || f->n >= VOICE_TICK_MAX || !text || !text[0]) return;
    size_t l = strlen(text);
    if (l >= sizeof f->text[0]) l = sizeof f->text[0] - 1;
    memcpy(f->text[f->n], text, l);
    f->text[f->n][l] = '\0';
    f->n++;
}

int voice_tick(VoiceRouter *vr, const RouteResult *rr, const ManeuverList *ml,
               int man_idx, double to_go_m, double speed_ms, double remain_m,
               int arrived, uint64_t now_ms, VoiceFrame *out)
{
    if (!out) return 0;
    out->n = 0;
    if (!vr) return 0;

    char buf[192];

    /* ---- 1) 限速（官方 speedLimit）----
     * 官方 kmh 取整：(int) Math.round(st.speedLimit * 3.6 / 5.0) * 5。
     * 本项目的"限速"就是当前段的速度上限（与模拟车速同源），取整照抄。 */
    int limit_kmh = (int)((speed_ms * 3.6 + 2.5) / 5.0) * 5;
    if (voice_router_maybe_say_limit(vr, limit_kmh, (int)(speed_ms * 3.6),
                                     buf, sizeof buf, now_ms))
        frame_push(out, buf);

    /* ---- 2) 到达（官方 arrival）---- */
    if (voice_router_maybe_say_arrive(vr, remain_m, buf, sizeof buf, now_ms))
        frame_push(out, buf);

    /* ---- 3) 沿路告警（官方 Alarms + Navigator.findAlarm）----
     * 扫描用"当前已走里程"作基准 ⇒ 只看前方的告警。 */
    if (rr) {
        double along = rr->total_m - remain_m;
        if (along < 0.0) along = 0.0;
        VoiceAlarm al;
        if (voice_scan_alarm(rr, along, speed_ms, &al)) {
            if (voice_router_maybe_say_alarm(vr, &al, buf, sizeof buf, now_ms))
                frame_push(out, buf);
        }
    }

    /* ---- 4) 转向（官方 turnPhrase）----
     * 官方把转向放最后：它是最重要的一句，不该被前面的限速/告警盖掉。 */
    if (ml && ml->n > 0 && man_idx >= 0 && man_idx < ml->n &&
        to_go_m >= 0.0) {
        if (voice_router_maybe_say(vr, man_idx, ml, to_go_m, speed_ms,
                                   buf, sizeof buf, now_ms))
            frame_push(out, buf);
    }

    /* ---- 5) 终点收尾语 ----
     * 官方 nav-desktop 的 "您已到达目的地附近，导航结束" 属于**界面**层
     * （NavigationDialogs.stopNavigation），不在 VoiceRouter 里。
     * 本项目把它放在同一处播（都是同一句话、同一个 TTS 通道），
     * 由调用方的 ann_finished 标志保证只播一次 —— 见 window.c。
     * ★ 这里只把"该播"这个决定做出来，文本由调用方取（官方同款分层）。 */
    (void)arrived;
    return out->n;
}
