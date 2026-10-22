#include "route/maneuver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "map/binfmt.h"
#include "map/road_tags.h"

/* 转角阈值（度）。前后各看 LOOK_M 米再算方位，避免把弯路的中间节点
   当成路口（OSM 一条 way 会被路口切成十几段，逐段算会每十米报一次转向）。 */
#define TURN_DEADZONE  20.0
#define TURN_SLIGHT    50.0
#define TURN_NORMAL   120.0
#define TURN_SHARP    160.0
#define LOOK_M         45.0
#define MIN_GAP_M     120.0    /* 两条指令至少间隔这么远，否则视为噪音 */
#define MERGE_SAME_M  400.0    /* 同向、同路名的相邻转向，隔这么近只报一次 */
#define STRAIGHT_M   3000.0    /* 直行超过这么远补一条"继续直行" */

static double seg_len(const RouteResult *rr, uint32_t i)
{
    return rr->seg_m ? (double)rr->seg_m[i] : 0.0;
}

static uint16_t seg_flags(const RouteResult *rr, uint32_t i)
{
    return rr->edge_flags ? rr->edge_flags[i] : 0;
}

/* 方位角（度，正北为 0，顺时针增大） */
static double bearing_deg(int32_t la0_e7, int32_t lo0_e7,
                          int32_t la1_e7, int32_t lo1_e7)
{
    double p1 = la0_e7 * 1e-7 * M_PI / 180.0;
    double p2 = la1_e7 * 1e-7 * M_PI / 180.0;
    double dlam = (lo1_e7 - lo0_e7) * 1e-7 * M_PI / 180.0;
    double y = sin(dlam) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dlam);
    double b = atan2(y, x) * 180.0 / M_PI;
    if (b < 0) b += 360.0;
    return b;
}

/* 把角度差归一到 (-180, 180] */
static double norm_turn(double d)
{
    while (d > 180.0) d -= 360.0;
    while (d <= -180.0) d += 360.0;
    return d;
}

/* 从点 i 沿 dir 方向累积到 LOOK_M 米（或到端点），返回落点下标。
   i 本身没有可走的路（退化）时返回 i。 */
static uint32_t walk(const RouteResult *rr, uint32_t i, int dir)
{
    double acc = 0;
    uint32_t j = i;
    if (dir > 0) {
        while (acc < LOOK_M && j + 1 < rr->n) { acc += seg_len(rr, j); j++; }
    } else {
        while (acc < LOOK_M && j > 0) { acc += seg_len(rr, j - 1); j--; }
    }
    return j;
}

/* 段 s（pts[s] → pts[s+1]）的路名。edge_idx[s] 属于 pts[s+1].prov。 */
static void road_name_of(const RouteResult *rr, const MapSet *ms,
                         uint32_t s, char *buf, size_t cap, uint8_t *cls_out)
{
    buf[0] = '\0';
    if (cls_out) *cls_out = 0xFF;
    if (s + 1 >= rr->n) return;
    uint32_t prov = rr->pts[s + 1].prov;
    uint32_t eidx = rr->edge_idx[s];
    if (!ms || prov >= (uint32_t)ms->n || eidx == REDGE_SEAM) return;
    mapset_ensure_loaded((MapSet *)ms, (int)prov);   /* 该省可能已被 set_view 卸载 */
    const MapSrc *src = &ms->srcs[prov];
    if ((uint64_t)eidx >= src->edges.hdr.rec_count) return;
    const uint8_t *er = src->edges.data + (uint64_t)eidx * NM_EDGE_BYTES;
    if (cls_out) *cls_out = er[EF_CLASS];
    uint32_t nid = nm_get_u32(er + EF_NAME_ID);
    const NamePool *np = &src->np;
    if (!nid || !np->ready || nid > np->n) return;
    const char *s2 = np->blob + np->off[nid];
    snprintf(buf, cap, "%s", s2);
}

static void ml_push(ManeuverList *ml, const Maneuver *m)
{
    if (ml->n == ml->cap) {
        int nc = ml->cap ? ml->cap * 2 : 32;
        Maneuver *t = realloc(ml->v, (size_t)nc * sizeof(Maneuver));
        if (!t) return;
        ml->v = t; ml->cap = nc;
    }
    ml->v[ml->n++] = *m;
}

void maneuver_free(ManeuverList *ml)
{
    if (!ml) return;
    free(ml->v);
    memset(ml, 0, sizeof(*ml));
}

