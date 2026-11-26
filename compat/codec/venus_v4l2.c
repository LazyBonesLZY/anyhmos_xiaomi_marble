/*
 * Clean-room Qualcomm SM7475 Venus V4L2 DMABUF M2M core implementation.
 * Copyright (C) 2026 Marble HMOS Bringup Project (clean-room MIT).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "venus_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

uint32_t venus_y_stride(uint32_t raw_pixfmt, uint32_t width)
{
    (void)raw_pixfmt;
    if (!width)
        return 0;
    return venus_align_u32(width, 128u);
}

uint32_t venus_y_scanlines(uint32_t raw_pixfmt, uint32_t height)
{
    (void)raw_pixfmt;
    if (!height)
        return 0;
    return venus_align_u32(height, 32u);
}

uint32_t venus_uv_stride(uint32_t raw_pixfmt, uint32_t width)
{
    (void)raw_pixfmt;
    if (!width)
        return 0;
    return venus_align_u32(width, 128u);
}

uint32_t venus_uv_scanlines(uint32_t raw_pixfmt, uint32_t height)
{
    (void)raw_pixfmt;
    if (!height)
        return 0;
    return venus_align_u32((height + 1u) >> 1, 16u);
}

uint32_t venus_raw_buffer_size(uint32_t raw_pixfmt, uint32_t width, uint32_t height)
{
    if (!width || !height)
        return 0;
    uint32_t ys = venus_y_stride(raw_pixfmt, width);
    uint32_t yl = venus_y_scanlines(raw_pixfmt, height);
    uint32_t uvs = venus_uv_stride(raw_pixfmt, width);
    uint32_t uvl = venus_uv_scanlines(raw_pixfmt, height);
    uint64_t total = (uint64_t)ys * yl + (uint64_t)uvs * uvl;
    total = (total + 4095ull) & ~4095ull;
    return total > UINT32_MAX ? 0u : (uint32_t)total;
}

static int sys_open(void *ud, const char *path, int flags)
{
    (void)ud;
    return open(path, flags);
}

static int sys_close(void *ud, int fd)
{
    (void)ud;
    return close(fd);
}

static int sys_ioctl(void *ud, int fd, unsigned long req, void *arg)
{
    (void)ud;
    int rc;
    do {
        rc = ioctl(fd, req, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static int sys_poll(void *ud, struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    (void)ud;
    int rc;
    do {
        rc = poll(fds, nfds, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

static void *sys_mmap(void *ud, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)ud;
    return mmap(NULL, len, prot, flags, fd, off);
}

static int sys_munmap(void *ud, void *addr, size_t len)
{
    (void)ud;
    return munmap(addr, len);
}

static int sys_dmabuf_alloc(void *ud, uint32_t size)
{
    (void)ud;
    if (size == 0)
        return -EINVAL;
    int heap = open(VENUS_DMA_HEAP, O_RDONLY | O_CLOEXEC);
    if (heap < 0)
        return -errno;
    struct dma_heap_allocation_data req;
    memset(&req, 0, sizeof(req));
    req.len = venus_align_u32(size, 4096u);
    req.fd_flags = O_RDWR | O_CLOEXEC;
    int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req);
    int saved = errno;
    close(heap);
    if (rc < 0)
        return -(saved ? saved : EIO);
    return (int)req.fd;
}

static int sys_dmabuf_sync(void *ud, int fd, uint64_t flags)
{
    (void)ud;
    if (fd < 0)
        return -EBADF;
    struct dma_buf_sync s;
    memset(&s, 0, sizeof(s));
    s.flags = flags;
    int rc;
    do {
        rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &s);
    } while (rc < 0 && errno == EINTR);
    return rc < 0 ? -errno : 0;
}

void venus_default_sys_ops(struct venus_sys_ops *ops)
{
    if (!ops)
        return;
    memset(ops, 0, sizeof(*ops));
    ops->open_fn = sys_open;
    ops->close_fn = sys_close;
    ops->ioctl_fn = sys_ioctl;
    ops->poll_fn = sys_poll;
    ops->mmap_fn = sys_mmap;
    ops->munmap_fn = sys_munmap;
    ops->dmabuf_alloc_fn = sys_dmabuf_alloc;
    ops->dmabuf_sync_fn = sys_dmabuf_sync;
}

static int v_ioctl(struct venus_ctx *ctx, unsigned long req, void *arg)
{
    int rc = ctx->ops.ioctl_fn(ctx->ops.userdata, ctx->fd, req, arg);
    return rc < 0 ? -(errno ? errno : EIO) : 0;
}

int venus_dmabuf_sync(struct venus_ctx *ctx, int fd, uint64_t flags)
{
    if (!ctx || !ctx->ops.dmabuf_sync_fn)
        return -EINVAL;
    return ctx->ops.dmabuf_sync_fn(ctx->ops.userdata, fd, flags);
}

static void init_buf_array(struct venus_buf *bufs, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        bufs[i].fd = -1;
        bufs[i].map = NULL;
        bufs[i].size = 0;
        bufs[i].index = i;
        bufs[i].queued = false;
    }
}

static void free_buf_array(struct venus_ctx *ctx, struct venus_buf *bufs, uint32_t count)
{
    for (uint32_t i = 0; i < count && i < VENUS_MAX_BUFS; i++) {
        if (bufs[i].map && bufs[i].map != MAP_FAILED && bufs[i].size > 0)
            ctx->ops.munmap_fn(ctx->ops.userdata, bufs[i].map, bufs[i].size);
        if (bufs[i].fd >= 0)
            ctx->ops.close_fn(ctx->ops.userdata, bufs[i].fd);
        bufs[i].fd = -1;
        bufs[i].map = NULL;
        bufs[i].size = 0;
        bufs[i].queued = false;
    }
}

static int alloc_buf_array(struct venus_ctx *ctx, struct venus_buf *bufs,
                           uint32_t count, uint32_t buf_size)
{
    if (count == 0 || count > VENUS_MAX_BUFS || buf_size == 0)
        return -EINVAL;
    uint32_t aligned = venus_align_u32(buf_size, 4096u);
    init_buf_array(bufs, count);
    for (uint32_t i = 0; i < count; i++) {
        int fd = ctx->ops.dmabuf_alloc_fn(ctx->ops.userdata, aligned);
        if (fd < 0) {
            free_buf_array(ctx, bufs, count);
            return fd;
        }
        void *map = ctx->ops.mmap_fn(ctx->ops.userdata, aligned,
                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (!map || map == MAP_FAILED) {
            int err = errno ? -errno : -ENOMEM;
            ctx->ops.close_fn(ctx->ops.userdata, fd);
            free_buf_array(ctx, bufs, count);
            return err;
        }
        bufs[i].fd = fd;
        bufs[i].map = map;
        bufs[i].size = aligned;
        bufs[i].index = i;
        bufs[i].queued = false;
    }
    return 0;
}

static int set_ext_ctrl(struct venus_ctx *ctx, uint32_t cid, int32_t val)
{
    struct v4l2_ext_control c;
    struct v4l2_ext_controls cs;
    memset(&c, 0, sizeof(c));
    memset(&cs, 0, sizeof(cs));
    c.id = cid;
    c.value = val;
    cs.ctrl_class = V4L2_CTRL_ID2CLASS(cid);
    cs.which = V4L2_CTRL_WHICH_CUR_VAL;
    cs.count = 1;
    cs.controls = &c;
    int rc = v_ioctl(ctx, VIDIOC_S_EXT_CTRLS, &cs);
    if (rc == 0)
        return 0;
    struct v4l2_control sc;
    memset(&sc, 0, sizeof(sc));
    sc.id = cid;
    sc.value = val;
    return v_ioctl(ctx, VIDIOC_S_CTRL, &sc);
}

static int get_ctrl(struct venus_ctx *ctx, uint32_t cid, int32_t *val)
{
    struct v4l2_control sc;
    memset(&sc, 0, sizeof(sc));
    sc.id = cid;
    int rc = v_ioctl(ctx, VIDIOC_G_CTRL, &sc);
    if (rc == 0 && val)
        *val = sc.value;
    return rc;
}

static void update_raw_geom(struct venus_ctx *ctx, const struct v4l2_format *raw_fmt,
                            uint32_t vis_w, uint32_t vis_h)
{
    uint32_t raw_fcc = raw_fmt->fmt.pix_mp.pixelformat;
    if (!raw_fcc)
        raw_fcc = ctx->fmt.raw_pixfmt;
    uint32_t ys = raw_fmt->fmt.pix_mp.plane_fmt[0].bytesperline;
    if (ys < vis_w)
        ys = venus_y_stride(raw_fcc, vis_w);
    uint32_t yl = raw_fmt->fmt.pix_mp.height;
    if (yl < vis_h)
        yl = venus_y_scanlines(raw_fcc, vis_h);
    uint32_t sz = raw_fmt->fmt.pix_mp.plane_fmt[0].sizeimage;
    uint32_t min_sz = venus_raw_buffer_size(raw_fcc, vis_w, vis_h);
    if (sz < min_sz)
        sz = min_sz;

    ctx->fmt.width = vis_w;
    ctx->fmt.height = vis_h;
    ctx->fmt.y_stride = ys;
    ctx->fmt.y_scanlines = yl;
    ctx->fmt.uv_offset = ys * yl;
    if (ctx->role == VENUS_ROLE_DECODER)
        ctx->fmt.out_buf_size = sz;
    else
        ctx->fmt.in_buf_size = sz;
}

int venus_ctx_open(struct venus_ctx *ctx, enum venus_role role,
                   uint32_t coded_fourcc, uint32_t raw_fourcc,
                   uint32_t init_w, uint32_t init_h,
                   const struct venus_sys_ops *ops)
{
    if (!ctx)
        return -EINVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->wake_efd = -1;
    ctx->role = role;
    init_buf_array(ctx->in_bufs, VENUS_MAX_BUFS);
    init_buf_array(ctx->out_bufs, VENUS_MAX_BUFS);

    if (ops)
        ctx->ops = *ops;
    else
        venus_default_sys_ops(&ctx->ops);

    const char *path = (role == VENUS_ROLE_DECODER) ? VENUS_DEV_DECODER : VENUS_DEV_ENCODER;
    int fd = ctx->ops.open_fn(ctx->ops.userdata, path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -(errno ? errno : ENOENT);
    ctx->fd = fd;

    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        int err = -(errno ? errno : ENOMEM);
        ctx->ops.close_fn(ctx->ops.userdata, ctx->fd);
        ctx->fd = -1;
        return err;
    }
    ctx->wake_efd = efd;

    ctx->fmt.coded_pixfmt = coded_fourcc ? coded_fourcc : V4L2_PIX_FMT_H264;
    ctx->fmt.raw_pixfmt = raw_fourcc ? raw_fourcc : V4L2_PIX_FMT_NV12;
    ctx->fmt.width = init_w ? init_w : 1280u;
    ctx->fmt.height = init_h ? init_h : 720u;
    ctx->fmt.crop_width = ctx->fmt.width;
    ctx->fmt.crop_height = ctx->fmt.height;
    return 0;
}

int venus_ctx_configure(struct venus_ctx *ctx, uint32_t width, uint32_t height,
                        uint32_t fps, uint32_t bitrate, uint32_t i_period,
                        int32_t bitrate_mode, int32_t profile, int32_t level)
{
    if (!ctx || ctx->fd < 0 || width == 0 || height == 0)
        return -EINVAL;

    struct v4l2_event_subscription sub;
    if (ctx->role == VENUS_ROLE_DECODER) {
        memset(&sub, 0, sizeof(sub));
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        (void)v_ioctl(ctx, VIDIOC_SUBSCRIBE_EVENT, &sub);
    }
    memset(&sub, 0, sizeof(sub));
    sub.type = V4L2_EVENT_EOS;
    (void)v_ioctl(ctx, VIDIOC_SUBSCRIBE_EVENT, &sub);

    struct v4l2_format out_q_fmt;
    struct v4l2_format cap_q_fmt;
    memset(&out_q_fmt, 0, sizeof(out_q_fmt));
    memset(&cap_q_fmt, 0, sizeof(cap_q_fmt));

    if (ctx->role == VENUS_ROLE_DECODER) {
        out_q_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        out_q_fmt.fmt.pix_mp.width = width;
        out_q_fmt.fmt.pix_mp.height = height;
        out_q_fmt.fmt.pix_mp.pixelformat = ctx->fmt.coded_pixfmt;
        out_q_fmt.fmt.pix_mp.num_planes = 1;
        out_q_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        int rc = v_ioctl(ctx, VIDIOC_S_FMT, &out_q_fmt);
        if (rc < 0)
            return rc;

        cap_q_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cap_q_fmt.fmt.pix_mp.width = width;
        cap_q_fmt.fmt.pix_mp.height = height;
        cap_q_fmt.fmt.pix_mp.pixelformat = ctx->fmt.raw_pixfmt;
        cap_q_fmt.fmt.pix_mp.num_planes = 1;
        cap_q_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        rc = v_ioctl(ctx, VIDIOC_S_FMT, &cap_q_fmt);
        if (rc < 0)
            return rc;

        uint32_t in_sz = out_q_fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        if (in_sz < 1024u * 1024u)
            in_sz = 2u * 1024u * 1024u;
        ctx->fmt.in_buf_size = venus_align_u32(in_sz, 4096u);
        update_raw_geom(ctx, &cap_q_fmt, width, height);
        ctx->fmt.crop_left = 0;
        ctx->fmt.crop_top = 0;
        ctx->fmt.crop_width = width;
        ctx->fmt.crop_height = height;
    } else {
        cap_q_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        cap_q_fmt.fmt.pix_mp.width = width;
        cap_q_fmt.fmt.pix_mp.height = height;
        cap_q_fmt.fmt.pix_mp.pixelformat = ctx->fmt.coded_pixfmt;
        cap_q_fmt.fmt.pix_mp.num_planes = 1;
        cap_q_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        int rc = v_ioctl(ctx, VIDIOC_S_FMT, &cap_q_fmt);
        if (rc < 0)
            return rc;

        out_q_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        out_q_fmt.fmt.pix_mp.width = width;
        out_q_fmt.fmt.pix_mp.height = height;
        out_q_fmt.fmt.pix_mp.pixelformat = ctx->fmt.raw_pixfmt;
        out_q_fmt.fmt.pix_mp.num_planes = 1;
        out_q_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        rc = v_ioctl(ctx, VIDIOC_S_FMT, &out_q_fmt);
        if (rc < 0)
            return rc;

        struct v4l2_selection sel;
        memset(&sel, 0, sizeof(sel));
        sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        sel.target = V4L2_SEL_TGT_CROP;
        sel.r.width = width;
        sel.r.height = height;
        (void)v_ioctl(ctx, VIDIOC_S_SELECTION, &sel);

        memset(&sel, 0, sizeof(sel));
        sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        sel.target = V4L2_SEL_TGT_COMPOSE;
        sel.r.width = width;
        sel.r.height = height;
        (void)v_ioctl(ctx, VIDIOC_S_SELECTION, &sel);

        /* Re-read OUTPUT and CAPTURE formats after selection updates compose */
        memset(&out_q_fmt, 0, sizeof(out_q_fmt));
        out_q_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (v_ioctl(ctx, VIDIOC_G_FMT, &out_q_fmt) == 0)
            update_raw_geom(ctx, &out_q_fmt, width, height);

        memset(&cap_q_fmt, 0, sizeof(cap_q_fmt));
        cap_q_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        (void)v_ioctl(ctx, VIDIOC_G_FMT, &cap_q_fmt);
        uint32_t out_sz = cap_q_fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        if (out_sz < 512u * 1024u)
            out_sz = 2u * 1024u * 1024u;
        ctx->fmt.out_buf_size = venus_align_u32(out_sz, 4096u);
        ctx->fmt.crop_left = 0;
        ctx->fmt.crop_top = 0;
        ctx->fmt.crop_width = width;
        ctx->fmt.crop_height = height;

        if (fps > 0) {
            struct v4l2_streamparm sp;
            memset(&sp, 0, sizeof(sp));
            sp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            sp.parm.capture.timeperframe.numerator = 1;
            sp.parm.capture.timeperframe.denominator = fps;
            (void)v_ioctl(ctx, VIDIOC_S_PARM, &sp);

            memset(&sp, 0, sizeof(sp));
            sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            sp.parm.output.timeperframe.numerator = 1;
            sp.parm.output.timeperframe.denominator = fps;
            (void)v_ioctl(ctx, VIDIOC_S_PARM, &sp);
        }

        if (bitrate_mode >= 0)
            (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, bitrate_mode);
        if (bitrate > 0)
            (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bitrate);
        if (i_period > 0)
            (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_GOP_SIZE, (int32_t)i_period);
        (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
                           V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
        (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);

        if (ctx->fmt.coded_pixfmt == V4L2_PIX_FMT_H264) {
            if (profile >= 0)
                (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_H264_PROFILE, profile);
            if (level >= 0)
                (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_H264_LEVEL, level);
        } else if (ctx->fmt.coded_pixfmt == V4L2_PIX_FMT_HEVC) {
            if (profile >= 0)
                (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, profile);
            if (level >= 0)
                (void)set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, level);
        }
    }

    int32_t min_in = 0, min_out = 0;
    if (get_ctrl(ctx, V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, &min_in) < 0)
        min_in = 4;
    if (get_ctrl(ctx, V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, &min_out) < 0)
        min_out = (ctx->role == VENUS_ROLE_DECODER) ? 8 : 4;

    ctx->fmt.in_buf_count = clamp_u32((uint32_t)(min_in > 0 ? min_in : 4), 4u, 8u);
    if (ctx->role == VENUS_ROLE_DECODER)
        ctx->fmt.out_buf_count = clamp_u32((uint32_t)(min_out > 0 ? min_out : 8), 4u, 16u);
    else
        ctx->fmt.out_buf_count = clamp_u32((uint32_t)(min_out > 0 ? min_out : 4), 4u, 8u);

    ctx->configured = true;
    return 0;
}

