# 第 4 课 · 自描述二进制与图集

> 模块：`src/map/binfmt.{c,h}`、`src/map/mapset.{c,h}`、`src/map/namepool.{c,h}`、`src/map/coverage.{c,h}`、`src/map/autobuild.{c,h}`、`src/map/road_tags.{c,h}`
> 前置：第 3 课 PBF 解析、内存映射（mmap）概念、C 结构体对齐与字节序常识。

## 一、教学目的

理解"为什么不用 8 字节头+定长记录，而要做自描述格式"，以及多省图集如何按需加载。学完后学生应能：读出一个 `.nmbin` 文件头的 magic 与字段表，手动解析第一个字段的偏移；解释 `dup_of`（北京⊂河北 100%）在渲染与路由中如何被跳过；理解 `namepool` 从 render 下沉到 map 层消除了什么模块倒置；看懂 `mapset_set_view` 的视口驱动加载与 LRU 淘汰；会用 `coverage_locate`/`mapset_coverage` 回答"这里有没有图"；理解 `autobuild_repair` 的启动自愈判据。

## 二、功能构成

| 文件 | 职责 |
|---|---|
| `binfmt.c/h` | `NMBinFile` 自描述格式：magic/版本/字段表/小端定长整型；mmap 打开与结构自检 |
| `mapset.c/h` | `MapSrc`/`MapSet`：每省一组 NMBinFile，视口驱动加载/卸载，后台解码线程 |
| `namepool.c/h` | 每省 `names.bin` 解析结果（路名池） |
| `coverage.c/h` | `mapset_coverage`/`coverage_locate`：有没有图、定位到省 |
| `autobuild.c/h` | `autobuild_repair`：源 pbf 还在而产物缺失/损坏时当场重建 |
| `road_tags.c/h` | `RoadTags` 与 `road_tags_parse`：道路分级与通行属性 |

**自描述头**（`binfmt.h:18-68`）：`NMBIN_MAGIC = "NAVMBIN\x01"`、`NMBIN_VERSION = 3u`（注意：旧文档曾误写成 `\x2`/4u，以源码为准）。`NMBinHeader`（128 字节）与 `NMBinField`（24 字节）完整定义见第三节。

**图集句柄**（`mapset.h:22-104`）：`MapSrc` 含 `nodes/edges/restrictions/lines/areas/polyrefs/tiles/edgelist/lanes/contours` 各一份 `NMBinFile`，及 `bbox[4]`、`dup_of`、`NamePool np`、`NmMutex *dmap_mtx / job_prov / job_done`（后台解码线程字段）等，完整定义见第三节。

## 三、完整实现代码

### 1. 自描述二进制 `NMBinHeader` / `NMBinField`（`binfmt.h:18-98`）

```c
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
```

读端结构（`binfmt.h:90-102`）：

```c
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
```

### 2. 小端读写助手与 `bytes_to_hdr`（`binfmt.c:17-97`）

```c
void nm_put_u16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)(v >> 8); }

void nm_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xff); p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff); p[3] = (uint8_t)((v >> 24) & 0xff);
}

void nm_put_i32(uint8_t *p, int32_t v) { nm_put_u32(p, (uint32_t)v); }

void nm_put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
}

uint16_t nm_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint32_t nm_get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int32_t nm_get_i32(const uint8_t *p) { return (int32_t)nm_get_u32(p); }

uint64_t nm_get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void hdr_to_bytes(const NMBinHeader *h, uint8_t *b)
{
    memset(b, 0, NMBIN_HDR_BYTES);
    memcpy(b, h->magic, 8);
    nm_put_u32(b + 8,  h->version);
    nm_put_u32(b + 12, h->kind);
    nm_put_u32(b + 16, h->rec_size);
    nm_put_u64(b + 20, h->rec_count);
    nm_put_u32(b + 28, h->n_fields);
    nm_put_u32(b + 32, h->flags);
    nm_put_u64(b + 36, h->payload_bytes);
    memcpy(b + 44, h->source, 32);
}

static void bytes_to_hdr(const uint8_t *b, NMBinHeader *h)
{
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, b, 8);
    h->version       = nm_get_u32(b + 8);
    h->kind          = nm_get_u32(b + 12);
    h->rec_size      = nm_get_u32(b + 16);
    h->rec_count     = nm_get_u64(b + 20);
    h->n_fields      = nm_get_u32(b + 28);
    h->flags         = nm_get_u32(b + 32);
    h->payload_bytes = nm_get_u64(b + 36);
    memcpy(h->source, b + 44, 32);
    h->source[31] = '\0';
}

static void field_to_bytes(const NMBinField *f, uint8_t *b)
{
    memset(b, 0, NMBIN_FIELD_BYTES);
    memcpy(b, f->name, 16);
    nm_put_u16(b + 16, f->offset);
    b[18] = f->type;
    b[19] = f->width;
}

static void bytes_to_field(const uint8_t *b, NMBinField *f)
{
    memset(f, 0, sizeof(*f));
    memcpy(f->name, b, 16);
    f->name[15] = '\0';
    f->offset = nm_get_u16(b + 16);
    f->type   = b[18];
    f->width  = b[19];
}
```

### 3. `nmbin_open` 与 `nmbin_check`（`binfmt.c:177-226`）

`nmbin_open` 用 `nm_mmap_file` 直接映射整个文件，头部校验在 `nmbin_check`：

