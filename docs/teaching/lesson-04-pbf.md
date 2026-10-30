# 第 4 课 · PBF 流式解析

> 模块：`src/pbf/pbf_stream.{c,h}`、`src/pbf/pbf_block.{c,h}`、`proto/osmformat.proto`
> 前置：第 2 课坐标量化（1e-7 度）、变长整数与 delta 编码概念。
> 环境：必须先 `source env.sh`（`libprotobuf-c` 装在 `~/.local`，见 `CMakeLists.txt:51-72`）。

## 一、教学目的

读懂 OSM PBF 的块结构，并避开**最经典的一个坑**——protobuf-c 的 stringtable 不是 NUL 结尾。学完后学生应能：用 `pbf_stream_next` 遍历一个 PBF 的所有 block；用 `pbf_str(sid)` 安全取字符串；解释为什么 `highway` 分类整类落空往往是因为拿 `data` 直接当 C 串。`PBF` 解析只负责"把下一个 blob 解成一段连续内存"与"把 stringtable 拷成 NUL 结尾副本"，坐标的 delta 累积解码（DenseNodes）并不在本模块，只暴露 `pbf_to_e7` 这一个换算工具（边界见第三节末）。

## 二、功能构成

| 文件 | 职责 |
|---|---|
| `pbf_stream.c/h` | 读 4 字节大端长度 → BlobHeader → Blob（zlib 解压）→ 返回类型/缓冲 |
| `pbf_block.c/h` | 把 `PrimitiveBlock` 薄封装；**把 stringtable 拷成 NUL 结尾副本** |
| `proto/osmformat.proto` | OSM 原始 schema（`DenseNodes` 的 delta 编码、`stringtable` 等） |

**关键常量与状态**（`pbf_stream.h:16-37`）：

```c
#define PBF_MAX_BLOB_BYTES (64u * 1024u * 1024u)  /* OSM 规范上限 64MB */

typedef struct PbfStream PbfStream;

typedef enum {
    PBF_OK   = 0,
    PBF_DONE = 1,
    PBF_ERR  = -1
} PbfStatus;

PbfStream *pbf_stream_open(const char *path);
void       pbf_stream_close(PbfStream *s);

PbfStatus  pbf_stream_next(PbfStream *s, const char **type,
                           const uint8_t **out_buf, size_t *out_len);

const char *pbf_stream_error(PbfStream *s);
uint64_t    pbf_stream_offset(PbfStream *s);
uint64_t    pbf_stream_file_size(PbfStream *s);
```

**解包句柄**（`pbf_block.h:18-36`）：

```c
typedef struct {
    PrimitiveBlock *pb;
    char          **strs;    /* n_str 个 NUL 结尾串 */
    size_t          n_str;
    char           *pool;    /* 字符串区（一次性释放） */
} PbfBlock;

int pbf_unpack_block(const uint8_t *buf, size_t len, PbfBlock **out);
void pbf_block_free(PbfBlock *b);

/* 越界返回 ""（OSM 约定 stringtable[0] 是空串） */
const char *pbf_str(PbfBlock *b, uint32_t sid);

/* granularity 单位是纳米度（1e-9），lat/lon 偏移同理；转成 1e-7 度要除以 100 */
static inline int64_t pbf_to_e7(PbfBlock *b, int64_t v, int64_t off)
{
    int64_t gran = (b && b->pb && b->pb->granularity) ? b->pb->granularity : 100;
    return (v * gran + off) / 100;
}
```

## 三、完整实现代码

### 1. 流状态结构与辅助函数（`pbf_stream.c:13-90`）

`PbfStream` 内部维护文件流、读偏移，以及四块可复用的缓冲（BlobHeader 原始字节、Blob 原始字节、解压后内容）。读大端长度、容量扩展、错误记录都是 `pbf_stream_next` 的支撑函数，一并给出完整代码：

