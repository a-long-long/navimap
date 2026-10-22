/* graph_check —— 对 graph_extract 的产物做结构自检。
 *
 * 用法：./graph_check <目录>
 *
 * 为什么单独一个工具：图一旦上规模，肉眼不可能发现问题；而这些不变量
 * （文件大小自洽、CSR 连续、边下标不越界、经纬度在 bbox 内）一旦破了，
 * 后面渲染和路由会以各种离谱的方式崩，而且很难回溯到落盘这一步。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "map/binfmt.h"
#include "map/road_tags.h"

static int fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("ASSERT-FAIL  "); printf(__VA_ARGS__); printf("\n"); fail = 1; } } while (0)

static void dump_fields(const NMBinFile *f)
{
    printf("  kind=%u rec_size=%u count=%llu fields=%u\n",
           f->hdr.kind, f->hdr.rec_size,
           (unsigned long long)f->hdr.rec_count, f->hdr.n_fields);
    for (uint32_t i = 0; i < f->hdr.n_fields; i++)
        printf("    %-12s off=%2u type=%u width=%u\n",
               f->fields[i].name, f->fields[i].offset,
               f->fields[i].type, f->fields[i].width);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: %s <目录>\n", argv[0]); return 2; }
    const char *dir = argv[1];
    char p[4096];
    NMBinFile nf, ef, mf, rf;

#define OPEN(f, name) do { \
        snprintf(p, sizeof(p), "%s/%s", dir, name); \
        if (nmbin_open(p, &f) != 0) { printf("ASSERT-FAIL  打不开/格式错: %s\n", p); return 1; } \
        printf("== %s ==\n", name); dump_fields(&f); \
    } while (0)

    OPEN(nf, "nodes.bin");
    OPEN(ef, "edges.bin");
    OPEN(mf, "osmid.bin");
    OPEN(rf, "restrictions.bin");

    char err[256];
#define SELFTEST(f, name) do { \
        if (nmbin_check(&f, err, sizeof(err)) != 0) { \
            printf("ASSERT-FAIL  %s: %s\n", name, err); fail = 1; } \
    } while (0)
    SELFTEST(nf, "nodes.bin"); SELFTEST(ef, "edges.bin");
    SELFTEST(mf, "osmid.bin"); SELFTEST(rf, "restrictions.bin");

    uint64_t nn = nf.hdr.rec_count, ne = ef.hdr.rec_count, nm = mf.hdr.rec_count;
    CHECK(nf.hdr.rec_size == NM_NODE_BYTES, "nodes rec_size=%u 应为 %d",
          nf.hdr.rec_size, NM_NODE_BYTES);
    CHECK(ef.hdr.rec_size == NM_EDGE_BYTES, "edges rec_size=%u 应为 %d",
          ef.hdr.rec_size, NM_EDGE_BYTES);
    CHECK(nm == nn, "osmid 条数 %llu != 节点数 %llu",
          (unsigned long long)nm, (unsigned long long)nn);

    /* CSR：edge_begin 单调、最后一项等于总边数、每段与 edge_count 一致 */
    if (nn && ne) {
        uint32_t first = nm_get_u32(nf.data + NF_EDGE_BEGIN);
        CHECK(first == 0, "nodes[0].edge_begin=%u 应为 0", first);
        uint64_t sampled_bad = 0, oob = 0, empty_nodes = 0;
        uint64_t step = nn > 200000 ? nn / 200000 : 1;
        uint32_t prev = 0;
        for (uint64_t i = 0; i < nn; i += step) {
            const uint8_t *r = nf.data + i * NM_NODE_BYTES;
            uint32_t b = nm_get_u32(r + NF_EDGE_BEGIN);
            uint16_t c = nm_get_u16(r + NF_EDGE_COUNT);
            if (i && b < prev) sampled_bad++;
            if ((uint64_t)b + c > ne) oob++;
            if (c == 0) empty_nodes++;
            prev = b;
        }
        CHECK(sampled_bad == 0, "edge_begin 非单调（抽样命中 %llu 处）",
              (unsigned long long)sampled_bad);
        CHECK(oob == 0, "有节点的 [edge_begin, +count) 越界（%llu 处）",
              (unsigned long long)oob);
        uint32_t last = nm_get_u32(nf.data + (nn - 1) * NM_NODE_BYTES + NF_EDGE_BEGIN);
        uint16_t lastc = nm_get_u16(nf.data + (nn - 1) * NM_NODE_BYTES + NF_EDGE_COUNT);
        CHECK((uint64_t)last + lastc == ne,
              "末节点 edge_begin(%u)+count(%u) != 总边数 %llu",
              last, lastc, (unsigned long long)ne);
        printf("  孤立节点(无边) 抽样 %llu\n", (unsigned long long)empty_nodes);

        /* 边目标下标必须落在节点范围内 */
        uint64_t bad_to = 0;
        uint64_t estep = ne > 200000 ? ne / 200000 : 1;
        for (uint64_t i = 0; i < ne; i += estep) {
            uint32_t to = nm_get_u32(ef.data + i * NM_EDGE_BYTES + EF_TO);
            if ((uint64_t)to >= nn) bad_to++;
        }
        CHECK(bad_to == 0, "有边的 to 越界（抽样命中 %llu 处）", (unsigned long long)bad_to);
    }

    /* 经纬度在合理范围（中国及周边） */
    {
        uint64_t bad = 0;
        uint64_t step = nn > 200000 ? nn / 200000 : 1;
        for (uint64_t i = 0; i < nn; i += step) {
            int32_t la = nm_get_i32(nf.data + i * NM_NODE_BYTES + NF_LAT);
            int32_t lo = nm_get_i32(nf.data + i * NM_NODE_BYTES + NF_LON);
            if (la < -90 * 10000000 || la > 90 * 10000000 ||
                lo < -180 * 10000000 || lo > 180 * 10000000) bad++;
            if (la == 0 && lo == 0) bad++;      /* 未回填的坐标 */
        }
        CHECK(bad == 0, "有 %llu 个节点坐标非法或未回填", (unsigned long long)bad);
    }

    /* 转向限制的两个边下标必须存在，且首尾相接 */
    {
        uint64_t nr = rf.hdr.rec_count, bad = 0, not_chain = 0;
        for (uint64_t i = 0; i < nr; i++) {
            const uint8_t *r = rf.data + i * NM_RESTR_BYTES;
            uint32_t fe = nm_get_u32(r + RF_FROM_EDGE);
            uint32_t te = nm_get_u32(r + RF_TO_EDGE);
            uint32_t via = nm_get_u32(r + RF_VIA_NODE);
            if (fe >= ne || te >= ne || (uint64_t)via >= nn) { bad++; continue; }
            uint32_t fe_to = nm_get_u32(ef.data + (uint64_t)fe * NM_EDGE_BYTES + EF_TO);
            if (fe_to != via) {
                if (not_chain < 3 && getenv("NAVIMAP_CHECK_VERBOSE"))
                    printf("  违规限制 %llu: kind=%u fe=%u(→%u) te=%u via=%u"
                           " prev=%u\n", (unsigned long long)i,
                           r[RF_KIND], fe, fe_to, te, via,
                           nm_get_u32(r + RF_PREV_EDGE));
                not_chain++;
            }
        }
        CHECK(bad == 0, "%llu 条转向限制的下标越界", (unsigned long long)bad);
        CHECK(not_chain == 0, "%llu 条转向限制的 from_edge 终点不是 via 节点",
              (unsigned long long)not_chain);
        printf("  转向限制 %llu 条\n", (unsigned long long)nr);
    }

    /* names.bin 是变长区（rec_count = 字节数），这里只报大小 */
    {
        NMBinFile nf2;
        snprintf(p, sizeof(p), "%s/names.bin", dir);
        if (nmbin_open(p, &nf2) == 0) {
            printf("nodes %llu  edges %llu  names %.1f KB\n",
                   (unsigned long long)nn, (unsigned long long)ne,
                   (double)nf2.hdr.payload_bytes / 1024.0);
            nmbin_close(&nf2);
        } else {
            printf("nodes %llu  edges %llu  names 打不开\n",
                   (unsigned long long)nn, (unsigned long long)ne);
        }
    }

    /* 线/面要素：下标范围、面闭合、kind 合法（文件可选，旧目录没有） */
    {
        NMBinFile lf, af, pf;
        snprintf(p, sizeof(p), "%s/lines.bin", dir);
        int have_lines = (nmbin_open(p, &lf) == 0);
        snprintf(p, sizeof(p), "%s/areas.bin", dir);
        int have_areas = (nmbin_open(p, &af) == 0);
        snprintf(p, sizeof(p), "%s/polyrefs.bin", dir);
        int have_pr = (nmbin_open(p, &pf) == 0);
        if (have_lines || have_areas) {
            CHECK(have_pr, "有 lines/areas 但没有 polyrefs.bin");
        }
        if (have_lines && have_areas && have_pr) {
            uint64_t nl = lf.hdr.rec_count, na = af.hdr.rec_count, np = pf.hdr.rec_count;
            uint64_t bad_ref = 0, open_area = 0, bad_kind = 0;
            for (uint64_t i = 0; i < nl; i++) {
                const uint8_t *r = lf.data + i * NM_LINE_BYTES;
                uint32_t off = nm_get_u32(r + LF_REF_OFF);
                uint32_t cnt = nm_get_u32(r + LF_REF_CNT);
                if ((uint64_t)off + cnt > np) bad_ref++;
                uint8_t cls = r[LF_CLASS];
                if (cls < RC_PATH || cls > RC_WATER) bad_kind++;
            }
            for (uint64_t i = 0; i < na; i++) {
                const uint8_t *r = af.data + i * NM_AREA_BYTES;
                uint32_t off = nm_get_u32(r + AF_REF_OFF);
                uint32_t cnt = nm_get_u32(r + AF_REF_CNT);
                if ((uint64_t)off + cnt > np) { bad_ref++; continue; }
                if (cnt < 4) { open_area++; continue; }
                uint32_t first = nm_get_u32(pf.data + (uint64_t)off * 4);
                uint32_t last  = nm_get_u32(pf.data + ((uint64_t)off + cnt - 1) * 4);
                if (first != last) open_area++;
                uint8_t k = r[AF_KIND];
                if (k < AK_BUILDING || k > AK_OTHER) bad_kind++;
            }
            CHECK(bad_ref == 0, "线/面有 %llu 条 ref 区间越界", (unsigned long long)bad_ref);
            CHECK(open_area == 0, "%llu 个面不闭合（首尾节点不同）", (unsigned long long)open_area);
            CHECK(bad_kind == 0, "%llu 条线/面的 class/kind 非法", (unsigned long long)bad_kind);
            printf("lines %llu  areas %llu  polyrefs %llu\n",
                   (unsigned long long)nl, (unsigned long long)na,
                   (unsigned long long)np);
            nmbin_close(&lf); nmbin_close(&af); nmbin_close(&pf);
        } else {
            if (have_lines) nmbin_close(&lf);
            if (have_areas) nmbin_close(&af);
            if (have_pr)    nmbin_close(&pf);
            printf("lines/areas 缺失（旧格式目录，跳过）\n");
        }
    }

    nmbin_close(&nf); nmbin_close(&ef); nmbin_close(&mf); nmbin_close(&rf);

    if (fail) return 1;
    printf("ASSERT-OK\n");
    return 0;
}
