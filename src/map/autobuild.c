#include "map/autobuild.h"
#include "utils/plat.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 产物状态。前四种都需要重建。 */
enum { PV_OK = 0, PV_MISSING, PV_INCOMPLETE, PV_CORRUPT, PV_STALE };

#define PV_MISSING_TXT     "产物不存在"
#define PV_INCOMPLETE_TXT  "产物不完整"

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static double now_ms(void)
{
    return (double)nm_now_ms();       /* 平台层：clock_gettime / QPC */
}

/* 省名白名单：会被拼进 --out= 路径，必须挡住 '/' 与 ./.. */
static int name_ok(const char *n)
{
    if (!n || !*n) return 0;
    if (strchr(n, '/')) return 0;
    if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return 0;
    for (const char *p = n; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

/* "beijing.osm.pbf" / "beijing.pbf" → "beijing" */
static int prov_name(const char *file, char *out, size_t cap)
{
    static const char *sfx[] = { ".osm.pbf", ".pbf" };
    size_t n = strlen(file);
    for (size_t k = 0; k < sizeof(sfx) / sizeof(sfx[0]); k++) {
        size_t sl = strlen(sfx[k]);
        if (n > sl && strcmp(file + n - sl, sfx[k]) == 0) {
            if (n - sl >= cap) return -1;
            memcpy(out, file, n - sl);
            out[n - sl] = '\0';
            return name_ok(out) ? 0 : -1;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* 探测：转换器、源目录                                                 */
/* ------------------------------------------------------------------ */

/* skedaddle_graph_extract 在哪：与本程序同目录 → 同目录的 build/ → PATH。
 * （第三档是 build/RelWithDebInfo/navimap 这种多发生器成本身的场景，
 *  顺带也覆盖仓库根下的 <根>/build/。） */
static int find_tool(char *out, size_t cap)
{
    char exe[4096];
    if (nm_exe_path(exe, sizeof(exe)) == 0) {
        char dir[4096];
        size_t n = strlen(exe);
        if (n < sizeof(dir)) {
            memcpy(dir, exe, n + 1);
            char *slash = (char *)nm_last_sep(dir);
            if (slash) {
                *slash = '\0';
                if (nm_which(dir, "skedaddle_graph_extract", out, cap) == 0) return 0;
                char sub[4096];
                if (nm_path_join(sub, sizeof(sub), dir, "build") == 0 &&
                    nm_which(sub, "skedaddle_graph_extract", out, cap) == 0) return 0;
                char up[4096];
                if (nm_path_join(up, sizeof(up), dir, "..") == 0 &&
                    nm_which(up, "skedaddle_graph_extract", out, cap) == 0) return 0;
            }
        }
    }
    return nm_find_in_path("skedaddle_graph_extract", out, cap);
}

/* sources/ 在哪：从 graph_dir 逐级往上找，最多 6 级 */
static int find_src_dir(char *out, size_t cap, const char *graph_dir)
{
    char cur[4096];
    snprintf(cur, sizeof(cur), "%s", graph_dir);
    size_t n = strlen(cur);
    while (n > 1 && (cur[n - 1] == '/' || cur[n - 1] == '\\')) cur[--n] = '\0';

    for (int i = 0; i < 6; i++) {
        if (nm_path_join(out, cap, cur, "sources") == 0 && nm_is_dir(out)) return 0;
        char *slash = (char *)nm_last_sep(cur);
        if (!slash || slash == cur) break;
        *slash = '\0';
    }
    snprintf(out, cap, "sources");
    return nm_is_dir(out) ? 0 : -1;
}

/* 该省对应的源 pbf（两种后缀都认）。找到返回 0 并填 pbf 路径。 */
static int find_pbf(const char *src_dir, const char *name, char *out, size_t cap)
{
    char base[512];
    int n = snprintf(base, sizeof(base), "%s.osm.pbf", name);
    if (n >= 0 && (size_t)n < sizeof(base) &&
        nm_path_join(out, cap, src_dir, base) == 0 && nm_file_exists(out)) return 0;

    n = snprintf(base, sizeof(base), "%s.pbf", name);
    if (n >= 0 && (size_t)n < sizeof(base) &&
        nm_path_join(out, cap, src_dir, base) == 0 && nm_file_exists(out)) return 0;
    return -1;
}

/* ------------------------------------------------------------------ */
/* manifest 读取（与 sync_maps.sh 的 mani_get 同语义）                    */
/* ------------------------------------------------------------------ */

static int mani_get(const char *dir, const char *key, char *out, size_t cap)
{
    char p[4096];
    if (nm_path_join(p, sizeof(p), dir, "manifest.txt") != 0) return -1;
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    char line[512];
    size_t klen = strlen(key);
    int got = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) != 0) continue;
        if (line[klen] != ' ' && line[klen] != '\t') continue;
        char *v = line + klen;
        while (*v == ' ' || *v == '\t') v++;
        char *hash = strchr(v, '#');            /* 行内注释（areas 那行有） */
        if (hash) *hash = '\0';
        char *nl = strpbrk(v, "\r\n");
        if (nl) *nl = '\0';
        size_t n = strlen(v);
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) v[--n] = '\0';
        snprintf(out, cap, "%s", v);
        got = 0;
        break;
    }
    fclose(f);
    return got;
}

/* ------------------------------------------------------------------ */
/* 体检：这个省的产物还能不能用                                          */
/* ------------------------------------------------------------------ */

static int bin_ok(const char *dir, const char *file, char *why, size_t cap)
{
    char p[4096], err[128];
    if (nm_path_join(p, sizeof(p), dir, file) != 0) {
        snprintf(why, cap, "%s 路径过长", file);
        return -1;
    }
    NMBinFile f;
    if (nmbin_open(p, &f) != 0) {
        snprintf(why, cap, "%s 打不开", file);
        return -1;
    }
    int rc = nmbin_check(&f, err, sizeof(err));
    nmbin_close(&f);
    if (rc != 0) {
        snprintf(why, cap, "%s 校验失败（%s）", file, err);
        return -1;
    }
    return 0;
}

static int prov_check(const char *dir, const char *pbf, char *why, size_t cap)
{
    char p[4096];

    /* 1) 产物目录在不在 */
    if (!nm_is_dir(dir)) {
        snprintf(why, cap, "%s", PV_MISSING_TXT);
        return PV_MISSING;
    }
    /* 2) 必需文件齐不齐（与 sync_maps 同一份清单） */
    if (nm_path_join(p, sizeof(p), dir, "manifest.txt") != 0 ||
        !nm_file_exists(p)) {
        snprintf(why, cap, "%s（缺 manifest.txt，上次转换没走完）", PV_INCOMPLETE_TXT);
        return PV_INCOMPLETE;
    }
    static const char *need[] = { "nodes.bin", "edges.bin", "osmid.bin" };
    for (size_t i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        if (nm_path_join(p, sizeof(p), dir, need[i]) != 0) {
            snprintf(why, cap, "%s（路径过长）", PV_INCOMPLETE_TXT);
            return PV_INCOMPLETE;
        }
        if (!nm_file_exists(p)) {
            snprintf(why, cap, "%s（缺 %s）", PV_INCOMPLETE_TXT, need[i]);
            return PV_INCOMPLETE;
        }
    }
    /* 3) 二进制自洽性（截断/半截文件在这里现形） */
    if (bin_ok(dir, "nodes.bin", why, cap) != 0) return PV_CORRUPT;
    if (bin_ok(dir, "edges.bin", why, cap) != 0) return PV_CORRUPT;

    /* 4) 格式版本 */
    char v[64];
    if (mani_get(dir, "binfmt_version", v, sizeof(v)) == 0) {
        int ver = atoi(v);
        if (ver > 0 && ver != NMBIN_VERSION) {
            snprintf(why, cap, "二进制格式升级 v%d → v%d", ver, NMBIN_VERSION);
            return PV_STALE;
        }
    }
    /* 5) 源指纹：源 pbf 换了就该重转 */
    uint64_t sb = 0;
    int64_t  sm = 0;
    if (nm_stat_file(pbf, &sb, &sm) != 0) { sb = 0; sm = 0; }
    if (mani_get(dir, "src_bytes", v, sizeof(v)) == 0) {
        uint64_t mb = strtoull(v, NULL, 10);
        if (sb && mb && mb != sb) {
            snprintf(why, cap, "源文件大小变了（%llu → %llu 字节）",
                     (unsigned long long)mb, (unsigned long long)sb);
            return PV_STALE;
        }
    }
    if (mani_get(dir, "src_mtime", v, sizeof(v)) == 0) {
        int64_t mm = strtoll(v, NULL, 10);
        if (sm && mm && mm != sm) {
            snprintf(why, cap, "源文件被换过了（mtime 不同）");
            return PV_STALE;
        }
    }
    why[0] = '\0';
    return PV_OK;
}

/* ------------------------------------------------------------------ */
/* 并发锁：两个实例同时发现缺失时，只有一个去转                            */
/* ------------------------------------------------------------------ */

static void lock_path(char *out, size_t cap, const char *parent, const char *name)
{
    char nm[512];
    int n = snprintf(nm, sizeof(nm), "%s.building", name);
    if (n < 0 || (size_t)n >= sizeof(nm) || nm_path_join(out, cap, parent, nm) != 0)
        out[0] = '\0';
}

static int lock_pid_alive(const char *lp)
{
    FILE *f = fopen(lp, "r");
    if (!f) return 0;
    int pid = 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    return nm_pid_alive(pid);
}

/* 0 = 拿到锁；-1 = 拿不到（超时）。锁里写自己的 pid，便于识别过期锁。 */
static int lock_acquire(const char *parent, const char *name)
{
    char lp[4096];
    lock_path(lp, sizeof(lp), parent, name);
    nm_mkdir_p(parent);

    for (int try = 0; try < 360; try++) {          /* 最多等约 3 分钟 */
        char b[32];
        snprintf(b, sizeof(b), "%d\n", nm_get_pid());
        if (nm_create_exclusive(lp, b) == 0) return 0;
        if (!lock_pid_alive(lp)) {                 /* 上一个人崩了，锁是孤儿 */
            nm_unlink(lp);
            continue;
        }
        if (try == 0)
            fprintf(stderr, "[自动重建] %s 有另一个进程正在转换，等待中…\n", name);
        nm_sleep_ms(500);
    }
    return -1;
}

static void lock_release(const char *parent, const char *name)
{
    char lp[4096];
    lock_path(lp, sizeof(lp), parent, name);
    nm_unlink(lp);
}

/* ------------------------------------------------------------------ */
/* 重建一个省                                                           */
/* ------------------------------------------------------------------ */

static int run_extract(const char *tool, const char *pbf, const char *dir)
{
    /* fork 会把 stdio 缓冲区复制一份，先冲干净（平台层内部也做一次） */
    fflush(NULL);
    char outarg[4224];      /* 路径 buffer(4096) + "--out=" 还有富余 */
    if (snprintf(outarg, sizeof(outarg), "--out=%s", dir) >= (int)sizeof(outarg))
        return -1;
    char *const av[] = { (char *)tool, outarg, (char *)pbf, NULL };
    int code = 0;
    if (nm_spawn_wait(tool, av, &code) != 0) return -1;
    return code == 0 ? 0 : -1;
}

/* 让位目录：prov_dir 同级 + ".old.<pid>"。带 .old. 是刻意的——
   mapset_load 会跳过 .old.* / .tmp.*，不会被当成一个省加载进来。 */
static int aside_path(char *out, size_t cap, const char *prov_dir)
{
    int n = snprintf(out, cap, "%s.old.%d", prov_dir, nm_get_pid());
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* 返回 0 = 重建成功 */
static int rebuild_one(const char *graph_parent, const char *prov_dir,
                       const char *name, const char *pbf, const char *tool)
{
    if (lock_acquire(graph_parent, name) != 0) {
        fprintf(stderr, "[自动重建] %s 等不到转换锁，放弃重建\n", name);
        return -1;
    }

    /* 旧产物先清掉：留着会和转换器新写的文件混在一起（比如更大的 osmid.bin
       配上新的 nodes.bin），出来的图是自相矛盾的。删不掉就改名让位 ——
       名字带 .old. 是因为 mapset_load 会跳过 .old.* / .tmp.*，不会被当成
       一个省加载进来（同一份数据装两遍）。 */
    if (nm_is_dir(prov_dir)) {
        char aside[4224];
        if (aside_path(aside, sizeof(aside), prov_dir) != 0) {
            fprintf(stderr, "[自动重建] %s 路径过长，放弃重建\n", name);
            lock_release(graph_parent, name);
            return -1;
        }
        if (nm_rmtree(prov_dir) != 0 && nm_rename(prov_dir, aside) != 0) {
            fprintf(stderr, "[自动重建] %s 旧产物清不掉（%s），放弃重建\n",
                    name, strerror(errno));
            lock_release(graph_parent, name);
            return -1;
        }
    }

    fprintf(stderr, "[自动重建] %s 转换中（%s）…\n", name, pbf);
    double t0 = now_ms();
    int rc = run_extract(tool, pbf, prov_dir);
    double ms = now_ms() - t0;

    if (rc != 0) {
        fprintf(stderr,
                "[自动重建] %s 转换失败（%.1f s）。源 pbf 可能已损坏，建议挪到 "
                "sources/_bad/ 后重试；\n"
                "           也可手工执行：%s --out=%s %s\n",
                name, ms / 1000.0, tool, prov_dir, pbf);
        lock_release(graph_parent, name);
        return -1;
    }

    char why[256] = "";
    int pv = prov_check(prov_dir, pbf, why, sizeof(why));
    if (pv != PV_OK) {
        fprintf(stderr, "[自动重建] %s 转换结束但产物仍不可用：%s\n", name, why);
        lock_release(graph_parent, name);
        return -1;
    }

    char nodes[64] = "-", edges[64] = "-";
    mani_get(prov_dir, "nodes", nodes, sizeof(nodes));
    mani_get(prov_dir, "edges", edges, sizeof(edges));
    fprintf(stderr, "[自动重建] %s 重建完成（%.1f s，节点 %s / 边 %s）\n",
            name, ms / 1000.0, nodes, edges);

    /* 让位的旧目录这时可以清了 */
    char aside[4224];
    if (aside_path(aside, sizeof(aside), prov_dir) == 0 && nm_is_dir(aside))
        nm_rmtree(aside);

    lock_release(graph_parent, name);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

/* 单省目录（products/graph/beijing）还是图集目录（products/graph）？
 * 目录被整个删掉时没法靠内容判断，这时看 basename 有没有同名 pbf ——
 * 有就说明它本该是个省目录。判错后果很不一样：把图集当单省会漏建 7 个省，
 * 把单省当图集会把 8 个省全建一遍。 */
static int is_single_prov(const char *graph_dir, const char *src_dir,
                          char *name_out, size_t cap)
{
    char p[4096];
    if (nm_path_join(p, sizeof(p), graph_dir, "manifest.txt") == 0 &&
        nm_file_exists(p))
        goto single;
    if (nm_path_join(p, sizeof(p), graph_dir, "nodes.bin") == 0 &&
        nm_file_exists(p))
        goto single;

    {
        const char *base = nm_base_name(graph_dir);
        char pbf[4096];
        if (find_pbf(src_dir, base, pbf, sizeof(pbf)) == 0 && name_ok(base)) {
            snprintf(name_out, cap, "%s", base);
            return 1;
        }
        return 0;
    }
single:
    {
        const char *base = nm_base_name(graph_dir);
        snprintf(name_out, cap, "%s", base);
        return 1;
    }
}

typedef struct { char **v; int n, cap; } NameList;

static void nl_add(NameList *l, const char *nm)
{
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i], nm) == 0) return;      /* 去重 */
    if (l->n == l->cap) {
        int ncap = l->cap ? l->cap * 2 : 16;
        char **t = realloc(l->v, (size_t)ncap * sizeof(char *));
        if (!t) return;
        l->v = t; l->cap = ncap;
    }
    l->v[l->n++] = nm_strdup(nm);
}