```c
struct PbfStream {
    FILE    *fp;
    uint64_t file_size;
    uint64_t offset;

    uint8_t *hdr_buf;    /* BlobHeader 原始字节 */
    size_t   hdr_cap;

    uint8_t *blob_buf;   /* Blob 原始字节（压缩） */
    size_t   blob_cap;

    uint8_t *out_buf;    /* 解压后内容 */
    size_t   out_cap;

    char     cur_type[32];  /* 当前 blob 的 type（"OSMHeader"/"OSMData"） */
    char     err[256];
};

static void pbf_set_err(PbfStream *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void pbf_set_err(PbfStream *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, sizeof(s->err), fmt, ap);
    va_end(ap);
}

static int ensure_cap(uint8_t **buf, size_t *cap, size_t need)
{
    if (*cap >= need) return 0;
    size_t ncap = *cap ? *cap : 4096;
    while (ncap < need) ncap *= 2;
    uint8_t *p = realloc(*buf, ncap);
    if (!p) return -1;
    *buf = p;
    *cap = ncap;
    return 0;
}

PbfStream *pbf_stream_open(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    PbfStream *s = calloc(1, sizeof(PbfStream));
    if (!s) { fclose(fp); return NULL; }
    s->fp = fp;

    if (fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        s->file_size = sz > 0 ? (uint64_t)sz : 0;
        rewind(fp);
    }
    return s;
}

void pbf_stream_close(PbfStream *s)
{
    if (!s) return;
    if (s->fp) fclose(s->fp);
    free(s->hdr_buf);
    free(s->blob_buf);
    free(s->out_buf);
    free(s);
}

const char *pbf_stream_error(PbfStream *s) { return s->err[0] ? s->err : NULL; }
uint64_t pbf_stream_offset(PbfStream *s)   { return s->offset; }
uint64_t pbf_stream_file_size(PbfStream *s){ return s->file_size; }

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}
```

### 2. 块结构遍历核心：`pbf_stream_next`（`pbf_stream.c:92-217`）

文件布局：`[4B 大端 BlobHeader 长度][BlobHeader][Blob] …`。`BlobHeader.type = "OSMHeader" | "OSMData"`；Blob 内含 zlib 压缩的 `HeaderBlock`/`PrimitiveBlock`（也支持 raw）。核心步骤：读 4 字节大端长度 → `blob_header__unpack` → `blob__unpack` → 取 `raw`/`zlib_data` 分支 → zlib `uncompress` 并**校验解压长度 == `raw_size`**。`hdr_len`/`datasize`/`raw_size` 必须 `>0` 且 `≤ PBF_MAX_BLOB_BYTES`，否则 `PBF_ERR`（截断文件报错退出而非崩溃）。

```c
PbfStatus pbf_stream_next(PbfStream *s, const char **type,
                          const uint8_t **out_buf, size_t *out_len)
{
    unsigned char len4[4];
    size_t got = fread(len4, 1, 4, s->fp);
    if (got == 0) return PBF_DONE;
    if (got != 4) {
        pbf_set_err(s, "blob header 长度读取不完整（只剩 %zu 字节）", got);
        return PBF_ERR;
    }
    s->offset += 4;

    uint32_t hdr_len = read_be32(len4);
    if (hdr_len == 0 || hdr_len > PBF_MAX_BLOB_BYTES) {
        pbf_set_err(s, "blob header 长度非法: %u（偏移 %llu）",
                    hdr_len, (unsigned long long)s->offset);
        return PBF_ERR;
    }
    if (ensure_cap(&s->hdr_buf, &s->hdr_cap, hdr_len) != 0) {
        pbf_set_err(s, "内存不足（hdr %u 字节）", hdr_len);
        return PBF_ERR;
    }
    if (fread(s->hdr_buf, 1, hdr_len, s->fp) != hdr_len) {
        pbf_set_err(s, "blob header 读取不完整（偏移 %llu）",
                    (unsigned long long)s->offset);
        return PBF_ERR;
    }
    s->offset += hdr_len;

    BlobHeader *bh = blob_header__unpack(NULL, hdr_len, s->hdr_buf);
    if (!bh) {
        pbf_set_err(s, "BlobHeader 解析失败（偏移 %llu）",
                    (unsigned long long)s->offset);
        return PBF_ERR;
    }
    /* datasize 在 schema 里是 required，protobuf-c 不给它生成 has_ 标记 */
    if (!bh->type) {
        pbf_set_err(s, "BlobHeader 缺少 type");
        blob_header__free_unpacked(bh, NULL);
        return PBF_ERR;
    }
    snprintf(s->cur_type, sizeof(s->cur_type), "%s", bh->type);
    uint32_t datasize = (uint32_t)bh->datasize;
    blob_header__free_unpacked(bh, NULL);

    if (datasize == 0 || datasize > PBF_MAX_BLOB_BYTES) {
        pbf_set_err(s, "datasize 非法: %u", datasize);
        return PBF_ERR;
    }
    if (ensure_cap(&s->blob_buf, &s->blob_cap, datasize) != 0) {
        pbf_set_err(s, "内存不足（blob %u 字节）", datasize);
        return PBF_ERR;
    }
    if (fread(s->blob_buf, 1, datasize, s->fp) != datasize) {
        pbf_set_err(s, "blob 读取不完整（偏移 %llu，需要 %u 字节）",
                    (unsigned long long)s->offset, datasize);
        return PBF_ERR;
    }
    s->offset += datasize;

    Blob *blob = blob__unpack(NULL, datasize, s->blob_buf);
    if (!blob) {
        pbf_set_err(s, "Blob 解析失败（偏移 %llu）", (unsigned long long)s->offset);
        return PBF_ERR;
    }

    const uint8_t *src = NULL;
    size_t src_len = 0;
    int compressed = 0;
    size_t expect = blob->has_raw_size ? (size_t)blob->raw_size : 0;

    if (blob->raw.data && blob->raw.len) {
        src = blob->raw.data; src_len = blob->raw.len;
    } else if (blob->zlib_data.data && blob->zlib_data.len) {
        src = blob->zlib_data.data; src_len = blob->zlib_data.len;
        compressed = 1;
    } else {
        blob__free_unpacked(blob, NULL);
        pbf_set_err(s, "不支持的压缩方式（只有 raw / zlib）");
        return PBF_ERR;
    }

    if (!compressed) {
        if (ensure_cap(&s->out_buf, &s->out_cap, src_len) != 0) {
            blob__free_unpacked(blob, NULL);
            pbf_set_err(s, "内存不足（raw %zu 字节）", src_len);
            return PBF_ERR;
        }
        memcpy(s->out_buf, src, src_len);
        blob__free_unpacked(blob, NULL);
        *type = s->cur_type;
        *out_buf = s->out_buf;
        *out_len = src_len;
        return PBF_OK;
    }

    if (expect == 0 || expect > PBF_MAX_BLOB_BYTES) {
        blob__free_unpacked(blob, NULL);
        pbf_set_err(s, "raw_size 非法: %zu", expect);
        return PBF_ERR;
    }
    if (ensure_cap(&s->out_buf, &s->out_cap, expect) != 0) {
        blob__free_unpacked(blob, NULL);
        pbf_set_err(s, "内存不足（解压 %zu 字节）", expect);
        return PBF_ERR;
    }

    uLongf dst_len = (uLongf)s->out_cap;
    int zr = uncompress(s->out_buf, &dst_len, src, (uLong)src_len);
    blob__free_unpacked(blob, NULL);
    if (zr != Z_OK) {
        pbf_set_err(s, "zlib 解压失败 (%d)，偏移 %llu", zr,
                    (unsigned long long)s->offset);
        return PBF_ERR;
    }
    if (dst_len != expect) {
        pbf_set_err(s, "解压长度 %lu 与 raw_size %zu 不一致",
                    (unsigned long)dst_len, expect);
        return PBF_ERR;
    }

    *type = s->cur_type;
    *out_buf = s->out_buf;
    *out_len = dst_len;
    return PBF_OK;
}
```

