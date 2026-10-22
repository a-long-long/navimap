#include "pbf/pbf_block.h"

#include <stdlib.h>
#include <string.h>
#include <protobuf-c/protobuf-c.h>

int pbf_unpack_block(const uint8_t *buf, size_t len, PbfBlock **out)
{
    PrimitiveBlock *pb = primitive_block__unpack(NULL, len, buf);
    if (!pb) return -1;

    PbfBlock *b = calloc(1, sizeof(PbfBlock));
    if (!b) { primitive_block__free_unpacked(pb, NULL); return -1; }
    b->pb = pb;

    size_t n = pb->stringtable ? pb->stringtable->n_s : 0;
    b->n_str = n;
    if (n == 0) { *out = b; return 0; }

    size_t total = 0;
    for (size_t i = 0; i < n; i++) total += pb->stringtable->s[i].len + 1;

    b->pool = malloc(total ? total : 1);
    b->strs = malloc(n * sizeof(char *));
    if (!b->pool || !b->strs) { pbf_block_free(b); return -1; }

    size_t off = 0;
    for (size_t i = 0; i < n; i++) {
        ProtobufCBinaryData d = pb->stringtable->s[i];
        b->strs[i] = b->pool + off;
        if (d.len) memcpy(b->pool + off, d.data, d.len);
        off += d.len;
        b->pool[off++] = '\0';
    }
    *out = b;
    return 0;
}

void pbf_block_free(PbfBlock *b)
{
    if (!b) return;
    if (b->pb) primitive_block__free_unpacked(b->pb, NULL);
    free(b->pool);
    free(b->strs);
    free(b);
}

const char *pbf_str(PbfBlock *b, uint32_t sid)
{
    if (!b || !b->strs || sid >= b->n_str) return "";
    return b->strs[sid];
}
