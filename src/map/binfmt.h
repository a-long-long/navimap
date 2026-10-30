#ifndef NAVIMAP_BINFMT_H
#define NAVIMAP_BINFMT_H

#include <stdint.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * 自描述二进制（NAVMBIN）
 *
 * 为什么要有这一层：上一版 graph_auto 的 .bin 只有"8 字节头 + 定长记录"，
 * 字段语义随源码一起丢了，现在只能靠"文件大小 ÷ 记录数"去反推。
 * 新格式把字段名、偏移、类型、宽度一并写进文件头，丢掉源码也能读。
 *
 * 落盘统一小端；结构只用定长整型，不用 struct 直接 fwrite（aarch64 对齐/填充
 * 会让同一份代码在不同编译器下写出不同布局）。
 * ------------------------------------------------------------------------- */

#define NMBIN_MAGIC   "NAVMBIN\x01"
/* v2：restrictions.bin 记录 16→20 字节（新增 RF_PREV_EDGE，支持 via=way 两段限制）。
   旧数据必须重抽——记录变长后按旧长度读会整表错位，宁可让 sync_maps 全量重建。
   v3：edgelist.bin 条目 8 → 28 字节（自带坐标/等级/标志，渲染不再随机访存，
   见下面 NM_XL2_* 的说明）。虽然读取端能按 rec_size 同时认两版、旧目录也能
   正常画（走慢路径），但**混合图集要不得**：一半省 v1、一半省 v2 时文件体积
   和帧时间都不一样而画面完全一样，出了问题没人会想到是版本混装。所以这里
   照例顶版本号，逼 sync_maps / 启动自愈全量重抽。 */
#define NMBIN_VERSION 3u

#define NMBIN_KIND_NODES        1u
#define NMBIN_KIND_EDGES        2u
#define NMBIN_KIND_NAMES        3u
#define NMBIN_KIND_RESTRICTIONS 4u
#define NMBIN_KIND_OSMID        5u
#define NMBIN_KIND_LINES        6u   /* 线要素：步行道/铁路/水系线（不进路由） */
#define NMBIN_KIND_AREAS        7u   /* 面要素：建筑/水面/绿地 */
#define NMBIN_KIND_POLYREFS     8u   /* 线/面引用的节点内部下标（u32 序列） */
#define NMBIN_KIND_EDGELIST    10u   /* {node_idx, edge_idx} 对，按 tile 排序 */
#define NMBIN_KIND_TILES       11u   /* 瓦片目录（渲染空间索引） */
#define NMBIN_KIND_LANES       12u   /* 每条边的车道转向串 id（u32，CSR 边序） */
#define NMBIN_KIND_CONTOURS        13u  /* 等高线记录（类 lines.bin，引用 contour_pts 下标） */
#define NMBIN_KIND_CONTOUR_PTS     14u  /* 等高线独立顶点池（lat/lon e7，8 字节） */
#define NMBIN_KIND_CONTOUR_POLYREFS 15u /* 等高线引用的顶点下标（u32 序列，指向 contour_pts） */

#define NMBIN_MAX_FIELDS 16
#define NMBIN_HDR_BYTES  128
#define NMBIN_FIELD_BYTES 24

/* 字段类型 */
#define NMT_I32 1
#define NMT_U32 2
#define NMT_I64 3
#define NMT_U64 4
#define NMT_F32 5
#define NMT_F64 6
#define NMT_U16 7
#define NMT_U8  8

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t kind;
    uint32_t rec_size;
    uint64_t rec_count;
    uint32_t n_fields;
    uint32_t flags;
    uint64_t payload_bytes;
    char     source[32];     /* 源文件名（UTF-8，可能截断） */
    uint8_t  reserved[44];
} NMBinHeader;               /* 128 字节 */

typedef struct {
    char     name[16];
    uint16_t offset;
    uint8_t  type;
    uint8_t  width;
    uint8_t  reserved[4];
} NMBinField;                /* 24 字节 */

/* 写：先 nmbin_begin() 写头（记录数先写 0），写完记录后 nmbin_finalize()
 * 回填记录数与 payload 长度。这样不需要事先知道条数。 */
typedef struct NMBinWriter NMBinWriter;

NMBinWriter *nmbin_create(const char *path, uint32_t kind, uint32_t rec_size,
                          const char *source_hint);
int  nmbin_add_field(NMBinWriter *w, const char *name, uint16_t offset,
                     uint8_t type, uint8_t width);
int  nmbin_write_records(NMBinWriter *w, const void *recs, uint64_t n);
int  nmbin_finalize(NMBinWriter *w);      /* 回填并关闭 */
void nmbin_abort(NMBinWriter *w);         /* 出错时丢弃 */

