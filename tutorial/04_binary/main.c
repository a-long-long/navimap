/* tutorial/04_binary/main.c — 第 4 课 · 自描述二进制与图集
 *
 * 累加内容：为什么不用"8 字节头 + 定长记录"，而要做自描述格式。本课写一个迷你版 NMBIN：
 * 文件头里存【字段名 / 偏移 / 类型 / 宽度】，数据区按字段偏移布局。读的时候不依赖任何 C 结构体，
 * 而是【解析文件头】反推出结构——这就是"丢掉源码也能读"的含义。
 *
 * 编译： gcc -std=c11 -O2 -o tut main.c && ./tut
 */
#include "../common.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define W 320
#define H 240
static uint32_t buf[W * H];

typedef struct { char name[16]; uint32_t off; uint8_t type; uint32_t width; } Field;

static void write_bin(const char *path) {
    FILE *f = fopen(path, "wb");
    uint8_t magic[8] = {'N','A','V','M','B','I','N',1};
    uint32_t ver = 3, nf = 3;
    fwrite(magic, 1, 8, f);
    fwrite(&ver, 4, 1, f);
    fwrite(&nf, 4, 1, f);
    /* 字段表：name(16) + off(4) + type(1:0=u32,1=i32) + width(4) */
    Field fl[3] = {{"id", 0, 0, 4}, {"lat", 4, 1, 4}, {"lon", 8, 1, 4}};
    for (int i = 0; i < 3; i++) {
        fwrite(fl[i].name, 1, 16, f);
        fwrite(&fl[i].off, 4, 1, f);
        fwrite(&fl[i].type, 1, 1, f);
        fwrite(&fl[i].width, 4, 1, f);
    }
    /* 3 条记录，每条 12 字节（id4 + lat4 + lon4） */
    uint32_t ids[3] = {1, 2, 3};
    int32_t  lats[3] = {399000000, 399010000, 399020000};
    int32_t  lons[3] = {1164000000, 1164010000, 1164020000};
    for (int i = 0; i < 3; i++) {
        fwrite(&ids[i], 4, 1, f);
        fwrite(&lats[i], 4, 1, f);
        fwrite(&lons[i], 4, 1, f);
    }
    fclose(f);
}

/* 读：完全靠文件头反推结构，不 include 任何写端的 struct */
static void read_bin(const char *path, int32_t *out_lat, int n) {
    FILE *f = fopen(path, "rb");
    uint8_t magic[8]; fread(magic, 1, 8, f);
    uint32_t ver, nf; fread(&ver, 4, 1, f); fread(&nf, 4, 1, f);
    printf("magic=%.7s ver=%u nfields=%u\n", (char *)magic, ver, nf);
    Field *fl = (Field *)malloc(sizeof(Field) * nf);
    uint32_t reclen = 0;
    for (int i = 0; i < (int)nf; i++) {
        fread(fl[i].name, 1, 16, f);
        fread(&fl[i].off, 4, 1, f);
        fread(&fl[i].type, 1, 1, f);
        fread(&fl[i].width, 4, 1, f);
        printf("  field[%-4s] off=%u type=%u width=%u\n", fl[i].name, fl[i].off, fl[i].type, fl[i].width);
        if (fl[i].off + fl[i].width > reclen) reclen = fl[i].off + fl[i].width;
    }
    long data_off = ftell(f);
    for (int r = 0; r < n; r++) {
        printf("  rec[%d]:\n", r);
        for (int i = 0; i < (int)nf; i++) {
            fseek(f, data_off + (long)r * reclen + fl[i].off, SEEK_SET);
            if (fl[i].type == 0) { uint32_t v; fread(&v, 4, 1, f); printf("    %s=%u\n", fl[i].name, v); }
            else { int32_t v; fread(&v, 4, 1, f); printf("    %s=%d (%.5f)\n", fl[i].name, v, v / 1e7);
                   if (strcmp(fl[i].name, "lat") == 0) out_lat[r] = v; }
        }
    }
    free(fl); fclose(f);
}

int main(void) {
    tut_cls(buf, W, H, 0xFFF4F1EA);
    write_bin("demo.nmbin");
    int32_t lat[3];
    read_bin("demo.nmbin", lat, 3);

    /* 用读回的数据画 3 根柱子（高度编码纬度），证明"读出来了且可用" */
    int bw = W / 6;
    for (int i = 0; i < 3; i++) {
        int h = (int)((lat[i] - 399000000) / 20000.0 * (H - 40)) + 10;
        int x = 40 + i * (bw + 30);
        tut_fill_rect(buf, W, H, x, H - h, bw, h, 0xFF1F6FB0);
    }
    if (tut_write_bmp("out.bmp", buf, W, H) != 0) { fprintf(stderr, "write failed\n"); return 1; }
    printf("OK: wrote demo.nmbin + out.bmp（柱子高度=纬度，读回即得）\n");
    return 0;
}
