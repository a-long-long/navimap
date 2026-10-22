#ifndef NAVIMAP_PBF_BLOCK_H
#define NAVIMAP_PBF_BLOCK_H

#include <stdint.h>
#include <stddef.h>

#include "osmformat.pb-c.h"

/* PrimitiveBlock 的薄封装。
 *
 * ⚠️ 关键：protobuf-c 的 stringtable 是 ProtobufCBinaryData（指针+长度），
 * **不保证以 '\0' 结尾**。直接把 data 当 C 串用，strcmp 会读到后面的垃圾，
 * 于是 "path" 变成 "path\x7f"、highway 分类全部落空、type=restriction 也认不出来。
 * 这个坑的表现是"统计数字看起来差不多但就是不对"，极难查。
 * 所以这里解包时就把整个 stringtable 拷成一份 NUL 结尾的副本，
 * pbf_str() 返回的永远是可以安全 strcmp 的 C 串。
 */
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

#endif
