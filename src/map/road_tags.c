#include "map/road_tags.h"
#include "utils/plat.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    const char *name;
    uint8_t     cls;
    uint8_t     drivable;
    uint16_t    speed;
} HwEnt;

/* 车速是缺省估计值；有 maxspeed 标签时以其为准。 */
static const HwEnt kHighway[] = {
    { "motorway",        RC_MOTORWAY,     1, 110 },
    { "motorway_link",   RC_MOTORWAY,     1,  60 },
    { "trunk",           RC_TRUNK,        1,  90 },
    { "trunk_link",      RC_TRUNK,        1,  50 },
    { "primary",         RC_PRIMARY,      1,  70 },
    { "primary_link",    RC_PRIMARY,      1,  50 },
    { "secondary",       RC_SECONDARY,    1,  60 },
    { "secundary",       RC_SECONDARY,    1,  60 },   /* OSM 经典拼写错误，河南/山西各有数条 */
    { "secondary_link",  RC_SECONDARY,    1,  45 },
    { "tertiary",        RC_TERTIARY,     1,  50 },
    { "tertiary_link",   RC_TERTIARY,     1,  40 },
    { "unclassified",    RC_UNCLASSIFIED, 1,  40 },
    { "residential",     RC_RESIDENTIAL,  1,  30 },
    { "living_street",   RC_LIVING,       1,  10 },
    { "service",         RC_SERVICE,      1,  20 },
    { "road",            RC_OTHER,        1,  30 },   /* 未分类但可通行 */
    { "track",           RC_OTHER,        1,  20 },   /* 野外土路，多数能走 */
    { "yes",             RC_OTHER,        1,  20 },   /* 打了 highway 但没写类型 */
    { "ford",            RC_OTHER,        1,  10 },   /* 涉水路 */
    /* 步行/骑行：不进机动车路由，但要显示（小区、公园不再是空白） */
    { "cycleway",        RC_PATH, 0, 20 },
    { "footway",         RC_PATH, 0,  5 },
    { "path",            RC_PATH, 0,  5 },
    { "pedestrian",      RC_PATH, 0,  5 },
    { "steps",           RC_PATH, 0,  3 },
    { "bridleway",       RC_PATH, 0, 10 },
    { "corridor",        RC_PATH, 0,  5 },
    { "platform",        RC_PATH, 0,  0 },
    { "raceway",         RC_OTHER, 0,  0 },
    { "bus_stop",        RC_OTHER, 0,  0 },
    { "bus_guideway",    RC_OTHER, 0,  0 },
    { "elevator",        RC_OTHER, 0,  0 },
    { "rest_area",       RC_OTHER, 0,  0 },
    { "services",        RC_OTHER, 0,  0 },
    { "escape",          RC_OTHER, 0, 20 },
    { "via_ferrata",     RC_OTHER, 0,  0 },
    { "scramble",        RC_OTHER, 0,  0 },
    { "abseiling",       RC_OTHER, 0,  0 },
    { "ladder",          RC_OTHER, 0,  0 },
    { "emergency_bay",   RC_OTHER, 0,  0 },
    { "street_lamp",     RC_OTHER, 0,  0 },
    { "traffic_island",  RC_OTHER, 0,  0 },
    /* 生命周期：在建/废弃/规划，一律不进路网 */
    { "construction",    RC_OTHER, 0,  0 },
    { "proposed",        RC_OTHER, 0,  0 },
    { "planned",         RC_OTHER, 0,  0 },
    { "abandoned",       RC_OTHER, 0,  0 },
    { "disused",         RC_OTHER, 0,  0 },
    { "razed",           RC_OTHER, 0,  0 },
    { "demolished",      RC_OTHER, 0,  0 },
    { "removed",         RC_OTHER, 0,  0 },
    { "no",              RC_OTHER, 0,  0 },
    { "dam",             RC_OTHER, 0,  0 },
    { "busway",          RC_OTHER, 0,  0 },   /* 公交专用，小客车通常禁行 */
    { NULL, 0, 0, 0 }
};

/* 表外、但已知"不可通行"的取值：命中就不计入 rt_unknown_drivable 闸门 */
int road_tag_value_is_known_nondrivable(const char *v)
{
    static const char *k[] = {
        "path", "construction", "bridleway", "abandoned", "escape", "platform",
        "raceway", "bus_stop", "busway", "proposed", "disused", "elevator",
        "rest_area", "services", "via_ferrata", "corridor", "scramble",
        "abseiling", "ladder", "no", "emergency_bay", "bus_guideway", "dam",
        "planned", "razed", "demolished", "removed", "street_lamp",
        "traffic_island", "footway", "cycleway", "pedestrian", "steps", NULL
    };
    for (int i = 0; k[i]; i++)
        if (strcmp(v, k[i]) == 0) return 1;
    return 0;
}

