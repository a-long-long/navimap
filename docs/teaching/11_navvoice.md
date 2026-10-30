# 第 11 课：导航层与语音

> 模块：`src/route/voice.{c,h}`、`src/ui/voice_wire.{c,h}`、`src/utils/plat.{c,h}`（重点 `nm_tts_speak`）
> 前置：第 10 课 `RouteResult`、GPS/模拟定位概念、TTS 冷却概念。

## 一、教学目的

理解从"一条路线"到"可播报的导航"的最后一公里，重点是一个极易出 bug 的架构纪律：**`voice.c` 只决定"说什么"（纯逻辑、零平台依赖），发声与接线分离，且接线只有一份**。

学完后学生应能：

- 解释为什么 `voice_wire.c` 是**唯一接线点**，生产与 `tools/voice_e2e.c` 自测共用同一份代码；
- 说明"到达帧例外"为什么必须先播报（`arrived` 那一帧不受 `paused` 抑制），否则收尾语永远播不出来（这是实打实踩过的 bug）；
- 解释 15s 同文本冷却的作用——`say()`（`say_dup`）是**所有**播报（限速/到达/告警/转向）的唯一出口，四类播报一视同仁；
- 区分"该播第几级"（策略，`voice_announce_state`）与"这句话怎么说"（话术，`voice_*_phrase`）两件事，二者都在 `voice.c`，而真正"出声"只在 `plat.c` 的 `nm_tts_speak`。

## 二、功能构成

| 文件 | 职责 | 关键位置 |
|---|---|---|
| `src/route/voice.h` | 播报策略的公开接口：分级提前量常量、告警类型、冷却常量、`VoiceRouter`/`VoiceFrame` 结构、各 `voice_*` 原型 | `voice.h:1` 起 |
| `src/route/voice.c` | **纯逻辑**：只决定"该播第几级 + 这句话怎么说"，不碰发声/线程 | `voice.c:1` 起 |
| `src/ui/voice_wire.h` | 接线层的状态集合 `NavVoiceCtx` 与入口 `nav_voice_frame` 声明 | `voice_wire.h:36` |
| `src/ui/voice_wire.c` | **唯一接线点**：取状态 → `voice_tick` 编排 → `nm_tts_speak` 发声；到达帧例外处理 | `voice_wire.c:15` |
| `src/utils/plat.h` | 平台抽象层声明，含离线 TTS 接口 `nm_tts_speak` 等 | `plat.h:150` 起 |
| `src/utils/plat.c` | `nm_tts_speak` 实现（常驻会话 + 队列 + 后台朗读线程） | `plat.c:1127` |

**分层要点**

- **策略层**：`src/route/voice.c`——抄写自 OsmAnd，纯逻辑、零平台依赖（不碰 SDL、不碰线程、不发声）。回答两个问题："现在该播第几级？"（`voice_announce_state`）和"这句话怎么说？"（`voice_turn_phrase`/`voice_alarm_phrase`/`voice_spoken_distance`）。
- **接线层**：`src/ui/voice_wire.c`——取哪些状态、按什么顺序交给 TTS。这一帧的编排（`voice_tick`）本身在 `voice.c`，接线层只负责"取状态 + 发声"。
- **发声层**：`nm_tts_speak`（`src/utils/plat.c`）——跨平台离线 TTS，Windows 走 PowerShell+SAPI 常驻会话，其它平台探测 espeak-ng/espeak/say/festival。

关键结构（均在 `voice.h`）：

- `VoiceRouter`（`voice.h:237`）：播报器状态，持有"播过什么"的全部记忆（层级、同文本冷却文本与时间、限速/超速/到达/告警的冷却与 `has_*` 标志）。
- `VoiceAlarm`（`voice.h:190`）/ `VoiceAlarmSlot`（`voice.h:230`）：沿路告警实例与已播级别表（官方 `IdentityHashMap` 的等价物）。
- `VoiceFrame`（`voice.h:335`）：一帧的播报决策结果，最多 `VOICE_TICK_MAX=4` 条。

## 三、完整实现代码

### 3.1 策略层接口 `src/route/voice.h`（完整）