static int queue_buf_internal(struct venus_ctx *ctx, uint32_t buf_type,
                              struct venus_buf *buf, uint32_t bytesused,
                              int64_t ts_us, uint32_t flags)
{
    struct v4l2_plane plane;
    struct v4l2_buffer vb;
    memset(&plane, 0, sizeof(plane));
    memset(&vb, 0, sizeof(vb));

    plane.m.fd = buf->fd;
    plane.length = buf->size;
    plane.bytesused = bytesused;
    plane.data_offset = 0;

    vb.type = buf_type;
    vb.memory = V4L2_MEMORY_DMABUF;
    vb.index = buf->index;
    vb.length = 1;
    vb.m.planes = &plane;
    vb.flags = flags;
    if (ts_us >= 0) {
        vb.timestamp.tv_sec = ts_us / 1000000LL;
        vb.timestamp.tv_usec = ts_us % 1000000LL;
    }

    int rc = v_ioctl(ctx, VIDIOC_QBUF, &vb);
    if (rc == 0)
        buf->queued = true;
    return rc;
}

int venus_ctx_start(struct venus_ctx *ctx)
{
    if (!ctx || ctx->fd < 0 || !ctx->configured)
        return -EINVAL;
    if (ctx->role == VENUS_ROLE_DECODER) {
        if (ctx->streaming_in)
            return 0;
    } else {
        if (ctx->streaming_in && ctx->streaming_out)
            return 0;
    }

    int rc = alloc_buf_array(ctx, ctx->in_bufs, ctx->fmt.in_buf_count, ctx->fmt.in_buf_size);
    if (rc < 0)
        return rc;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    req.count = ctx->fmt.in_buf_count;
    rc = v_ioctl(ctx, VIDIOC_REQBUFS, &req);
    if (rc < 0)
        goto fail;

    uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rc = v_ioctl(ctx, VIDIOC_STREAMON, &t);
    if (rc < 0)
        goto fail;
    ctx->streaming_in = true;
    ctx->streaming_out = false;
    ctx->eos_sent = false;

    /* Stateful SM7475 Venus decoder starts INPUT only (START_INPUT) and waits for
     * V4L2_EVENT_SOURCE_CHANGE before allocating/streaming CAPTURE_MPLANE. */
    if (ctx->role == VENUS_ROLE_DECODER)
        return 0;

    rc = alloc_buf_array(ctx, ctx->out_bufs, ctx->fmt.out_buf_count, ctx->fmt.out_buf_size);
    if (rc < 0)
        goto fail;

    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    req.count = ctx->fmt.out_buf_count;
    rc = v_ioctl(ctx, VIDIOC_REQBUFS, &req);
    if (rc < 0)
        goto fail;

    for (uint32_t i = 0; i < ctx->fmt.out_buf_count; i++) {
        rc = queue_buf_internal(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                &ctx->out_bufs[i], 0u, -1, 0u);
        if (rc < 0)
            goto fail;
    }

    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rc = v_ioctl(ctx, VIDIOC_STREAMON, &t);
    if (rc < 0)
        goto fail;
    ctx->streaming_out = true;
    return 0;

fail:
    (void)venus_ctx_stop(ctx);
    return rc;
}

