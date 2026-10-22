#ifndef NAVIMAP_STRPOOL_H
#define NAVIMAP_STRPOOL_H

#include <stddef.h>
#include <stdint.h>

/* 字符串池：路名去重。id 0 保留 = "无名"。 */

typedef struct StrPool StrPool;

StrPool *sp_create(size_t cap_hint);
void     sp_destroy(StrPool *p);

/* 加入字符串并返回 id（已存在则复用）。空串/NULL 返回 0。 */
uint32_t sp_intern(StrPool *p, const char *s, size_t len);
uint32_t sp_intern_cstr(StrPool *p, const char *s);

const char *sp_get(StrPool *p, uint32_t id);
size_t      sp_count(StrPool *p);        /* 不含 id 0 */
size_t      sp_bytes(StrPool *p);        /* 字符区字节数（含结尾 0） */

/* 序列化成 [u32 count][u32 off[count+1]][bytes]，供 names.bin 落盘 */
void sp_serialize(StrPool *p, uint8_t **out, size_t *out_len);

#endif
