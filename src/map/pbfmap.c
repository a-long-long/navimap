/* 从 OSM PBF 直接解码出显示层数据（省粒度）。
 *
 * 与 graph_build.c 的关系：显示层的**分类规则与记录布局完全复用** graph_build
 * 的那套（road_tags 分类 + 三分支分流 + polyrefs 下标分配），只是把"落盘"
 * 换成"写进内存缓冲"。渲染端（paint.c）消费的是 NMBinFile 的字节视图，
 * 因此本模块产出的缓冲与落盘的同名文件**逐字节一致** ⇒ 渲染端零改动。
 *
 * 为什么"省粒度"：实测 PBF 的 data blob 平均覆盖全省 43% 的纬度跨度
 * （北京 621 个 blob，单个跨 0.99°/1.33°，z11 视口仍命中 53%），
 * 按 blob 跳读不划算 ⇒ 整省解一次，缓存起来。
 *
 * ★ 为什么**不扫第二遍 PBF 取坐标**：polyrefs 存的是"节点内部下标"，
 * 这个下标空间 = nmbin nodes.bin 的下标空间（都按 osm id 升序、都去掉了
 * 文件里不存在的节点）。所以只要拿 nodes.bin 的 osm_id 数组做二分查表，
 * 就能把 way 的节点 id 映射成内部下标 —— 省掉整遍 PBF 扫描（河北约 4 s）
 * 与两份坐标数组（河北 105 MB）。
 *
 * ★ 下标空间必须与 graph_build 一致，体现在两处收集规则上：
 *   ① refs 收集**包含路网 way 的节点**（graph_build 在"路由/线/面"三分支
 *      **之前**就累加 refs），少收一类 ⇒ 下标错位 ⇒ 坐标全错；
 *   ② 名字池 id 空间与落盘版一致（路网 way 也要 intern，虽然不建记录）。
 *
 * 为什么"显示层"不包含路网：路网是 edges/nodes/edgelist 那套 CSR，
 * 渲染要的是"边 + 两端坐标"（edgelist v2），而 A* 需要全图 ⇒ 路网仍用 nmbin。
 */

#include "map/pbfmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map/road_tags.h"
#include "pbf/pbf_block.h"
#include "pbf/pbf_stream.h"
#include "utils/strpool.h"

/* ------------------------------------------------------------------ */
/* 小工具                                                               */
/* ------------------------------------------------------------------ */

typedef struct { char *p; size_t n, cap, esz; } Buf;

static int buf_reserve(Buf *b, size_t add)
{
    size_t need = b->n + add;
    if (need <= b->cap) return 0;
    size_t ncap = b->cap ? b->cap : 64;
    while (ncap < need) ncap *= 2;
    char *np = realloc(b->p, ncap * b->esz);
    if (!np) return -1;
    b->p = np; b->cap = ncap;
    return 0;
}