int venus_ctx_acquire_free_input(struct venus_ctx *ctx)
{
    if (!ctx)
        return -EINVAL;
    for (uint32_t i = 0; i < ctx->fmt.in_buf_count; i++) {
        if (!ctx->in_bufs[i].queued && ctx->in_bufs[i].fd >= 0)
            return (int)i;
    }
    return -1;
}

int venus_ctx_queue_input(struct venus_ctx *ctx, uint32_t idx, uint32_t bytesused,
                          int64_t ts_us, uint32_t flags)
{
    if (!ctx || !ctx->streaming_in || idx >= ctx->fmt.in_buf_count)
        return -EINVAL;
    struct venus_buf *buf = &ctx->in_bufs[idx];
    if (buf->queued || buf->fd < 0 || bytesused > buf->size)
        return -EINVAL;
    return queue_buf_internal(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, buf,
                              bytesused, ts_us, flags);
}

int venus_ctx_requeue_output(struct venus_ctx *ctx, uint32_t idx)
{
    if (!ctx || !ctx->streaming_out || idx >= ctx->fmt.out_buf_count)
        return -EINVAL;
    struct venus_buf *buf = &ctx->out_bufs[idx];
    if (buf->queued || buf->fd < 0)
        return -EINVAL;
    return queue_buf_internal(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, buf,
                              0u, -1, 0u);
}

int venus_ctx_dq_input(struct venus_ctx *ctx, uint32_t *out_idx)
{
    if (!ctx || !ctx->streaming_in)
        return -EINVAL;
    struct v4l2_plane plane;
    struct v4l2_buffer vb;
    memset(&plane, 0, sizeof(plane));
    memset(&vb, 0, sizeof(vb));
    vb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    vb.memory = V4L2_MEMORY_DMABUF;
    vb.length = 1;
    vb.m.planes = &plane;
    int rc = v_ioctl(ctx, VIDIOC_DQBUF, &vb);
    if (rc < 0)
        return rc;
    if (vb.index < ctx->fmt.in_buf_count)
        ctx->in_bufs[vb.index].queued = false;
    if (out_idx)
        *out_idx = vb.index;
    return 0;
}

int venus_ctx_dq_output(struct venus_ctx *ctx, uint32_t *out_idx,
                        uint32_t *bytesused, int64_t *ts_us, uint32_t *flags)
{
    if (!ctx || !ctx->streaming_out)
        return -EINVAL;
    struct v4l2_plane plane;
    struct v4l2_buffer vb;
    memset(&plane, 0, sizeof(plane));
    memset(&vb, 0, sizeof(vb));
    vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    vb.memory = V4L2_MEMORY_DMABUF;
    vb.length = 1;
    vb.m.planes = &plane;
    int rc = v_ioctl(ctx, VIDIOC_DQBUF, &vb);
    if (rc < 0)
        return rc;
    if (vb.index < ctx->fmt.out_buf_count)
        ctx->out_bufs[vb.index].queued = false;
    if (out_idx)
        *out_idx = vb.index;
    if (bytesused)
        *bytesused = plane.bytesused;
    if (ts_us)
        *ts_us = (int64_t)vb.timestamp.tv_sec * 1000000LL + (int64_t)vb.timestamp.tv_usec;
    if (flags)
        *flags = vb.flags;
    return 0;
}