```h
#ifndef NAVIMAP_VOICE_H
#define NAVIMAP_VOICE_H

#include <stddef.h>
#include <stdint.h>
#include "route/maneuver.h"
#include "route/route.h"

/* ---------------------------------------------------------------------------
 * 语音播报策略（抄写自 OsmAnd）
 *
 * ★ 分层抄，每层各取一个官方源（都在 E:\Osmand 里）：
 *
 *   1) 分级提前量  ← OsmAnd/OsmAnd/src/net/osmand/plus/routing/data/
 *                     AnnounceTimeDistances.java
 *                  官方唯一的"速度 × 秒数"模型（nav-desktop 自己的注释也写着
 *                  "对标官方 VoiceRouter + AnnounceTimeDistances"）。
 *   2) 中文话术    ← nav-desktop/src/main/osmand/nav/route/VoiceRouter.java
 *                    + nav-desktop/src/main/osmand/nav/model/Alarm.java
 *                    + nav-desktop/src/main/osmand/nav/model/AlarmType.java
 *                  这是 OsmAnd 仓库里**唯一的中文桌面语音导航实现**（纯 Java、
 *                  无 Android 依赖），与本项目同构：同样的"分级 / 路口合并 /
 *                  同文本去抖 / 告警两级 / 限速去抖"。而且它的 action() 词表
 *                  与本项目面板文案（maneuver_kind_text）逐条对得上
 *                  ⇒ 面板显示什么，语音就说什么。
 *   3) 沿路告警    ← nav-desktop/src/main/osmand/nav/route/Alarms.java
 *                    + nav-desktop/src/main/osmand/nav/route/Navigator.java
 *                    （findAlarm）+ nav-desktop/src/main/osmand/nav/core/Units.java
 *
 * 为什么值得抄：原来的播报是"固定 300m / 60m 两级 + 一句直白文案"，
 * 高速上根本来不及提醒（120 km/h 时 300 m 只剩 9 秒），而且没有"现在左转"、
 * 没有相邻路口合并、没有去抖 ⇒ 连续两个路口会连着刷屏。OsmAnd 的模型是
 * "**提前量 = 速度 × 秒数**"：距路口的距离随车速自动放大，快路上提前说、
 * 慢路上不啰嗦。
 *
 * 本模块是**纯逻辑、零平台依赖**：只决定"该说什么"，不碰发声。
 * 发声在 utils/plat.c（nm_tts_speak）。这样判据能在 CLI 里无头跑。
 * ------------------------------------------------------------------------- */

/* ---- 播报层级 ----
 * 数值越大 = 越接近路口 = 越该说。VOICE_NONE = 不播。
 * ★ 判定顺序必须"从最紧急往回查"：命中就返回**最紧急**的那一级。
 *   （反过来查会把"超远预告"当成"提前预告"返回 ⇒ 高速下提前量错判。） */
enum {
    VOICE_NONE = -1,
    VOICE_PREPARE_LONG = 0,   /* 超远预告：只给高速路（官方 issue 1411） */
    VOICE_PREPARE      = 1,   /* 提前预告："前方 800 米左转" */
    VOICE_TURN_IN      = 2,   /* 临近："前方 200 米左转" */
    VOICE_TURN_NOW     = 3,   /* 立即："现在左转" */
};

/* ---- 沿路告警类型（抄 AlarmType.java）----
 * 本项目路网里没有测速摄像头那类**点标注**（OSM 标签没进 products），
 * 只有隧道/收费站/桥梁这类**边标志位**能认（EF_TUNNEL/EF_TOLL/EF_BRIDGE），
 * 所以只抄能落地的那几类（词表仍保留全部官方类型，便于话术对照与将来接数据）。 */
enum {
    VOICE_ALARM_NONE = 0,
    VOICE_ALARM_TUNNEL,       /* 隧道（官方 TUNNEL） */
    VOICE_ALARM_TOLL,         /* 收费站（官方 TOLL_BOOTH） */
    VOICE_ALARM_BRIDGE,       /* 桥梁（本项目扩展：官方 AlarmType 无此项） */
    VOICE_ALARM_SPEED_CAMERA, /* 测速摄像头 */
    VOICE_ALARM_STOP,         /* 停车让行 */
    VOICE_ALARM_RAILWAY,      /* 铁路道口 */
    VOICE_ALARM_PEDESTRIAN,   /* 人行横道 */
    VOICE_ALARM_COUNT
};

/* 官方 AlarmType.priority 的数值（越小越优先；官方 Alarms.priority() 用它排序）。
 *   TOLL_BOOTH = 6、TUNNEL = 11 —— 官方原值。
 *   BRIDGE 是本项目扩展，取 12（隧道与桥梁都是"区间型"，量级相当）。 */
#define VOICE_ALARM_PRIO_TOLL      6
#define VOICE_ALARM_PRIO_TUNNEL   11
#define VOICE_ALARM_PRIO_BRIDGE   12
/* 官方 AlarmType.MAXIMUM.priority = 10（"加塞提示"那一档，见 Alarms.priority）。 */
#define VOICE_ALARM_PRIO_MAXIMUM  10

/* ---------------------------------------------------------------------------
 * 1) 分级提前量（抄 AnnounceTimeDistances.java）
 *
 * 官方用"速度 × 秒数"：PREPARE = speed*115s、TURN_IN = speed*22s、
 * TURN_NOW = speed*min(sqrt(speed*3.6), 8)（约 6.7 秒）。
 * 本项目没有"应用模式"（车/自行车/步行），只有一个默认速度，
 * 所以把官方的**汽车档**数值固定下来，同时保留"按当前速度放大"的行为：
 * 官方 isDistanceLess() 是 dist <= max(leadDist, curSpeed/DEFAULT_SPEED*leadDist)
 * —— 车速比默认快，提前量按比例放大。这里照抄这个比例放大。
 * ------------------------------------------------------------------------- */

/* 默认速度（m/s）：官方 **car 档**的 DEFAULT_SPEED = 12.5 m/s（45 km/h）。
 * 官方出处：OsmandSettings.DEFAULT_SPEED.setModeDefaultValue(CAR, 12.5f)。
 * 官方注释里的 car 档数值都由它推出：PREPARE_LONG 3750 m / PREPARE 1438 m /
 * TURN_IN 275 m / TURN_NOW 83 m —— 本模块的公式与之逐项对齐。 */
#define VOICE_DEFAULT_SPEED_MS   12.5
/* 官方 PREPARE = speed*115s（car 1438 m）。 */
#define VOICE_PREPARE_SEC        115.0
/* 官方 PREPARE_LONG = speed*300s（car 3750 m）。 */
#define VOICE_PREPARE_LONG_SEC   300.0
/* 官方 TURN_IN = speed*22s（car 275 m）。 */
#define VOICE_TURN_IN_SEC        22.0
/* 官方 TURN_NOW ≈ speed*min(sqrt(speed*3.6),8) ⇒ car 83 m。
 * 这个常量只是"无速度时的兜底"，实际值按 sqrt 公式算（见 voice.c）。 */
#define VOICE_TURN_NOW_SEC       6.7
/* 官方 POSITIONING_TOLERANCE = 12 m（GPS 定位误差兜底）。 */
#define VOICE_POS_TOLERANCE_M    12.0
/* 官方高速门槛：speed >= 30 m/s 才播 PREPARE_LONG（issue 1411）。 */
#define VOICE_FAST_SPEED_MS      30.0
/* 到达播报距离（抄 nav-desktop VoiceRouter.arrival() 的 distRemaining < 200）。 */
#define VOICE_ARRIVE_M           200.0
/* 相邻两个路口距离小于此值 ⇒ 合并成一句播报（官方 VoiceRouter.turnPhrase 的 150 m）。 */
#define VOICE_MERGE_M            150.0

/* 当前状态下的**基准**分级提前量（米）。返回 4 个阈值，依次是
 * PREPARE_LONG / PREPARE / TURN_IN / TURN_NOW。
 *
 * ★ 基准 = 按**默认速度**算（官方就是这么做的：阈值是"车型档位"的属性，
 *   不是当前车速的属性），当前车速只用来在 voice_announce_state() 里
 *   做等比缩放。所以这个函数的参数是"默认速度"而不是"当前速度"。
 * default_speed_ms <= 0 时用 VOICE_DEFAULT_SPEED_MS。
 * ★ 与官方 isDistanceLess 一致：高速时按 cur/DEFAULT 的比例放大。 */
void voice_lead_distances(double default_speed_ms, double out_lead[4]);

/* 给定距下一条指令的距离，算该播第几级（VOICE_*）。
 * speed_ms <= 0 用默认速度。VOICE_NONE = 现在不该播。
 * ★ 与官方 getImminentTurnStatus 同义：命中就返回**最紧急**的一级。 */
int  voice_announce_state(double to_go_m, double speed_ms);

/* 便捷判定：是不是"现在"级别（该说"现在左转"而不是"前方 X 米左转"）。 */
int  voice_is_now(double to_go_m, double speed_ms);

/* ---------------------------------------------------------------------------
 * 2) 话术模板（抄 VoiceRouter.turnPhrase / action / Alarm.phrase）
 *
 * 文案风格照 OsmAnd 桌面版中文：
 *   远："前方 800 米左转，进入建国路"
 *   近："现在左转，进入建国路"
 *   合并："前方 200 米左转，然后紧接着右转"
 *   环岛："前方 300 米进入环岛，从第 3 个出口驶出"
 *   到达："前方 100 米到达目的地"
 *   告警："前方 500 米有测速摄像头"（官方原码会拼成"前方 500 米前方有…"，
 *         本模块去掉重复前缀，见 voice.c 的说明）
 * ------------------------------------------------------------------------- */

/* 转向动作短语（抄 nav-desktop VoiceRouter.action()）。
 * type 取 MAN_*；exit_no > 0 时环岛会带"从第 N 个出口驶出"。
 * ★ 返回值可能是静态缓冲，调用方必须**立即**用掉。 */
const char *voice_action_text(int man_type, int exit_no);

/* 距离口语化（抄 nav-desktop core/Units.spoken() → dist() → metersText()）。
 * |d| < 10 m   ⇒ "5.0 米"
 * |d| < 1000 m ⇒ 整十米（"240 米"）
 * |d| < 100 km ⇒ 一位小数公里（"1.5 公里"）
 * else         ⇒ 整数公里（"120 公里"）
 * 与 maneuver.c 的 fmt_distance 不同：语音用"240 米"而不是"237 米"，
 * 免得 TTS 念出一串拗口数字。 */
void voice_spoken_distance(double meters, char *buf, size_t n);

/* 完整转向句。to_go_m = 距该指令的距离（米），now != 0 ⇒ 强制"现在"句式。
 * road 可为 NULL/空。buf 至少 256 字节。
 * 返回写入的字节数。 */
int  voice_turn_phrase(const Maneuver *m, double to_go_m, int now,
                       char *buf, size_t n);

/* 告警句（抄 nav-desktop model/Alarm.phrase()）。
 * dist = 距告警点距离（米）；alarm 取 VOICE_ALARM_*；
 * tunnel_len_m 仅隧道用（<=0 表示未知）。 */
int  voice_alarm_phrase(int alarm, double dist, double tunnel_len_m,
                        char *buf, size_t n);

/* ---------------------------------------------------------------------------
 * 3) 沿路告警扫描（抄 Alarms.java + Navigator.findAlarm + Alarms.priority）
 * ------------------------------------------------------------------------- */

/* 官方 Alarms.MIN_TUNNEL_M：隧道太短不值得提示。 */
#define VOICE_ALARM_MIN_TUNNEL_M   60.0
/* 官方 Alarms.priority() 的最远提示距离。 */
#define VOICE_ALARM_MAX_DIST_M     1500.0

/* 一个沿路告警实例。 */
typedef struct {
    int      type;      /* VOICE_ALARM_* */
    int      inst;      /* 实例号：同类型的不同位置算不同实例
                         *（官方用 Alarm 对象的身份做 key，见下） */
    double   dist_m;    /* 距当前位置还有多远（米） */
    double   len_m;     /* 区间型（隧道/收费路段/桥梁）总长；点状为 0 */
} VoiceAlarm;

/* 扫前方沿路告警，返回"当前最值得提示"的那一个。
 * rr      路线（认 edge_flags 里的 EF_TUNNEL/EF_TOLL/EF_BRIDGE）
 * from_m  当前已走里程（米，沿路线）
 * speed_ms 当前速度（m/s；官方 priority() 要用它算 timeSec）
 * 返回 1 = 找到（out 填好）；0 = 没有值得提示的。
 * ★ 官方用 IdentityHashMap<Alarm,Integer> 给**每个告警实例**记已播级别，
 *   所以一条路上有两个隧道时第二个仍会播；本模块用 inst 复刻这一点。 */
int  voice_scan_alarm(const RouteResult *rr, double from_m, double speed_ms,
                      VoiceAlarm *out);

/* ---------------------------------------------------------------------------
 * 4) 播报节奏（抄 nav-desktop VoiceRouter 的状态与冷却）
 * ------------------------------------------------------------------------- */

/* 冷却时长（毫秒），抄 nav-desktop VoiceRouter 的常量。 */
#define VOICE_COOLDOWN_SAME_TEXT_MS   15000   /* 同文本冷却 */
#define VOICE_COOLDOWN_LIMIT_MS       60000   /* 限速变化播报冷却 */
#define VOICE_COOLDOWN_OVER_MS        45000   /* 超速播报冷却（官方 OVER_COOLDOWN） */
#define VOICE_COOLDOWN_ARRIVE_MS      60000   /* 到达播报冷却 */
#define VOICE_COOLDOWN_ALARM_MS        3000   /* 告警与转向播报错开 */

/* 告警实例的已播级别表（官方 IdentityHashMap 的等价物）。
 * 官方按 Alarm 对象身份记，本模块按 (type, inst) 记。
 * ★ inst = 该告警在路线里的**段下标**（位置恒定，与扫描到哪儿无关）。
 *   ⇒ 用 int32_t：长途路线段数可过 32767，int16_t 会溢出成负数，
 *     撞上别的告警的槽位。
 * ★ 槽位数取 32：官方 IdentityHashMap 是无界的（每条路线有多少告警就记多少）。
 *   本表是定长的，满了之后会复用"已播到最高级"的槽 ⇒ 极长路线上
 *   很老的告警可能被当成新告警重播一次。32 个槽足够覆盖绝大多数路线。 */
#define VOICE_ALARM_SLOTS 32
typedef struct {
    int16_t type;      /* VOICE_ALARM_*；-1 = 空槽 */
    int32_t inst;      /* 实例号（= 段下标） */
    uint8_t stage;     /* 0=未播 1=远级 2=近级 */
} VoiceAlarmSlot;

/* 播报器状态。调用方持有它（每条路线一个），随路线重置。 */
typedef struct {
    /* 上一条指令的下标与已播层级：变了下标 ⇒ 层级重置 */
    int      last_man_idx;
    int      last_state;
    /* 上一句文本与时间：同文本在冷却期内不重复播 */
    char     last_text[192];
    uint64_t last_text_ms;
    /* 限速播报：上次播的限速值与时间 */
    int      last_limit_kmh;
    uint64_t last_limit_ms;
    /* 超速播报：上次播的时间 */
    uint64_t last_over_ms;
    /* 到达播报：是否已播 + 时间 */
    int      arrived_said;
    uint64_t last_arrive_ms;
    /* 告警：上次播报时间（与转向播报错开） */
    uint64_t last_alarm_ms;
    /* 告警实例的已播级别表（官方 IdentityHashMap 的等价物） */
    VoiceAlarmSlot alarm_slot[VOICE_ALARM_SLOTS];
    /* ★ "这个冷却计时器有没有起算过"的标志。
     *   不能用"时间戳 == 0 表示没播过"——now_ms 是**调用方注入**的，
     *   自测会从 1000 开始喂，真实时钟也可能很小。官方靠
     *   System.currentTimeMillis()（epoch，永远很大）侥幸没暴露这个问题，
     *   抄过来时必须显式补这个标志。 */
    uint8_t  has_limit_said;
    uint8_t  has_over_said;
    uint8_t  has_arrive_said;
    uint8_t  has_alarm_said;
} VoiceRouter;

/* 初始化（新路线 / 重新规划后调）。 */
void voice_router_reset(VoiceRouter *vr);

/* 本帧是否该播转向。*out_text 写入话术（至少 256 字节），返回 1=该播。
 *
 * man_idx     当前指令下标（变了就重置层级）
 * ml          指令列表（官方 st.current / st.after 都从这里取）
 *             ★ 传 ml 而不是裸 Maneuver*：官方的"路口合并"判据用的是
 *               st.after.distToNext（**下一条**指令到再下一条的距离），
 *               光给一条指令算不出来。ml 可为 NULL ⇒ 不合并。
 * to_go_m     距该指令的距离（米）
 * speed_ms    当前速度（m/s）
 * out_text    输出话术
 * now_ms      当前时刻（毫秒，单调时钟）。**由调用方传入**而不是内部取
 *             —— 这样冷却逻辑能被自测确定性地驱动（自测可以随便喂
 *             "1 秒后""16 秒后"来验证冷却到期，不受真实时间影响）。
 */
int  voice_router_maybe_say(VoiceRouter *vr, int man_idx, const ManeuverList *ml,
                            double to_go_m, double speed_ms,
                            char *out_text, size_t n, uint64_t now_ms);

/* 本帧是否该播限速变化。返回 1=该播（话术写入 out_text）。
 * limit_kmh：当前限速（<=0 视为无限速）；cur_kmh：当前车速。
 * ★ 抄官方 nav-desktop VoiceRouter.speedLimit()：
 *   · 首次不播（一上路就说话太吵）
 *   · 限速变化幅度 >= 10 km/h 才播
 *   · 超速（cur_kmh > limit_kmh + 10）时另播一句"您已超速…"，冷却 45s
 *     （官方 OVER_COOLDOWN = 45000）
 * ★ 官方的 kmh 取整是 round(限速m/s * 3.6 / 5) * 5（对齐 5 的倍数），
 *   这个取整由**调用方**做（本函数收的是已取整的 km/h）。 */
int  voice_router_maybe_say_limit(VoiceRouter *vr, int limit_kmh, int cur_kmh,
                                  char *out_text, size_t n, uint64_t now_ms);

/* 本帧是否该播告警。返回 1=该播（话术写入 out_text）。
 * al 取自 voice_scan_alarm()。
 * ★ 与官方一致：每个告警实例最多两级
 *   （进范围报一次"前方 500 米有…"，60 m 内再补一句"有…"）。 */
int  voice_router_maybe_say_alarm(VoiceRouter *vr, const VoiceAlarm *al,
                                  char *out_text, size_t n, uint64_t now_ms);

/* 本帧是否该播"即将到达"。返回 1=该播。dist_remaining = 剩余里程（米）。 */
int  voice_router_maybe_say_arrive(VoiceRouter *vr, double dist_remaining,
                                   char *out_text, size_t n, uint64_t now_ms);

/* ---------------------------------------------------------------------------
 * 5) 每帧编排（抄 nav-desktop VoiceRouter.update()）
 * ------------------------------------------------------------------------- */

/* 一帧最多播几句（官方一帧最多 4 类：限速 / 到达 / 告警 / 转向）。 */
#define VOICE_TICK_MAX 4

/* 一帧的播报决策结果。 */
typedef struct {
    int  n;                              /* 实际条数 */
    char text[VOICE_TICK_MAX][192];      /* 逐条文本 */
} VoiceFrame;

/* 一帧的播报决策（抄官方 VoiceRouter.update()）。
 *
 * vr     播报器状态（会被更新：层级、冷却、告警已播级别）
 * rr     路线（沿路告警要扫 edge_flags）；可为 NULL
 * ml     转向指令列表；可为 NULL
 * man_idx 当前指令下标（官方 State.index 同义）；<0 = 无当前指令
 * to_go_m 距该指令的距离（米）
 * speed_ms 当前速度（m/s）
 * remain_m 剩余里程（米）
 * arrived  是否已停在终点
 * now_ms  当前时刻（毫秒，单调时钟）
 * out    输出（写入本帧该播的句子）
 *
 * 返回本帧播出的条数（0 = 不播）。
 * ★ 官方同文本冷却对**所有**类别生效：这里把四类都过同一个 say_dup()。
 * ★ 官方顺序 speedLimit → arrival → alarm → turn 严格照抄。 */
int  voice_tick(VoiceRouter *vr, const RouteResult *rr, const ManeuverList *ml,
                int man_idx, double to_go_m, double speed_ms, double remain_m,
                int arrived, uint64_t now_ms, VoiceFrame *out);

#endif /* NAVIMAP_VOICE_H */
```

