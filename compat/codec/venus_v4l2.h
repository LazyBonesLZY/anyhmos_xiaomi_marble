/*
 * Clean-room Qualcomm SM7475 Venus V4L2 DMABUF M2M core header.
 * Copyright (C) 2026 Marble HMOS Bringup Project (clean-room MIT).
 */
#ifndef MARBLE_VENUS_V4L2_H
#define MARBLE_VENUS_V4L2_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <linux/videodev2.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef V4L2_PIX_FMT_H264
#define V4L2_PIX_FMT_H264 v4l2_fourcc('H', '2', '6', '4')
#endif
#ifndef V4L2_PIX_FMT_HEVC
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#endif
#ifndef V4L2_PIX_FMT_VP9
#define V4L2_PIX_FMT_VP9 v4l2_fourcc('V', 'P', '9', '0')
#endif
#ifndef V4L2_PIX_FMT_HEIC
#define V4L2_PIX_FMT_HEIC v4l2_fourcc('H', 'E', 'I', 'C')
#endif

#define VENUS_DEV_DECODER "/dev/video32"
#define VENUS_DEV_ENCODER "/dev/video33"
#define VENUS_DMA_HEAP    "/dev/dma_heap/system"

#define VENUS_MAX_BUFS 24

enum venus_role {
    VENUS_ROLE_DECODER = 0,
    VENUS_ROLE_ENCODER = 1,
};

enum venus_poll_flags {
    VENUS_POLL_INPUT_READY  = 1u << 0, /* OUTPUT_MPLANE has dequeuable buffer */
    VENUS_POLL_OUTPUT_READY = 1u << 1, /* CAPTURE_MPLANE has dequeuable buffer */
    VENUS_POLL_EVENT        = 1u << 2, /* V4L2 event pending (POLLPRI) */
    VENUS_POLL_WAKEUP       = 1u << 3, /* Wakeup eventfd signaled */
    VENUS_POLL_ERR          = 1u << 4,
};

struct venus_sys_ops {
    void *userdata;
    int (*open_fn)(void *userdata, const char *path, int flags);
    int (*close_fn)(void *userdata, int fd);
    int (*ioctl_fn)(void *userdata, int fd, unsigned long req, void *arg);
    int (*poll_fn)(void *userdata, struct pollfd *fds, nfds_t nfds, int timeout_ms);
    void *(*mmap_fn)(void *userdata, size_t len, int prot, int flags, int fd, off_t off);
    int (*munmap_fn)(void *userdata, void *addr, size_t len);
    int (*dmabuf_alloc_fn)(void *userdata, uint32_t size);
    int (*dmabuf_sync_fn)(void *userdata, int fd, uint64_t flags);
};

struct venus_buf {
    int fd;
    void *map;
    uint32_t size;
    uint32_t index;
    bool queued;
};

struct venus_format_info {
    uint32_t width;
    uint32_t height;
    uint32_t crop_left;
    uint32_t crop_top;
    uint32_t crop_width;
    uint32_t crop_height;
    uint32_t y_stride;
    uint32_t y_scanlines;
    uint32_t uv_offset;
    uint32_t raw_pixfmt;
    uint32_t coded_pixfmt;
    uint32_t in_buf_size;
    uint32_t out_buf_size;
    uint32_t in_buf_count;
    uint32_t out_buf_count;
};

struct venus_ctx {
    enum venus_role role;
    int fd;
    int wake_efd;
    bool configured;
    bool streaming_in;
    bool streaming_out;
    bool eos_sent;
    struct venus_sys_ops ops;
    struct venus_format_info fmt;
    struct venus_buf in_bufs[VENUS_MAX_BUFS];
    struct venus_buf out_bufs[VENUS_MAX_BUFS];
};

/* SM7475 msm_media_info.h geometry helpers */
static inline uint32_t venus_align_u32(uint32_t x, uint32_t align)
{
    if (align == 0)
        return x;
    return ((x + align - 1u) / align) * align;
}

uint32_t venus_y_stride(uint32_t raw_pixfmt, uint32_t width);
uint32_t venus_y_scanlines(uint32_t raw_pixfmt, uint32_t height);
uint32_t venus_uv_stride(uint32_t raw_pixfmt, uint32_t width);
uint32_t venus_uv_scanlines(uint32_t raw_pixfmt, uint32_t height);
uint32_t venus_raw_buffer_size(uint32_t raw_pixfmt, uint32_t width, uint32_t height);

/* Initialize default Linux syscall ops. */
void venus_default_sys_ops(struct venus_sys_ops *ops);

/* Open /dev/video32 or /dev/video33 and wakeup eventfd. Returns 0 or -errno. */
int venus_ctx_open(struct venus_ctx *ctx, enum venus_role role,
                   uint32_t coded_fourcc, uint32_t raw_fourcc,
                   uint32_t init_w, uint32_t init_h,
                   const struct venus_sys_ops *ops);

/* Configure formats, buffer counts, event subscriptions, and encoder params.
 * profile / level / bitrate_mode: negative values leave driver defaults. */
int venus_ctx_configure(struct venus_ctx *ctx, uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t bitrate, uint32_t i_period,
                        int32_t bitrate_mode, int32_t profile, int32_t level);