int venus_ctx_send_eos(struct venus_ctx *ctx)
{
    if (!ctx || ctx->fd < 0)
        return -EINVAL;
    if (ctx->eos_sent)
        return 0;
    if (ctx->role == VENUS_ROLE_DECODER && !ctx->streaming_out)
        return -EAGAIN;

    int rc;
    if (ctx->role == VENUS_ROLE_DECODER) {
        struct v4l2_decoder_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmd = V4L2_DEC_CMD_STOP;
        rc = v_ioctl(ctx, VIDIOC_DECODER_CMD, &cmd);
    } else {
        struct v4l2_encoder_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.cmd = V4L2_ENC_CMD_STOP;
        rc = v_ioctl(ctx, VIDIOC_ENCODER_CMD, &cmd);
    }
    if (rc == 0) {
        ctx->eos_sent = true;
        return 0;
    }
    if (rc == -ENOTTY || rc == -EINVAL) {
        int slot = venus_ctx_acquire_free_input(ctx);
        if (slot >= 0) {
            rc = venus_ctx_queue_input(ctx, (uint32_t)slot, 0u, 0, V4L2_BUF_FLAG_LAST);
            if (rc == 0)
                ctx->eos_sent = true;
        }
    }
    return rc;
}

int venus_ctx_poll(struct venus_ctx *ctx, int timeout_ms, uint32_t *out_events)
{
    if (!ctx || ctx->fd < 0 || !out_events)
        return -EINVAL;
    *out_events = 0;

    struct pollfd pfds[2];
    nfds_t nfds = 0;
    pfds[nfds].fd = ctx->fd;
    pfds[nfds].events = POLLIN | POLLOUT | POLLPRI;
    pfds[nfds].revents = 0;
    nfds++;

    if (ctx->wake_efd >= 0) {
        pfds[nfds].fd = ctx->wake_efd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        nfds++;
    }

    int rc = ctx->ops.poll_fn(ctx->ops.userdata, pfds, nfds, timeout_ms);
    if (rc < 0)
        return -(errno ? errno : EIO);
    if (rc == 0)
        return 0;

    uint32_t ev = 0;
    if (pfds[0].revents & POLLOUT)
        ev |= VENUS_POLL_INPUT_READY;
    if (pfds[0].revents & POLLIN)
        ev |= VENUS_POLL_OUTPUT_READY;
    if (pfds[0].revents & POLLPRI)
        ev |= VENUS_POLL_EVENT;
    if (pfds[0].revents & (POLLERR | POLLNVAL))
        ev |= VENUS_POLL_ERR;

    if (nfds > 1 && (pfds[1].revents & POLLIN)) {
        uint64_t val = 0;
        while (read(ctx->wake_efd, &val, sizeof(val)) == (ssize_t)sizeof(val)) {
        }
        ev |= VENUS_POLL_WAKEUP;
    }

    *out_events = ev;
    return 0;
}

void venus_ctx_wakeup(struct venus_ctx *ctx)
{
    if (!ctx || ctx->wake_efd < 0)
        return;
    uint64_t v = 1;
    ssize_t unused = write(ctx->wake_efd, &v, sizeof(v));
    (void)unused;
}

int venus_ctx_dq_event(struct venus_ctx *ctx, struct v4l2_event *ev)
{
    if (!ctx || ctx->fd < 0 || !ev)
        return -EINVAL;
    memset(ev, 0, sizeof(*ev));
    return v_ioctl(ctx, VIDIOC_DQEVENT, ev);
}

int venus_ctx_handle_source_change(struct venus_ctx *ctx)
{
    if (!ctx || ctx->fd < 0 || ctx->role != VENUS_ROLE_DECODER)
        return -EINVAL;

    bool had_cap = ctx->streaming_out || ctx->out_bufs[0].fd >= 0;
    if (ctx->streaming_out) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        int rc = v_ioctl(ctx, VIDIOC_STREAMOFF, &t);
        if (rc < 0)
            return rc;
        ctx->streaming_out = false;
    }

    struct v4l2_requestbuffers req;
    if (had_cap) {
        memset(&req, 0, sizeof(req));
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        req.memory = V4L2_MEMORY_DMABUF;
        req.count = 0;
        (void)v_ioctl(ctx, VIDIOC_REQBUFS, &req);

        free_buf_array(ctx, ctx->out_bufs, VENUS_MAX_BUFS);
    }

    struct v4l2_format cap_fmt;
    memset(&cap_fmt, 0, sizeof(cap_fmt));
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    int rc = v_ioctl(ctx, VIDIOC_G_FMT, &cap_fmt);
    if (rc < 0)
        return rc;

    cap_fmt.fmt.pix_mp.pixelformat = ctx->fmt.raw_pixfmt;
    (void)v_ioctl(ctx, VIDIOC_S_FMT, &cap_fmt);
    (void)v_ioctl(ctx, VIDIOC_G_FMT, &cap_fmt);

    struct v4l2_selection sel;
    memset(&sel, 0, sizeof(sel));
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    uint32_t vis_w = cap_fmt.fmt.pix_mp.width;
    uint32_t vis_h = cap_fmt.fmt.pix_mp.height;
    if (v_ioctl(ctx, VIDIOC_G_SELECTION, &sel) == 0 &&
        sel.r.width > 0 && sel.r.height > 0) {
        ctx->fmt.crop_left = (uint32_t)sel.r.left;
        ctx->fmt.crop_top = (uint32_t)sel.r.top;
        ctx->fmt.crop_width = sel.r.width;
        ctx->fmt.crop_height = sel.r.height;
        vis_w = sel.r.width;
        vis_h = sel.r.height;
    } else {
        ctx->fmt.crop_left = 0;
        ctx->fmt.crop_top = 0;
        ctx->fmt.crop_width = vis_w;
        ctx->fmt.crop_height = vis_h;
    }
    update_raw_geom(ctx, &cap_fmt, vis_w, vis_h);

    int32_t min_out = 0;
    if (get_ctrl(ctx, V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, &min_out) < 0)
        min_out = 8;
    ctx->fmt.out_buf_count = clamp_u32((uint32_t)(min_out > 0 ? min_out : 8), 4u, 16u);

    rc = alloc_buf_array(ctx, ctx->out_bufs, ctx->fmt.out_buf_count, ctx->fmt.out_buf_size);
    if (rc < 0)
        return rc;

    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_DMABUF;
    req.count = ctx->fmt.out_buf_count;
    rc = v_ioctl(ctx, VIDIOC_REQBUFS, &req);
    if (rc < 0)
        return rc;

    for (uint32_t i = 0; i < ctx->fmt.out_buf_count; i++) {
        rc = queue_buf_internal(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                &ctx->out_bufs[i], 0u, -1, 0u);
        if (rc < 0)
            return rc;
    }

    uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rc = v_ioctl(ctx, VIDIOC_STREAMON, &t);
    if (rc < 0)
        return rc;
    ctx->streaming_out = true;
    return 0;
}

int venus_ctx_flush(struct venus_ctx *ctx, bool flush_in, bool flush_out)
{
    if (!ctx || ctx->fd < 0)
        return -EINVAL;
    bool was_in = ctx->streaming_in;
    bool was_out = ctx->streaming_out;
    int rc = 0;
    if (flush_in && was_in) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        int r = v_ioctl(ctx, VIDIOC_STREAMOFF, &t);
        if (r < 0)
            rc = r;
        ctx->streaming_in = false;
        for (uint32_t i = 0; i < ctx->fmt.in_buf_count; i++)
            ctx->in_bufs[i].queued = false;
    }
    if (flush_out && was_out) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        int r = v_ioctl(ctx, VIDIOC_STREAMOFF, &t);
        if (r < 0)
            rc = r;
        ctx->streaming_out = false;
        for (uint32_t i = 0; i < ctx->fmt.out_buf_count; i++)
            ctx->out_bufs[i].queued = false;
    }
    if (rc < 0)
        return rc;

    if (flush_in && was_in) {
        uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        rc = v_ioctl(ctx, VIDIOC_STREAMON, &t);
        if (rc < 0)
            return rc;
        ctx->streaming_in = true;
        ctx->eos_sent = false;
    }
    if (flush_out && was_out) {
        for (uint32_t i = 0; i < ctx->fmt.out_buf_count; i++) {
            rc = queue_buf_internal(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                    &ctx->out_bufs[i], 0u, -1, 0u);
            if (rc < 0)
                return rc;
        }
        uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        rc = v_ioctl(ctx, VIDIOC_STREAMON, &t);
        if (rc < 0)
            return rc;
        ctx->streaming_out = true;
    }
    return 0;
}

