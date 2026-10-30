/* tutorial/03_pbf/main.c — 第 3 课 · PBF 流式解析（精髓复刻）
 *
 * 累加内容：PBF 最经典的三个坑——① varint(LEB128) 变长编码；② DenseNodes 的 delta 编码；
 * ③ stringtable 不是 NUL 结尾。本课不引入 protobuf-c，而是用几十行把这三点的"本质"复刻出来，
 * 让你亲手 decode 一段 DenseNodes 并把节点画出来。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"

#define W 320
#define H 240
static uint32_t buf[W * H];

/* —— LEB128 变长无符号整数：PBF 里所有整数都这么存 —— */
static void wr_varint(uint8_t *out, int *n, uint64_t v) {
    do { uint8_t b = v & 0x7f; v >>= 7; if (v) b |= 0x80; out[(*n)++] = b; }
    while (v);
}
static uint64_t rd_varint(const uint8_t **pp, const uint8_t *end) {
    uint64_t v = 0; int s = 0; const uint8_t *p = *pp;
    while (p < end) { uint8_t b = *p++; v |= (uint64_t)(b & 0x7f) << s; if (!(b & 0x80)) break; s += 7; }
    *pp = p; return v;
}

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);

    /* —— 造一段 DenseNodes：4 个节点（北京附近一条小路的顶点）——
     * 真实 PBF 里 id/lat/lon 都是【delta】编码：只存相邻差，绝大多数是小整数。 */
    struct { int32_t id, lat, lon; } nodes[4] = {
        {100, 399000000, 1164000000},
        {101, 399001000, 1164010000},
        {102, 399002000, 1164020000},
        {103, 399003000, 1164030000},
    };
    uint8_t blob[256]; int n = 0;
    int32_t last_id = 0, last_lat = 0, last_lon = 0;
    for (int i = 0; i < 4; i++) {
        wr_varint(blob, &n, (uint64_t)(nodes[i].id  - last_id));  last_id  = nodes[i].id;
        wr_varint(blob, &n, (uint64_t)(nodes[i].lat - last_lat)); last_lat = nodes[i].lat;
        wr_varint(blob, &n, (uint64_t)(nodes[i].lon - last_lon)); last_lon = nodes[i].lon;
    }

    /* —— decode：还原绝对值 —— */
    const uint8_t *p = blob, *end = blob + n;
    int32_t rid = 0, rlat = 0, rlon = 0;
    int cnt = 0;
    printf("解码 DenseNodes（delta 还原）：\n");
    while (p < end && cnt < 4) {
        rid  += (int32_t)rd_varint(&p, end);
        rlat += (int32_t)rd_varint(&p, end);
        rlon += (int32_t)rd_varint(&p, end);
        printf("  id=%d  lat=%.4f  lon=%.4f\n", rid, rlat / 1e7, rlon / 1e7);
        /* 画到缓冲：以这 4 点 bbox 为视框 */
        int x = (int)((rlon - 1164000000) / 3000.0 * (W - 1));
        int y = (int)((399003000 - rlat) / 3000.0 * (H - 1));
        tut_fill_rect(buf, W, H, x - 3, y - 3, 7, 7, 0xFF1F6FB0);
        if (cnt > 0) { /* 连成 way */
            int px = (int)((nodes[cnt-1].lon - 1164000000) / 3000.0 * (W - 1));
            int py = (int)((399003000 - nodes[cnt-1].lat) / 3000.0 * (H - 1));
            tut_line(buf, W, H, px, py, x, y, 0xFFB03A1F);
        }
        cnt++;
    }

    /* —— 坑③：stringtable 不是 NUL 结尾 ——
     * protobuf-c 的 BinaryData 不保证 '\0'。直接当 C 串会越界读到下一个字段。
     * 下面 blob2 是 "highway"（7 字节，无 NUL）： */
    const uint8_t blob2[] = {'h','i','g','h','w','a','y'};
    /* 错误写法（注释掉，真跑会读越界）：printf("%s\n", (const char*)blob2); */
    /* 正确写法：按已知长度拷贝成 NUL 结尾 */
    char key[16]; int klen = (int)sizeof(blob2);
    if (klen < (int)sizeof(key)) { memcpy(key, blob2, klen); key[klen] = '\0'; }
    printf("从 stringtable 安全取键: \"%s\"（长度 %d，非 NUL 结尾，必须按长度拷）\n", key, klen);

    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote out.bmp — 4 个解码节点 + 连成的 way\n");
    return 0;
}