### 3.2 接线层声明 `src/ui/voice_wire.h`（完整）

```h
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
```

### 3.3 唯一接线点 `src/ui/voice_wire.c`（完整）

```c
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
```

### 3.4 策略层实现 `src/route/voice.c`（关键逻辑，完整）

> 下面给出 `voice.c` 的核心函数，全部为**完整体**（无 `...` 省略）。文件 `src/route/voice.c` 共 898 行，含全部告警扫描与冷却逻辑；此处列出教学主线所涉及的全部关键函数，其余（如 `alarm_push`、`alarm_priority`、`alarm_type_priority`）为辅助函数，可于 `src/route/voice.c` 查阅完整实现。

```c
/* voice.c —— 语音播报策略（抄写自 OsmAnd，2026-10-25）
 *
 * 对应源文件见 voice.h 的头部清单。本文件是**纯逻辑**：不碰 SDL、不碰线程、
 * 不发声，只回答两个问题："现在该播第几级？" 和 "这句话怎么说？"
 * 发声由 utils/plat.c 的 nm_tts_speak 负责。
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
     *   官方源码注释里的数字是"四舍五入"过的，两者可能差 1。 */

    /* 官方 TURN_NOW = speed * min(sqrt(speed*3.6), 8)，再被
     * POSITIONING_TOLERANCE(12m) 兜底。 */
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

    /* ★ 基准阈值按**默认速度**算。 */
    double lead[4];
    voice_lead_distances(VOICE_DEFAULT_SPEED_MS, lead);

    /* ★ 判定顺序 = 从最紧急往回查，命中即返回**最紧急**的那一级。
     *   ★★ 反过来查（先查 PREPARE_LONG）会有真 bug：高速时 PREPARE_LONG 的
     *      圈（3750 m × 速度比例）远大于 PREPARE 的圈（1437 m × 速度比例），
     *      先查它就会让 2000 m 处返回 PREPARE_LONG（更弱的一级），
     *      ⇒ 中间那级永远播不出来。 */
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
    /* 环岛带出口号时用静态缓冲。★ 调用方必须**立即**用掉返回值。 */
    static char rb[64];
    if (man_type == MAN_ROUNDABOUT && exit_no > 0) {
        snprintf(rb, sizeof rb, "进入环岛，从第 %d 个出口驶出", exit_no);
        return rb;
    }
    return act;
}

void voice_spoken_distance(double meters, char *buf, size_t n)
{
    if (!buf || n == 0) return;
    double a = meters < 0.0 ? -meters : meters;
    if (a < 1e-6) {
        snprintf(buf, n, "0 公里");        /* 官方 dist() 的零值分支 */
    } else if (a < 10.0) {
        snprintf(buf, n, "%.1f 米", meters);
    } else if (a < 1000.0) {
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

    if (m->type == MAN_START) {
        if (m->road[0]) return snprintf(buf, n, "沿%s出发", m->road);
        return snprintf(buf, n, "从起点出发");
    }

    if (m->type == MAN_ARRIVE) {
        return snprintf(buf, n, "前方%s到达目的地", d);
    }

    const char *act = voice_action_text(m->type, m->exit_no);
    int k;
    if (now) k = snprintf(buf, n, "现在%s", act);
    else     k = snprintf(buf, n, "前方%s%s", d, act);

    if (m->road[0] && strcmp(m->road, "目的地") != 0) {
        size_t used = (size_t)(k > 0 ? k : 0);
        if (used < n) snprintf(buf + used, n - used, "，进入%s", m->road);
    }
    return (int)strlen(buf);
}

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

    if (dist <= 60.0) return snprintf(buf, n, "%s", g_alarm[alarm].say);
    return snprintf(buf, n, "前方%s%s", d, g_alarm[alarm].core);
}

int voice_scan_alarm(const RouteResult *rr, double from_m, double speed_ms,
                      VoiceAlarm *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof *out);
    if (!rr || !rr->ok || rr->n < 2) return 0;
    if (!rr->edge_flags || !rr->seg_m) return 0;

    uint32_t nseg = rr->n - 1;
    double speed = speed_ms > 0.5 ? speed_ms : 12.0;

    double acc = 0.0;

    AlarmCand *cand = NULL;
    size_t n_cand = 0, cap_cand = 0;

    double tunnel_start = -1.0;
    double tunnel_len  = 0.0;
    int    tunnel_inst  = 0;

    for (uint32_t i = 0; i < nseg; i++) {
        double seg_len = (double)rr->seg_m[i];
        uint8_t fl = rr->edge_flags[i];

        if (fl & EF_TUNNEL) {
            if (tunnel_start < 0.0) {
                tunnel_start = acc;
                tunnel_len = 0.0;
                tunnel_inst = (int)i;
            }
            tunnel_len += seg_len;
        } else if (tunnel_start >= 0.0) {
            if (tunnel_len >= VOICE_ALARM_MIN_TUNNEL_M) {
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
            tunnel_start = -1.0;
            tunnel_len = 0.0;
        }

        for (int t = 0; t < 2; t++) {
            int type = (t == 0) ? VOICE_ALARM_TOLL : VOICE_ALARM_BRIDGE;
            uint16_t bit = (t == 0) ? EF_TOLL : EF_BRIDGE;
            if (!(fl & bit)) continue;
            double d = acc - from_m;
            if (d < -20.0) continue;
            if (d < 0.0) d = 0.0;
            if (alarm_push(&cand, &n_cand, &cap_cand, type, d, 0.0, acc,
                           (int)i) != 0) {
                free(cand); return 0;
            }
        }

        acc += seg_len;
    }

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

    int best = -1;
    int best_prio = 999;
    for (size_t k = 0; k < n_cand; k++) {
        double d = cand[k].dist_m;
        double t = d / speed;
        int pr = alarm_priority(cand[k].type, t, d);
        if (pr >= 999) continue;
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
    if (st <= vr->last_state) return 0;
    vr->last_state = st;

    int now = (st == VOICE_TURN_NOW);
    voice_turn_phrase(man, to_go_m, now, out_text, n);
    if (!out_text[0]) { vr->last_state = -1; return 0; }

    if (ml && man_idx + 1 < ml->n) {
        const Maneuver *after = &ml->v[man_idx + 1];
        double dtn = maneuver_dist_to_next(ml, man_idx);
        if (dtn >= 0.0 && dtn < VOICE_MERGE_M && after->type != MAN_ARRIVE) {
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

    if (limit_kmh > 0) {
        if (vr->last_limit_kmh < 0) {
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
                if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
                return 1;
            }
        }
    } else {
        vr->last_limit_kmh = -1;
    }

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

    int stage = (al->dist_m <= 60.0) ? 2 : 1;

    int slot = alarm_slot_find(vr, al->type, al->inst);
    if (vr->alarm_slot[slot].type == (int16_t)al->type &&
        vr->alarm_slot[slot].inst == (int32_t)al->inst &&
        stage <= vr->alarm_slot[slot].stage)
        return 0;

    if (vr->has_alarm_said &&
        now_ms - vr->last_alarm_ms < VOICE_COOLDOWN_ALARM_MS) return 0;

    voice_alarm_phrase(al->type, al->dist_m, al->len_m, out_text, n);
    if (!out_text[0]) return 0;

    vr->alarm_slot[slot].type  = (int16_t)al->type;
    vr->alarm_slot[slot].inst  = (int32_t)al->inst;
    vr->alarm_slot[slot].stage = (uint8_t)stage;
    vr->last_alarm_ms = now_ms;
    vr->has_alarm_said = 1;
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
    snprintf(out_text, n, "前方即将到达目的地");
    if (say_dup(vr, out_text, now_ms)) { out_text[0] = '\0'; return 0; }
    return 1;
}

/* ---------------------------------------------------------------------------
 * 5) 每帧编排（抄 nav-desktop VoiceRouter.update()）
 * ------------------------------------------------------------------------- */

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

    /* ---- 1) 限速（官方 speedLimit）---- */
    int limit_kmh = (int)((speed_ms * 3.6 + 2.5) / 5.0) * 5;
    if (voice_router_maybe_say_limit(vr, limit_kmh, (int)(speed_ms * 3.6),
                                     buf, sizeof buf, now_ms))
        frame_push(out, buf);

    /* ---- 2) 到达（官方 arrival）---- */
    if (voice_router_maybe_say_arrive(vr, remain_m, buf, sizeof buf, now_ms))
        frame_push(out, buf);

    /* ---- 3) 沿路告警（官方 Alarms + Navigator.findAlarm）---- */
    if (rr) {
        double along = rr->total_m - remain_m;
        if (along < 0.0) along = 0.0;
        VoiceAlarm al;
        if (voice_scan_alarm(rr, along, speed_ms, &al)) {
            if (voice_router_maybe_say_alarm(vr, &al, buf, sizeof buf, now_ms))
                frame_push(out, buf);
        }
    }

    /* ---- 4) 转向（官方 turnPhrase）---- */
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
     * 由调用方的 ann_finished 标志保证只播一次 —— 见 window.c。 */
    (void)arrived;
    return out->n;
}
```