int venus_ctx_stop(struct venus_ctx *ctx)
{
    if (!ctx)
        return -EINVAL;
    if (ctx->fd >= 0) {
        if (ctx->streaming_in) {
            uint32_t t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            (void)v_ioctl(ctx, VIDIOC_STREAMOFF, &t);
            ctx->streaming_in = false;
        }
        if (ctx->streaming_out) {
            uint32_t t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            (void)v_ioctl(ctx, VIDIOC_STREAMOFF, &t);
            ctx->streaming_out = false;
        }
        struct v4l2_requestbuffers req;
        memset(&req, 0, sizeof(req));
        req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        req.memory = V4L2_MEMORY_DMABUF;
        (void)v_ioctl(ctx, VIDIOC_REQBUFS, &req);
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        (void)v_ioctl(ctx, VIDIOC_REQBUFS, &req);
    }
    free_buf_array(ctx, ctx->in_bufs, VENUS_MAX_BUFS);
    free_buf_array(ctx, ctx->out_bufs, VENUS_MAX_BUFS);
    ctx->eos_sent = false;
    return 0;
}

void venus_ctx_close(struct venus_ctx *ctx)
{
    if (!ctx)
        return;
    (void)venus_ctx_stop(ctx);
    if (ctx->fd >= 0) {
        ctx->ops.close_fn(ctx->ops.userdata, ctx->fd);
        ctx->fd = -1;
    }
    if (ctx->wake_efd >= 0) {
        close(ctx->wake_efd);
        ctx->wake_efd = -1;
    }
    ctx->configured = false;
}

static void copy_uv_row(uint8_t *dst_row, const uint8_t *src_row, uint32_t bytes, bool swap_uv)
{
    if (!swap_uv) {
        memcpy(dst_row, src_row, bytes);
        return;
    }
    uint32_t i = 0;
    for (; i + 1u < bytes; i += 2u) {
        dst_row[i] = src_row[i + 1u];
        dst_row[i + 1u] = src_row[i];
    }
    if (i < bytes)
        dst_row[i] = src_row[i];
}

int venus_nv12_to_linear_dst(const uint8_t *src_y, uint32_t src_uv_off,
                             uint32_t src_stride, uint8_t *dst,
                             size_t dst_max_bytes, uint32_t dst_stride,
                             uint32_t dst_y_lines, uint32_t crop_left,
                             uint32_t crop_top, uint32_t crop_w,
                             uint32_t crop_h, bool swap_uv)
{
    if (!src_y || !dst || !crop_w || !crop_h || src_stride < crop_w ||
        dst_stride < crop_w || dst_y_lines < crop_h)
        return -EINVAL;

    uint32_t uv_h = (crop_h + 1u) / 2u;
    uint32_t uv_copy_w = (crop_w + 1u) & ~1u;
    if (uv_copy_w > dst_stride)
        uv_copy_w = dst_stride;

    size_t dst_uv_off = (size_t)dst_stride * dst_y_lines;
    size_t y_end = (size_t)(crop_h - 1u) * dst_stride + crop_w;
    size_t uv_end = dst_uv_off + (size_t)(uv_h - 1u) * dst_stride + uv_copy_w;
    if (y_end > dst_max_bytes || uv_end > dst_max_bytes)
        return -ENOSPC;

    for (uint32_t r = 0; r < crop_h; r++) {
        const uint8_t *srow = src_y + (size_t)(crop_top + r) * src_stride + crop_left;
        uint8_t *drow = dst + (size_t)r * dst_stride;
        memcpy(drow, srow, crop_w);
    }

    const uint8_t *src_uv = src_y + src_uv_off;
    uint8_t *dst_uv = dst + dst_uv_off;
    uint32_t src_uv_top = crop_top / 2u;
    uint32_t src_uv_left = crop_left & ~1u;
    for (uint32_t r = 0; r < uv_h; r++) {
        const uint8_t *srow = src_uv + (size_t)(src_uv_top + r) * src_stride + src_uv_left;
        uint8_t *drow = dst_uv + (size_t)r * dst_stride;
        copy_uv_row(drow, srow, uv_copy_w, swap_uv);
    }
    return 0;
}

int venus_linear_src_to_nv12(const uint8_t *src, size_t src_size,
                             uint32_t src_stride, uint32_t src_y_lines,
                             uint8_t *dst_y, uint32_t dst_uv_off,
                             uint32_t dst_stride, size_t dst_max_bytes,
                             uint32_t w, uint32_t h, bool swap_uv)
{
    if (!src || !dst_y || !w || !h || src_stride < w || dst_stride < w || src_y_lines < h)
        return -EINVAL;

    uint32_t uv_h = (h + 1u) / 2u;
    uint32_t uv_w = (w + 1u) & ~1u;
    size_t src_uv_off = (size_t)src_stride * src_y_lines;
    size_t src_uv_end = src_uv_off + (size_t)(uv_h - 1u) * src_stride + uv_w;
    size_t dst_y_end = (size_t)(h - 1u) * dst_stride + w;
    size_t dst_uv_end = (size_t)dst_uv_off + (size_t)(uv_h - 1u) * dst_stride + uv_w;
    if (src_uv_end > src_size || dst_y_end > dst_max_bytes || dst_uv_end > dst_max_bytes)
        return -ENOSPC;

    for (uint32_t r = 0; r < h; r++) {
        memcpy(dst_y + (size_t)r * dst_stride, src + (size_t)r * src_stride, w);
    }

    const uint8_t *src_uv = src + src_uv_off;
    uint8_t *dst_uv = dst_y + dst_uv_off;
    for (uint32_t r = 0; r < uv_h; r++) {
        copy_uv_row(dst_uv + (size_t)r * dst_stride,
                    src_uv + (size_t)r * src_stride, uv_w, swap_uv);
    }
    return 0;
}

int venus_nv12_to_i420_dst(const uint8_t *src_y, uint32_t src_uv_off,
                           uint32_t src_stride, uint8_t *dst,
                           size_t dst_max_bytes, uint32_t dst_stride,
                           uint32_t dst_y_lines, uint32_t crop_left,
                           uint32_t crop_top, uint32_t crop_w,
                           uint32_t crop_h)
{
    if (!src_y || !dst || !crop_w || !crop_h || src_stride < crop_w ||
        dst_stride < crop_w || (dst_stride & 1u) != 0u || dst_y_lines < crop_h)
        return -EINVAL;

    uint32_t uv_w = (crop_w + 1u) / 2u;
    uint32_t uv_h = (crop_h + 1u) / 2u;
    uint32_t dst_uv_stride = dst_stride / 2u;
    if (dst_uv_stride < uv_w)
        return -EINVAL;

    size_t u_off = (size_t)dst_stride * dst_y_lines;
    size_t uv_plane_sz = (size_t)dst_uv_stride * ((dst_y_lines + 1u) / 2u);
    size_t v_off = u_off + uv_plane_sz;
    size_t y_end = (size_t)(crop_h - 1u) * dst_stride + crop_w;
    size_t v_end = v_off + (size_t)(uv_h - 1u) * dst_uv_stride + uv_w;
    if (y_end > dst_max_bytes || v_end > dst_max_bytes)
        return -ENOSPC;

    for (uint32_t r = 0; r < crop_h; r++) {
        const uint8_t *srow = src_y + (size_t)(crop_top + r) * src_stride + crop_left;
        uint8_t *drow = dst + (size_t)r * dst_stride;
        memcpy(drow, srow, crop_w);
    }

    const uint8_t *src_uv = src_y + src_uv_off;
    uint8_t *dst_u = dst + u_off;
    uint8_t *dst_v = dst + v_off;
    uint32_t src_uv_top = crop_top / 2u;
    uint32_t src_uv_left = crop_left & ~1u;
    for (uint32_t r = 0; r < uv_h; r++) {
        const uint8_t *srow = src_uv + (size_t)(src_uv_top + r) * src_stride + src_uv_left;
        uint8_t *urow = dst_u + (size_t)r * dst_uv_stride;
        uint8_t *vrow = dst_v + (size_t)r * dst_uv_stride;
        for (uint32_t x = 0; x < uv_w; x++) {
            urow[x] = srow[x * 2u];
            vrow[x] = srow[x * 2u + 1u];
        }
    }
    return 0;
}