/* 环岛：数第几个出口。
   判定"这是出口"= 该环上路口有非环岛的出边（且不回头）。我们驶出的那条
   在下一段不是环岛时自然被计入，所以不用特判。 */
static int count_side_exits(const MapSet *ms, const RouteResult *rr, uint32_t k)
{
    uint32_t prov = rr->pts[k].prov;
    if (!ms || prov >= (uint32_t)ms->n) return 0;
    mapset_ensure_loaded((MapSet *)ms, (int)prov);   /* 该省可能已被 set_view 卸载 */
    const MapSrc *s = &ms->srcs[prov];
    uint32_t node = rr->pts[k].node;
    if ((uint64_t)node >= s->nodes.hdr.rec_count) return 0;
    const uint8_t *nr = s->nodes.data + (uint64_t)node * NM_NODE_BYTES;
    uint32_t b = nm_get_u32(nr + NF_EDGE_BEGIN);
    uint16_t c = nm_get_u16(nr + NF_EDGE_COUNT);
    uint64_t ne = s->edges.hdr.rec_count;
    uint32_t prev = k > 0 ? rr->pts[k - 1].node : 0xFFFFFFFFu;

    int cnt = 0;
    for (uint32_t i = 0; i < c; i++) {
        uint64_t e = (uint64_t)b + i;
        if (e >= ne) break;
        const uint8_t *er = s->edges.data + e * NM_EDGE_BYTES;
        uint32_t to = nm_get_u32(er + EF_TO);
        if (to == prev) continue;                     /* 回头路不算出口 */
        if (nm_get_u16(er + EF_FLAGS) & EF_ROUNDABOUT) continue;
        cnt++;
    }
    return cnt;
}

/* entry = 进入环岛的那个点（环岛第一段边的起点）。返回出口序号（>=1）。 */
static int roundabout_exit_no(const RouteResult *rr, const MapSet *ms,
                              uint32_t entry)
{
    int exits = 0;
    uint32_t k = entry;
    while (k + 1 < rr->n && (seg_flags(rr, k) & EF_ROUNDABOUT)) {
        k++;                                   /* 走到环上的下一个路口 */
        exits += count_side_exits(ms, rr, k);   /* 含我们最后驶出的那条 */
    }
    return exits;                              /* 0 = 数据不全，文案退化为"进入环岛" */
}

void fmt_distance(double meters, char *buf, size_t n)
{
    if (meters < 1000.0) snprintf(buf, n, "%.0f 米", meters);
    else if (meters < 10000.0) snprintf(buf, n, "%.2f 公里", meters / 1000.0);
    else snprintf(buf, n, "%.0f 公里", meters / 1000.0);
}

const char *maneuver_kind_text(const Maneuver *m)
{
    static char rb[32];
    switch (m->type) {
    case MAN_START:          return "出发";
    case MAN_STRAIGHT:       return "直行";
    case MAN_SLIGHT_LEFT:    return "靠左";
    case MAN_LEFT:           return "左转";
    case MAN_SHARP_LEFT:     return "向左急转";
    case MAN_SLIGHT_RIGHT:   return "靠右";
    case MAN_RIGHT:          return "右转";
    case MAN_SHARP_RIGHT:    return "向右急转";
    case MAN_UTURN:          return "掉头";
    case MAN_ROUNDABOUT:
        if (m->exit_no > 0) {
            snprintf(rb, sizeof(rb), "环岛第%d出口", m->exit_no);
            return rb;
        }
        return "进入环岛";
    case MAN_ARRIVE:         return "到达";
    default:                 return "?";
    }
}

void maneuver_text(const Maneuver *m, char *buf, size_t n)
{
    char d[32];
    fmt_distance(m->dist_m, d, sizeof(d));
    if (m->type == MAN_START) {
        if (m->road[0]) snprintf(buf, n, "沿%s出发", m->road);
        else            snprintf(buf, n, "从起点出发");
        return;
    }
    if (m->type == MAN_ARRIVE) {
        snprintf(buf, n, "行驶%s后到达目的地", d);
        return;
    }
    if (m->type == MAN_STRAIGHT) {
        if (m->road[0]) snprintf(buf, n, "沿%s直行 %s", m->road, d);
        else            snprintf(buf, n, "直行%s", d);
        return;
    }
    if (m->type == MAN_ROUNDABOUT) {
        if (m->exit_no > 0)
            snprintf(buf, n, "%s后进入环岛，第%d个出口驶出", d, m->exit_no);
        else
            snprintf(buf, n, "%s后进入环岛", d);
        return;
    }
    const char *k = maneuver_kind_text(m);
    if (m->road[0]) snprintf(buf, n, "%s后%s进入%s", d, k, m->road);
    else            snprintf(buf, n, "%s后%s", d, k);
}