```c
int nmbin_open(const char *path, NMBinFile *out)
{
    memset(out, 0, sizeof(*out));
    size_t flen = 0;
    void *p = nm_mmap_file(path, &flen);      /* 平台层：mmap / CreateFileMapping */
    if (!p) return -1;

    bytes_to_hdr((const uint8_t *)p, &out->hdr);
    if (memcmp(out->hdr.magic, NMBIN_MAGIC, 8) != 0) { nm_munmap(p, flen); return -1; }
    const uint8_t *fp = (const uint8_t *)p + NMBIN_HDR_BYTES;
    for (uint32_t i = 0; i < out->hdr.n_fields && i < NMBIN_MAX_FIELDS; i++)
        bytes_to_field(fp + i * NMBIN_FIELD_BYTES, &out->fields[i]);

    out->data = (uint8_t *)p + NMBIN_HDR_BYTES + NMBIN_FIELD_BYTES * NMBIN_MAX_FIELDS;
    out->data_len = flen - (NMBIN_HDR_BYTES + NMBIN_FIELD_BYTES * NMBIN_MAX_FIELDS);
    out->mapped_len = flen;
    out->mapped = 1;
    return 0;
}

void nmbin_close(NMBinFile *f)
{
    if (f && f->mapped && f->data) nm_munmap(f->data - (NMBIN_HDR_BYTES + NMBIN_FIELD_BYTES * NMBIN_MAX_FIELDS),
                                             f->mapped_len);
    if (f) { f->data = NULL; f->mapped = 0; }
}

int nmbin_check(const NMBinFile *f, char *err, size_t err_cap)
{
#define ERR(...) do { snprintf(err, err_cap, __VA_ARGS__); return -1; } while (0)
    if (memcmp(f->hdr.magic, NMBIN_MAGIC, 8) != 0) ERR("magic 不对");
    if (f->hdr.version != NMBIN_VERSION) ERR("版本 %u 不支持", f->hdr.version);
    if (f->hdr.rec_size == 0) ERR("rec_size 为 0");
    if (f->hdr.payload_bytes != f->hdr.rec_count * (uint64_t)f->hdr.rec_size)
        ERR("payload_bytes %llu != rec_count %llu × rec_size %u",
            (unsigned long long)f->hdr.payload_bytes,
            (unsigned long long)f->hdr.rec_count, f->hdr.rec_size);
    if (f->data_len != f->hdr.payload_bytes)
        ERR("文件实际记录区 %zu 字节，头部声明 %llu 字节",
            f->data_len, (unsigned long long)f->hdr.payload_bytes);
    for (uint32_t i = 0; i < f->hdr.n_fields; i++) {
        const NMBinField *fd = &f->fields[i];
        if ((uint32_t)fd->offset + fd->width > f->hdr.rec_size)
            ERR("字段 %s 越界（offset %u + width %u > rec_size %u）",
                fd->name, fd->offset, fd->width, f->hdr.rec_size);
    }
    err[0] = '\0';
    return 0;
#undef ERR
}
```

> 写入侧的 `nmbin_create`/`nmbin_add_field`/`nmbin_write_records`/`nmbin_finalize` 实现（含"先写 0 记录数、写完回填"的小技巧）见 `binfmt.c:98-175`。为控制篇幅省略其完整体，可在 `src/map/binfmt.c:98-175` 查阅。

### 4. NamePool（下沉到 map 层，`namepool.h:19-24`、`namepool.c:24-60`）

```c
typedef struct {
    uint32_t    n;
    uint32_t   *off;      /* n+1，off[id] → blob 内偏移 */
    char       *blob;
    int         ready;
} NamePool;

int name_pool_open(NamePool *np, const char *names_path)
{
    memset(np, 0, sizeof(*np));

    NMBinFile nf;
    if (nmbin_open(names_path, &nf) != 0) return -1;

    const uint8_t *d = nf.data;
    uint32_t cnt = nm_get_u32(d);
    size_t offs_bytes = (size_t)(cnt + 1) * 4;
    if (nf.data_len < 4 + offs_bytes) { nmbin_close(&nf); return -1; }

    np->n = cnt;
    np->off = calloc(cnt + 1, sizeof(uint32_t));
    np->blob = malloc(nf.data_len - 4 - offs_bytes + 1);
    if (!np->off || !np->blob) {
        name_pool_close(np);
        nmbin_close(&nf);
        return -1;
    }
    for (uint32_t i = 0; i <= cnt; i++)
        np->off[i] = nm_get_u32(d + 4 + (size_t)i * 4);
    memcpy(np->blob, d + 4 + offs_bytes, nf.data_len - 4 - offs_bytes);
    np->blob[nf.data_len - 4 - offs_bytes] = '\0';

    nmbin_close(&nf);
    np->ready = 1;
    return 0;
}

void name_pool_close(NamePool *np)
{
    if (!np) return;
    free(np->off);
    free(np->blob);
    memset(np, 0, sizeof(*np));   /* 失败路径的指针都已置空，重复调用安全 */
}
```

### 5. `MapSrc` 全字段与 `MapSet`（`mapset.h:22-104`）

