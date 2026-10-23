#include "utils/mem.h"
#include "utils/plat.h"

#include <malloc.h>
#include <stdio.h>

/* 必须在任何业务分配之前生效，所以用构造函数（优先级 101，早于大多数库）。
 *
 * ★ 只在"参数确实存在"的平台编译：mallopt/M_MMAP_THRESHOLD 是 glibc 的
 *   （musl / macOS / Windows 没有或语义不同），__attribute__((constructor))
 *   是 GCC/Clang 的（MSVC 没有）。两个条件都不满足时整段消失——不能留个
 *   空函数，-Wall -Wextra 会把未使用的 static 函数当警告（零警告门禁）。
 *
 * 映射本身走平台层（nm_map_file/nm_unmap_file）：POSIX 是 mmap，
 * Windows 是 CreateFileMapping + MapViewOfFile。 */
#if !defined(_WIN32) && defined(M_MMAP_THRESHOLD) && defined(__GNUC__)
__attribute__((constructor(101)))
static void nm_pin_allocator(void)
{
    mallopt(M_MMAP_THRESHOLD, NM_MMAP_THRESHOLD_BYTES);
    mallopt(M_TRIM_THRESHOLD, -1);
}
#endif

void *nm_mmap_file(const char *path, size_t *out_len)
{
    return nm_map_file(path, out_len);
}

int nm_munmap(void *addr, size_t len)
{
    nm_unmap_file(addr, len);
    return 0;
}