/* 图集模式：sources/ 下所有 pbf 对应的省名 */
static void list_provs(const char *src_dir, NameList *l)
{
    NmDir *d = nm_dir_open(src_dir);
    if (!d) return;
    const char *de;
    char nm[256];
    while ((de = nm_dir_next(d)) != NULL) {
        if (de[0] == '.') continue;
        if (prov_name(de, nm, sizeof(nm)) != 0) continue;
        nl_add(l, nm);
    }
    nm_dir_close(d);
}

/* 反向补一遍：products/graph 下已经存在的省目录。
 * 只扫 sources/ 的话，"pbf 和产物都没了"会完全静默 —— 省份悄无声息少一个。
 * 扫进来的这些省即使坏了也重建不了（没有原料），但至少要说出来。
 * .tmp.* / .old.* 是过渡目录，必须跳过（与 mapset_load 同一份过滤名单）。 */
static void list_existing_provs(const char *graph_dir, NameList *l)
{
    NmDir *d = nm_dir_open(graph_dir);
    if (!d) return;
    const char *de;
    char p[4096];
    while ((de = nm_dir_next(d)) != NULL) {
        if (de[0] == '.') continue;
        if (strstr(de, ".tmp.") || strstr(de, ".old.")) continue;
        if (!name_ok(de)) continue;
        if (nm_path_join(p, sizeof(p), graph_dir, de) != 0) continue;
        if (!nm_is_dir(p)) continue;
        nl_add(l, de);
    }
    nm_dir_close(d);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

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

    /* 省目录的父目录（= 图集根）。单省模式下产物放 graph_dir 本身，
       它的父目录才是放锁文件的地方。 */
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
                fprintf(stderr, "[自动重建] 找不到 skedaddle_graph_extract，无法自动重建。"
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

int mapset_load_repair(MapSet *ms, const char *graph_dir,
                       const AutoBuildOpt *opt, AutoBuildStat *st_out)
{
    autobuild_repair(graph_dir, opt, st_out);
    return mapset_load(ms, graph_dir);
}
