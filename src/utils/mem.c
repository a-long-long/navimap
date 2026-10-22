#include "utils/mem.h"

#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

/* 必须在任何业务分配之前生效，所以用构造函数（优先级 101，早于大多数库）。 */
__attribute__((constructor(101)))
static void nm_pin_allocator(void)
{
    mallopt(M_MMAP_THRESHOLD, NM_MMAP_THRESHOLD_BYTES);
    mallopt(M_TRIM_THRESHOLD, -1);
}

void *nm_mmap_file(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    if (st.st_size <= 0) { close(fd); return NULL; }

    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;

    *out_len = (size_t)st.st_size;
    return p;
}

void nm_madvise_random(void *addr, size_t len)
{
    if (addr && len) (void)madvise(addr, len, MADV_RANDOM);
}

void nm_madvise_sequential(void *addr, size_t len)
{
    if (addr && len) (void)madvise(addr, len, MADV_SEQUENTIAL);
}

int nm_munmap(void *addr, size_t len)
{
    if (!addr) return 0;
    return munmap(addr, len);
}
