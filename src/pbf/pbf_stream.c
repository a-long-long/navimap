#include "pbf/pbf_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <protobuf-c/protobuf-c.h>

#include "fileformat.pb-c.h"

#define PBF_BUF_CHUNK (1u << 20)

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