int maneuver_build(const RouteResult *rr, const MapSet *ms, ManeuverList *out)
{
    memset(out, 0, sizeof(*out));
    if (!rr || !rr->ok || rr->n < 2) return 0;

    Maneuver m;
    memset(&m, 0, sizeof(m));
    m.idx = 0;
    m.type = MAN_START;
    road_name_of(rr, ms, 0, m.road, sizeof(m.road), &m.cls);
    ml_push(out, &m);

    double acc = 0;                 /* 自上一条指令起累积的路程 */
    uint32_t last = 0;              /* 上一条指令的点下标 */
    for (uint32_t i = 1; i + 1 < rr->n; i++) {
        acc += seg_len(rr, i - 1);
        uint16_t fprev = seg_flags(rr, i - 1);
        uint16_t fcur  = seg_flags(rr, i);

        if (fcur & EF_ROUNDABOUT) {
            if (fprev & EF_ROUNDABOUT) continue;      /* 环岛内部不报转向 */
            memset(&m, 0, sizeof(m));
            m.idx = i;
            m.type = MAN_ROUNDABOUT;
            m.dist_m = acc;
            m.exit_no = roundabout_exit_no(rr, ms, i);
            road_name_of(rr, ms, i, m.road, sizeof(m.road), &m.cls);
            ml_push(out, &m);
            acc = 0; last = i;
            continue;
        }
        if (fprev & EF_ROUNDABOUT) continue;          /* 刚出环，方向变化不算路口 */

        uint32_t ja = walk(rr, i, +1), jb = walk(rr, i, -1);
        if (ja == i || jb == i) continue;
        double ahead  = bearing_deg(rr->lat_e7[i], rr->lon_e7[i],
                                    rr->lat_e7[ja], rr->lon_e7[ja]);
        double behind = bearing_deg(rr->lat_e7[jb], rr->lon_e7[jb],
                                    rr->lat_e7[i], rr->lon_e7[i]);
        double turn = norm_turn(ahead - behind);

        if (fabs(turn) < TURN_DEADZONE) {
            /* 长距离没有路口：补一条"继续直行"，免得面板上几公里没动静 */
            if (acc >= STRAIGHT_M && i - last > 1) {
                memset(&m, 0, sizeof(m));
                m.idx = i;
                m.type = MAN_STRAIGHT;
                m.dist_m = acc;
                m.turn_deg = turn;
                road_name_of(rr, ms, i, m.road, sizeof(m.road), &m.cls);
                ml_push(out, &m);
                acc = 0; last = i;
            }
            continue;
        }
        if (acc < MIN_GAP_M) continue;                /* 太密集，当噪音丢掉 */

        memset(&m, 0, sizeof(m));
        m.idx = i;
        m.dist_m = acc;
        m.turn_deg = turn;
        road_name_of(rr, ms, i, m.road, sizeof(m.road), &m.cls);
        /* 立交匝道常被切成好几段：同一条路上连着两个同向转向，只报头一个，
           后面的里程继续累积到下一个真正的指令上（acc 不在这里清零）。 */
        double at0 = fabs(turn);
        int t0 = at0 >= TURN_SHARP ? MAN_UTURN
               : at0 >= TURN_NORMAL ? (turn > 0 ? MAN_SHARP_RIGHT : MAN_SHARP_LEFT)
               : at0 >= TURN_SLIGHT ? (turn > 0 ? MAN_RIGHT : MAN_LEFT)
               : (turn > 0 ? MAN_SLIGHT_RIGHT : MAN_SLIGHT_LEFT);
        m.type = t0;
        if (out->n > 0 && acc < MERGE_SAME_M && m.road[0]) {
            const Maneuver *p = &out->v[out->n - 1];
            if (p->type == m.type && p->type != MAN_START &&
                p->type != MAN_ARRIVE && strcmp(p->road, m.road) == 0)
                continue;
        }
        ml_push(out, &m);
        acc = 0; last = i;
    }

    /* 最后一段计入"到达" */
    acc += seg_len(rr, rr->n - 2);
    memset(&m, 0, sizeof(m));
    m.idx = rr->n - 1;
    m.type = MAN_ARRIVE;
    m.dist_m = acc;
    ml_push(out, &m);

    double sum = 0;
    for (int i = 0; i < out->n; i++) sum += out->v[i].dist_m;
    out->sum_m = sum;
    return out->n;
}