```c
typedef struct {
    char      name[64];      /* 目录名（省名） */
    char      dir[4096];     /* 省目录绝对路径：派生文件（rev.bin 等）要按它找 */
    NMBinFile nodes, edges;
    NMBinFile restrictions;             /* 转向限制（可选，阶段 8 路由用） */
    int       has_restr;
    NMBinFile lines, areas, polyrefs;   /* 线/面要素（旧目录可没有） */
    int       has_features;             /* 三者齐备才为 1 */
    /* ---- 等高线（SRTM 生成，可选）---- */
    NMBinFile contours, contour_polyrefs, contour_pts;
    int       has_contours;             /* 三者齐备才为 1 */
    int32_t  *contour_bbox;            /* 等高线缓存（paint_contours 懒填，nc*4 个 i32） */
    /* ★ 显示层的"来源"：0 = 从 nmbin 打开（默认）；1 = 从 PBF 运行时解码。 */
    int       dmap_from_pbf;
    int       pbf_display_mode;  /* 1 = 本省走 PBF 显示层（不打开 nmbin 的
                                   * lines/areas/polyrefs） */
    PbfMap    dmap;                     /* 从 PBF 解码出的显示层（内存缓冲） */
    uint64_t  dmap_used;                /* LRU 时间戳（越大越新） */
    int       dmap_inflight;  /* 后台解码进行中：src_unload 不得卸 nodes 映射 */
    int       dmap_failed;    /* 该省 PBF 解码失败：重载前不再重试 */
    int       dmap_progress;  /* 0..100，后台解码实时进度（dmap_inflight/排队期间有效） */
    NMBinFile tiles, edgelist;          /* 空间索引（可选） */
    int       has_tiles;
    int       loaded;       /* 本省份 mmap 是否已打开；mapset_set_view 按需置 1 / 清 0。 */
    int       dup_of;       /* >=0：本省数据被 srcs[dup_of] 完全包含（同一份
                             * OSM 数据被多边形裁切时的常见结果，实测
                             * 北京/天津 ⊂ 河北 100%）。路由缝合时跳过本省的
                             * 副本——留着只会让每个路口被展开两次。 */
    int32_t   bbox[4];      /* e7: lat0 lon0 lat1 lon1，来自 manifest； */
    NamePool  np;           /* 该省的路名池（names.bin） */
    NMBinFile lanes;        /* 每条边的车道转向串 id（lanes.bin，可选） */
    NamePool  lnp;          /* 车道转向串池（lane_names.bin，可选） */
    int       has_lanes;    /* lanes.bin + lane_names.bin 齐备 */
    int32_t  *area_bbox;    /* 面缓存（paint_areas 懒填，na*4 个 i32） */
    int32_t  *line_bbox;    /* 线缓存（paint_lines 懒填，nl*4 个 i32）。 */

    /* ---- 显示层分 tile（运行时按需加载，只 mmap 视口内 tile）---- */
    int       has_disp_tiles;
    int64_t   disp_tile_e7, disp_lat0, disp_lon0;  /* 网格参数（e7） */
    int       disp_ncols, disp_nrows;
    uint8_t  *dareas, *dlines, *dpoly;   /* 拼接缓冲（手动 malloc/free） */
    size_t    dea, del, depo;             /* 三个缓冲当前字节数 */
    int64_t   disp_sig;      /* 上次拼接的 tile 集合签名，避免每帧重建 */
} MapSrc;

typedef struct {
    MapSrc *srcs;
    int     n, cap;
    int32_t bbox[4];       /* 全体省份的并集（e7） */
    /* ---- PBF 显示层按需解码 + LRU ---- */
    char    pbf_dir[4096];
    int     dmap_lru_cap;  /* 同时驻留几个省的解码结果（默认 3） */
    uint64_t dmap_clock;   /* LRU 时钟（单调递增） */

    /* ---- 后台解码线程（避免同步解码阻塞渲染 0.6~4.4s）---- */
    NmMutex  *dmap_mtx;         /* 保护下面的任务槽；NULL 则退回同步解码 */
    int       dmap_quit;        /* 置 1 让 worker 退出 */
    int       dmap_stopped;     /* worker 已退出（free 时等待） */
    int       dmap_worker_started;
    int       job_prov;         /* 正在解码的省下标；-1 = 空闲 */
    int       job_done;         /* 有结果待发布 */
    int       job_done_prov;    /* 待发布结果属于哪个省 */
    int       job_ok;           /* 1 成功 / 0 失败 */
    PbfMap    job_result;       /* 解码产物（malloc 缓冲），发布时移交 src */
} MapSet;
```

### 6. `src_open_files`：加载即校验（`mapset.c:294-431`）

