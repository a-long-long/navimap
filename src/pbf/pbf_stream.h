#ifndef NAVIMAP_PBF_STREAM_H
#define NAVIMAP_PBF_STREAM_H

#include <stddef.h>
#include <stdint.h>

/* OSM PBF 文件结构：
 *   [4B 大端 BlobHeader 长度][BlobHeader][Blob][4B 长度][BlobHeader][Blob]...
 *   BlobHeader.type = "OSMHeader" | "OSMData"
 *   Blob 里是压缩后的 HeaderBlock / PrimitiveBlock（绝大多数是 zlib，
 *   也有 raw / lz4 / zstd；raw_size 给出解压后长度）。
 *
 * 这个文件只负责"把下一个 blob 解成一段连续内存"，不解释内容。
 */

#define PBF_MAX_BLOB_BYTES (64u * 1024u * 1024u)  /* OSM 规范上限 64MB */

typedef struct PbfStream PbfStream;

typedef enum {
    PBF_OK   = 0,
    PBF_DONE = 1,
    PBF_ERR  = -1
} PbfStatus;

PbfStream *pbf_stream_open(const char *path);
void       pbf_stream_close(PbfStream *s);

/* 读取下一个 blob。成功时 *type 指向 "OSMHeader"/"OSMData"（静态字符串，无需释放），
 * *out_buf 指向内部缓冲区（下一次调用后失效），*out_len 为解压后字节数。
 * 文件结束返回 PBF_DONE。 */
PbfStatus  pbf_stream_next(PbfStream *s, const char **type,
                           const uint8_t **out_buf, size_t *out_len);

const char *pbf_stream_error(PbfStream *s);
uint64_t    pbf_stream_offset(PbfStream *s);
uint64_t    pbf_stream_file_size(PbfStream *s);

#endif