> 说明：`voice.c` 中 `scaled_lead`/`now_lead`/`maneuver_start_m`/`maneuver_dist_to_next`/`alarm_push`/`alarm_type_priority`/`alarm_priority` 等辅助函数与上方主线函数均位于 `src/route/voice.c`，为控制篇幅此处未逐字展开，可在 `src/route/voice.c` 查阅完整体。其中 `voice_announce_state`、`voice_action_text`、`voice_turn_phrase`、`voice_alarm_phrase`、`voice_scan_alarm`、`say_dup`、各 `voice_router_maybe_say*`、`voice_tick` 已**完整**嵌入上文。

### 3.5 发声层声明 `src/utils/plat.h`（语音播报部分，完整）

```h
/* ---------------- 语音播报（离线 TTS） ----------------
 * 跨平台离线发声：Windows 走 PowerShell + System.Speech（SAPI），
 * 其它平台按 PATH 探测 espeak-ng / espeak / say。真正的发声在分离线程里
 * 做（nm_thread_spawn），调用方（导航每帧推进）绝不被阻塞。
 * 设 NAVIMAP_TTS_DRYRUN=文件路径 时只把文本写进该文件、不真正发声，
 * 用于无头自测（本机没有音频设备 / 没有 TTS 引擎时也能断言集成是否正确）。 */
int  nm_tts_speak(const char *text);          /* 返回 1=已提交(或 dryrun 已写) / 0=无引擎 */
int  nm_tts_available(void);                  /* 本机有可用引擎？（不含 dryrun） */
/* 累计被**受理**的播报句数（只读观测口，不改变任何状态）。
 * ★ 计数点在入队**之前**、调用线程内、无竞争 —— 不能读队列长度：后台
 *   朗读线程会异步喝空队列，"入队后立刻读 size()"是与活消费者赛跑
 *   （OsmAnd 的 Speech.OFFERED 踩过同一个坑）。 */
int  nm_tts_offered_count(void);
/* 累计已**交付引擎**的句数（计数点在交给引擎之后、后台线程内）。
 * 与受理数配对可断言"队列真的被消费了" —— 只看受理数证明不了 worker 在跑。
 * ★ 不等于"已播完"：常驻会话下写入管道就返回，合成还在子进程那边进行中。
 *   要等真正播完用 nm_tts_flush()。 */
int  nm_tts_delivered_count(void);
/* 等待"已受理的句子全部播完"。实现：关掉常驻会话的写端 ⇒ 子进程读到 EOF
 * ⇒ 循环退出 ⇒ 退出前会把当前句说完 ⇒ 子进程退出即精确的"播完"边界。
 * 返回 0 = 已排空并播完；-1 = 没有会话可关（无引擎）。 */
int  nm_tts_flush(void);
```