static const HwEnt *find_hw(const char *v)
{
    for (int i = 0; kHighway[i].name; i++)
        if (strcmp(kHighway[i].name, v) == 0) return &kHighway[i];
    return NULL;
}

static int eq_ci(const char *a, const char *b)
{
    return nm_strcasecmp(a, b) == 0;   /* 平台层：strcasecmp / _stricmp */
}

static int access_denied(const char *v)
{
    if (!v) return 0;
    return eq_ci(v, "no") || eq_ci(v, "private") || eq_ci(v, "customers") ||
           eq_ci(v, "permit") || eq_ci(v, "military") || eq_ci(v, "emergency");
}

/* landuse/natural/leisure → 面要素 kind。
 * 返回 AK_NONE 表示"这个值不构成有意义的面"（如 landuse=residential 的
 * 大块居住区面在低 zoom 会把画面涂满，先不收）。 */
uint8_t road_area_kind_from_key(const char *k, const char *v)
{
    if (!k || !v) return AK_NONE;
    if (strcmp(k, "building") == 0)
        return eq_ci(v, "no") ? AK_NONE : AK_BUILDING;
    if (strcmp(k, "landuse") == 0) {
        if (eq_ci(v, "reservoir") || eq_ci(v, "basin")) return AK_WATER;
        if (eq_ci(v, "forest") || eq_ci(v, "grass") || eq_ci(v, "meadow") ||
            eq_ci(v, "village_green") || eq_ci(v, "orchard") ||
            eq_ci(v, "vineyard") || eq_ci(v, "farmland") || eq_ci(v, "farmyard"))
            return AK_GREEN;
        return AK_OTHER;
    }
    if (strcmp(k, "natural") == 0) {
        if (eq_ci(v, "water") || eq_ci(v, "bay") || eq_ci(v, "wetland"))
            return AK_WATER;
        if (eq_ci(v, "wood") || eq_ci(v, "scrub") || eq_ci(v, "grassland") ||
            eq_ci(v, "heath"))
            return AK_GREEN;
        return AK_NONE;   /* coastline/peak 等线/点要素不进面 */
    }
    if (strcmp(k, "leisure") == 0) {
        if (eq_ci(v, "park") || eq_ci(v, "garden") || eq_ci(v, "pitch") ||
            eq_ci(v, "playground") || eq_ci(v, "golf_course") ||
            eq_ci(v, "common"))
            return AK_GREEN;
        return AK_OTHER;
    }
    if (strcmp(k, "amenity") == 0) {
        if (eq_ci(v, "parking") || eq_ci(v, "school") || eq_ci(v, "university") ||
            eq_ci(v, "hospital") || eq_ci(v, "college"))
            return AK_OTHER;
        return AK_NONE;   /* 零散的 amenity 点位太多，面只收大片设施 */
    }
    return AK_NONE;
}

static const char *kRailKinds[] = {
    "rail", "light_rail", "subway", "tram", "narrow_gauge",
    "funicular", "monorail", "preserved", NULL
};