```c
static int src_open_files(MapSrc *s)
{
    s->dmap_inflight = 0;   /* 重载 = 重新尝试解码（清掉失败标记） */
    s->dmap_failed   = 0;
    char p[4096];
    char err[256];

    snprintf(p, sizeof(p), "%s/nodes.bin", s->dir);
    if (nmbin_open(p, &s->nodes) != 0) {
        fprintf(stderr, "图集: %s 打不开 %s，跳过\n", s->name, p);
        return -1;
    }
    if (nmbin_check(&s->nodes, err, sizeof(err)) != 0) {
        fprintf(stderr, "图集: %s nodes.bin 校验失败（%s），跳过该省\n", s->name, err);
        goto fail;
    }
    snprintf(p, sizeof(p), "%s/edges.bin", s->dir);
    if (nmbin_open(p, &s->edges) != 0) {
        fprintf(stderr, "图集: %s 打不开 %s，跳过\n", s->name, p);
        goto fail;
    }
    if (nmbin_check(&s->edges, err, sizeof(err)) != 0) {
        fprintf(stderr, "图集: %s edges.bin 校验失败（%s），跳过该省\n", s->name, err);
        goto fail;
    }

    /* 转向限制可选：路由用，没有就当省内没有任何禁转 */
    snprintf(p, sizeof(p), "%s/restrictions.bin", s->dir);
    s->has_restr = (nmbin_open(p, &s->restrictions) == 0);
    if (s->has_restr && nmbin_check(&s->restrictions, err, sizeof(err)) != 0) {
        fprintf(stderr, "图集: %s restrictions.bin 校验失败（%s），按无限制处理\n",
                s->name, err);
        nmbin_close(&s->restrictions);
        s->has_restr = 0;
    }

    /* 线/面要素可选：三者齐备才算有。PBF 模式下不从 nmbin 打开这三个文件。 */
    if (s->pbf_display_mode) {
        s->has_features = 0;      /* 等解码后再置 1 */
    } else if (disp_open(s) == 0) {
        s->has_features = 0;      /* 等 set_view 按视口加载 tile 后再置 1 */
    } else {
        s->has_features = 1;
        snprintf(p, sizeof(p), "%s/lines.bin", s->dir);
        if (nmbin_open(p, &s->lines) != 0) s->has_features = 0;
        if (s->has_features) {
            snprintf(p, sizeof(p), "%s/areas.bin", s->dir);
            if (nmbin_open(p, &s->areas) != 0) s->has_features = 0;
        }
        if (s->has_features) {
            snprintf(p, sizeof(p), "%s/polyrefs.bin", s->dir);
            if (nmbin_open(p, &s->polyrefs) != 0) s->has_features = 0;
        }
        if (!s->has_features)
            fprintf(stderr, "图集: %s 无 lines/areas/polyrefs，只显示路网\n", s->name);
    }

    /* 等高线（SRTM 生成）可选：三件套齐备才算有。 */
    s->has_contours = 1;
    char pc[4096], pp[4096], pt[4096];
    snprintf(pc, sizeof(pc), "%s/%s_contours.bin",        s->dir, s->name);
    snprintf(pp, sizeof(pp), "%s/%s_contour_polyrefs.bin", s->dir, s->name);
    snprintf(pt, sizeof(pt), "%s/%s_contour_pts.bin",      s->dir, s->name);
    if (nmbin_open(pc, &s->contours) != 0) {
        snprintf(pc, sizeof(pc), "%s/contours.bin", s->dir);
        if (nmbin_open(pc, &s->contours) != 0) s->has_contours = 0;
    }
    if (s->has_contours) {
        if (nmbin_open(pp, &s->contour_polyrefs) != 0) {
            snprintf(pp, sizeof(pp), "%s/contour_polyrefs.bin", s->dir);
            if (nmbin_open(pp, &s->contour_polyrefs) != 0) s->has_contours = 0;
        }
    }
    if (s->has_contours) {
        if (nmbin_open(pt, &s->contour_pts) != 0) {
            snprintf(pt, sizeof(pt), "%s/contour_pts.bin", s->dir);
            if (nmbin_open(pt, &s->contour_pts) != 0) s->has_contours = 0;
        }
    }
    if (s->has_contours)
        fprintf(stderr, "图集: %s 有等高线（%llu 条）\n", s->name,
                (unsigned long long)s->contours.hdr.rec_count);

    /* 空间索引可选：两者齐备才算有 */
    s->has_tiles = 1;
    snprintf(p, sizeof(p), "%s/tiles.bin", s->dir);
    if (nmbin_open(p, &s->tiles) != 0) s->has_tiles = 0;
    if (s->has_tiles) {
        snprintf(p, sizeof(p), "%s/edgelist.bin", s->dir);
        if (nmbin_open(p, &s->edgelist) != 0) s->has_tiles = 0;
    }

    /* 路名池可选 */
    snprintf(p, sizeof(p), "%s/names.bin", s->dir);
    if (name_pool_open(&s->np, p) != 0)
        fprintf(stderr, "图集: %s 无路名池（不画路名标注）\n", s->name);

    /* 车道指示可选：lanes.bin + lane_names.bin 齐备才有 */
    s->has_lanes = 1;
    snprintf(p, sizeof(p), "%s/lanes.bin", s->dir);
    if (nmbin_open(p, &s->lanes) != 0 || nmbin_check(&s->lanes, err, sizeof(err)) != 0)
        s->has_lanes = 0;
    if (s->has_lanes) {
        snprintf(p, sizeof(p), "%s/lane_names.bin", s->dir);
        if (name_pool_open(&s->lnp, p) != 0) s->has_lanes = 0;
    }

    /* 视口裁剪用的 bbox。没有 manifest（旧目录）就置全域，宁可多画。 */
    if (manifest_bbox(s->dir, s->bbox) != 0) {
        s->bbox[0] = -900000000; s->bbox[1] = -1800000000;
        s->bbox[2] =  900000000; s->bbox[3] =  1800000000;
    }
    return 0;

fail:
    nmbin_close(&s->nodes);
    nmbin_close(&s->edges);
    nmbin_close(&s->restrictions);
    nmbin_close(&s->lines);
    nmbin_close(&s->areas);
    nmbin_close(&s->polyrefs);
    nmbin_close(&s->tiles);
    nmbin_close(&s->edgelist);
    name_pool_close(&s->np);
    nmbin_close(&s->lanes);
    name_pool_close(&s->lnp);
    return -1;
}
```

### 7. `mapset_load` 全量（`mapset.c:693-766`）