/* 读 */
typedef struct {
    NMBinHeader hdr;
    NMBinField  fields[NMBIN_MAX_FIELDS];
    uint8_t    *data;                     /* 记录区（mmap 或 malloc） */
    size_t      data_len;
    size_t      mapped_len;
    int         mapped;
} NMBinFile;

int  nmbin_open(const char *path, NMBinFile *out);   /* 0 成功 */
void nmbin_close(NMBinFile *f);
int  nmbin_check(const NMBinFile *f, char *err, size_t err_cap);  /* 结构自检 */

/* 小端读写助手 */
void     nm_put_u16(uint8_t *p, uint16_t v);
void     nm_put_u32(uint8_t *p, uint32_t v);
void     nm_put_i32(uint8_t *p, int32_t v);
void     nm_put_u64(uint8_t *p, uint64_t v);
uint16_t nm_get_u16(const uint8_t *p);
uint32_t nm_get_u32(const uint8_t *p);
int32_t  nm_get_i32(const uint8_t *p);
uint64_t nm_get_u64(const uint8_t *p);

/* 各文件记录布局（也写进文件头，这里只是建图时的常量） */
#define NM_NODE_BYTES 28
#define NM_EDGE_BYTES 18
#define NM_RESTR_BYTES 20
#define NM_OSMID_BYTES 8

/* nodes.bin 记录字段偏移 */
#define NF_LAT        0    /* i32  1e-7 度 */
#define NF_LON        4    /* i32 */
#define NF_EDGE_BEGIN 8    /* u32  CSR 起始下标 */
#define NF_EDGE_COUNT 12   /* u16 */
#define NF_NODE_FLAGS 14   /* u16 */
#define NF_OSM_ID     16   /* u64 */
#define NF_CLASS_MASK 24   /* u32  经过该节点的道路等级位掩码 */

/* edges.bin 记录字段偏移 */
#define EF_TO        0    /* u32  目标节点内部下标 */
#define EF_DIST_CM   4    /* u32  段长（厘米） */
#define EF_NAME_ID   8    /* u32  路名池 id，0 = 无名 */
#define EF_FLAGS     12   /* u16 */
#define EF_CLASS     14   /* u8 */
#define EF_SPEED     15   /* u8   km/h */
#define EF_LANES     16   /* u16 */

/* restrictions.bin 记录字段偏移
 *
 * kind 语义：
 *   0 = 单点禁止：在 via 节点，走完 from_edge 后不能接着走 to_edge
 *   1 = 单点必须（only_*）
 *   2 = via=way 两段禁止：只有"再前一条边是 prev_edge"时才禁。
 *       即  from_edge(prev) → from_edge(本记录的 from_edge，即 via way 那条)
 *           → to_edge  这条三边序列被禁。
 *       为什么必须带 prev：同一个 (via way → to way) 转向，对不同来路约束不同
 *       （实测北京同一对 via/to 同时存在 no_u_turn 与 no_left_turn，来路不同）。
 *       压成单点限制会误伤从第三条路合法进入的车。 */
#define RF_FROM_EDGE 0    /* u32 */
#define RF_TO_EDGE   4    /* u32 */
#define RF_VIA_NODE  8    /* u32 */
#define RF_KIND      12   /* u8   0=禁止 1=必须 2=via-way 两段 */
#define RF_DIRMASK   13   /* u8   1=左转 2=直行 4=右转 8=掉头 */
#define RF_RESERVED  14   /* u16 */
#define RF_PREV_EDGE 16   /* u32  kind=2 时有效：三段序列里的第一条边 */

/* lines.bin：线要素（步行道/铁路/水系线）。ref_off 指向 polyrefs.bin 的 u32 下标 */
#define NM_LINE_BYTES 24
#define LF_OSM_ID   0    /* u64 */
#define LF_REF_OFF  8    /* u32 */
#define LF_REF_CNT  12   /* u32 */
#define LF_CLASS    16   /* u8   RC_PATH / RC_RAIL / RC_WATER */
#define LF_FLAGS    17   /* u8   EF_TUNNEL/EF_BRIDGE 等照用 */
#define LF_NAME_ID  20   /* u32 名字池 id，0 = 无名 */

/* areas.bin：面要素。ref_off 指向 polyrefs.bin，节点按环绕顺序，首尾同点 */
#define NM_AREA_BYTES 20
#define AF_OSM_ID   0    /* u64 */
#define AF_REF_OFF  8    /* u32 */
#define AF_REF_CNT  12   /* u32 */
#define AF_KIND     16   /* u8   AK_BUILDING / AK_WATER / AK_GREEN / AK_OTHER */
#define AF_RESERVED 17   /* u8[4] */