static void buf_free(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

/* 在升序 int64 数组里找第一个 >= key 的位置 */
static size_t lower_bound_i64(const int64_t *a, size_t n, int64_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (a[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

static const char *str_fn(uint32_t sid, void *ud)
{
    return pbf_str((PbfBlock *)ud, sid);
}

/* ------------------------------------------------------------------ */
/* 内存版 nmbin：构造与落盘版同布局                                      */
/* ------------------------------------------------------------------ */

/* 把一个 NMBinFile 填成"内存缓冲"形态：data 由 malloc 持有，mapped=0。
 * hdr 的字段与落盘版一致（magic/version/kind/rec_size/rec_count/
 * payload_bytes/source），字段表也照填 —— nmbin_check 会校验它们。 */
static void memfile_init(NMBinFile *f, uint32_t kind, uint32_t rec_size,
                         const char *source_hint)
{
    memset(f, 0, sizeof(*f));
    memcpy(f->hdr.magic, NMBIN_MAGIC, 8);
    f->hdr.version  = NMBIN_VERSION;
    f->hdr.kind     = kind;
    f->hdr.rec_size = rec_size;
    f->hdr.n_fields = 0;
    f->hdr.rec_count = 0;
    if (source_hint) snprintf(f->hdr.source, sizeof(f->hdr.source), "%s", source_hint);
}

static void memfile_add_field(NMBinFile *f, const char *name, uint16_t offset,
                              uint8_t type, uint8_t width)
{
    if (f->hdr.n_fields >= NMBIN_MAX_FIELDS) return;
    NMBinField *fd = &f->fields[f->hdr.n_fields++];
    memset(fd, 0, sizeof(*fd));
    snprintf(fd->name, sizeof(fd->name), "%s", name);
    fd->offset = offset;
    fd->type   = type;
    fd->width  = width;
}

/* 把 Buf 里的记录搬进 f->data（并按 rec_count 记账）。
 * f->data 之后由 pbfmap_free 释放。 */
static int memfile_finish(NMBinFile *f, const Buf *b, uint64_t nrec)
{
    f->hdr.rec_count = nrec;
    f->hdr.payload_bytes = nrec * (uint64_t)f->hdr.rec_size;
    if (nrec == 0) { f->data = NULL; f->data_len = 0; return 0; }
    size_t bytes = (size_t)nrec * f->hdr.rec_size;
    uint8_t *d = malloc(bytes);
    if (!d) return -1;
    memcpy(d, b->p, bytes);
    f->data = d;
    f->data_len = bytes;
    f->mapped = 0;         /* malloc 持有，不是 mmap */
    f->mapped_len = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 解码主流程                                                          */
/* ------------------------------------------------------------------ */

/* 与 graph_build 的 LineRec / AreaRec 同构（字段顺序一致）。 */
typedef struct {
    int64_t  osm_id;
    uint32_t ref_off, ref_cnt;
    uint8_t  cls, flags;
    uint32_t name_id;
} LineRec;

typedef struct {
    int64_t  osm_id;
    uint32_t ref_off, ref_cnt;
    uint8_t  kind;
} AreaRec;

int pbfmap_decode(const char *pbf_path, const NMBinFile *nodes_index,
                  PbfMap *out)
{
    if (!out || !pbf_path || !nodes_index) return -1;
    memset(out, 0, sizeof(*out));

    /* ---- 节点 id 表：来自 nmbin nodes.bin（已升序、已去掉死节点）----
     * 它就是 polyrefs 要引用的"内部下标"空间。 */
    uint64_t n_nodes = nodes_index->hdr.rec_count;
    int64_t *node_ids = malloc((n_nodes ? n_nodes : 1) * sizeof(int64_t));
    if (!node_ids) return -1;
    for (uint64_t i = 0; i < n_nodes; i++) {
        const uint8_t *nr = nodes_index->data + i * NM_NODE_BYTES;
        node_ids[i] = (int64_t)nm_get_u64(nr + NF_OSM_ID);
    }
    /* 保险：确认是升序（否则二分查表会错）。不该发生，但代价只是 O(n)。 */
    int sorted = 1;
    for (uint64_t i = 1; i < n_nodes; i++)
        if (node_ids[i] < node_ids[i - 1]) { sorted = 0; break; }
    if (!sorted)
        qsort(node_ids, n_nodes, sizeof(int64_t), cmp_i64);

    PbfStream *s = pbf_stream_open(pbf_path);
    if (!s) { free(node_ids); return -1; }

    /* ---- 收集：refs（way 节点 id 序列）+ 线/面记录 ---- */
    Buf refs;  memset(&refs,  0, sizeof(refs));  refs.esz  = sizeof(int64_t);
    Buf lines; memset(&lines, 0, sizeof(lines)); lines.esz = sizeof(LineRec);
    Buf areas; memset(&areas, 0, sizeof(areas)); areas.esz = sizeof(AreaRec);
    /* 名字池：id 空间必须与落盘版一致 ⇒ intern 顺序也必须一致。
     * graph_build 在 pass1 里按 PBF 遍历序对"路网 way"和"线要素 way"
     * 依次 sp_intern（面要素 way 不 intern）。这里照抄同一顺序，
     * 差别只在路网 way 只 intern 不建记录。 */
    StrPool *names = sp_create(1 << 20);
    if (!names) {
        free(node_ids);
        buf_free(&refs); buf_free(&lines); buf_free(&areas);
        pbf_stream_close(s);
        return -1;
    }

    const char *type; const uint8_t *buf; size_t len;
    PbfStatus rc;
    int failed = 0;

    while ((rc = pbf_stream_next(s, &type, &buf, &len)) == PBF_OK) {
        if (!type || strcmp(type, "OSMData") != 0) continue;
        PbfBlock *blk = NULL;
        if (pbf_unpack_block(buf, len, &blk) != 0) { failed = 1; break; }

        for (size_t gi = 0; gi < blk->pb->n_primitivegroup; gi++) {
            PrimitiveGroup *pg = blk->pb->primitivegroup[gi];
            if (!pg) continue;

            for (size_t i = 0; i < pg->n_ways; i++) {
                Way *w = pg->ways[i];
                if (!w) continue;

                RoadTags t;
                road_tags_parse(w->keys, w->n_keys, w->vals, w->n_vals,
                                str_fn, blk, &t);
                /* ★ refs 的收集范围必须与 graph_build 完全一致：它在
                 * "路由/线/面"三分支**之前**就把 way 的节点累加进去，
                 * 于是节点下标空间 = 全部三类 way 引用过的节点。
                 * polyrefs 里的下标就是这套空间，少收一类 ⇒ 下标错位 ⇒
                 * 坐标全错、画出面块乱飞。所以路网 way 也要累加 refs。 */
                if (!t.drivable && !t.show && !t.area_kind) continue;
                if (w->n_refs < 2) continue;

                uint32_t off = (uint32_t)(refs.n / sizeof(int64_t));
                if (buf_reserve(&refs, w->n_refs * sizeof(int64_t)) != 0) {
                    failed = 1; break;
                }
                int64_t *rp = (int64_t *)refs.p;
                int64_t acc = 0;
                for (size_t k = 0; k < w->n_refs; k++) {
                    acc += w->refs[k];
                    rp[refs.n / sizeof(int64_t)] = acc;
                    refs.n += sizeof(int64_t);
                }

                if (t.drivable) {
                    /* 路网部分不在本模块（走 nmbin 的 CSR/边表），
                     * 但节点必须进 refs（见上面的 ★），
                     * 名字也必须 intern（名字池 id 空间要与落盘版一致）。 */
                    if (t.name) sp_intern(names, t.name, t.name_len);
                    continue;
                }
                if (t.show) {
                    LineRec lr;
                    memset(&lr, 0, sizeof(lr));
                    lr.osm_id  = w->id;
                    lr.ref_off = off;
                    lr.ref_cnt = (uint32_t)w->n_refs;
                    lr.cls     = t.road_class;
                    lr.flags   = 0;
                    if (t.bridge) lr.flags |= EF_BRIDGE;
                    if (t.tunnel) lr.flags |= EF_TUNNEL;
                    lr.name_id = t.name ? sp_intern(names, t.name, t.name_len) : 0;
                    if (buf_reserve(&lines, sizeof(LineRec)) != 0) { failed = 1; break; }
                    memcpy(lines.p + lines.n, &lr, sizeof(LineRec));
                    lines.n += sizeof(LineRec);
                } else {
                    /* 面要素候选：只有闭合 way 才是面（与 graph_build 同一判据） */
                    uint32_t cnt = (uint32_t)w->n_refs;
                    const int64_t *r = (const int64_t *)refs.p;
                    if (cnt >= 4 && r[off] == r[off + cnt - 1]) {
                        AreaRec ar;
                        memset(&ar, 0, sizeof(ar));
                        ar.osm_id  = w->id;
                        ar.ref_off = off;
                        ar.ref_cnt = cnt;
                        ar.kind    = t.area_kind;
                        if (buf_reserve(&areas, sizeof(AreaRec)) != 0) { failed = 1; break; }
                        memcpy(areas.p + areas.n, &ar, sizeof(AreaRec));
                        areas.n += sizeof(AreaRec);
                    }
                }
            }
            if (failed) break;
        }
        pbf_block_free(blk);
        if (failed) break;
    }
    pbf_stream_close(s);
    if (failed || rc == PBF_ERR) {
        free(node_ids);
        buf_free(&refs); buf_free(&lines); buf_free(&areas);
        sp_destroy(names);
        return -1;
    }

    /* ---- polyrefs：refs 里的 osm id → nmbin nodes.bin 的内部下标 ----
     * graph_build 的 write_lines_areas 逻辑：逐个查 id，查不到就把整条
     * 要素丢弃（ref_cnt=0），并回退已写入的下标。 */
    Buf pr; memset(&pr, 0, sizeof(pr)); pr.esz = 4;
    LineRec *lr = (LineRec *)lines.p;
    AreaRec  *ar = (AreaRec *)areas.p;
    size_t nlines = lines.n / sizeof(LineRec);
    size_t nareas = areas.n / sizeof(AreaRec);
    const int64_t *rp = (const int64_t *)refs.p;

    uint32_t cursor = 0;
    for (size_t i = 0; i < nlines; i++) {
        uint32_t off = cursor;
        int dropped = 0;
        for (uint32_t k = 0; k < lr[i].ref_cnt; k++) {
            int64_t id = rp[lr[i].ref_off + k];
            size_t p = lower_bound_i64(node_ids, n_nodes, id);
            if (p >= n_nodes || node_ids[p] != id) { dropped = 1; break; }
            uint32_t idx = (uint32_t)p;
            if (buf_reserve(&pr, 4) != 0) { dropped = 1; break; }
            memcpy(pr.p + pr.n, &idx, 4); pr.n += 4;
            cursor++;
        }
        if (dropped) { pr.n = (size_t)off * 4; cursor = off; lr[i].ref_cnt = 0; continue; }
        lr[i].ref_off = off;
    }
    for (size_t i = 0; i < nareas; i++) {
        uint32_t off = cursor;
        int dropped = 0;
        for (uint32_t k = 0; k < ar[i].ref_cnt; k++) {
            int64_t id = rp[ar[i].ref_off + k];
            size_t p = lower_bound_i64(node_ids, n_nodes, id);
            if (p >= n_nodes || node_ids[p] != id) { dropped = 1; break; }
            uint32_t idx = (uint32_t)p;
            if (buf_reserve(&pr, 4) != 0) { dropped = 1; break; }
            memcpy(pr.p + pr.n, &idx, 4); pr.n += 4;
            cursor++;
        }
        if (dropped) { pr.n = (size_t)off * 4; cursor = off; ar[i].ref_cnt = 0; continue; }
        ar[i].ref_off = off;
    }

    /* ---- 组装三个内存版 nmbin ---- */
    int rc_ret = 0;
    const char *src_hint = pbf_path;

    memfile_init(&out->polyrefs, NMBIN_KIND_POLYREFS, 4, src_hint);
    memfile_add_field(&out->polyrefs, "node_idx", 0, NMT_U32, 4);
    if (memfile_finish(&out->polyrefs, &pr, pr.n / 4) != 0) { rc_ret = -1; goto done; }

    /* lines：跳过 ref_cnt==0 的丢弃条目 */
    {
        Buf lb; memset(&lb, 0, sizeof(lb)); lb.esz = NM_LINE_BYTES;
        for (size_t i = 0; i < nlines; i++) {
            if (lr[i].ref_cnt == 0) continue;
            uint8_t rec[NM_LINE_BYTES];
            memset(rec, 0, sizeof(rec));
            nm_put_u64(rec + LF_OSM_ID,  (uint64_t)lr[i].osm_id);
            nm_put_u32(rec + LF_REF_OFF, lr[i].ref_off);
            nm_put_u32(rec + LF_REF_CNT, lr[i].ref_cnt);
            rec[LF_CLASS]  = lr[i].cls;
            rec[LF_FLAGS]  = lr[i].flags;
            nm_put_u32(rec + LF_NAME_ID, lr[i].name_id);
            if (buf_reserve(&lb, sizeof(rec)) != 0) { rc_ret = -1; break; }
            memcpy(lb.p + lb.n, rec, sizeof(rec));
            lb.n += sizeof(rec);
        }
        memfile_init(&out->lines, NMBIN_KIND_LINES, NM_LINE_BYTES, src_hint);
        memfile_add_field(&out->lines, "osm_id",  LF_OSM_ID,  NMT_U64, 8);
        memfile_add_field(&out->lines, "ref_off", LF_REF_OFF, NMT_U32, 4);
        memfile_add_field(&out->lines, "ref_cnt", LF_REF_CNT, NMT_U32, 4);
        memfile_add_field(&out->lines, "class",   LF_CLASS,   NMT_U8,  1);
        memfile_add_field(&out->lines, "flags",   LF_FLAGS,   NMT_U8,  1);
        memfile_add_field(&out->lines, "name_id", LF_NAME_ID, NMT_U32, 4);
        if (rc_ret == 0 && memfile_finish(&out->lines, &lb, lb.n / NM_LINE_BYTES) != 0)
            rc_ret = -1;
        buf_free(&lb);
    }

    /* areas */
    {
        Buf ab; memset(&ab, 0, sizeof(ab)); ab.esz = NM_AREA_BYTES;
        for (size_t i = 0; i < nareas; i++) {
            if (ar[i].ref_cnt == 0) continue;
            uint8_t rec[NM_AREA_BYTES];
            memset(rec, 0, sizeof(rec));
            nm_put_u64(rec + AF_OSM_ID,  (uint64_t)ar[i].osm_id);
            nm_put_u32(rec + AF_REF_OFF, ar[i].ref_off);
            nm_put_u32(rec + AF_REF_CNT, ar[i].ref_cnt);
            rec[AF_KIND] = ar[i].kind;
            if (buf_reserve(&ab, sizeof(rec)) != 0) { rc_ret = -1; break; }
            memcpy(ab.p + ab.n, rec, sizeof(rec));
            ab.n += sizeof(rec);
        }
        memfile_init(&out->areas, NMBIN_KIND_AREAS, NM_AREA_BYTES, src_hint);
        memfile_add_field(&out->areas, "osm_id",  AF_OSM_ID,  NMT_U64, 8);
        memfile_add_field(&out->areas, "ref_off", AF_REF_OFF, NMT_U32, 4);
        memfile_add_field(&out->areas, "ref_cnt", AF_REF_CNT, NMT_U32, 4);
        memfile_add_field(&out->areas, "kind",    AF_KIND,    NMT_U8,  1);
        if (rc_ret == 0 && memfile_finish(&out->areas, &ab, ab.n / NM_AREA_BYTES) != 0)
            rc_ret = -1;
        buf_free(&ab);
    }

    out->n_areas   = out->areas.hdr.rec_count;
    out->n_lines   = out->lines.hdr.rec_count;
    out->n_polyrefs = out->polyrefs.hdr.rec_count;

done:
    free(node_ids);
    buf_free(&refs); buf_free(&lines); buf_free(&areas);
    buf_free(&pr);
    sp_destroy(names);
    if (rc_ret != 0) pbfmap_free(out);
    return rc_ret;
}

void pbfmap_free(PbfMap *m)
{
    if (!m) return;
    free(m->areas.data);
    free(m->lines.data);
    free(m->polyrefs.data);
    memset(m, 0, sizeof(*m));
}