```c
int mapset_load(MapSet *ms, const char *graph_dir)
{
    memset(ms, 0, sizeof(*ms));

    /* PBF 显示层后台解码：建锁 + 初始空闲任务槽（job_prov=-1 表示无在途）。 */
    ms->dmap_mtx = nm_mutex_create();
    ms->job_prov = -1;

    /* 单省目录：下面直接有 nodes.bin */
    char p[4096];
    if (nm_path_join(p, sizeof(p), graph_dir, "nodes.bin") == 0 &&
        nm_file_exists(p)) {
        const char *nm = nm_base_name(graph_dir);
        if (mapset_add(ms, graph_dir, nm) != 0) {
            fprintf(stderr, "图集: %s 加载失败\n", graph_dir);
            return -1;
        }
        return 0;
    }

    /* 父目录：扫描含 manifest.txt 的子目录。目录枚举顺序由文件系统决定，
       先收集名字再排序，保证加载顺序（以及日志）可复现。 */
    char **names = NULL;
    int nn = 0, cap = 0;
    NmDir *d = nm_dir_open(graph_dir);
    if (!d) {
        fprintf(stderr, "图集: 既不是省目录也没有 nodes.bin：%s\n", graph_dir);
        return -1;
    }
    const char *de;
    while ((de = nm_dir_next(d)) != NULL) {
        if (de[0] == '.') continue;
        /* 跳过 sync_maps 的过渡目录：.tmp.* 是转换中的半成品，.old.* 是已换下、
           删除失败时留下的旧产物。两者都含 manifest.txt，不排除就会被当成
           一个"省"加载进来 —— 同一份数据装两遍（内存翻倍 + 渲染重复）。 */
        if (strstr(de, ".tmp.")) continue;
        if (strstr(de, ".old.")) continue;
        if (nm_path_join(p, sizeof(p), graph_dir, de) != 0) continue;
        if (!nm_is_dir(p)) continue;
        char mf[4096];
        if (nm_path_join(mf, sizeof(mf), p, "manifest.txt") != 0) continue;
        if (!nm_file_exists(mf)) continue;    /* 没有产物指纹的不算省 */
        if (nn == cap) {
            cap = cap ? cap * 2 : 16;
            char **t = realloc(names, (size_t)cap * sizeof(char *));
            if (!t) break;
            names = t;
        }
        names[nn++] = nm_strdup(de);
    }
    nm_dir_close(d);

    if (nn == 0) {
        fprintf(stderr, "图集: %s 下没有含 manifest.txt 的省目录\n", graph_dir);
        return -1;
    }
    qsort(names, (size_t)nn, sizeof(char *), cmp_str);

    int ok = 0;
    for (int i = 0; i < nn; i++) {
        if (nm_path_join(p, sizeof(p), graph_dir, names[i]) == 0 &&
            mapset_add(ms, p, names[i]) == 0) ok++;
        free(names[i]);
    }
    free(names);

    if (ok == 0) {
        fprintf(stderr, "图集: %s 下一个省都没加载成功\n", graph_dir);
        return -1;
    }
    printf("图集: 共 %d 个省\n", ms->n);
    detect_duplicates(ms);
    return 0;
}
```

### 8. `mapset_set_view` 全量（含 LRU 淘汰，`mapset.c:787-854`）

```c
void mapset_set_view(MapSet *ms, const int32_t vb[4])
{
    if (!ms || ms->n == 0) return;
    mapset_publish_display(ms);   /* 把后台线程已解完的省挂上显示层 */
    /* 边距：视口四周各扩 50%，让即将滚入的相邻省提前打开，也覆盖吸附/导航预览 */
    int32_t m  = (vb[2] - vb[0]) / 2;
    int32_t mn = (vb[3] - vb[1]) / 2;
    int32_t eb[4] = { vb[0] - m, vb[1] - mn, vb[2] + m, vb[3] + mn };

    for (int i = 0; i < ms->n; i++) {
        MapSrc *s = &ms->srcs[i];
        int ov = bbox_overlap(s->bbox, eb);
        if (ov && !s->loaded) {
            s->loaded = (src_open_files(s) == 0);
        } else if (!ov && s->loaded) {
            src_unload(s);
            s->loaded = 0;
        }
        /* PBF 显示层：视口进省 ⇒ 后台线程异步解码（见 dmap_worker）。 */
        if (ov && ms->pbf_dir[0] && s->pbf_display_mode) {
            if (!s->dmap_from_pbf) {
                mapset_dispatch_display(ms, i);   /* 无任务在途才派发，异步 */
            } else {
                s->dmap_used = ++ms->dmap_clock;  /* 已就绪才刷新 LRU */
            }
        }

        /* 显示层分 tile：视口进省 ⇒ 按视口只加载相交 tile 并拼接进
         * areas/lines/polyrefs（同步、仅 mmap 视口内少量 tile，轻量不卡）。 */
        if (ov && s->has_disp_tiles) {
            src_load_disp_tiles(s, eb);
        }
    }

    /* LRU 淘汰：驻留超上限时，把**不在视口内**且最久未用的省显示层丢掉。 */
    if (ms->pbf_dir[0] && ms->dmap_lru_cap > 0) {
        for (;;) {
            int live = 0;
            for (int i = 0; i < ms->n; i++)
                if (ms->srcs[i].dmap_from_pbf) live++;
            if (live <= ms->dmap_lru_cap) break;
            /* 找 LRU 且不在视口内的 */
            int victim = -1;
            uint64_t oldest = UINT64_MAX;
            for (int i = 0; i < ms->n; i++) {
                MapSrc *s = &ms->srcs[i];
                if (!s->dmap_from_pbf) continue;
                if (bbox_overlap(s->bbox, eb)) continue;   /* 视口内，不淘汰 */
                if (s->dmap_used < oldest) { oldest = s->dmap_used; victim = i; }
            }
            if (victim < 0) break;    /* 全在视口内 ⇒ 不淘汰 */
            MapSrc *s = &ms->srcs[victim];
            pbfmap_free(&s->dmap);
            memset(&s->lines, 0, sizeof(s->lines));
            memset(&s->areas, 0, sizeof(s->areas));
            memset(&s->polyrefs, 0, sizeof(s->polyrefs));
            s->has_features = 0;
            s->dmap_from_pbf = 0;
        }
    }
}
```

> `bbox_overlap`（`mapset.c:777-780`，四角交集判定）与 `src_unload`（`mapset.c:446-498`，关文件/释放缓存但保留身份）为 `set_view` 的支撑函数，完整代码见源文件；为控制篇幅此处不重复粘贴，可在 `src/map/mapset.c:446-498` 与 `src/map/mapset.c:777-780` 查阅。

### 9. `dup_of` 检测：`detect_duplicates`（`mapset.c:608-657`）