### 3. 解包句柄与 ★ 经典坑：stringtable 不以 `\0` 结尾（`pbf_block.h:9-17` 注释 + `pbf_block.c:7-52`）

`pbf_block.h` 注释原文：

> ⚠️ 关键：protobuf-c 的 stringtable 是 ProtobufCBinaryData（指针+长度），**不保证以 `\0` 结尾**。直接把 data 当 C 串用，strcmp 会读到后面的垃圾，于是 "path" 变成 "path\x7f"、highway 分类全部落空、type=restriction 也认不出来。这个坑的表现是"统计数字看起来差不多但就是不对"，极难查。所以这里解包时就把整个 stringtable 拷成一份 NUL 结尾的副本，`pbf_str()` 返回的永远是可以安全 strcmp 的 C 串。

`pbf_unpack_block` 因此把整个 stringtable 拷成一份 NUL 结尾副本（关键：每个串手动补 0），`pbf_str` 永远返回可安全 `strcmp` 的 C 串；越界返回 `""`：

```c
int pbf_unpack_block(const uint8_t *buf, size_t len, PbfBlock **out)
{
    PrimitiveBlock *pb = primitive_block__unpack(NULL, len, buf);
    if (!pb) return -1;

    PbfBlock *b = calloc(1, sizeof(PbfBlock));
    if (!b) { primitive_block__free_unpacked(pb, NULL); return -1; }
    b->pb = pb;

    size_t n = pb->stringtable ? pb->stringtable->n_s : 0;
    b->n_str = n;
    if (n == 0) { *out = b; return 0; }

    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += pb->stringtable->s[i].len + 1;

    b->pool = malloc(total ? total : 1);
    b->strs = malloc(n * sizeof(char *));
    if (!b->pool || !b->strs) { pbf_block_free(b); return -1; }

    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        ProtobufCBinaryData d = pb->stringtable->s[i];
        b->strs[i] = b->pool + off;
        if (d.len) memcpy(b->pool + off, d.data, d.len);
        off += d.len;
        b->pool[off++] = '\0';
    }
    *out = b;
    return 0;
}

void pbf_block_free(PbfBlock *b)
{
    if (!b) return;
    if (b->pb) primitive_block__free_unpacked(b->pb, NULL);
    free(b->pool);
    free(b->strs);
    free(b);
}

const char *pbf_str(PbfBlock *b, uint32_t sid)
{
    if (!b || !b->strs || sid >= b->n_str) return "";
    return b->strs[sid];
}
```

