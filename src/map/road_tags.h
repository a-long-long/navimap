#ifndef NAVIMAP_ROAD_TAGS_H
#define NAVIMAP_ROAD_TAGS_H

#include <stdint.h>
#include <stddef.h>

/* 道路分级与通行属性。
 *
 * 设计取舍：只保留"导航最小集"。OSM 的 highway 有上百种取值，这里列一张
 * 白名单表；表外的取值按"可能可通行"对待并计数报警 —— 因为漏掉一条能走的
 * 路，比多画一条走不通的路严重得多。上一版 meta.txt 里的
 *   rt_unknown_drivable = 0   ← 闸门，必须保持为 0
 * 就是这个意思。
 */

/* 渲染 LOD 用：0 高速/国道，1 主干，2 次干，3 支路/小路，4 非机动车，
 * 5-7 是"只显示、不进路由"的线要素（走独立的 lines.bin，不污染路由 CSR） */
#define RC_MOTORWAY    0
#define RC_TRUNK       0
#define RC_PRIMARY     1
#define RC_SECONDARY   1
#define RC_TERTIARY    2
#define RC_UNCLASSIFIED 2
#define RC_RESIDENTIAL 3
#define RC_SERVICE     3
#define RC_LIVING      3
#define RC_OTHER       4
#define RC_PATH        5   /* footway/path/steps/cycleway/pedestrian */
#define RC_RAIL        6   /* railway=rail/subway/tram… 且无 highway */
#define RC_WATER       7   /* waterway=river/stream/canal… 线状水系 */

/* 面要素分类（areas.bin 的 kind 字段） */
#define AK_NONE     0
#define AK_BUILDING 1
#define AK_WATER    2
#define AK_GREEN    3
#define AK_OTHER    4   /* landuse/leisure/amenity 的其余取值 */

/* 每条边的通行标志位 */
#define EF_REVERSE     0x0001u  /* 该边是 way 的逆向边（渲染避让、调试用） */
#define EF_ROUNDABOUT  0x0002u
#define EF_BRIDGE      0x0004u
#define EF_TUNNEL      0x0008u
#define EF_TOLL        0x0010u
#define EF_FERRY       0x0020u
#define EF_UNPAVED     0x0040u
#define EF_NO_ACCESS   0x0080u  /* access/motor_vehicle 禁行：路由排除，渲染照画 */

typedef struct {
    uint8_t  road_class;      /* RC_* */
    uint8_t  drivable;        /* 1 = 纳入路网 */
    uint8_t  oneway;          /* 0 双向，1 正向，-1 逆向（存成 2） */
    uint8_t  roundabout;
    uint8_t  bridge;
    uint8_t  tunnel;
    uint8_t  toll;
    uint8_t  ferry;
    uint8_t  unpaved;
    uint8_t  no_access;       /* 禁行 */
    uint16_t speed_kmh;
    uint16_t lanes;
    uint8_t  unknown_value;   /* highway 取值不在表里 */
    uint8_t  whitelisted;     /* 取值在"已知不可通行"白名单里（不报警） */
    uint8_t  show;            /* 1 = 线要素：不进路由，只显示（lines.bin） */
    uint8_t  area_kind;       /* AK_*：闭合 way 可作为面要素（areas.bin） */
    const char *name;         /* 指向 block 的 stringtable，用完即弃 */
    size_t   name_len;
} RoadTags;

/* 解析一个 Way 的 keys/vals（stringtable 下标）→ RoadTags。
 * str(sid) 由调用方提供，因为 stringtable 只在 block 生命周期内有效。 */
typedef const char *(*TagStrFn)(uint32_t sid, void *ud);

void road_tags_parse(const uint32_t *keys, size_t n_keys,
                     const uint32_t *vals, size_t n_vals,
                     TagStrFn str, void *ud,
                     RoadTags *out);

/* landuse/natural/leisure 取值 → 面要素 kind（AK_*） */
uint8_t road_area_kind_from_key(const char *k, const char *v);

/* "已知不可通行"的 highway 取值白名单：命中就不计入 rt_unknown_drivable */
int road_tag_value_is_known_nondrivable(const char *v);

#endif