```c
#define DUP_SAMPLE 1000

static void detect_duplicates(MapSet *ms)
{
    for (int i = 0; i < ms->n; i++) ms->srcs[i].dup_of = -1;

    for (int i = 0; i < ms->n; i++) {
        MapSrc *a = &ms->srcs[i];
        if (!a->has_tiles) continue;
        uint64_t na = a->nodes.hdr.rec_count;
        if (na == 0) continue;
        for (int j = 0; j < ms->n; j++) {
            if (i == j) continue;
            MapSrc *b = &ms->srcs[j];
            if (!b->has_tiles) continue;
            uint64_t nb = b->nodes.hdr.rec_count;
            if (nb < na) continue;
            if (a->bbox[0] < b->bbox[0] || a->bbox[1] < b->bbox[1] ||
                a->bbox[2] > b->bbox[2] || a->bbox[3] > b->bbox[3])
                continue;                       /* bbox 不包含 */

            /* ★ 采样必须从 edgelist 取，不能从 nodes 取 */
            uint64_t nea = a->edgelist.hdr.rec_count;
            if (nea == 0) continue;
            uint64_t stride = nea / DUP_SAMPLE;
            if (stride == 0) stride = 1;
            uint64_t tested = 0, hit = 0;
            for (uint64_t n = 0; n < nea && tested < DUP_SAMPLE; n += stride) {
                uint64_t ck = xl_from_key(a, n);
                if (ck == ~0ULL) continue;
                int32_t la = coord_key_lat(ck), lo = coord_key_lon(ck);
                double wx, wy;
                nm_mercator((double)la / 1e7, (double)lo / 1e7, &wx, &wy);
                double Z = (double)(1u << NM_TILE_Z);
                int tx = (int)(wx * Z), ty = (int)(wy * Z);
                tested++;
                if (tile_has_coord(b, tx, ty, ck)) hit++;
            }
            if (tested >= 100 && hit * 100 >= tested * 99) {
                a->dup_of = j;
                printf("图集: %s 的数据被 %s 完全包含（抽样 %llu/%llu 命中），"
                       "路由缝合时跳过其副本\n",
                       a->name, b->name,
                       (unsigned long long)hit, (unsigned long long)tested);
                break;
            }
        }
    }
}
```

### 10. `mapset_coverage` 与 `coverage_locate`（`coverage.c:38-94`、`coverage.c:211-278`）

```c
enum { COV_NONE = 0, COV_BBOX_ONLY = 1, COV_HAS_ROAD = 2, COV_UNKNOWN = -1 };

int mapset_coverage(const MapSet *ms, double lat, double lon, double radius_m,
                    char *prov_out, size_t cap)
{
    if (prov_out && cap) prov_out[0] = '\0';
    if (!ms || ms->n == 0) return COV_NONE;

    int32_t lat_e7 = (int32_t)(lat * 1e7);
    int32_t lon_e7 = (int32_t)(lon * 1e7);

    /* ---- 粗判：落在哪个省的 bbox 内 ---- */
    int hit = -1;
    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (lat_e7 < s->bbox[0] || lon_e7 < s->bbox[1] ||
            lat_e7 > s->bbox[2] || lon_e7 > s->bbox[3])
            continue;
        mapset_ensure_loaded((MapSet *)ms, i);
        if (hit < 0) hit = i;
    }
    if (hit < 0) return COV_NONE;                 /* 一个省的 bbox 都没盖住 */

    int detail = mapset_detail_prov(ms, lat_e7, lon_e7, hit);
    if (detail < 0 || detail >= ms->n) detail = hit;
    if (prov_out && cap) snprintf(prov_out, cap, "%s", ms->srcs[detail].name);

    /* 细判：那附近到底有没有路 */
    int have_index = 0;
    for (int i = 0; i < ms->n; i++)
        if (ms->srcs[i].loaded && ms->srcs[i].has_tiles) { have_index = 1; break; }
    if (!have_index) return COV_UNKNOWN;

    double wx, wy;
    nm_mercator(lat, lon, &wx, &wy);
    double Z = (double)(1u << NM_TILE_Z);
    int cx = (int)(wx * Z), cy = (int)(wy * Z);

    double tile_m = 40075000.0 / Z * cos(lat * 3.14159265358979 / 180.0);
    if (tile_m < 1.0) tile_m = 1.0;
    int r = (int)(radius_m / tile_m);
    if (r < 1) r = 1;
    if (r > 3) r = 3;                             /* 最多 7×7，够了 */

    CovHit h;
    if (tiles_have_edge(ms, cx, cy, r, &h) > 0) {
        if (h.prov >= 0 && h.prov < ms->n && prov_out && cap)
            snprintf(prov_out, cap, "%s", ms->srcs[h.prov].name);
        return COV_HAS_ROAD;
    }
    return COV_BBOX_ONLY;
}

int coverage_locate(const MapSet *ms, double lat, double lon, CovPlace *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->nearest_km = -1.0;
    if (!ms || ms->n == 0) return -1;

    int32_t lat_e7 = (int32_t)(lat * 1e7);
    int32_t lon_e7 = (int32_t)(lon * 1e7);

    /* ---- 落在哪个省的 bbox 里 ---- */
    int hit = -1;
    for (int i = 0; i < ms->n; i++) {
        const MapSrc *s = &ms->srcs[i];
        if (lat_e7 < s->bbox[0] || lon_e7 < s->bbox[1] ||
            lat_e7 > s->bbox[2] || lon_e7 > s->bbox[3])
            continue;
        if (hit < 0) hit = i;
    }

    if (hit >= 0) {
        int detail = mapset_detail_prov(ms, lat_e7, lon_e7, hit);
        if (detail < 0 || detail >= ms->n) detail = hit;
        const char *id = ms->srcs[detail].name;
        const char *cn = prov_cn(id);
        snprintf(out->prov_id, sizeof(out->prov_id), "%s", id);
        snprintf(out->prov,    sizeof(out->prov),    "%s", (cn && *cn) ? cn : id);
        snprintf(out->country, sizeof(out->country), "%s", (cn && *cn) ? "中国" : "");
        out->nearest_km = 0.0;
        snprintf(out->dl_name,  sizeof(out->dl_name),  "%s", id);
        snprintf(out->dl_label, sizeof(out->dl_label), "%s", (cn && *cn) ? cn : id);
        return detail;
    }

    /* ---- 一个 bbox 都没盖住：先用省会表推断"大致是哪个省"，再报距离 ---- */
    double nearest_loaded_km = 1e18;
    for (int i = 0; i < ms->n; i++) {
        double d = bbox_dist_km(ms->srcs[i].bbox, lat_e7, lon_e7);
        if (d < nearest_loaded_km) nearest_loaded_km = d;
    }

    double cap_km = -1.0;
    int ci = nearest_capital(lat, lon, &cap_km);
    if (ci >= 0) {
        out->prov_guessed = 1;
        out->nearest_km   = cap_km;
        snprintf(out->prov_id, sizeof(out->prov_id), "%s", CAPITAL[ci].id);
        snprintf(out->prov,    sizeof(out->prov),    "%s", CAPITAL[ci].cn);
        snprintf(out->dl_name,  sizeof(out->dl_name),  "%s", CAPITAL[ci].id);
        snprintf(out->dl_label, sizeof(out->dl_label), "%s", CAPITAL[ci].cn);
    }
    if (nearest_loaded_km < 1e17)
        out->nearest_loaded_km = nearest_loaded_km;

    /* 国别：只有"离某个已加载省足够近"才敢说中国境内 */
    if (nearest_loaded_km <= COV_INLAND_KM)
        snprintf(out->country, sizeof(out->country), "%s", "中国");

    return -1;
}
```