### 3.6 发声实现 `src/utils/plat.c`：`nm_tts_speak`（完整）

> `nm_tts_speak` 是全部播报的终点（`voice_wire.c` 调用它）。它先走 `NAVIMAP_TTS_DRYRUN` 同步写文件（无头自测用它断言"真实交出去的话"），再判断本机是否有引擎，最后把文本送入队列由后台线程朗读。

```c
/* ---- DRYRUN：无头自测用，把"发声"重定向到文件（确定性） ---- */
static int tts_dryrun_write(const char *text)
{
    const char *dry = getenv("NAVIMAP_TTS_DRYRUN");
    if (!dry || !*dry) return 0;
    FILE *f = fopen(dry, "ab");
    if (!f) return 0;
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
    return 1;
}

/* （…… TTS 队列 / 常驻会话 / 后台朗读线程等机制略，见 src/utils/plat.c ……）

int nm_tts_speak(const char *text)
{
    if (!text || !*text) return 0;

    /* DRYRUN 同步写，避免起线程（自测要确定性） */
    if (tts_dryrun_write(text)) return 1;

    /* 本机没有可用引擎就不受理（无头环境静默） */
    if (!nm_tts_available()) return 0;

    if (!tts_queue_init()) {
        /* 起不了队列（内存不足等极端情况）⇒ 退回"每句一个进程"的旧行为。
         * 宁可慢也别不播：这是导航功能。 */
        tts_speak_once(text);
        return 1;
    }

    char *copy = nm_strdup(text);
    if (!copy) return 0;
    nm_mutex_lock(g_tts.lock);
    g_tts.offered++;
    /* 队列满 ⇒ 丢掉最老的（导航播报宁可少一句也不要延迟堆积） */
    while (g_tts.count >= TTS_QUEUE_MAX) {
        char *old = tts_queue_pop();
        if (!old) break;
        free(old);
    }
    tts_queue_push(copy);
    nm_cond_signal(g_tts.not_empty);
    nm_mutex_unlock(g_tts.lock);
    return 1;
}
```