int venus_i420_src_to_nv12(const uint8_t *src, size_t src_size,
                           uint32_t src_stride, uint32_t src_y_lines,
                           uint8_t *dst_y, uint32_t dst_uv_off,
                           uint32_t dst_stride, size_t dst_max_bytes,
                           uint32_t w, uint32_t h)
{
    if (!src || !dst_y || !w || !h || src_stride < w ||
        (src_stride & 1u) != 0u || dst_stride < w || src_y_lines < h)
        return -EINVAL;

    uint32_t uv_w = (w + 1u) / 2u;
    uint32_t uv_h = (h + 1u) / 2u;
    uint32_t src_uv_stride = src_stride / 2u;
    if (src_uv_stride < uv_w)
        return -EINVAL;

    size_t u_off = (size_t)src_stride * src_y_lines;
    size_t uv_plane_sz = (size_t)src_uv_stride * ((src_y_lines + 1u) / 2u);
    size_t v_off = u_off + uv_plane_sz;
    size_t src_v_end = v_off + (size_t)(uv_h - 1u) * src_uv_stride + uv_w;
    size_t dst_y_end = (size_t)(h - 1u) * dst_stride + w;
    size_t dst_uv_end = (size_t)dst_uv_off + (size_t)(uv_h - 1u) * dst_stride + uv_w * 2u;
    if (src_v_end > src_size || dst_y_end > dst_max_bytes || dst_uv_end > dst_max_bytes)
        return -ENOSPC;

    for (uint32_t r = 0; r < h; r++)
        memcpy(dst_y + (size_t)r * dst_stride, src + (size_t)r * src_stride, w);

    const uint8_t *src_u = src + u_off;
    const uint8_t *src_v = src + v_off;
    uint8_t *dst_uv = dst_y + dst_uv_off;
    for (uint32_t r = 0; r < uv_h; r++) {
        const uint8_t *urow = src_u + (size_t)r * src_uv_stride;
        const uint8_t *vrow = src_v + (size_t)r * src_uv_stride;
        uint8_t *drow = dst_uv + (size_t)r * dst_stride;
        for (uint32_t x = 0; x < uv_w; x++) {
            drow[x * 2u] = urow[x];
            drow[x * 2u + 1u] = vrow[x];
        }
    }
    return 0;
}

int venus_ctx_force_keyframe(struct venus_ctx *ctx)
{
    if (!ctx || ctx->fd < 0 || ctx->role != VENUS_ROLE_ENCODER)
        return -EINVAL;
#ifdef V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME
    return set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
#else
    return 0;
#endif
}

int venus_ctx_set_bitrate(struct venus_ctx *ctx, uint32_t bitrate)
{
    if (!ctx || ctx->fd < 0 || ctx->role != VENUS_ROLE_ENCODER || bitrate == 0)
        return -EINVAL;
    return set_ext_ctrl(ctx, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bitrate);
}

#ifdef VENUS_V4L2_HOST_TEST
#include <assert.h>

struct mock_state {
    enum venus_role role;
    uint32_t w;
    uint32_t h;
    uint32_t new_w;
    uint32_t new_h;
    bool trigger_sc;
    bool sc_pending;
    bool eos_pending;
    uint32_t out_req_cnt;
    uint32_t cap_req_cnt;
    bool in_queued[VENUS_MAX_BUFS];
    bool out_queued[VENUS_MAX_BUFS];
    bool in_ready[VENUS_MAX_BUFS];
    bool out_ready[VENUS_MAX_BUFS];
    int next_memfd;
};

static int mock_open(void *ud, const char *path, int flags)
{
    struct mock_state *ms = (struct mock_state *)ud;
    (void)flags;
    if (strcmp(path, VENUS_DEV_DECODER) == 0)
        ms->role = VENUS_ROLE_DECODER;
    else if (strcmp(path, VENUS_DEV_ENCODER) == 0)
        ms->role = VENUS_ROLE_ENCODER;
    else
        return -1;
    return 100;
}

static int mock_close(void *ud, int fd)
{
    (void)ud;
    if (fd >= 1000)
        close(fd);
    return 0;
}

static int mock_dmabuf_alloc(void *ud, uint32_t size)
{
    (void)ud;
    int fd = memfd_create("venus_mock", MFD_CLOEXEC);
    assert(fd >= 0);
    int rc = ftruncate(fd, (off_t)size);
    assert(rc == 0);
    return fd;
}

static int mock_dmabuf_sync(void *ud, int fd, uint64_t flags)
{
    (void)ud;
    (void)fd;
    (void)flags;
    return 0;
}

static void *mock_mmap(void *ud, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)ud;
    return mmap(NULL, len, prot, flags, fd, off);
}

static int mock_munmap(void *ud, void *addr, size_t len)
{
    (void)ud;
    return munmap(addr, len);
}