> `CovPlace` 结构与 `coverage_hint`（生成面向用户文案）的实现见 `coverage.h:35-47` 与 `coverage.c:280-387`；为控制篇幅省略其完整体，可在对应行号查阅。

### 11. `autobuild_repair` 全量（`autobuild.c:471-586`）

```c
int autobuild_repair(const char *graph_dir, const AutoBuildOpt *opt,
                     AutoBuildStat *st_out)
{
    AutoBuildStat st;
    memset(&st, 0, sizeof(st));
    double t0 = now_ms();

    AutoBuildOpt o;
    o.enable  = 1;
    o.src_dir = NULL;
    o.tool    = NULL;
    if (opt) o = *opt;

    char src_dir[4096], tool[4096];
    if (o.src_dir && *o.src_dir) {
        snprintf(src_dir, sizeof(src_dir), "%s", o.src_dir);
    } else if (find_src_dir(src_dir, sizeof(src_dir), graph_dir) != 0) {
        fprintf(stderr, "[自动重建] 探测不到源地图目录（sources/），跳过自检；"
                        "可用 --sources=<目录> 指定。\n");
        goto done;
    }

    char single_name[256] = "";
    int single = is_single_prov(graph_dir, src_dir, single_name, sizeof(single_name));

    char parent[4096];
    if (single) {
        snprintf(parent, sizeof(parent), "%s", graph_dir);
        char *slash = (char *)nm_last_sep(parent);
        if (slash && slash != parent) *slash = '\0';
        else snprintf(parent, sizeof(parent), ".");
    } else {
        snprintf(parent, sizeof(parent), "%s", graph_dir);
    }

    NameList nl = { NULL, 0, 0 };
    if (single) {
        nl_add(&nl, single_name);
    } else {
        list_provs(src_dir, &nl);
        list_existing_provs(graph_dir, &nl);   /* 补上"没有 pbf 的省目录" */
    }
    char **names = nl.v;
    int nn = nl.n;
    if (nn > 1) qsort(names, (size_t)nn, sizeof(char *), cmp_str);

    /* 转换器只在真的要干活时才找，别让正常启动多一次探测开销 */
    int have_tool = 0;
    for (int i = 0; i < nn; i++) {
        const char *name = names[i];
        char pbf[4096], prov_dir[4096], why[256];
        st.checked++;          /* 扫到的每个目录都算检查过，包括"没有原料"的 */

        if (single) {
            size_t gl = strlen(graph_dir);
            if (gl >= sizeof(prov_dir)) continue;
            memcpy(prov_dir, graph_dir, gl + 1);
        }
        else if (nm_path_join(prov_dir, sizeof(prov_dir), graph_dir, name) != 0)
            continue;                        /* 路径过长，跳过这个省 */

        if (find_pbf(src_dir, name, pbf, sizeof(pbf)) != 0) {
            /* 源都没有：只有在产物也不可用时才值得喊一声 */
            if (prov_check(prov_dir, "(无)", why, sizeof(why)) != PV_OK) {
                fprintf(stderr, "[自动重建] %s 产物不可用（%s），但 sources/ 下没有 "
                                "%s.osm.pbf —— 没有原料，无法重建。\n",
                        name, why, name);
                st.nosrc++;
            }
            continue;
        }

        int pv = prov_check(prov_dir, pbf, why, sizeof(why));
        if (pv == PV_OK) { st.ok++; continue; }

        if (!o.enable) {
            fprintf(stderr, "[自动重建] %s 需要重建（%s）。已用 --no-autobuild 关闭"
                            "自动重建，手工执行：\n"
                            "           ./scripts/sync_maps.sh --only=%s\n",
                    name, why, name);
            st.skipped++;
            continue;
        }

        if (!have_tool) {
            if (o.tool && *o.tool) snprintf(tool, sizeof(tool), "%s", o.tool);
            else if (find_tool(tool, sizeof(tool)) != 0) {
                fprintf(stderr, "[自动重建] 找不到 graph_extract，无法自动重建。"
                                "手工执行：./scripts/sync_maps.sh\n");
                st.failed += (nn - i);
                break;
            }
            have_tool = 1;
        }

        fprintf(stderr, "[自动重建] %s 需要重建：%s\n", name, why);
        if (rebuild_one(parent, prov_dir, name, pbf, tool) == 0) st.rebuilt++;
        else st.failed++;
    }

    for (int i = 0; i < nn; i++) free(names[i]);
    free(names);

done:
    st.elapsed_ms = now_ms() - t0;
    if (st.checked && (st.rebuilt || st.failed || st.nosrc || st.skipped))
        fprintf(stderr,
                "[自动重建] 检查 %d 省：完好 %d、重建 %d、失败 %d、无源 %d、跳过 %d"
                "（%.0f ms）\n",
                st.checked, st.ok, st.rebuilt, st.failed, st.nosrc, st.skipped,
                st.elapsed_ms);
    if (st_out) *st_out = st;
    return 0;
}
```