> 说明：`nm_tts_speak` 上方 `tts_speak_once`/`tts_backend_speak`/`tts_worker`/`tts_queue_*` 等（约 `plat.c:828`–`plat.c:1125`）为 TTS 队列与常驻会话机制，为控制篇幅仅列签名与调用关系；`nm_tts_speak` 本身为**完整体**。完整实现见 `src/utils/plat.c`。

## 四、与其他功能之间的联系

- **上游（消费）**：第 10 课 `RouteResult`（`rr`）、`ManeuverList`（`man`，由 `maneuver_build` 生成）、`NavSim`（`sim`，由 `navsim_start`/`navsim_tick` 推进）。`nav_voice_frame` 从 `NavSim` 取剩余里程/速度/到达标志、`navsim_next_maneuver` 求当前指令下标与距离。
- **下游 · 第 12 课**：`nav_voice_frame` 被 Qt 主循环的 `MapCanvas::tick()`→`app_nav_sim_tick`→（window.c 的 `app_nav_sim_tick`）调用；Qt 的 V 键 / 设置菜单把 `App.voice_on` 置位，即 `NavVoiceCtx.voice_on`。`NavSim` 的 `off_m`/`proj_m` 供 Qt 偏航重算 UI；`VoiceFrame`/`VoiceRouter` 纯逻辑被 `tools/voice_e2e.c` 复用做无头端到端验收。
- **横切**：发声经由第 2 课 `plat.c` 的平台抽象层（`nm_tts_speak`/`nm_thread_spawn`/`nm_proc_*`），**不链 SDL**；语音判定与渲染（第 8 课 `navdraw`）完全解耦。
- **唯一接线纪律**：`voice.c` 纯逻辑 + `plat.c` 发声，二者之间**只有** `voice_wire.c:nav_voice_frame` 这一份接线。自测 `tools/voice_e2e.c` 调的就是这一份——若把发声循环改坏，端到端判据必然变红。