static int mock_ioctl(void *ud, int fd, unsigned long req, void *arg)
{
    struct mock_state *ms = (struct mock_state *)ud;
    (void)fd;
    if (req == VIDIOC_SUBSCRIBE_EVENT)
        return 0;
    if (req == VIDIOC_S_FMT) {
        struct v4l2_format *f = (struct v4l2_format *)arg;
        if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (ms->role == VENUS_ROLE_DECODER) {
                f->fmt.pix_mp.width = venus_align_u32(f->fmt.pix_mp.width, 16u);
                f->fmt.pix_mp.height = venus_align_u32(f->fmt.pix_mp.height, 16u);
                f->fmt.pix_mp.plane_fmt[0].sizeimage = 2u * 1024u * 1024u;
            } else {
                uint32_t w = f->fmt.pix_mp.width;
                uint32_t h = f->fmt.pix_mp.height;
                ms->w = w;
                ms->h = h;
                f->fmt.pix_mp.width = venus_y_stride(f->fmt.pix_mp.pixelformat, w);
                f->fmt.pix_mp.height = venus_y_scanlines(f->fmt.pix_mp.pixelformat, h);
                f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width;
                f->fmt.pix_mp.plane_fmt[0].sizeimage = venus_raw_buffer_size(V4L2_PIX_FMT_NV12, w, h);
            }
        } else {
            if (ms->role == VENUS_ROLE_DECODER) {
                uint32_t w = ms->sc_pending ? ms->new_w : f->fmt.pix_mp.width;
                uint32_t h = ms->sc_pending ? ms->new_h : f->fmt.pix_mp.height;
                ms->w = w;
                ms->h = h;
                f->fmt.pix_mp.width = venus_y_stride(V4L2_PIX_FMT_NV12, w);
                f->fmt.pix_mp.height = venus_y_scanlines(V4L2_PIX_FMT_NV12, h);
                f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width;
                f->fmt.pix_mp.plane_fmt[0].sizeimage = venus_raw_buffer_size(V4L2_PIX_FMT_NV12, w, h);
            } else {
                f->fmt.pix_mp.plane_fmt[0].sizeimage = 1024u * 1024u;
            }
        }
        return 0;
    }
    if (req == VIDIOC_G_FMT) {
        struct v4l2_format *f = (struct v4l2_format *)arg;
        uint32_t w = ms->new_w ? ms->new_w : ms->w;
        uint32_t h = ms->new_h ? ms->new_h : ms->h;
        f->fmt.pix_mp.num_planes = 1;
        if ((ms->role == VENUS_ROLE_DECODER && f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
            (ms->role == VENUS_ROLE_ENCODER && f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)) {
            f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            f->fmt.pix_mp.width = venus_y_stride(V4L2_PIX_FMT_NV12, w);
            f->fmt.pix_mp.height = venus_y_scanlines(V4L2_PIX_FMT_NV12, h);
            f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width;
            f->fmt.pix_mp.plane_fmt[0].sizeimage = venus_raw_buffer_size(V4L2_PIX_FMT_NV12, w, h);
        } else {
            f->fmt.pix_mp.width = w;
            f->fmt.pix_mp.height = h;
            f->fmt.pix_mp.plane_fmt[0].sizeimage = 1024u * 1024u;
        }
        return 0;
    }
    if (req == VIDIOC_S_SELECTION)
        return 0;
    if (req == VIDIOC_G_SELECTION) {
        struct v4l2_selection *s = (struct v4l2_selection *)arg;
        s->r.left = 0;
        s->r.top = 0;
        s->r.width = ms->new_w ? ms->new_w : ms->w;
        s->r.height = ms->new_h ? ms->new_h : ms->h;
        return 0;
    }
    if (req == VIDIOC_S_PARM || req == VIDIOC_S_EXT_CTRLS || req == VIDIOC_S_CTRL)
        return 0;
    if (req == VIDIOC_G_CTRL) {
        struct v4l2_control *c = (struct v4l2_control *)arg;
        if (c->id == V4L2_CID_MIN_BUFFERS_FOR_OUTPUT)
            c->value = 4;
        else if (c->id == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE)
            c->value = (ms->role == VENUS_ROLE_DECODER) ? 6 : 4;
        return 0;
    }
    if (req == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = (struct v4l2_requestbuffers *)arg;
        assert(r->memory == V4L2_MEMORY_DMABUF);
        if (r->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
            ms->out_req_cnt = r->count;
        else
            ms->cap_req_cnt = r->count;
        return 0;
    }
    if (req == VIDIOC_STREAMON || req == VIDIOC_STREAMOFF) {
        uint32_t t = *(uint32_t *)arg;
        if (req == VIDIOC_STREAMOFF && t == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
            memset(ms->out_ready, 0, sizeof(ms->out_ready));
        return 0;
    }
    if (req == VIDIOC_QBUF) {
        struct v4l2_buffer *vb = (struct v4l2_buffer *)arg;
        assert(vb->memory == V4L2_MEMORY_DMABUF);
        assert(vb->length == 1 && vb->m.planes[0].m.fd >= 0);
        if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            ms->in_queued[vb->index] = true;
            ms->in_ready[vb->index] = true;
            if (ms->trigger_sc) {
                ms->trigger_sc = false;
                ms->sc_pending = true;
            } else {
                for (uint32_t i = 0; i < ms->cap_req_cnt; i++) {
                    if (ms->out_queued[i]) {
                        ms->out_ready[i] = true;
                        break;
                    }
                }
            }
        } else {
            ms->out_queued[vb->index] = true;
        }
        return 0;
    }
    if (req == VIDIOC_DQBUF) {
        struct v4l2_buffer *vb = (struct v4l2_buffer *)arg;
        if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            for (uint32_t i = 0; i < ms->out_req_cnt; i++) {
                if (ms->in_ready[i]) {
                    ms->in_ready[i] = false;
                    ms->in_queued[i] = false;
                    vb->index = i;
                    return 0;
                }
            }
        } else {
            for (uint32_t i = 0; i < ms->cap_req_cnt; i++) {
                if (ms->out_ready[i]) {
                    ms->out_ready[i] = false;
                    ms->out_queued[i] = false;
                    vb->index = i;
                    vb->m.planes[0].bytesused = ms->eos_pending ? 0u : 1234u;
                    vb->flags = ms->eos_pending ? V4L2_BUF_FLAG_LAST : 0u;
                    return 0;
                }
            }
        }
        errno = EAGAIN;
        return -1;
    }
    if (req == VIDIOC_DQEVENT) {
        struct v4l2_event *ev = (struct v4l2_event *)arg;
        if (ms->sc_pending) {
            ms->sc_pending = false;
            ev->type = V4L2_EVENT_SOURCE_CHANGE;
            ev->u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
            return 0;
        }
        errno = ENOENT;
        return -1;
    }
    if (req == VIDIOC_DECODER_CMD) {
        struct v4l2_decoder_cmd *c = (struct v4l2_decoder_cmd *)arg;
        if (c->cmd == V4L2_DEC_CMD_STOP) {
            ms->eos_pending = true;
            for (uint32_t i = 0; i < ms->cap_req_cnt; i++) {
                if (ms->out_queued[i]) {
                    ms->out_ready[i] = true;
                    break;
                }
            }
        }
        return 0;
    }
    if (req == VIDIOC_ENCODER_CMD) {
        struct v4l2_encoder_cmd *c = (struct v4l2_encoder_cmd *)arg;
        if (c->cmd == V4L2_ENC_CMD_STOP) {
            ms->eos_pending = true;
            for (uint32_t i = 0; i < ms->cap_req_cnt; i++) {
                if (ms->out_queued[i]) {
                    ms->out_ready[i] = true;
                    break;
                }
            }
        }
        return 0;
    }
    return 0;
}

static int mock_poll(void *ud, struct pollfd *fds, nfds_t nfds, int timeout_ms)
{
    struct mock_state *ms = (struct mock_state *)ud;
    (void)timeout_ms;
    int cnt = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd == 100) {
            if (ms->sc_pending) {
                fds[i].revents |= POLLPRI;
                cnt++;
            }
            for (uint32_t j = 0; j < ms->cap_req_cnt; j++) {
                if (ms->out_ready[j]) {
                    fds[i].revents |= POLLIN;
                    cnt++;
                    break;
                }
            }
        }
    }
    return cnt;
}

