#include "map/binfmt.h"
#include "utils/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct NMBinWriter {
    FILE    *fp;
    char     path[4096];
    NMBinHeader hdr;
    NMBinField  fields[NMBIN_MAX_FIELDS];
    uint64_t recs_written;
    int      failed;
};

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

NMBinWriter *nmbin_create(const char *path, uint32_t kind, uint32_t rec_size,
                          const char *source_hint)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return NULL;
    NMBinWriter *w = calloc(1, sizeof(NMBinWriter));
    if (!w) { fclose(fp); return NULL; }
    snprintf(w->path, sizeof(w->path), "%s", path);
    w->fp = fp;
    memcpy(w->hdr.magic, NMBIN_MAGIC, 8);
    w->hdr.version  = NMBIN_VERSION;
    w->hdr.kind     = kind;
    w->hdr.rec_size = rec_size;
    w->hdr.n_fields = 0;
    w->hdr.rec_count = 0;
    if (source_hint) snprintf(w->hdr.source, sizeof(w->hdr.source), "%s", source_hint);

    uint8_t hdrb[NMBIN_HDR_BYTES];
    hdr_to_bytes(&w->hdr, hdrb);
    if (fwrite(hdrb, 1, NMBIN_HDR_BYTES, fp) != NMBIN_HDR_BYTES) { fclose(fp); free(w); return NULL; }
    /* 字段表先留白，finalize 时回填（此时还不知道有几个字段） */
    uint8_t zeros[NMBIN_FIELD_BYTES * NMBIN_MAX_FIELDS];
    memset(zeros, 0, sizeof(zeros));
    if (fwrite(zeros, 1, sizeof(zeros), fp) != sizeof(zeros)) { fclose(fp); free(w); return NULL; }
    return w;
}

int nmbin_add_field(NMBinWriter *w, const char *name, uint16_t offset,
                    uint8_t type, uint8_t width)
{
    if (!w || w->hdr.n_fields >= NMBIN_MAX_FIELDS) return -1;
    NMBinField *f = &w->fields[w->hdr.n_fields++];
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->offset = offset; f->type = type; f->width = width;
    return 0;
}

int nmbin_write_records(NMBinWriter *w, const void *recs, uint64_t n)
{
    if (!w || w->failed) return -1;
    if (n == 0) return 0;
    size_t bytes = (size_t)n * w->hdr.rec_size;
    if (fwrite(recs, 1, bytes, w->fp) != bytes) { w->failed = 1; return -1; }
    w->recs_written += n;
    return 0;
}

int nmbin_finalize(NMBinWriter *w)
{
    if (!w) return -1;
    if (w->failed) { fclose(w->fp); remove(w->path); free(w); return -1; }

    w->hdr.rec_count = w->recs_written;
    w->hdr.payload_bytes = w->recs_written * (uint64_t)w->hdr.rec_size;

    if (fseek(w->fp, 0, SEEK_SET) != 0) { fclose(w->fp); free(w); return -1; }
    uint8_t hdrb[NMBIN_HDR_BYTES];
    hdr_to_bytes(&w->hdr, hdrb);
    if (fwrite(hdrb, 1, NMBIN_HDR_BYTES, w->fp) != NMBIN_HDR_BYTES) { fclose(w->fp); free(w); return -1; }

    uint8_t fb[NMBIN_FIELD_BYTES];
    for (uint32_t i = 0; i < NMBIN_MAX_FIELDS; i++) {
        if (i < w->hdr.n_fields) field_to_bytes(&w->fields[i], fb);
        else memset(fb, 0, sizeof(fb));
        if (fwrite(fb, 1, sizeof(fb), w->fp) != sizeof(fb)) { fclose(w->fp); free(w); return -1; }
    }
    fclose(w->fp);
    free(w);
    return 0;
}

void nmbin_abort(NMBinWriter *w)
{
    if (!w) return;
    if (w->fp) fclose(w->fp);
    remove(w->path);
    free(w);
}

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