void road_tags_parse(const uint32_t *keys, size_t n_keys,
                     const uint32_t *vals, size_t n_vals,
                     TagStrFn str, void *ud, RoadTags *out)
{
    memset(out, 0, sizeof(*out));
    out->road_class = RC_OTHER;

    const char *hw = NULL;
    const char *name = NULL, *name_zh = NULL;
    const char *oneway = NULL, *junction = NULL, *maxspeed = NULL;
    const char *access = NULL, *motor = NULL, *vehicle = NULL;
    const char *surface = NULL;
    const char *lanes = NULL;
    const char *railway = NULL, *waterway = NULL;
    const char *building = NULL;
    const char *landuse = NULL, *natural = NULL, *leisure = NULL, *amenity = NULL;

    size_t n = n_keys < n_vals ? n_keys : n_vals;
    for (size_t i = 0; i < n; i++) {
        const char *k = str(keys[i], ud);
        const char *v = str(vals[i], ud);
        if (!k || !v) continue;

        if (strcmp(k, "highway") == 0) hw = v;
        else if (strcmp(k, "name") == 0) name = v;
        else if (strcmp(k, "name:zh") == 0 || strcmp(k, "name:zh-Hans") == 0) name_zh = v;
        else if (strcmp(k, "oneway") == 0) oneway = v;
        else if (strcmp(k, "junction") == 0) junction = v;
        else if (strcmp(k, "maxspeed") == 0) maxspeed = v;
        else if (strcmp(k, "access") == 0) access = v;
        else if (strcmp(k, "motor_vehicle") == 0) motor = v;
        else if (strcmp(k, "vehicle") == 0) vehicle = v;
        else if (strcmp(k, "surface") == 0) surface = v;
        else if (strcmp(k, "lanes") == 0) lanes = v;
        else if (strcmp(k, "railway") == 0)  railway = v;
        else if (strcmp(k, "waterway") == 0) waterway = v;
        else if (strcmp(k, "building") == 0) building = v;
        else if (strcmp(k, "landuse") == 0)  landuse = v;
        else if (strcmp(k, "natural") == 0)  natural = v;
        else if (strcmp(k, "leisure") == 0)  leisure = v;
        else if (strcmp(k, "amenity") == 0)  amenity = v;
        else if (strcmp(k, "bridge") == 0 && !eq_ci(v, "no")) out->bridge = 1;
        else if (strcmp(k, "tunnel") == 0 && !eq_ci(v, "no")) out->tunnel = 1;
        else if (strcmp(k, "toll") == 0 && !eq_ci(v, "no")) out->toll = 1;
        else if (strcmp(k, "route") == 0 && eq_ci(v, "ferry")) out->ferry = 1;
    }

    if (name_zh && *name_zh) { out->name = name_zh; out->name_len = strlen(name_zh); }
    else if (name && *name)  { out->name = name;    out->name_len = strlen(name); }

    if (!hw) {
        /* 不是路：可能是铁路线 / 水系线 / 面要素（也可能是 building 等纯面） */
        if (railway) {
            for (int i = 0; kRailKinds[i]; i++)
                if (eq_ci(railway, kRailKinds[i])) {
                    out->show = 1; out->road_class = RC_RAIL; break;
                }
        } else if (waterway && !eq_ci(waterway, "riverbank")) {
            /* riverbank 是闭合面（交给 area_kind 处理），其余是线 */
            out->show = 1; out->road_class = RC_WATER;
        }
    } else if (find_hw(hw)) {
        const HwEnt *e = find_hw(hw);
        out->road_class = e->cls;
        out->drivable   = e->drivable;
        out->speed_kmh  = e->speed;
        if (e->cls == RC_PATH) out->show = 1;   /* 步行道：显示但不路由 */
    } else {
        if (getenv("NAVMAP_DEBUG")) fprintf(stderr, "DBG unknown highway=%s\n", hw);
        /* 表外取值：按"可能可通行"处理，并标记出来让上层计数报警 */
        out->unknown_value = 1;
        out->drivable      = 1;
        out->road_class    = RC_OTHER;
        out->speed_kmh     = 30;
        out->whitelisted   = road_tag_value_is_known_nondrivable(hw) ? 1 : 0;
    }

    /* 面要素：与线判定互斥时优先面（building 上的 footway 少见，面优先）。
       这里只打标记；是否真的闭合由 graph_build 解码 refs 后判断。 */
    out->area_kind = road_area_kind_from_key("building", building);
    if (out->area_kind == AK_NONE) out->area_kind = road_area_kind_from_key("landuse", landuse);
    if (out->area_kind == AK_NONE) out->area_kind = road_area_kind_from_key("natural", natural);
    if (out->area_kind == AK_NONE) out->area_kind = road_area_kind_from_key("leisure", leisure);
    if (out->area_kind == AK_NONE) out->area_kind = road_area_kind_from_key("amenity", amenity);

    if (oneway) {
        if (eq_ci(oneway, "yes") || eq_ci(oneway, "1") || eq_ci(oneway, "true"))
            out->oneway = 1;
        else if (eq_ci(oneway, "-1") || eq_ci(oneway, "reverse"))
            out->oneway = 2;   /* 逆向 */
    }
    if (junction && eq_ci(junction, "roundabout")) {
        out->roundabout = 1;
        if (!out->oneway) out->oneway = 1;   /* 环岛默认单向 */
    }

    if (maxspeed) {
        /* "60"、"60 km/h"、"60;80"、"CN:urban" 之类，只吃纯数字前缀 */
        int v = atoi(maxspeed);
        if (v > 0 && v < 200) out->speed_kmh = (uint16_t)v;
    }
    if (lanes) {
        int v = atoi(lanes);
        if (v > 0 && v < 32) out->lanes = (uint16_t)v;
    }
    if (surface) {
        static const char *soft[] = { "gravel", "dirt", "sand", "grass", "ground",
                                      "unpaved", "earth", "mud", "cobblestone", NULL };
        for (int i = 0; soft[i]; i++)
            if (eq_ci(surface, soft[i])) { out->unpaved = 1; break; }
    }
    if (access_denied(access) || access_denied(motor) || access_denied(vehicle))
        out->no_access = 1;
}
