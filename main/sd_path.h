/**
 * @file sd_path.h
 * @brief VFS 路径到 FatFs 根路径的转换（视频/音轨直连 FatFs 共用）。
 *
 * ESP-IDF VFS 的 off_t 是 32 位有符号，只能定位到 2 GiB；视频和 WAV 可能
 * 接近 FAT32 的 4 GiB-1 上限，因此这些模块直接调用 FatFs（FSIZE_t 为
 * 32 位无符号）。VFS 挂载点不属于 FAT 路径：
 *   /sdcard/HEI_MAO_POD_SYS/...  ->  /HEI_MAO_POD_SYS/...
 * FF_FS_RPATH=0 时无盘符路径使用默认卷 0；sd_card 只挂载一个 FAT 卷，
 * 且 sd_card.c 的 f_getfree(SD_MOUNT_POINT) 也依赖同一个默认卷。
 * 转换失败时调用方必须直接拒绝，不能访问文件系统。
 */
#ifndef SD_PATH_H
#define SD_PATH_H

#include "sd_card.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool sd_path_to_fatfs(const char *path, char *out, size_t cap)
{
    if (!path) return false;
    const size_t prefix = sizeof(SD_MOUNT_POINT) - 1;
    if (strncmp(path, SD_MOUNT_POINT, prefix) || path[prefix] != '/' ||
        !path[prefix + 1]) return false;
    const char *relative = path + prefix;
    size_t len = strlen(relative);
    if (len >= cap) return false;
    memcpy(out, relative, len + 1);
    return true;
}

#endif /* SD_PATH_H */