/* contours.bin：等高线。记录布局复用 lines.bin 的 24 字节槽（偏移同 lines），
 * 但语义不同：osm_id 槽存"高程（米，u64）"，ref_off/ref_cnt 指向
 * contours_polyrefs.bin（u32 序列），每个下标再指向 contours_pts.bin 的顶点。
 * class 字段区分"计曲线（每 index 一条加粗/加深）"与"首曲线"。
 * 不进 OSM 导入管线，由 skedaddle_gen_contours 直接产出 navimap 专属二进制。 */
#define NM_CONTOUR_BYTES 24
#define CF_ELEV      0    /* u64  高程（米）— 复用 lines 的 LF_OSM_ID 槽 */
#define CF_REF_OFF   8    /* u32 */
#define CF_REF_CNT   12   /* u32 */
#define CF_CLASS     16   /* u8   0=首曲线 1=计曲线/索引（加粗加深） */
#define CF_FLAGS     17   /* u8 */
#define CF_NAME_ID   20   /* u32 保留（等高线无名） */

/* contour_pts.bin：等高线顶点池（不依赖 OSM 节点）。每条 8 字节。 */
#define NM_CONTOUR_PT_BYTES 8
#define CP_LAT  0    /* i32  e7 度 */
#define CP_LON  4    /* i32  e7 度 */

/* polyrefs.bin：u32 一条 */

/* 空间索引（Z=14 Web Mercator 瓦片，约 2.4km/格 @北京纬度）。
 * edges.bin 是 CSR 顺序（路由依赖），不能重排，所以索引存
 * {node_idx, edge_idx} 对：edgelist.bin 按 tile 排序，tiles.bin 是
 * 每 tile 在 edgelist 里的 [off, off+cnt) 目录。
 * tiles.bin 的 payload 前置 16 字节网格元数据：x0,y0,nx,ny（u32）。
 * 长边会加入两端点覆盖的所有 tile（重复出现无害，画两次而已）。 */
#define NM_TILE_Z          14
#define NM_TILE_REC_BYTES  8
#define TF_EDGE_OFF 0    /* u32 edgelist 起始下标 */
#define TF_EDGE_CNT 4    /* u32 */
#define NM_XL_BYTES 8
#define XL_NODE     0    /* u32 from 节点内部下标 */
#define XL_EDGE     4    /* u32 CSR 边下标 */

/* ---- edgelist v2（自带绘制所需字段，渲染只读顺序数据）----
 *
 * 为什么要有 v2：v1 的条目只有两个下标，渲染时每条都要**随机**读一次
 * edges.bin（只为查等级：实测 z9 有 87% 的条目查完就被丢掉）再随机读两次
 * nodes.bin（470 MB 的大数组）。实测这三笔随机访存占河北 z9 路网 469 ms 的
 * 一大半，而软件预取 / 各种小缓存都压不下去（详见 PLAN.md §21）。
 *
 * v2 把绘制真正要用的字段全部塞进条目：等级/标志/两端经纬度 + 边下标（仅
 * zoom≥15 取路名时用）。渲染于是只顺序扫 edgelist，不再碰 nodes/edges。
 *
 * 兼容：靠 NMBinHeader.rec_size 区分（8 = v1，28 = v2），读取端两条路径都在，
 * 旧目录不重建也能正常显示（只是走慢路径）。
 *
 * ★ 为什么还是留了 node_idx（28 而不是 24 字节）：edgelist 不只给渲染用。
 *   跨省缝合（route.c sew_cb）要拿 from 节点下标去 expand，重复省检测
 *   （mapset.c detect_duplicates）要从条目取采样点。这两个是正确性问题，
 *   不是性能问题，不能为了省 4 字节让它们退化成"再随机读一次 nodes.bin"。
 *   渲染是顺序扫，多带 4 字节只是多 14% 的顺序带宽，换来的却是
 *   路由/覆盖/去重三条路径零改动。 */
#define NM_XL2_BYTES 28
#define XL2_FROM_LAT 0    /* i32 e7 */
#define XL2_FROM_LON 4    /* i32 e7 */
#define XL2_TO_LAT   8    /* i32 e7 */
#define XL2_TO_LON   12   /* i32 e7 */
#define XL2_FLAGS    16   /* u16 与 EF_FLAGS 同编码（EF_REVERSE 位） */
#define XL2_CLS      18   /* u8  道路等级 0..4 */
#define XL2_PAD      19   /* u8  保留，置 0（凑 4 字节对齐） */
#define XL2_EDGE     20   /* u32 CSR 边下标（供 zoom≥15 取路名） */
#define XL2_NODE     24   /* u32 from 节点内部下标（缝合/去重/采样用） */

#endif
