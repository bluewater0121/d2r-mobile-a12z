// 扩展属性：宿主只存「物理文件名 → 客体文件名」这一项（hle_fs.c 的大小写冲突别名）。
// Windows 用 NTFS 备用数据流（ADS）承载，语义与 Linux 的 user.* 命名空间一致：没有就是 ENODATA。
// Extended attributes: the host stores exactly one item, "physical file name -> guest file name" (the case-collision alias in hle_fs.c).
// Windows carries it in an NTFS alternate data stream (ADS), with the semantics of Linux's user.* namespace: absent means ENODATA.
#pragma once
#include <sys/types.h>
ssize_t getxattr(const char *path, const char *name, void *value, size_t size);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
int setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int removexattr(const char *path, const char *name);
int lremovexattr(const char *path, const char *name);
#define XATTR_NOFOLLOW 0x0001
#define XATTR_CREATE 0x0002
#define XATTR_REPLACE 0x0004