### 4. ★ 边界说明：本模块只暴露 `pbf_to_e7`，DenseNodes 的 delta 累积解码在 `graph_build.c`

`pbf_block.h` 只给了 `pbf_to_e7`，它把"纳米度 × granularity + offset"再除以 100 换算成 1e-7 度（`v*gran + off)/100`，`granularity` 默认 100）。但 DenseNodes 的 `id/lat/lon` 三个数组是 **ZigZag + packed + 增量（delta）编码**：第一个值是绝对值，其后每个值是与前一个的差——这个 running accumulator 的维护**不在** `pbf_block.c`，而落在第 6 课的 `src/map/graph_build.c`。本节课只负责把 block 解出、并给出单个增量值到 e7 坐标的换算。

`graph_build.c:389-402` 的真实解码循环（只展示 delta 累积这一段，全文见该文件）：

```c
            int64_t acc_id = 0, acc_lat = 0, acc_lon = 0;
            for (size_t i = 0; i < dn->n_id; i++) {
                acc_id  += dn->id[i];
                acc_lat += dn->lat[i];
                acc_lon += dn->lon[i];
                size_t pos = lower_bound_i64(g->ids, g->n_ids, acc_id);
                if (pos >= g->n_ids || g->ids[pos] != acc_id) continue;
                int32_t la = (int32_t)pbf_to_e7(blk, acc_lat, blk->pb->lat_offset);
                int32_t lo = (int32_t)pbf_to_e7(blk, acc_lon, blk->pb->lon_offset);
                g->lat[pos]    = la;
                g->lon[pos]    = lo;
                g->osm_id[pos] = acc_id;
                bb_update(g, la, lo);
            }
```

也就是说：`pbf_block.c` 的"边界"是——它产出的是**已经解包、字符串安全**的 `PrimitiveBlock`（`pb->dense->lat[i]` 是增量值），但不替你做 `acc += lat[i]` 的累加；累加和最终换算必须到 `graph_build.c` 调用 `pbf_to_e7` 这一层才闭合。把 delta 解码误放到 `pbf_block` 里，会让第 6 课复用 PBF 的其他消费者（如 `pbfmap.c`）各自重复实现；把它留在 `graph_build.c`，则 `pbf_block` 保持"纯解包、无状态"的清晰边界。

## 四、与其他功能之间的联系

- **下游 · 第 5 课（自描述二进制）与第 6 课（建图）**：所有消费 PBF 的工具与建图代码都走同一循环 `while (pbf_stream_next(...)==PBF_OK) { if OSMData: pbf_unpack_block }`：
  - `map/graph_build.c` —— 建路网图（DenseNodes delta 累积 + `pbf_to_e7`，配合 `nm_fast_dist_m` 算边权）。
  - `map/pbfmap.c` —— 主解码阶段（第 13 课 Qt 前端的显示层数据源）。
  - `tools/places_build.c` —— 地名/POI（第 10 课）。
  - `tools/tag_stats.c`、`restr_probe.c`、`pbf_info.c` —— 标签统计与转向限制探测。
- **与 L3（平台抽象）关系**：`pbf_stream` 只依赖标准 `zlib` 与 `protobuf-c`，与 L3 的平台层无直接耦合；解出的路名经 `StrPool`/NamePool（L3/L5）去重后落 `names.bin`。
- **与 L2（坐标系）关系**：`pbf_to_e7` 是把 OSM 纳米度坐标归一到第 2 课定义的 1e-7 度整数坐标的关键换算，下游所有几何运算都以 e7 为单位。

## 五、演示、练习与验收

- **演示**：`./build_rel/pbf_info sources/beijing.osm.pbf`（看 block 结构、way 数、node 数）。
- **练习**：用 `pbf_info` 对比 `bbbike.pbf`（截断文件）与正常文件，确认截断文件**报错退出而非崩溃**（走 `PBF_ERR` 分支）。
- **验收**：① 能解释为什么 `pbf_str` 不能直接返回 protobuf-c 的原始 `data` 指针（缺少 NUL 结尾）；② 能指出 `pbf_block` 的边界——它给出的是增量值而非坐标，`acc +=` 累加与 `pbf_to_e7` 换算在 `graph_build.c`；③ 能画出 `pbf_stream_next` 的"读大端长度 → 解 BlobHeader → 解 Blob → zlib 解压并校验 raw_size"四步。