/* Allocate DMA-BUFs, REQBUFS(DMABUF), queue CAPTURE buffers, and STREAMON both ports. */
int venus_ctx_start(struct venus_ctx *ctx);

/* Queue a prepared input buffer (ctx->in_bufs[idx]) to OUTPUT_MPLANE. */
int venus_ctx_queue_input(struct venus_ctx *ctx, uint32_t idx, uint32_t bytesused,
                          int64_t ts_us, uint32_t flags);

/* Find an unqueued input buffer index; returns index [0..in_buf_count-1] or -1. */
int venus_ctx_acquire_free_input(struct venus_ctx *ctx);

/* Re-queue an output (CAPTURE_MPLANE) buffer back to Venus. */
int venus_ctx_requeue_output(struct venus_ctx *ctx, uint32_t idx);

/* Dequeue a completed input buffer; returns 0 (fills *out_idx), -EAGAIN, or -errno. */
int venus_ctx_dq_input(struct venus_ctx *ctx, uint32_t *out_idx);

/* Dequeue a completed output buffer; returns 0, -EAGAIN, or -errno. */
int venus_ctx_dq_output(struct venus_ctx *ctx, uint32_t *out_idx,
                        uint32_t *bytesused, int64_t *ts_us, uint32_t *flags);

/* Send V4L2_DEC_CMD_STOP or V4L2_ENC_CMD_STOP (with fallback zero-byte LAST QBUF). */
int venus_ctx_send_eos(struct venus_ctx *ctx);

/* Poll V4L2 fd + wakeup eventfd. Returns 0 on ready/timeout, -errno on error. */
int venus_ctx_poll(struct venus_ctx *ctx, int timeout_ms, uint32_t *out_events);

/* Signal wakeup eventfd so blocked poll returns promptly. */
void venus_ctx_wakeup(struct venus_ctx *ctx);

/* Drain event from V4L2 fd. Returns 0 if event read into *ev, -EAGAIN/-ENOENT if empty. */
int venus_ctx_dq_event(struct venus_ctx *ctx, struct v4l2_event *ev);

/* Handle V4L2_EVENT_SOURCE_CHANGE: reallocate CAPTURE buffers and resume output. */
int venus_ctx_handle_source_change(struct venus_ctx *ctx);

/* Flush queued buffers via STREAMOFF + re-queue CAPTURE + STREAMON (both queues). */
int venus_ctx_flush(struct venus_ctx *ctx, bool flush_in, bool flush_out);

/* STREAMOFF and release DMA-BUF buffers cleanly. */
int venus_ctx_stop(struct venus_ctx *ctx);

/* Stop if needed and close all fds. Safe to call multiple times. */
void venus_ctx_close(struct venus_ctx *ctx);

/* Copy Venus 128x32 NV12/NV21 plane into caller linear NV12/NV21 destination,
 * with strict bounds checking against dst_max_bytes (so trailing metadata pages
 * such as BufferHandle's CropStore are never modified). */
int venus_nv12_to_linear_dst(const uint8_t *src_y, uint32_t src_uv_off,
                             uint32_t src_stride, uint8_t *dst,
                             size_t dst_max_bytes, uint32_t dst_stride,
                             uint32_t dst_y_lines, uint32_t crop_left,
                             uint32_t crop_top, uint32_t crop_w,
                             uint32_t crop_h, bool swap_uv);

/* Pack caller linear NV12/NV21 buffer into Venus 128x32-aligned DMA-BUF plane. */
int venus_linear_src_to_nv12(const uint8_t *src, size_t src_size,
                             uint32_t src_stride, uint32_t src_y_lines,
                             uint8_t *dst_y, uint32_t dst_uv_off,
                             uint32_t dst_stride, size_t dst_max_bytes,
                             uint32_t w, uint32_t h, bool swap_uv);

/* Copy Venus 128x32 NV12 plane into caller planar I420 (Y, U, V) destination,
 * with strict bounds checking against dst_max_bytes (preserving CropStore tail). */
int venus_nv12_to_i420_dst(const uint8_t *src_y, uint32_t src_uv_off,
                           uint32_t src_stride, uint8_t *dst,
                           size_t dst_max_bytes, uint32_t dst_stride,
                           uint32_t dst_y_lines, uint32_t crop_left,
                           uint32_t crop_top, uint32_t crop_w,
                           uint32_t crop_h);

/* Pack caller planar I420 (Y, U, V) buffer into Venus 128x32-aligned NV12 DMA-BUF. */
int venus_i420_src_to_nv12(const uint8_t *src, size_t src_size,
                           uint32_t src_stride, uint32_t src_y_lines,
                           uint8_t *dst_y, uint32_t dst_uv_off,
                           uint32_t dst_stride, size_t dst_max_bytes,
                           uint32_t w, uint32_t h);

/* Optional force keyframe ioctl/ctrl helper for encoder. */
int venus_ctx_force_keyframe(struct venus_ctx *ctx);

/* Optional dynamic bitrate update helper for encoder. */
int venus_ctx_set_bitrate(struct venus_ctx *ctx, uint32_t bitrate);

/* Sync helper wrappers honoring ctx->ops.dmabuf_sync_fn. */
int venus_dmabuf_sync(struct venus_ctx *ctx, int fd, uint64_t flags);

#ifdef __cplusplus
}
#endif

#endif /* MARBLE_VENUS_V4L2_H */