int main(void)
{
    /* (a) SM7475 msm_media_info.h geometry check */
    assert(venus_y_stride(V4L2_PIX_FMT_NV12, 256) == 256);
    assert(venus_y_scanlines(V4L2_PIX_FMT_NV12, 256) == 256);
    assert(venus_raw_buffer_size(V4L2_PIX_FMT_NV12, 256, 256) == 98304);

    assert(venus_y_stride(V4L2_PIX_FMT_NV12, 1080) == 1152);
    assert(venus_y_scanlines(V4L2_PIX_FMT_NV12, 1920) == 1920);
    assert(venus_uv_scanlines(V4L2_PIX_FMT_NV12, 1920) == 960);
    assert(venus_raw_buffer_size(V4L2_PIX_FMT_NV12, 1080, 1920) ==
           venus_align_u32(1152u * 1920u + 1152u * 960u, 4096u));

    assert(venus_y_stride(V4L2_PIX_FMT_NV12, 176) == 256);
    assert(venus_y_scanlines(V4L2_PIX_FMT_NV12, 144) == 160);
    assert(venus_uv_scanlines(V4L2_PIX_FMT_NV12, 144) == 80);
    assert(venus_raw_buffer_size(V4L2_PIX_FMT_NV12, 176, 144) == 61440);

    assert(venus_y_stride(V4L2_PIX_FMT_NV12, 3840) == 3840);
    assert(venus_y_scanlines(V4L2_PIX_FMT_NV12, 2160) == 2176);

    /* (b) Linear NV12->NV21 repack with CropStore trailing canary protection */
    const uint32_t w = 300, h = 200;
    uint32_t src_stride = venus_y_stride(V4L2_PIX_FMT_NV12, w); /* 384 */
    uint32_t src_yl = venus_y_scanlines(V4L2_PIX_FMT_NV12, h); /* 224 */
    uint32_t src_sz = venus_raw_buffer_size(V4L2_PIX_FMT_NV12, w, h);
    uint8_t *src = calloc(1, src_sz);
    assert(src != NULL);
    for (uint32_t r = 0; r < h; r++)
        memset(src + r * src_stride, (int)(16 + (r & 0x3f)), w);
    uint8_t *src_uv = src + (size_t)src_stride * src_yl;
    for (uint32_t r = 0; r < h / 2; r++) {
        for (uint32_t c = 0; c < w; c += 2) {
            src_uv[r * src_stride + c] = 0x55;
            src_uv[r * src_stride + c + 1] = 0xaa;
        }
    }

    uint32_t dst_stride = 512; /* Marble 256-aligned stride for w=300 */
    size_t pix_bytes = (size_t)dst_stride * h + (size_t)dst_stride * (h / 2);
    size_t total_alloc = venus_align_u32((uint32_t)pix_bytes, 4096u) + 4096u;
    uint8_t *dst_full = malloc(total_alloc);
    assert(dst_full != NULL);
    memset(dst_full, 0x5A, total_alloc); /* sentinel including trailing CropStore page */
    size_t tail_off = venus_align_u32((uint32_t)pix_bytes, 4096u);
    int rc = venus_nv12_to_linear_dst(src, src_stride * src_yl, src_stride,
                                      dst_full, tail_off, dst_stride, h,
                                      0, 0, w, h, true);
    assert(rc == 0);
    assert(dst_full[0] == 16 && dst_full[(h - 1) * dst_stride + w - 1] == (uint8_t)(16 + ((h - 1) & 0x3f)));
    assert(dst_full[(size_t)dst_stride * h] == 0xaa);
    assert(dst_full[(size_t)dst_stride * h + 1] == 0x55);
    for (size_t i = tail_off; i < total_alloc; i++)
        assert(dst_full[i] == 0x5A);

    uint8_t *repacked = calloc(1, src_sz);
    assert(venus_linear_src_to_nv12(dst_full, tail_off, dst_stride, h,
                                    repacked, src_stride * src_yl, src_stride, src_sz,
                                    w, h, true) == 0);
    assert(repacked[src_stride * src_yl] == 0x55);
    assert(repacked[src_stride * src_yl + 1] == 0xaa);

    /* (b2) I420 planar decode/encode repack with CropStore trailing canary protection */
    memset(dst_full, 0x5A, total_alloc);
    assert(venus_nv12_to_i420_dst(src, src_stride * src_yl, src_stride,
                                  dst_full, tail_off, dst_stride, h,
                                  0, 0, w, h) == 0);
    size_t i420_u = (size_t)dst_stride * h;
    size_t i420_v = i420_u + (size_t)(dst_stride / 2u) * (h / 2u);
    assert(dst_full[i420_u] == 0x55 && dst_full[i420_u + (w / 2u) - 1u] == 0x55);
    assert(dst_full[i420_v] == 0xaa && dst_full[i420_v + (w / 2u) - 1u] == 0xaa);
    for (size_t i = tail_off; i < total_alloc; i++)
        assert(dst_full[i] == 0x5A);
    memset(repacked, 0, src_sz);
    assert(venus_i420_src_to_nv12(dst_full, tail_off, dst_stride, h,
                                  repacked, src_stride * src_yl, src_stride, src_sz,
                                  w, h) == 0);
    assert(repacked[src_stride * src_yl] == 0x55);
    assert(repacked[src_stride * src_yl + 1] == 0xaa);
    free(src);
    free(dst_full);
    free(repacked);

    /* (c) Mocked V4L2 decoder session with SOURCE_CHANGE + EOS */
    struct mock_state ms;
    memset(&ms, 0, sizeof(ms));
    ms.trigger_sc = true;
    ms.new_w = 1920;
    ms.new_h = 1080;

    struct venus_sys_ops ops = {
        .userdata = &ms,
        .open_fn = mock_open,
        .close_fn = mock_close,
        .ioctl_fn = mock_ioctl,
        .poll_fn = mock_poll,
        .mmap_fn = mock_mmap,
        .munmap_fn = mock_munmap,
        .dmabuf_alloc_fn = mock_dmabuf_alloc,
        .dmabuf_sync_fn = mock_dmabuf_sync,
    };

    struct venus_ctx ctx;
    assert(venus_ctx_open(&ctx, VENUS_ROLE_DECODER, V4L2_PIX_FMT_H264,
                          V4L2_PIX_FMT_NV12, 1280, 720, &ops) == 0);
    assert(venus_ctx_configure(&ctx, 1280, 720, 30, 0, 0, -1, -1, -1) == 0);
    assert(venus_ctx_start(&ctx) == 0);
    assert(ctx.streaming_in && !ctx.streaming_out);
    assert(venus_ctx_send_eos(&ctx) == -EAGAIN);

    int slot = venus_ctx_acquire_free_input(&ctx);
    assert(slot >= 0);
    assert(venus_ctx_queue_input(&ctx, (uint32_t)slot, 512, 1000, 0) == 0);

    uint32_t ev = 0;
    assert(venus_ctx_poll(&ctx, 100, &ev) == 0);
    assert(ev & VENUS_POLL_EVENT);
    struct v4l2_event v4ev;
    assert(venus_ctx_dq_event(&ctx, &v4ev) == 0);
    assert(v4ev.type == V4L2_EVENT_SOURCE_CHANGE);
    assert(venus_ctx_handle_source_change(&ctx) == 0);
    assert(ctx.streaming_out);
    assert(ctx.fmt.crop_width == 1920 && ctx.fmt.crop_height == 1080);

    slot = venus_ctx_acquire_free_input(&ctx);
    assert(slot >= 0);
    assert(venus_ctx_queue_input(&ctx, (uint32_t)slot, 256, 2000, 0) == 0);
    uint32_t out_idx = 0, bytes = 0, flags = 0;
    int64_t ts = 0;
    assert(venus_ctx_dq_output(&ctx, &out_idx, &bytes, &ts, &flags) == 0);
    assert(bytes == 1234 && !(flags & V4L2_BUF_FLAG_LAST));
    assert(venus_ctx_requeue_output(&ctx, out_idx) == 0);

    assert(venus_ctx_send_eos(&ctx) == 0);
    assert(venus_ctx_dq_output(&ctx, &out_idx, &bytes, &ts, &flags) == 0);
    assert(bytes == 0 && (flags & V4L2_BUF_FLAG_LAST));
    assert(venus_ctx_flush(&ctx, true, true) == 0);
    venus_ctx_close(&ctx);

    /* (c2) Start -> flush(true, true) -> close on decoder before SOURCE_CHANGE */
    memset(&ms, 0, sizeof(ms));
    ms.w = 1280; ms.h = 720;
    assert(venus_ctx_open(&ctx, VENUS_ROLE_DECODER, V4L2_PIX_FMT_HEVC,
                          V4L2_PIX_FMT_NV12, 1280, 720, &ops) == 0);
    assert(venus_ctx_configure(&ctx, 1280, 720, 30, 0, 0, -1, -1, -1) == 0);
    assert(venus_ctx_start(&ctx) == 0);
    assert(ctx.streaming_in && !ctx.streaming_out);
    assert(venus_ctx_flush(&ctx, true, true) == 0);
    assert(ctx.streaming_in && !ctx.streaming_out);
    venus_ctx_close(&ctx);

    /* (d) Mocked V4L2 encoder session */
    memset(&ms, 0, sizeof(ms));
    ms.w = 640;
    ms.h = 480;
    assert(venus_ctx_open(&ctx, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_HEVC,
                          V4L2_PIX_FMT_NV12, 640, 480, &ops) == 0);
    assert(venus_ctx_configure(&ctx, 640, 480, 30, 2000000, 30,
                               V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, -1, -1) == 0);
    assert(venus_ctx_start(&ctx) == 0);
    slot = venus_ctx_acquire_free_input(&ctx);
    assert(slot >= 0);
    assert(venus_ctx_queue_input(&ctx, (uint32_t)slot, ctx.fmt.in_buf_size, 33333, 0) == 0);
    assert(venus_ctx_dq_output(&ctx, &out_idx, &bytes, &ts, &flags) == 0);
    assert(bytes == 1234);
    assert(venus_ctx_send_eos(&ctx) == 0);
    venus_ctx_close(&ctx);

    printf("VENUS_V4L2_HOST_TEST_OK\n");
    return 0;
}
#endif
