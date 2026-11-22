/* SPDX-License-Identifier: MIT */
#ifndef HMOS_DMABUF_SYNC_UAPI_H
#define HMOS_DMABUF_SYNC_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define HMOS_DMABUF_SYNC_DEVICE "/dev/hmos_dmabuf_sync"
#define HMOS_DMABUF_SYNC_VERSION 1U
#define HMOS_DMABUF_SYNC_FEATURE_EXPORT (1U << 0)
#define HMOS_DMABUF_SYNC_FEATURE_IMPORT (1U << 1)
#define HMOS_DMABUF_SYNC_FEATURES_REQUIRED \
   (HMOS_DMABUF_SYNC_FEATURE_EXPORT | HMOS_DMABUF_SYNC_FEATURE_IMPORT)
#define HMOS_DMABUF_SYNC_READ (1U << 0)
#define HMOS_DMABUF_SYNC_WRITE (1U << 1)
#define HMOS_DMABUF_SYNC_VALID_FLAGS \
   (HMOS_DMABUF_SYNC_READ | HMOS_DMABUF_SYNC_WRITE)

/* 查询版本及已实现的同步文件能力 */
struct hmos_dmabuf_sync_abi {
   __u32 version;
   __u32 features;
   __u64 reserved;
};

/* 导出描述符归调用者所有，导入仅借用描述符 */
struct hmos_dmabuf_sync_file {
   __s32 dma_buf_fd;
   __u32 flags;
   __s32 fence_fd;
   __u32 reserved;
};

#define HMOS_DMABUF_SYNC_QUERY \
   _IOR('S', 0, struct hmos_dmabuf_sync_abi)
#define HMOS_DMABUF_SYNC_EXPORT \
   _IOWR('S', 1, struct hmos_dmabuf_sync_file)
#define HMOS_DMABUF_SYNC_IMPORT \
   _IOW('S', 2, struct hmos_dmabuf_sync_file)

#endif