## 五、演示、练习与验收

- **演示**：
  - `./build_rel/navmap_qt`（GUI）模拟导航，听转向/限速/到达/隧道播报；用 V 键静音验证 `voice_on` 开关。
  - 无头端到端：`NAVIMAP_TTS_DRYRUN=/tmp/tts.txt ./build_rel/voice_e2e`（直接断言真实发声文本，不依赖音频设备）。
  - 冷却/到达帧/去抖专项：`NAVIMAP_TTS_DRYRUN=/tmp/tts.txt ./build_rel/voice_e2e`，对照用例 A8（到达帧必播收尾语）与负向用例 #9/#10（模拟 `paused` 提前置位时收尾语仍播出）。
  - 自检：`NAVIMAP_TTS_DRYRUN=/tmp/tts.txt ./build_rel/voice_selftest`。
- **练习**：在 `voice.c` 的 `g_alarm` 表里加一条新告警（如"前方 200 米有收费站"的二级话术），用 `voice_e2e` + `NAVIMAP_TTS_DRYRUN` 验证它真的被说出；或改 `VOICE_COOLDOWN_SAME_TEXT_MS` 验证同文本冷却对四类播报一视同仁。
- **验收**：能解释并对照源码说明
  1. `voice_wire.c` 为什么是**唯一**接线点（对比 `tools/voice_e2e.c` 共用同一函数）；
  2. 到达帧为什么不受 `paused` 抑制（`voice_wire.c:49` 的 `suppress` 表达式）；
  3. `say_dup`（同文本 15s 冷却）是所有播报的唯一出口（`voice.c` 中四处 `say_dup` 调用）；
  4. `voice.c` 纯逻辑、零平台依赖，`nm_tts_speak` 才真正出声。