> `prov_check`（五条判据：目录缺失/文件不全/binfmt 校验失败/版本不符/源指纹不符）、`rebuild_one`、`find_tool`/`find_src_dir`/`find_pbf`、`lock_acquire`/`lock_release` 等支撑函数为完整自愈逻辑，见 `autobuild.c:175-377` 与 `autobuild.c:62-116`；为控制篇幅省略其完整体，可在对应行号查阅。

### 12. `RoadTags` 全字段与 `road_tags_parse` 全量（`road_tags.h:49-76`、`road_tags.c:153-276`）

```c
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
    const char *turn_lanes_fwd;
    size_t   turn_lanes_fwd_len;
    const char *turn_lanes_bwd;
    size_t   turn_lanes_bwd_len;
} RoadTags;

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
    const char *tl = NULL, *tl_fwd = NULL, *tl_bwd = NULL;
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
        else if (strcmp(k, "turn:lanes") == 0) tl = v;
        else if (strcmp(k, "turn:lanes:forward") == 0) tl_fwd = v;
        else if (strcmp(k, "turn:lanes:backward") == 0) tl_bwd = v;
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

    if (tl_fwd && *tl_fwd) { out->turn_lanes_fwd = tl_fwd; out->turn_lanes_fwd_len = strlen(tl_fwd); }
    else if (tl && *tl)    { out->turn_lanes_fwd = tl;     out->turn_lanes_fwd_len = strlen(tl); }
    if (tl_bwd && *tl_bwd) { out->turn_lanes_bwd = tl_bwd; out->turn_lanes_bwd_len = strlen(tl_bwd); }

    if (!hw) {
        if (railway) {
            for (int i = 0; kRailKinds[i]; i++)
                if (eq_ci(railway, kRailKinds[i])) {
                    out->show = 1; out->road_class = RC_RAIL; break;
                }
        } else if (waterway && !eq_ci(waterway, "riverbank")) {
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
        out->unknown_value = 1;
        out->drivable      = 1;
        out->road_class    = RC_OTHER;
        out->speed_kmh     = 30;
        out->whitelisted   = road_tag_value_is_known_nondrivable(hw) ? 1 : 0;
    }

    /* 面要素：与线判定互斥时优先面。 */
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
```

## 四、与其他功能之间的联系

- **上游 · 第 5 课（建图）**：`graph_build` 写出 `nodes/edges/… .bin`；`autobuild_repair` 是其损坏兜底（第 13 课架构门禁也复用其判据）。
- **下游 · 第 8 课（渲染）**：`paint_frame` 经 `mapset` 迭代 `srcs` 取数据画多省；`line_bbox`/`area_bbox` 懒填缓存把每帧白投影从 75% 降到可忽略。
- **下游 · 第 10 课（路由）**：`sew.c` 经 `mapset_query_tile`（遍历每省 `tiles.bin` 网格，命中 `(tx,ty)` 即回调）取跨省边；`dup_of` 在缝合时被跳过，避免同一路口被展开两次。
- **下游 · 第 9 课（搜索/地点）**：`coverage_locate` 把"用户现在在哪"反查到省，供地点查询与"该下哪块"提示。
- **与 L2（平台抽象）关系**：`nmbin_open` 的 `nm_mmap_file`、后台线程 `nm_thread_spawn`、锁 `nm_mutex_create`、进程派生 `nm_spawn_wait` 全部来自 L2，说明自描述格式只是数据布局，IO/并发/进程仍由平台层统一。
- **与 L1（坐标系）关系**：所有 `bbox`/`bbox[4]` 均为 e7 整数坐标；`mapset_detail_prov` 用 `coord_key_e7` 做坐标键比较。

## 五、演示、练习与验收

- **演示**：`./build_rel/dmap_selftest`（后台异步解码：set_view 不阻塞 + 进度回调）；`./build_rel/disp_tile_selftest`（显示层分 tile 拼接）。
- **练习**：用 `xxd` 看一个 `.nmbin` 文件头的 magic 与字段表（偏移 0 起 8 字节应为 `NAVMBIN\x01`，偏移 8 的 u32 版本应为 3），手动解析出第一个字段的偏移（头 128 字节 + 字段表从 128 起，每个 24 字节）。
- **验收**：① 能说清"自描述二进制 vs 定长记录"的取舍，以及为什么本项目选 NAVMBIN（丢源码也能读、避免 aarch64 对齐/填充差异）；② 能解释 `dup_of`（北京⊂河北 100%）如何在路由缝合时被跳过、在渲染时因按 OSM id 去重而不受影响；③ 能解释 `namepool` 下沉到 map 层消除了 `map→render` 模块倒置；④ 能指出 `mapset_set_view` 的 LRU 淘汰只淘汰"不在视口内且最久未用"的 PBF 解码省。
