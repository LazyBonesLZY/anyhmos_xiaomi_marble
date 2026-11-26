/*
 * Clean-room OpenMAX IL 1.1.2 Core + Qualcomm SM7475 Venus hardware codecs
 * (libOMX_Core.z.so -> OMX.marble.video.{decoder,encoder}.{avc,hevc}).
 * Copyright (C) 2026 Marble HMOS Bringup Project (clean-room MIT).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "omx_il_minimal.h"
#include "venus_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#define OMX_LOG_PATH "/data/local/tmp/marble-codec/omx-venus.log"
#define OMX_MAX_PORT_BUFS 24u
#define OMX_QUEUE_CAP 64u
#define OMX_CSD_CAP (64u * 1024u)

#define MARBLE_USAGE_DEFAULT 0x2Bull /* CPU_READ|CPU_WRITE|MEM_DMA|MEM_MMZ_CACHE */

struct comp_desc {
    const char *name;
    const char *role;
    enum venus_role v_role;
    uint32_t v4l2_coded_fcc;
    OMX_VIDEO_CODINGTYPE omx_coding;
};

static const struct comp_desc g_comp_table[] = {
    {
        .name = "OMX.marble.video.decoder.avc",
        .role = "video_decoder.avc",
        .v_role = VENUS_ROLE_DECODER,
        .v4l2_coded_fcc = V4L2_PIX_FMT_H264,
        .omx_coding = OMX_VIDEO_CodingAVC,
    },
    {
        .name = "OMX.marble.video.decoder.hevc",
        .role = "video_decoder.hevc",
        .v_role = VENUS_ROLE_DECODER,
        .v4l2_coded_fcc = V4L2_PIX_FMT_HEVC,
        .omx_coding = OMX_VIDEO_CodingHEVC,
    },
    {
        .name = "OMX.marble.video.encoder.avc",
        .role = "video_encoder.avc",
        .v_role = VENUS_ROLE_ENCODER,
        .v4l2_coded_fcc = V4L2_PIX_FMT_H264,
        .omx_coding = OMX_VIDEO_CodingAVC,
    },
    {
        .name = "OMX.marble.video.encoder.hevc",
        .role = "video_encoder.hevc",
        .v_role = VENUS_ROLE_ENCODER,
        .v4l2_coded_fcc = V4L2_PIX_FMT_HEVC,
        .omx_coding = OMX_VIDEO_CodingHEVC,
    },
};

#define NUM_COMPS (sizeof(g_comp_table) / sizeof(g_comp_table[0]))

static const struct venus_sys_ops *g_test_sys_ops = NULL;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_core_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_init_refs = 0;

static void omx_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void omx_log(const char *fmt, ...)
{
    pthread_mutex_lock(&g_log_mu);
    FILE *fp = fopen(OMX_LOG_PATH, "ae");
    if (fp) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(fp, fmt, ap);
        fputc('\n', fp);
        va_end(ap);
        fclose(fp);
    }
    pthread_mutex_unlock(&g_log_mu);
}

enum buf_alloc_kind {
    BUF_KIND_NONE = 0,
    BUF_KIND_HEAP = 1,
    BUF_KIND_DMABUF_OWNED = 2,
    BUF_KIND_DMABUF_IMPORTED = 3,
    BUF_KIND_HANDLE = 4,
    BUF_KIND_DYNAMIC_HANDLE = 5,
    BUF_KIND_USER_PTR = 6,
};

struct omx_buf_slot {
    OMX_BUFFERHEADERTYPE hdr;
    bool in_use;
    bool held_by_comp;
    uint32_t port_index;
    enum buf_alloc_kind kind;
    int dma_fd;
    void *mapped;
    uint32_t mapped_len;
};

struct hdr_queue {
    OMX_BUFFERHEADERTYPE *items[OMX_QUEUE_CAP];
    uint32_t head;
    uint32_t count;
};

static bool q_push(struct hdr_queue *q, OMX_BUFFERHEADERTYPE *h)
{
    if (q->count >= OMX_QUEUE_CAP)
        return false;
    uint32_t tail = (q->head + q->count) % OMX_QUEUE_CAP;
    q->items[tail] = h;
    q->count++;
    return true;
}

static OMX_BUFFERHEADERTYPE *q_pop(struct hdr_queue *q)
{
    if (q->count == 0)
        return NULL;
    OMX_BUFFERHEADERTYPE *h = q->items[q->head];
    q->head = (q->head + 1u) % OMX_QUEUE_CAP;
    q->count--;
    return h;
}

struct omx_port_state {
    OMX_PARAM_PORTDEFINITIONTYPE def;
    uint32_t ohos_buf_type;
    uint32_t ohos_pix_fmt;
    struct omx_buf_slot slots[OMX_MAX_PORT_BUFS];
    uint32_t alloc_count;
};

struct pending_cmd {
    bool active;
    OMX_COMMANDTYPE cmd;
    OMX_U32 param1;
};

struct venus_omx_comp {
    OMX_COMPONENTTYPE omx;
    const struct comp_desc *desc;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t worker;
    bool worker_started;
    bool worker_polling;
    bool exiting;

    OMX_STATETYPE state;
    OMX_STATETYPE target_state;
    bool state_transition_pending;
    bool port_disable_pending[2];
    bool port_enable_pending[2];

    OMX_CALLBACKTYPE cb;
    OMX_PTR app_data;

    struct omx_port_state ports[2];
    struct hdr_queue in_q;
    struct hdr_queue out_q;

    struct venus_ctx venus;
    bool venus_active;
    bool out_reconfig_pending;
    bool eos_event_emitted;
    bool pending_eos;
    bool logged_first_in;
    bool logged_first_out;

    /* Codec config/bitrate/frame-rate parameters */
    uint32_t bitrate;
    int32_t bitrate_mode;
    uint32_t i_period;
    int32_t profile;
    int32_t level;
    uint32_t qp_min;
    uint32_t qp_max;
    uint32_t quality;
    uint64_t vendor_usage;
    OMX_CONFIG_RECTTYPE crop_rect;
    OhosCodecVideoColorspace colorspace;

    /* CSD scratch buffer for decoders when CODECCONFIG arrives separately */
    uint8_t csd[OMX_CSD_CAP];
    uint32_t csd_len;
    bool csd_pending;
    bool csd_has_sent;
};

static uint32_t align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

static OMX_VERSIONTYPE omx_spec_ver(void)
{
    OMX_VERSIONTYPE v;
    v.s.nVersionMajor = 1;
    v.s.nVersionMinor = 1;
    v.s.nRevision = 2;
    v.s.nStep = 0;
    return v;
}

static uint32_t normalize_yuv420_pixfmt(uint32_t fmt)
{
    if (fmt == OHOS_PIXEL_FMT_YCBCR_420_SP ||
        fmt == OHOS_PIXEL_FMT_YCRCB_420_SP ||
        fmt == OHOS_PIXEL_FMT_YCBCR_420_P) {
        return fmt;
    }
    if (fmt == (uint32_t)OMX_COLOR_FormatYUV420SemiPlanar ||
        fmt == (uint32_t)OMX_COLOR_FormatYUV420PackedSemiPlanar) {
        return OHOS_PIXEL_FMT_YCBCR_420_SP;
    }
    if (fmt == (uint32_t)OMX_COLOR_FormatYUV420Planar ||
        fmt == (uint32_t)OMX_COLOR_FormatYUV420PackedPlanar) {
        return OHOS_PIXEL_FMT_YCBCR_420_P;
    }
    return 0u;
}

static void refresh_port_sizes(struct venus_omx_comp *c, uint32_t p)
{
    struct omx_port_state *ps = &c->ports[p];
    uint32_t w = ps->def.format.video.nFrameWidth ? ps->def.format.video.nFrameWidth : 1280u;
    uint32_t h = ps->def.format.video.nFrameHeight ? ps->def.format.video.nFrameHeight : 720u;
    bool raw_port = (c->desc->v_role == VENUS_ROLE_DECODER && p == 1) ||
                    (c->desc->v_role == VENUS_ROLE_ENCODER && p == 0);
    if (raw_port) {
        /* Matches libdisplay_buffer_vdi_impl.z.so (-DACE5_LINEAR_STRIDE_ALIGNMENT=128) and Venus 128-byte stride */
        uint32_t stride = align_up(w, 128u);
        ps->def.format.video.nStride = (OMX_S32)stride;
        ps->def.format.video.nSliceHeight = h;
        uint32_t sz = (ps->ohos_pix_fmt == OHOS_PIXEL_FMT_YCBCR_420_P)
                      ? (stride * h + 2u * (stride / 2u) * ((h + 1u) / 2u))
                      : (stride * h + stride * ((h + 1u) / 2u));
        if (sz < ps->def.nBufferSize && c->venus_active && p == 1 &&
            ps->ohos_buf_type != CODEC_BUFFER_TYPE_HANDLE &&
            ps->ohos_buf_type != CODEC_BUFFER_TYPE_DYNAMIC_HANDLE) {
            sz = c->venus.fmt.out_buf_size;
        }
        ps->def.nBufferSize = sz < 4096u ? 4096u : sz;
    } else {
        ps->def.format.video.nStride = (OMX_S32)align_up(w, 16u);
        ps->def.format.video.nSliceHeight = h;
        uint32_t sz = align_up(w, 64u) * align_up(h, 64u) * 3u / 2u;
        if (sz < 2u * 1024u * 1024u)
            sz = 2u * 1024u * 1024u;
        if (sz > 8u * 1024u * 1024u)
            sz = 8u * 1024u * 1024u;
        ps->def.nBufferSize = sz;
    }
}

static bool port_is_populated(const struct omx_port_state *ps)
{
    return ps->alloc_count >= ps->def.nBufferCountActual && ps->def.nBufferCountActual > 0;
}

static bool all_enabled_ports_populated(const struct venus_omx_comp *c)
{
    for (uint32_t i = 0; i < 2; i++) {
        if (c->ports[i].def.bEnabled && !c->ports[i].def.bPopulated)
            return false;
    }
    return true;
}

static bool all_buffers_freed(const struct venus_omx_comp *c)
{
    return c->ports[0].alloc_count == 0 && c->ports[1].alloc_count == 0;
}

static int alloc_sys_dmabuf(uint32_t size)
{
    if (g_test_sys_ops && g_test_sys_ops->dmabuf_alloc_fn)
        return g_test_sys_ops->dmabuf_alloc_fn(g_test_sys_ops->userdata, size);
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

static uint32_t raw_v4l2_pixfmt(const struct venus_omx_comp *c)
{
    uint32_t raw_port = (c->desc->v_role == VENUS_ROLE_DECODER) ? 1u : 0u;
    if (c->ports[raw_port].ohos_pix_fmt == OHOS_PIXEL_FMT_YCRCB_420_SP)
        return V4L2_PIX_FMT_NV21;
    return V4L2_PIX_FMT_NV12;
}

static int32_t map_v4l2_profile(const struct venus_omx_comp *c, uint32_t omx_prof)
{
    if (c->desc->v4l2_coded_fcc == V4L2_PIX_FMT_H264) {
        if (omx_prof == 1u) return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
        if (omx_prof == 2u) return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
        if (omx_prof == 8u) return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
        return -1;
    }
    if (omx_prof == 1u) return V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN;
    if (omx_prof == 2u) return V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10;
    return -1;
}

static int start_venus_session_locked(struct venus_omx_comp *c)
{
    if (c->venus_active)
        return 0;
    uint32_t w = c->ports[0].def.format.video.nFrameWidth;
    uint32_t h = c->ports[0].def.format.video.nFrameHeight;
    if (c->ports[1].def.format.video.nFrameWidth > 0) {
        if (c->desc->v_role == VENUS_ROLE_ENCODER) {
            w = c->ports[0].def.format.video.nFrameWidth;
            h = c->ports[0].def.format.video.nFrameHeight;
        }
    }
    uint32_t fps = c->ports[0].def.format.video.xFramerate >> 16;
    if (fps == 0)
        fps = c->ports[1].def.format.video.xFramerate >> 16;
    if (fps == 0)
        fps = 30u;

    int rc = venus_ctx_open(&c->venus, c->desc->v_role, c->desc->v4l2_coded_fcc,
                            raw_v4l2_pixfmt(c), w, h, g_test_sys_ops);
    if (rc < 0) {
        omx_log("venus_ctx_open fail comp=%s rc=%d", c->desc->name, rc);
        return rc;
    }
    rc = venus_ctx_configure(&c->venus, w, h, fps, c->bitrate, c->i_period,
                             c->bitrate_mode, map_v4l2_profile(c, (uint32_t)c->profile), -1);
    if (rc < 0) {
        omx_log("venus_ctx_configure fail comp=%s rc=%d", c->desc->name, rc);
        venus_ctx_close(&c->venus);
        return rc;
    }
    rc = venus_ctx_start(&c->venus);
    if (rc < 0) {
        omx_log("venus_ctx_start fail comp=%s rc=%d", c->desc->name, rc);
        venus_ctx_close(&c->venus);
        return rc;
    }
    c->venus_active = true;
    c->eos_event_emitted = false;
    c->pending_eos = false;
    c->logged_first_in = false;
    c->logged_first_out = false;
    omx_log("venus session started comp=%s %ux%u fps=%u pix=%u",
            c->desc->name, w, h, fps,
            c->ports[(c->desc->v_role == VENUS_ROLE_DECODER) ? 1u : 0u].ohos_pix_fmt);
    return 0;
}

static void stop_venus_session_locked(struct venus_omx_comp *c)
{
    if (!c->venus_active)
        return;
    while (c->worker_polling) {
        venus_ctx_wakeup(&c->venus);
        pthread_cond_wait(&c->cv, &c->mu);
    }
    venus_ctx_close(&c->venus);
    c->venus_active = false;
    c->out_reconfig_pending = false;
    c->pending_eos = false;
    c->csd_len = 0;
    c->csd_pending = false;
    c->csd_has_sent = false;
    omx_log("venus session stopped comp=%s", c->desc->name);
}

static struct omx_buf_slot *find_slot_by_hdr(struct venus_omx_comp *c,
                                             OMX_BUFFERHEADERTYPE *hdr,
                                             uint32_t *out_port)
{
    for (uint32_t p = 0; p < 2; p++) {
        for (uint32_t i = 0; i < OMX_MAX_PORT_BUFS; i++) {
            struct omx_buf_slot *s = &c->ports[p].slots[i];
            if (s->in_use && &s->hdr == hdr) {
                if (out_port)
                    *out_port = p;
                return s;
            }
        }
    }
    return NULL;
}

static void return_port_queues_locked(struct venus_omx_comp *c, uint32_t port,
                                      OMX_BUFFERHEADERTYPE *ret_bufs[],
                                      uint32_t *ret_cnt, bool *is_input_ret)
{
    struct hdr_queue *q = (port == 0) ? &c->in_q : &c->out_q;
    uint32_t n = 0;
    OMX_BUFFERHEADERTYPE *h;
    while ((h = q_pop(q)) != NULL) {
        struct omx_buf_slot *s = find_slot_by_hdr(c, h, NULL);
        if (s)
            s->held_by_comp = false;
        h->nFilledLen = 0;
        if (n < OMX_MAX_PORT_BUFS)
            ret_bufs[n++] = h;
    }
    *ret_cnt = n;
    *is_input_ret = (port == 0);
}

static void deliver_returned_bufs(struct venus_omx_comp *c,
                                  const OMX_CALLBACKTYPE *cb, OMX_PTR app_data,
                                  OMX_BUFFERHEADERTYPE *ret_bufs[],
                                  uint32_t ret_cnt, bool is_input)
{
    for (uint32_t i = 0; i < ret_cnt; i++) {
        if (is_input && cb->EmptyBufferDone)
            cb->EmptyBufferDone(&c->omx, app_data, ret_bufs[i]);
        else if (!is_input && cb->FillBufferDone)
            cb->FillBufferDone(&c->omx, app_data, ret_bufs[i]);
    }
}

static void check_pending_transitions_locked(struct venus_omx_comp *c,
                                             bool *emit_state_evt, OMX_STATETYPE *new_state,
                                             bool *emit_err_evt, OMX_ERRORTYPE *err_val,
                                             bool *emit_port_enable, uint32_t *pe_port)
{
    *emit_state_evt = false;
    *emit_err_evt = false;
    *emit_port_enable = false;

    for (uint32_t p = 0; p < 2; p++) {
        if (c->port_enable_pending[p] &&
            (c->state == OMX_StateLoaded || c->ports[p].def.bPopulated)) {
            c->port_enable_pending[p] = false;
            if (p == 1 && c->desc->v_role == VENUS_ROLE_DECODER)
                c->out_reconfig_pending = false;
            *emit_port_enable = true;
            *pe_port = p;
        }
    }

    if (!c->state_transition_pending)
        return;

    if (c->state == OMX_StateLoaded && c->target_state == OMX_StateIdle) {
        if (all_enabled_ports_populated(c)) {
            int rc = start_venus_session_locked(c);
            c->state_transition_pending = false;
            if (rc == 0) {
                c->state = OMX_StateIdle;
                *emit_state_evt = true;
                *new_state = OMX_StateIdle;
                venus_ctx_wakeup(&c->venus);
            } else {
                *emit_err_evt = true;
                *err_val = OMX_ErrorHardware;
            }
        }
    } else if ((c->state == OMX_StateIdle || c->state == OMX_StateExecuting || c->state == OMX_StatePause) &&
               c->target_state == OMX_StateLoaded) {
        stop_venus_session_locked(c);
        if (all_buffers_freed(c)) {
            c->state_transition_pending = false;
            c->state = OMX_StateLoaded;
            *emit_state_evt = true;
            *new_state = OMX_StateLoaded;
        }
    }
}

static bool resolve_raw_buf(struct omx_buf_slot *slot, uint32_t port_buf_type,
                            uint32_t default_stride, uint32_t default_h,
                            uint32_t default_pix_fmt,
                            uint8_t **out_ptr, size_t *out_max_bytes,
                            uint32_t *out_stride, uint32_t *out_lines,
                            uint32_t *out_pix_fmt, int *out_sync_fd, bool *out_unmap,
                            size_t *out_map_len)
{
    *out_ptr = NULL;
    *out_max_bytes = 0;
    *out_stride = default_stride;
    *out_lines = default_h;
    *out_pix_fmt = default_pix_fmt;
    *out_sync_fd = -1;
    *out_unmap = false;
    *out_map_len = 0;

    if (port_buf_type == CODEC_BUFFER_TYPE_HANDLE ||
        port_buf_type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE) {
        struct MarbleBufferHandle *bh = NULL;
        if (port_buf_type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE ||
            slot->kind == BUF_KIND_DYNAMIC_HANDLE) {
            struct MarbleDynamicBuffer *db = (struct MarbleDynamicBuffer *)slot->hdr.pBuffer;
            bh = db ? db->bufferHandle : NULL;
        } else {
            bh = (struct MarbleBufferHandle *)slot->hdr.pBuffer;
        }
        if (!bh || bh->stride <= 0 || bh->height <= 0 || bh->size <= 0)
            return false;
        uint32_t st = (uint32_t)bh->stride;
        uint32_t ht = (uint32_t)bh->height;
        uint32_t norm_fmt = normalize_yuv420_pixfmt((uint32_t)bh->format);
        uint32_t fmt = norm_fmt ? norm_fmt : default_pix_fmt;
        uint32_t pix_bytes = (fmt == OHOS_PIXEL_FMT_YCBCR_420_P)
                             ? (st * ht + 2u * (st / 2u) * ((ht + 1u) / 2u))
                             : (st * ht + st * ((ht + 1u) / 2u));
        uint32_t sz = (pix_bytes <= (uint32_t)bh->size) ? pix_bytes : (uint32_t)bh->size;
        void *va = NULL;
        if (bh->fd >= 0) {
            size_t mlen = (size_t)bh->size;
            void *p = mmap(NULL, mlen, PROT_READ | PROT_WRITE, MAP_SHARED, bh->fd, 0);
            if (p != MAP_FAILED) {
                va = p;
                *out_unmap = true;
                *out_map_len = mlen;
                *out_sync_fd = bh->fd;
            }
        }
        if (!va && bh->virAddr && bh->virAddr != MAP_FAILED)
            va = bh->virAddr;
        if (!va)
            return false;
        *out_ptr = (uint8_t *)va;
        *out_max_bytes = sz;
        *out_stride = st;
        *out_lines = ht;
        *out_pix_fmt = fmt;
        return true;
    }

    if (slot->hdr.pBuffer && slot->hdr.nAllocLen > 0) {
        *out_ptr = slot->hdr.pBuffer;
        *out_max_bytes = slot->hdr.nAllocLen;
        *out_stride = default_stride;
        *out_lines = default_h;
        *out_pix_fmt = default_pix_fmt;
        *out_sync_fd = (slot->kind == BUF_KIND_DMABUF_OWNED ||
                        slot->kind == BUF_KIND_DMABUF_IMPORTED) ? slot->dma_fd : -1;
        return true;
    }
    return false;
}

static void release_raw_buf(struct venus_ctx *vctx, uint8_t *va, bool do_unmap,
                            size_t map_len, int sync_fd, uint64_t sync_dir)
{
    if (sync_fd >= 0 && vctx)
        (void)venus_dmabuf_sync(vctx, sync_fd, DMA_BUF_SYNC_END | sync_dir);
    if (do_unmap && va && map_len > 0)
        munmap(va, map_len);
}

static void *omx_worker_main(void *arg)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)arg;

    for (;;) {
        pthread_mutex_lock(&c->mu);
        while (!c->exiting &&
               (c->state != OMX_StateExecuting || !c->venus_active)) {
            pthread_cond_wait(&c->cv, &c->mu);
        }
        if (c->exiting) {
            pthread_mutex_unlock(&c->mu);
            break;
        }

        /* 1. Try submitting one pending input buffer if Venus has a free input slot */
        bool did_work = false;
        OMX_BUFFERHEADERTYPE *ebd_hdr = NULL;
        bool emit_in_err = false;
        OMX_ERRORTYPE in_err_code = OMX_ErrorNone;
        if (c->in_q.count > 0 && c->ports[0].def.bEnabled) {
            OMX_BUFFERHEADERTYPE *head_hdr = c->in_q.items[c->in_q.head];
            struct omx_buf_slot *in_slot = find_slot_by_hdr(c, head_hdr, NULL);

            if ((head_hdr->nFlags & OMX_BUFFERFLAG_CODECCONFIG) &&
                c->desc->v_role == VENUS_ROLE_DECODER &&
                head_hdr->nFilledLen > 0 && head_hdr->pBuffer != NULL) {
                if (c->csd_has_sent) {
                    c->csd_len = 0;
                    c->csd_has_sent = false;
                }
                if (head_hdr->nFilledLen <= OMX_CSD_CAP &&
                    c->csd_len <= OMX_CSD_CAP - head_hdr->nFilledLen) {
                    memcpy(c->csd + c->csd_len, head_hdr->pBuffer + head_hdr->nOffset,
                           head_hdr->nFilledLen);
                    c->csd_len += head_hdr->nFilledLen;
                    c->csd_pending = true;
                } else {
                    omx_log("csd overflow err=-ENOSPC csd=%u add=%u cap=%u comp=%s",
                            c->csd_len, head_hdr->nFilledLen, OMX_CSD_CAP, c->desc->name);
                    emit_in_err = true;
                    in_err_code = OMX_ErrorOverflow;
                }
                q_pop(&c->in_q);
                if (in_slot)
                    in_slot->held_by_comp = false;
                ebd_hdr = head_hdr;
                did_work = true;
            } else if ((head_hdr->nFlags & OMX_BUFFERFLAG_EOS) &&
                       (head_hdr->nFilledLen == 0 ||
                        (c->desc->v_role == VENUS_ROLE_ENCODER &&
                         c->ports[0].ohos_buf_type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE))) {
                /*
                 * CodecDynaBuffer::EmptyThisBuffer unconditionally sets filledLen = sizeof(DynamicBuffer)
                 * whenever a SurfaceBuffer handle is attached, even when HEncoder sends an empty EOS
                 * (attr.size == 0, flags = EOS). Treat DYNAMIC_HANDLE encoder EOS as EOS-only so Venus
                 * does not encode a phantom stale frame before flushing.
                 */
                if (venus_ctx_send_eos(&c->venus) < 0)
                    c->pending_eos = true;
                q_pop(&c->in_q);
                if (in_slot)
                    in_slot->held_by_comp = false;
                ebd_hdr = head_hdr;
                did_work = true;
            } else {
                int vslot = venus_ctx_acquire_free_input(&c->venus);
                if (vslot >= 0) {
                    q_pop(&c->in_q);
                    if (in_slot)
                        in_slot->held_by_comp = false;

                    struct venus_buf *vb = &c->venus.in_bufs[vslot];
                    uint32_t bytesused = 0;
                    uint32_t csd_bytes = 0;
                    int pack_rc = 0;
                    (void)venus_dmabuf_sync(&c->venus, vb->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);

                    if (c->desc->v_role == VENUS_ROLE_DECODER) {
                        uint32_t pay = head_hdr->nFilledLen;
                        csd_bytes = (c->csd_pending && c->csd_len > 0) ? c->csd_len : 0u;
                        if (!head_hdr->pBuffer) {
                            pack_rc = -EINVAL;
                        } else if (pay > vb->size || csd_bytes > vb->size - pay) {
                            pack_rc = -ENOSPC;
                        } else {
                            if (csd_bytes > 0u) {
                                memcpy(vb->map, c->csd, csd_bytes);
                                bytesused += csd_bytes;
                                c->csd_pending = false;
                                c->csd_has_sent = true;
                            }
                            if (pay > 0u) {
                                memcpy((uint8_t *)vb->map + bytesused,
                                       head_hdr->pBuffer + head_hdr->nOffset, pay);
                                bytesused += pay;
                            }
                        }
                    } else {
                        uint8_t *raw_va = NULL;
                        size_t raw_max = 0, map_len = 0;
                        uint32_t rs = c->ports[0].def.format.video.nStride;
                        uint32_t rh = c->ports[0].def.format.video.nSliceHeight;
                        uint32_t in_fmt = c->ports[0].ohos_pix_fmt;
                        bool do_unmap = false;
                        int sync_fd = -1;
                        if (in_slot && resolve_raw_buf(in_slot, c->ports[0].ohos_buf_type,
                                                       rs, rh, c->ports[0].ohos_pix_fmt,
                                                       &raw_va, &raw_max, &rs, &rh,
                                                       &in_fmt, &sync_fd, &do_unmap, &map_len)) {
                            if (sync_fd >= 0)
                                (void)venus_dmabuf_sync(&c->venus, sync_fd,
                                                        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
                            if (in_fmt == OHOS_PIXEL_FMT_YCBCR_420_P) {
                                pack_rc = venus_i420_src_to_nv12(
                                    raw_va, raw_max, rs, rh,
                                    (uint8_t *)vb->map, c->venus.fmt.uv_offset,
                                    c->venus.fmt.y_stride, vb->size,
                                    c->venus.fmt.crop_width, c->venus.fmt.crop_height);
                            } else {
                                pack_rc = venus_linear_src_to_nv12(
                                    raw_va, raw_max, rs, rh,
                                    (uint8_t *)vb->map, c->venus.fmt.uv_offset,
                                    c->venus.fmt.y_stride, vb->size,
                                    c->venus.fmt.crop_width, c->venus.fmt.crop_height,
                                    in_fmt == OHOS_PIXEL_FMT_YCRCB_420_SP);
                            }
                            bytesused = c->venus.fmt.in_buf_size;
                            release_raw_buf(&c->venus, raw_va, do_unmap, map_len, sync_fd,
                                            DMA_BUF_SYNC_READ);
                        } else {
                            pack_rc = -EINVAL;
                        }
                    }
                    (void)venus_dmabuf_sync(&c->venus, vb->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

                    if (pack_rc == 0 && bytesused > 0) {
                        int qrc = venus_ctx_queue_input(&c->venus, (uint32_t)vslot, bytesused,
                                                        head_hdr->nTimeStamp, 0u);
                        if (qrc < 0) {
                            omx_log("venus_ctx_queue_input err=%d comp=%s", qrc, c->desc->name);
                            emit_in_err = true;
                            in_err_code = OMX_ErrorHardware;
                        } else if (!c->logged_first_in) {
                            c->logged_first_in = true;
                            omx_log("first input queued comp=%s bytes=%u", c->desc->name, bytesused);
                        }
                    } else if (pack_rc != 0) {
                        omx_log("input pack err=%d csd=%u pay=%u vb_size=%u comp=%s",
                                pack_rc, csd_bytes, head_hdr->nFilledLen, vb->size, c->desc->name);
                        emit_in_err = true;
                        in_err_code = (pack_rc == -ENOSPC) ? OMX_ErrorOverflow : OMX_ErrorBadParameter;
                    } else {
                        omx_log("input empty non-eos buffer skipped comp=%s flags=0x%x",
                                c->desc->name, head_hdr->nFlags);
                    }
                    if (head_hdr->nFlags & OMX_BUFFERFLAG_EOS) {
                        if (venus_ctx_send_eos(&c->venus) < 0)
                            c->pending_eos = true;
                    }
                    ebd_hdr = head_hdr;
                    did_work = true;
                }
            }
        }
        if (c->pending_eos && c->venus.streaming_out && c->in_q.count == 0) {
            if (venus_ctx_send_eos(&c->venus) == 0) {
                c->pending_eos = false;
                omx_log("deferred EOS sent comp=%s", c->desc->name);
            }
        }

        OMX_CALLBACKTYPE cb = c->cb;
        OMX_PTR app_data = c->app_data;
        pthread_mutex_unlock(&c->mu);

        if (ebd_hdr && cb.EmptyBufferDone)
            cb.EmptyBufferDone(&c->omx, app_data, ebd_hdr);
        if (emit_in_err && cb.EventHandler)
            cb.EventHandler(&c->omx, app_data, OMX_EventError, (OMX_U32)in_err_code, 0u, NULL);

        /* 2. Drain any completed Venus input slots & V4L2 events + output frames */
        uint32_t dummy_idx = 0;
        pthread_mutex_lock(&c->mu);
        while (c->venus_active && venus_ctx_dq_input(&c->venus, &dummy_idx) == 0)
            did_work = true;

        bool emit_sc = false;
        if (c->venus_active) {
            struct v4l2_event ev;
            while (venus_ctx_dq_event(&c->venus, &ev) == 0) {
                did_work = true;
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    uint32_t old_w = c->ports[1].def.format.video.nFrameWidth;
                    uint32_t old_h = c->ports[1].def.format.video.nFrameHeight;
                    int sc_rc = venus_ctx_handle_source_change(&c->venus);
                    omx_log("SOURCE_CHANGE rc=%d new=%ux%u old=%ux%u min_buf=%u",
                            sc_rc, c->venus.fmt.crop_width, c->venus.fmt.crop_height,
                            old_w, old_h, c->venus.fmt.out_buf_count);
                    if (sc_rc == 0) {
                        uint32_t nw = c->venus.fmt.crop_width;
                        uint32_t nh = c->venus.fmt.crop_height;
                        c->crop_rect.nLeft = (OMX_S32)c->venus.fmt.crop_left;
                        c->crop_rect.nTop = (OMX_S32)c->venus.fmt.crop_top;
                        c->crop_rect.nWidth = nw;
                        c->crop_rect.nHeight = nh;
                        if (nw != old_w || nh != old_h ||
                            c->venus.fmt.out_buf_count > c->ports[1].def.nBufferCountActual) {
                            c->ports[1].def.format.video.nFrameWidth = nw;
                            c->ports[1].def.format.video.nFrameHeight = nh;
                            if (c->venus.fmt.out_buf_count > c->ports[1].def.nBufferCountMin)
                                c->ports[1].def.nBufferCountMin = c->venus.fmt.out_buf_count;
                            if (c->venus.fmt.out_buf_count > c->ports[1].def.nBufferCountActual)
                                c->ports[1].def.nBufferCountActual = c->venus.fmt.out_buf_count;
                            refresh_port_sizes(c, 1u);
                            c->out_reconfig_pending = true;
                            emit_sc = true;
                        }
                        if (c->pending_eos && venus_ctx_send_eos(&c->venus) == 0) {
                            c->pending_eos = false;
                            omx_log("deferred EOS sent after SOURCE_CHANGE comp=%s", c->desc->name);
                        }
                    }
                }
            }
        }
        cb = c->cb;
        app_data = c->app_data;
        pthread_mutex_unlock(&c->mu);

        if (emit_sc && cb.EventHandler)
            cb.EventHandler(&c->omx, app_data, OMX_EventPortSettingsChanged, 1u, 0u, NULL);

        /* 3. Try matching one Venus CAPTURE output buffer to one queued OMX output buffer */
        for (;;) {
            pthread_mutex_lock(&c->mu);
            if (c->exiting || c->state != OMX_StateExecuting || !c->venus_active ||
                c->out_reconfig_pending || !c->ports[1].def.bEnabled || c->out_q.count == 0) {
                pthread_mutex_unlock(&c->mu);
                break;
            }
            uint32_t v_idx = 0, v_bytes = 0, v_flags = 0;
            int64_t v_ts = 0;
            int dq = venus_ctx_dq_output(&c->venus, &v_idx, &v_bytes, &v_ts, &v_flags);
            if (dq < 0) {
                pthread_mutex_unlock(&c->mu);
                break;
            }
            did_work = true;
            OMX_BUFFERHEADERTYPE *out_hdr = q_pop(&c->out_q);
            struct omx_buf_slot *out_slot = find_slot_by_hdr(c, out_hdr, NULL);
            if (out_slot)
                out_slot->held_by_comp = false;

            bool is_eos = (v_bytes == 0u) || ((v_flags & V4L2_BUF_FLAG_LAST) != 0u);
            out_hdr->nOffset = 0u;
            out_hdr->nTimeStamp = v_ts;
            out_hdr->nFlags = 0u;
            if (is_eos)
                out_hdr->nFlags |= OMX_BUFFERFLAG_EOS;

            if (v_bytes > 0u) {
                struct venus_buf *vb = &c->venus.out_bufs[v_idx];
                (void)venus_dmabuf_sync(&c->venus, vb->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);

                if (c->desc->v_role == VENUS_ROLE_DECODER) {
                    uint8_t *dst_va = NULL;
                    size_t dst_max = 0, map_len = 0;
                    uint32_t ds = (uint32_t)c->ports[1].def.format.video.nStride;
                    uint32_t dl = c->ports[1].def.format.video.nSliceHeight;
                    uint32_t out_fmt = c->ports[1].ohos_pix_fmt;
                    bool do_unmap = false;
                    int sync_fd = -1;
                    if (out_slot && resolve_raw_buf(out_slot, c->ports[1].ohos_buf_type,
                                                    ds, dl, c->ports[1].ohos_pix_fmt,
                                                    &dst_va, &dst_max, &ds, &dl,
                                                    &out_fmt, &sync_fd, &do_unmap, &map_len)) {
                        if (sync_fd >= 0)
                            (void)venus_dmabuf_sync(&c->venus, sync_fd,
                                                    DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
                        int copy_rc;
                        if (out_fmt == OHOS_PIXEL_FMT_YCBCR_420_P) {
                            copy_rc = venus_nv12_to_i420_dst(
                                (const uint8_t *)vb->map, c->venus.fmt.uv_offset,
                                c->venus.fmt.y_stride, dst_va, dst_max, ds, dl,
                                c->venus.fmt.crop_left, c->venus.fmt.crop_top,
                                c->venus.fmt.crop_width, c->venus.fmt.crop_height);
                            if (copy_rc == 0)
                                out_hdr->nFilledLen = ds * dl + 2u * (ds / 2u) * ((dl + 1u) / 2u);
                            else
                                out_hdr->nFilledLen = 0u;
                        } else {
                            copy_rc = venus_nv12_to_linear_dst(
                                (const uint8_t *)vb->map, c->venus.fmt.uv_offset,
                                c->venus.fmt.y_stride, dst_va, dst_max, ds, dl,
                                c->venus.fmt.crop_left, c->venus.fmt.crop_top,
                                c->venus.fmt.crop_width, c->venus.fmt.crop_height,
                                out_fmt == OHOS_PIXEL_FMT_YCRCB_420_SP);
                            if (copy_rc == 0)
                                out_hdr->nFilledLen = ds * dl + ds * ((dl + 1u) / 2u);
                            else
                                out_hdr->nFilledLen = 0u;
                        }
                        if (!c->logged_first_out) {
                            c->logged_first_out = true;
                            omx_log("first output delivered comp=%s pix_fmt=%u stride=%u lines=%u filled=%u copy_rc=%d",
                                    c->desc->name, out_fmt, ds, dl, out_hdr->nFilledLen, copy_rc);
                        }
                        release_raw_buf(&c->venus, dst_va, do_unmap, map_len, sync_fd,
                                        DMA_BUF_SYNC_WRITE);
                    } else {
                        out_hdr->nFilledLen = 0u;
                        omx_log("resolve_raw_buf failed comp=%s buf_type=%u",
                                c->desc->name, c->ports[1].ohos_buf_type);
                    }
                } else {
                    if (out_hdr->pBuffer && v_bytes <= out_hdr->nAllocLen) {
                        memcpy(out_hdr->pBuffer, vb->map, v_bytes);
                        out_hdr->nFilledLen = v_bytes;
                        if (v_flags & V4L2_BUF_FLAG_KEYFRAME)
                            out_hdr->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;
                    } else {
                        out_hdr->nFilledLen = 0u;
                    }
                }

                (void)venus_dmabuf_sync(&c->venus, vb->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            } else {
                out_hdr->nFilledLen = 0u;
            }

            (void)venus_ctx_requeue_output(&c->venus, v_idx);
            bool fire_eos_evt = is_eos && !c->eos_event_emitted;
            if (fire_eos_evt) {
                c->eos_event_emitted = true;
                omx_log("output EOS delivered comp=%s", c->desc->name);
            }
            cb = c->cb;
            app_data = c->app_data;
            pthread_mutex_unlock(&c->mu);

            if (cb.FillBufferDone)
                cb.FillBufferDone(&c->omx, app_data, out_hdr);
            if (fire_eos_evt && cb.EventHandler)
                cb.EventHandler(&c->omx, app_data, OMX_EventBufferFlag, 1u, OMX_BUFFERFLAG_EOS, NULL);
        }

        if (!did_work) {
            pthread_mutex_lock(&c->mu);
            bool can_poll = (!c->exiting && c->state == OMX_StateExecuting && c->venus_active);
            if (can_poll)
                c->worker_polling = true;
            pthread_mutex_unlock(&c->mu);
            if (can_poll) {
                uint32_t ev_flags = 0;
                (void)venus_ctx_poll(&c->venus, 20, &ev_flags);
                pthread_mutex_lock(&c->mu);
                c->worker_polling = false;
                pthread_cond_broadcast(&c->cv);
                pthread_mutex_unlock(&c->mu);
            }
        }
    }
    return NULL;
}

static OMX_ERRORTYPE comp_GetComponentVersion(OMX_HANDLETYPE hComp, OMX_STRING name,
                                              OMX_VERSIONTYPE *compVer,
                                              OMX_VERSIONTYPE *specVer,
                                              OMX_UUIDTYPE *uuid)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c)
        return OMX_ErrorInvalidComponent;
    if (name) {
        strncpy(name, c->desc->name, 31);
        name[31] = '\0';
    }
    if (compVer) *compVer = omx_spec_ver();
    if (specVer) *specVer = omx_spec_ver();
    if (uuid) memset(uuid, 0, sizeof(*uuid));
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE comp_SendCommand(OMX_HANDLETYPE hComp, OMX_COMMANDTYPE cmd,
                                      OMX_U32 param1, OMX_PTR cmdData)
{
    (void)cmdData;
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c)
        return OMX_ErrorInvalidComponent;

    OMX_BUFFERHEADERTYPE *ret_in[OMX_MAX_PORT_BUFS];
    OMX_BUFFERHEADERTYPE *ret_out[OMX_MAX_PORT_BUFS];
    uint32_t n_in = 0, n_out = 0;
    bool dummy = false;
    bool emit_cmd_done = false;
    OMX_U32 done_data1 = (OMX_U32)cmd;
    OMX_U32 done_data2 = param1;
    bool emit_err = false;
    OMX_ERRORTYPE err_code = OMX_ErrorNone;

    pthread_mutex_lock(&c->mu);
    OMX_CALLBACKTYPE cb = c->cb;
    OMX_PTR app_data = c->app_data;

    switch (cmd) {
    case OMX_CommandStateSet: {
        OMX_STATETYPE target = (OMX_STATETYPE)param1;
        omx_log("SendCommand StateSet %d -> %d (%s)", c->state, target, c->desc->name);
        if (c->state == target) {
            emit_cmd_done = true;
            break;
        }
        if (c->state == OMX_StateLoaded && target == OMX_StateIdle) {
            c->target_state = target;
            c->state_transition_pending = true;
            if (all_enabled_ports_populated(c)) {
                int rc = start_venus_session_locked(c);
                c->state_transition_pending = false;
                if (rc == 0) {
                    c->state = OMX_StateIdle;
                    emit_cmd_done = true;
                } else {
                    emit_err = true;
                    err_code = OMX_ErrorHardware;
                }
            }
        } else if (c->state == OMX_StateIdle && target == OMX_StateExecuting) {
            c->state = OMX_StateExecuting;
            emit_cmd_done = true;
            pthread_cond_broadcast(&c->cv);
            if (c->venus_active)
                venus_ctx_wakeup(&c->venus);
        } else if (c->state == OMX_StateExecuting && (target == OMX_StateIdle || target == OMX_StatePause)) {
            c->state = target;
            if (target == OMX_StateIdle) {
                return_port_queues_locked(c, 0u, ret_in, &n_in, &dummy);
                return_port_queues_locked(c, 1u, ret_out, &n_out, &dummy);
                if (c->csd_len > 0)
                    c->csd_pending = true;
                if (c->venus_active)
                    (void)venus_ctx_flush(&c->venus, true, true);
            }
            emit_cmd_done = true;
        } else if (c->state == OMX_StatePause && target == OMX_StateExecuting) {
            c->state = OMX_StateExecuting;
            emit_cmd_done = true;
            pthread_cond_broadcast(&c->cv);
        } else if ((c->state == OMX_StateIdle || c->state == OMX_StatePause ||
                    c->state == OMX_StateExecuting) && target == OMX_StateLoaded) {
            return_port_queues_locked(c, 0u, ret_in, &n_in, &dummy);
            return_port_queues_locked(c, 1u, ret_out, &n_out, &dummy);
            stop_venus_session_locked(c);
            c->csd_len = 0;
            c->csd_pending = false;
            c->csd_has_sent = false;
            c->target_state = OMX_StateLoaded;
            c->state_transition_pending = true;
            if (all_buffers_freed(c)) {
                c->state_transition_pending = false;
                c->state = OMX_StateLoaded;
                emit_cmd_done = true;
            }
        } else {
            pthread_mutex_unlock(&c->mu);
            return OMX_ErrorIncorrectStateTransition;
        }
        break;
    }
    case OMX_CommandFlush: {
        bool fi = (param1 == 0u || param1 == 0xFFFFFFFFu);
        bool fo = (param1 == 1u || param1 == 0xFFFFFFFFu);
        if (!fi && !fo) {
            pthread_mutex_unlock(&c->mu);
            return OMX_ErrorBadPortIndex;
        }
        if (fi)
            return_port_queues_locked(c, 0u, ret_in, &n_in, &dummy);
        if (fo)
            return_port_queues_locked(c, 1u, ret_out, &n_out, &dummy);
        /* Preserve cached SPS/PPS CSD across flush (seek); re-prepend on next post-flush frame. */
        if (fi && c->csd_len > 0)
            c->csd_pending = true;
        c->eos_event_emitted = false;
        c->pending_eos = false;
        if (c->venus_active)
            (void)venus_ctx_flush(&c->venus, fi, fo);
        pthread_mutex_unlock(&c->mu);
        deliver_returned_bufs(c, &cb, app_data, ret_in, n_in, true);
        deliver_returned_bufs(c, &cb, app_data, ret_out, n_out, false);
        if (cb.EventHandler) {
            if (fi) cb.EventHandler(&c->omx, app_data, OMX_EventCmdComplete, OMX_CommandFlush, 0u, NULL);
            if (fo) cb.EventHandler(&c->omx, app_data, OMX_EventCmdComplete, OMX_CommandFlush, 1u, NULL);
        }
        return OMX_ErrorNone;
    }
    case OMX_CommandPortDisable: {
        if (param1 > 1u) {
            pthread_mutex_unlock(&c->mu);
            return OMX_ErrorBadPortIndex;
        }
        c->ports[param1].def.bEnabled = OMX_FALSE;
        if (param1 == 0u)
            return_port_queues_locked(c, 0u, ret_in, &n_in, &dummy);
        else
            return_port_queues_locked(c, 1u, ret_out, &n_out, &dummy);
        if (c->ports[param1].alloc_count == 0u) {
            c->port_disable_pending[param1] = false;
            emit_cmd_done = true;
        } else {
            c->port_disable_pending[param1] = true;
        }
        break;
    }
    case OMX_CommandPortEnable: {
        if (param1 > 1u) {
            pthread_mutex_unlock(&c->mu);
            return OMX_ErrorBadPortIndex;
        }
        c->ports[param1].def.bEnabled = OMX_TRUE;
        if (c->state == OMX_StateLoaded || c->ports[param1].def.bPopulated) {
            c->port_enable_pending[param1] = false;
            if (param1 == 1u && c->desc->v_role == VENUS_ROLE_DECODER)
                c->out_reconfig_pending = false;
            emit_cmd_done = true;
        } else {
            c->port_enable_pending[param1] = true;
        }
        break;
    }
    default:
        pthread_mutex_unlock(&c->mu);
        return OMX_ErrorNotImplemented;
    }

    pthread_mutex_unlock(&c->mu);
    deliver_returned_bufs(c, &cb, app_data, ret_in, n_in, true);
    deliver_returned_bufs(c, &cb, app_data, ret_out, n_out, false);
    if (emit_cmd_done && cb.EventHandler)
        cb.EventHandler(&c->omx, app_data, OMX_EventCmdComplete, done_data1, done_data2, NULL);
    else if (emit_err && cb.EventHandler)
        cb.EventHandler(&c->omx, app_data, OMX_EventError, (OMX_U32)err_code, 0u, NULL);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE comp_GetParameter(OMX_HANDLETYPE hComp, OMX_INDEXTYPE idx, OMX_PTR ptr)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !ptr)
        return OMX_ErrorBadParameter;

    pthread_mutex_lock(&c->mu);
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    switch ((uint32_t)idx) {
    case OMX_IndexParamVideoInit: {
        OMX_PORT_PARAM_TYPE *p = (OMX_PORT_PARAM_TYPE *)ptr;
        p->nPorts = 2u;
        p->nStartPortNumber = 0u;
        break;
    }
    case OMX_IndexParamAudioInit:
    case OMX_IndexParamImageInit:
    case OMX_IndexParamOtherInit: {
        OMX_PORT_PARAM_TYPE *p = (OMX_PORT_PARAM_TYPE *)ptr;
        p->nPorts = 0u;
        p->nStartPortNumber = 0u;
        break;
    }
    case OMX_IndexParamStandardComponentRole: {
        OMX_PARAM_COMPONENTROLETYPE *r = (OMX_PARAM_COMPONENTROLETYPE *)ptr;
        memset(r->cRole, 0, sizeof(r->cRole));
        strncpy((char *)r->cRole, c->desc->role, sizeof(r->cRole) - 1);
        break;
    }
    case OMX_IndexParamPortDefinition: {
        OMX_PARAM_PORTDEFINITIONTYPE *pd = (OMX_PARAM_PORTDEFINITIONTYPE *)ptr;
        if (pd->nPortIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        refresh_port_sizes(c, pd->nPortIndex);
        *pd = c->ports[pd->nPortIndex].def;
        break;
    }
    case OMX_IndexParamVideoPortFormat: {
        OMX_VIDEO_PARAM_PORTFORMATTYPE *vf = (OMX_VIDEO_PARAM_PORTFORMATTYPE *)ptr;
        if (vf->nPortIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        bool coded_port = (c->desc->v_role == VENUS_ROLE_DECODER && vf->nPortIndex == 0u) ||
                          (c->desc->v_role == VENUS_ROLE_ENCODER && vf->nPortIndex == 1u);
        if (coded_port) {
            if (vf->nIndex > 0u) {
                ret = OMX_ErrorNoMore;
                break;
            }
            vf->eCompressionFormat = c->desc->omx_coding;
            vf->eColorFormat = OMX_COLOR_FormatUnused;
        } else {
            static const uint32_t raw_fmts[5] = {
                OHOS_PIXEL_FMT_YCBCR_420_SP,
                OHOS_PIXEL_FMT_YCRCB_420_SP,
                OHOS_PIXEL_FMT_YCBCR_420_P,
                (uint32_t)OMX_COLOR_FormatYUV420SemiPlanar,
                (uint32_t)OMX_COLOR_FormatYUV420Planar,
            };
            if (vf->nIndex >= 5u) {
                ret = OMX_ErrorNoMore;
                break;
            }
            vf->eCompressionFormat = OMX_VIDEO_CodingUnused;
            vf->eColorFormat = (OMX_COLOR_FORMATTYPE)raw_fmts[vf->nIndex];
        }
        vf->xFramerate = c->ports[vf->nPortIndex].def.format.video.xFramerate;
        break;
    }
    case Codec_IndexCodecVideoPortFormat: {
        OhosCodecVideoPortFormatParam *p = (OhosCodecVideoPortFormatParam *)ptr;
        if (p->portIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        bool raw_port = (c->desc->v_role == VENUS_ROLE_DECODER && p->portIndex == 1u) ||
                        (c->desc->v_role == VENUS_ROLE_ENCODER && p->portIndex == 0u);
        if (!raw_port) {
            if (p->codecColorIndex > 0u) {
                ret = OMX_ErrorNoMore;
                break;
            }
            p->codecCompressFormat = c->desc->omx_coding;
            p->codecColorFormat = 0u;
        } else {
            if (p->codecColorIndex == 0u)
                p->codecColorFormat = OHOS_PIXEL_FMT_YCBCR_420_SP;
            else if (p->codecColorIndex == 1u)
                p->codecColorFormat = OHOS_PIXEL_FMT_YCRCB_420_SP;
            else if (p->codecColorIndex == 2u)
                p->codecColorFormat = OHOS_PIXEL_FMT_YCBCR_420_P;
            else
                ret = OMX_ErrorNoMore;
            p->codecCompressFormat = 0u;
        }
        p->framerate = c->ports[p->portIndex].def.format.video.xFramerate;
        break;
    }
    case Codec_IndexParamSupportBufferType: {
        OhosSupportBufferType *sb = (OhosSupportBufferType *)ptr;
        if (sb->portIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        bool raw_port = (c->desc->v_role == VENUS_ROLE_DECODER && sb->portIndex == 1u) ||
                        (c->desc->v_role == VENUS_ROLE_ENCODER && sb->portIndex == 0u);
        if (!raw_port) {
            /* Bitstream ports: ashmem ONLY (no DMA_MEM_FD or CreateHardwareAllocator fails) */
            sb->bufferTypes = CODEC_BUFFER_TYPE_AVSHARE_MEM_FD;
        } else if (c->desc->v_role == VENUS_ROLE_DECODER) {
            /* Decoder output: static & dynamic BufferHandle for Buffer and Surface modes */
            sb->bufferTypes = CODEC_BUFFER_TYPE_HANDLE |
                              CODEC_BUFFER_TYPE_DYNAMIC_HANDLE;
        } else {
            sb->bufferTypes = CODEC_BUFFER_TYPE_HANDLE |
                              CODEC_BUFFER_TYPE_DYNAMIC_HANDLE |
                              CODEC_BUFFER_TYPE_AVSHARE_MEM_FD;
        }
        break;
    }
    case Codec_IndexParamUseBufferType: {
        OhosUseBufferType *ub = (OhosUseBufferType *)ptr;
        if (ub->portIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        ub->bufferType = c->ports[ub->portIndex].ohos_buf_type;
        break;
    }
    case Codec_IndexParamGetBufferHandleUsage: {
        OhosGetBufferHandleUsageParams *ug = (OhosGetBufferHandleUsageParams *)ptr;
        if (ug->portIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        ug->usage = MARBLE_USAGE_DEFAULT;
        break;
    }
    case OMX_IndexParamVideoBitrate: {
        OMX_VIDEO_PARAM_BITRATETYPE *br = (OMX_VIDEO_PARAM_BITRATETYPE *)ptr;
        br->eControlRate = (c->bitrate_mode == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR)
                           ? OMX_Video_ControlRateConstant
                           : OMX_Video_ControlRateVariable;
        br->nTargetBitrate = c->bitrate;
        break;
    }
    case OMX_IndexParamVideoAvc: {
        OMX_VIDEO_PARAM_AVCTYPE *avc = (OMX_VIDEO_PARAM_AVCTYPE *)ptr;
        avc->eProfile = c->profile > 0 ? (OMX_U32)c->profile : 1u;
        avc->eLevel = c->level > 0 ? (OMX_U32)c->level : 1u;
        avc->nPFrames = c->i_period > 0 ? (c->i_period - 1u) : 29u;
        avc->nBFrames = 0u;
        break;
    }
    case Codec_IndexParamVideoHevc: {
        OhosCodecVideoParamHevc *hv = (OhosCodecVideoParamHevc *)ptr;
        hv->profile = c->profile > 0 ? (uint32_t)c->profile : 1u;
        hv->level = c->level > 0 ? (uint32_t)c->level : 1u;
        hv->keyFrameInterval = c->i_period;
        break;
    }
    case OMX_IndexParamVideoProfileLevelQuerySupported:
    case OMX_IndexParamVideoProfileLevelCurrent: {
        OMX_VIDEO_PARAM_PROFILELEVELTYPE *pl = (OMX_VIDEO_PARAM_PROFILELEVELTYPE *)ptr;
        pl->eProfile = c->profile > 0 ? (OMX_U32)c->profile : 1u;
        pl->eLevel = c->level > 0 ? (OMX_U32)c->level : 0x100u;
        break;
    }
    case Codec_IndexColorAspects: {
        OhosCodecVideoColorspace *cs = (OhosCodecVideoColorspace *)ptr;
        *cs = c->colorspace;
        break;
    }
    case Codec_IndexParamControlRateConstantQuality: {
        OhosControlRateConstantQuality *cq = (OhosControlRateConstantQuality *)ptr;
        cq->qualityValue = c->quality;
        break;
    }
    case OMX_IndexParamVideoQuantization: {
        OMX_VIDEO_PARAM_QUANTIZATIONTYPE *qp = (OMX_VIDEO_PARAM_QUANTIZATIONTYPE *)ptr;
        qp->nQpI = c->qp_min;
        qp->nQpP = c->qp_max;
        qp->nQpB = c->qp_max;
        break;
    }
    default:
        ret = OMX_ErrorUnsupportedIndex;
        break;
    }
    pthread_mutex_unlock(&c->mu);
    return ret;
}

static OMX_ERRORTYPE comp_SetParameter(OMX_HANDLETYPE hComp, OMX_INDEXTYPE idx, OMX_PTR ptr)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !ptr)
        return OMX_ErrorBadParameter;

    pthread_mutex_lock(&c->mu);
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    switch ((uint32_t)idx) {
    case OMX_IndexParamStandardComponentRole:
        break;
    case OMX_IndexParamPortDefinition: {
        const OMX_PARAM_PORTDEFINITIONTYPE *pd = (const OMX_PARAM_PORTDEFINITIONTYPE *)ptr;
        if (pd->nPortIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        struct omx_port_state *ps = &c->ports[pd->nPortIndex];
        if (pd->format.video.nFrameWidth > 0u)
            ps->def.format.video.nFrameWidth = pd->format.video.nFrameWidth;
        if (pd->format.video.nFrameHeight > 0u)
            ps->def.format.video.nFrameHeight = pd->format.video.nFrameHeight;
        if (pd->format.video.xFramerate > 0u)
            ps->def.format.video.xFramerate = pd->format.video.xFramerate;
        if (pd->format.video.nBitrate > 0u) {
            ps->def.format.video.nBitrate = pd->format.video.nBitrate;
            c->bitrate = pd->format.video.nBitrate;
        }
        if (pd->nBufferCountActual >= ps->def.nBufferCountMin &&
            pd->nBufferCountActual <= OMX_MAX_PORT_BUFS)
            ps->def.nBufferCountActual = pd->nBufferCountActual;
        bool raw_port = (c->desc->v_role == VENUS_ROLE_DECODER && pd->nPortIndex == 1u) ||
                        (c->desc->v_role == VENUS_ROLE_ENCODER && pd->nPortIndex == 0u);
        uint32_t norm_pd_fmt = normalize_yuv420_pixfmt((uint32_t)pd->format.video.eColorFormat);
        if (raw_port && norm_pd_fmt != 0u) {
            ps->ohos_pix_fmt = norm_pd_fmt;
            ps->def.format.video.eColorFormat = (OMX_COLOR_FORMATTYPE)norm_pd_fmt;
        }
        if (pd->nPortIndex == 0u &&
            (c->ports[1].def.format.video.nFrameWidth != ps->def.format.video.nFrameWidth ||
             c->ports[1].def.format.video.nFrameHeight != ps->def.format.video.nFrameHeight)) {
            c->ports[1].def.format.video.nFrameWidth = ps->def.format.video.nFrameWidth;
            c->ports[1].def.format.video.nFrameHeight = ps->def.format.video.nFrameHeight;
            refresh_port_sizes(c, 1u);
        }
        if (pd->nPortIndex == 0u || pd->nPortIndex == 1u) {
            c->crop_rect.nWidth = ps->def.format.video.nFrameWidth;
            c->crop_rect.nHeight = ps->def.format.video.nFrameHeight;
        }
        refresh_port_sizes(c, pd->nPortIndex);
        if (pd->nBufferSize > ps->def.nBufferSize)
            ps->def.nBufferSize = pd->nBufferSize;
        break;
    }
    case OMX_IndexParamVideoPortFormat: {
        const OMX_VIDEO_PARAM_PORTFORMATTYPE *vf = (const OMX_VIDEO_PARAM_PORTFORMATTYPE *)ptr;
        if (vf->nPortIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        uint32_t norm_vf_fmt = normalize_yuv420_pixfmt((uint32_t)vf->eColorFormat);
        if (norm_vf_fmt != 0u) {
            c->ports[vf->nPortIndex].ohos_pix_fmt = norm_vf_fmt;
            c->ports[vf->nPortIndex].def.format.video.eColorFormat = (OMX_COLOR_FORMATTYPE)norm_vf_fmt;
            refresh_port_sizes(c, vf->nPortIndex);
        }
        if (vf->xFramerate > 0u)
            c->ports[vf->nPortIndex].def.format.video.xFramerate = vf->xFramerate;
        break;
    }
    case Codec_IndexCodecVideoPortFormat: {
        const OhosCodecVideoPortFormatParam *p = (const OhosCodecVideoPortFormatParam *)ptr;
        if (p->portIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        uint32_t norm_cv_fmt = normalize_yuv420_pixfmt(p->codecColorFormat);
        if (norm_cv_fmt != 0u) {
            c->ports[p->portIndex].ohos_pix_fmt = norm_cv_fmt;
            c->ports[p->portIndex].def.format.video.eColorFormat =
                (OMX_COLOR_FORMATTYPE)norm_cv_fmt;
            refresh_port_sizes(c, p->portIndex);
        }
        if (p->framerate > 0u)
            c->ports[p->portIndex].def.format.video.xFramerate = p->framerate;
        break;
    }
    case Codec_IndexParamUseBufferType: {
        const OhosUseBufferType *ub = (const OhosUseBufferType *)ptr;
        if (ub->portIndex > 1u) {
            ret = OMX_ErrorBadPortIndex;
            break;
        }
        c->ports[ub->portIndex].ohos_buf_type = ub->bufferType;
        refresh_port_sizes(c, ub->portIndex);
        break;
    }
    case OMX_IndexParamVideoBitrate: {
        const OMX_VIDEO_PARAM_BITRATETYPE *br = (const OMX_VIDEO_PARAM_BITRATETYPE *)ptr;
        if (br->nTargetBitrate > 0u)
            c->bitrate = br->nTargetBitrate;
        c->bitrate_mode = (br->eControlRate == OMX_Video_ControlRateConstant)
                          ? V4L2_MPEG_VIDEO_BITRATE_MODE_CBR
                          : V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
        break;
    }
    case OMX_IndexParamVideoAvc: {
        const OMX_VIDEO_PARAM_AVCTYPE *avc = (const OMX_VIDEO_PARAM_AVCTYPE *)ptr;
        c->profile = (int32_t)avc->eProfile;
        c->level = (int32_t)avc->eLevel;
        c->i_period = avc->nPFrames + 1u;
        break;
    }
    case Codec_IndexParamVideoHevc: {
        const OhosCodecVideoParamHevc *hv = (const OhosCodecVideoParamHevc *)ptr;
        c->profile = (int32_t)hv->profile;
        c->level = (int32_t)hv->level;
        if (hv->keyFrameInterval > 0u)
            c->i_period = hv->keyFrameInterval;
        break;
    }
    case Codec_IndexColorAspects: {
        c->colorspace = *(const OhosCodecVideoColorspace *)ptr;
        break;
    }
    case Codec_IndexParamControlRateConstantQuality: {
        c->quality = ((const OhosControlRateConstantQuality *)ptr)->qualityValue;
        break;
    }
    case OMX_IndexParamVideoQuantization: {
        const OMX_VIDEO_PARAM_QUANTIZATIONTYPE *qp = (const OMX_VIDEO_PARAM_QUANTIZATIONTYPE *)ptr;
        c->qp_min = qp->nQpI;
        c->qp_max = qp->nQpP;
        break;
    }
    case Codec_IndexParamSetBufferHandleUsage: {
        const OhosGetBufferHandleUsageParams *ug = (const OhosGetBufferHandleUsageParams *)ptr;
        c->vendor_usage = ug->usage;
        break;
    }
    case Codec_IndexParamWorkingFrequency:
    case Codec_IndexParamProcessName:
    case Codec_IndexParamLowLatency:
    case Codec_IndexParamVrrOrMvUpload:
    case Codec_IndexParamFreezeBuffers:
    case Codec_IndexParamFreqControl:
    case Codec_IndexParamLowPowerMode:
    case Codec_IndexParamLppTargetPts:
    case Codec_IndexParamCodecScenario:
    case Codec_IndexParamMasteringDisplay:
    case Codec_IndexParamContentLightLevel:
    case Codec_IndexParamIdrAppendXps:
    case OMX_IndexParamCompBufferSupplier:
        break;
    default:
        /* Log unsupported param index so live diagnostics show any missing index immediately */
        omx_log("SetParameter unsupported idx=0x%x comp=%s", (uint32_t)idx, c->desc->name);
        ret = OMX_ErrorUnsupportedIndex;
        break;
    }
    pthread_mutex_unlock(&c->mu);
    return ret;
}

static OMX_ERRORTYPE comp_GetConfig(OMX_HANDLETYPE hComp, OMX_INDEXTYPE idx, OMX_PTR ptr)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !ptr)
        return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->mu);
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    switch ((uint32_t)idx) {
    case OMX_IndexConfigCommonOutputCrop:
    case OMX_IndexConfigCommonInputCrop: {
        OMX_CONFIG_RECTTYPE *r = (OMX_CONFIG_RECTTYPE *)ptr;
        r->nLeft = c->crop_rect.nLeft;
        r->nTop = c->crop_rect.nTop;
        r->nWidth = c->crop_rect.nWidth ? c->crop_rect.nWidth : c->ports[1].def.format.video.nFrameWidth;
        r->nHeight = c->crop_rect.nHeight ? c->crop_rect.nHeight : c->ports[1].def.format.video.nFrameHeight;
        break;
    }
    default:
        ret = OMX_ErrorUnsupportedIndex;
        break;
    }
    pthread_mutex_unlock(&c->mu);
    return ret;
}

static OMX_ERRORTYPE comp_SetConfig(OMX_HANDLETYPE hComp, OMX_INDEXTYPE idx, OMX_PTR ptr)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !ptr)
        return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->mu);
    OMX_ERRORTYPE ret = OMX_ErrorNone;
    switch ((uint32_t)idx) {
    case OMX_IndexConfigVideoBitrate: {
        const OMX_VIDEO_CONFIG_BITRATETYPE *br = (const OMX_VIDEO_CONFIG_BITRATETYPE *)ptr;
        if (br->nEncodeBitrate > 0u) {
            c->bitrate = br->nEncodeBitrate;
            if (c->venus_active && c->desc->v_role == VENUS_ROLE_ENCODER)
                (void)venus_ctx_set_bitrate(&c->venus, c->bitrate);
        }
        break;
    }
    case OMX_IndexConfigVideoFramerate: {
        const OMX_CONFIG_FRAMERATETYPE *fr = (const OMX_CONFIG_FRAMERATETYPE *)ptr;
        if (fr->nPortIndex <= 1u && fr->xEncodeFramerate > 0u)
            c->ports[fr->nPortIndex].def.format.video.xFramerate = fr->xEncodeFramerate;
        break;
    }
    case OMX_IndexConfigVideoIntraVOPRefresh: {
        const OMX_CONFIG_INTRAREFRESHVOPTYPE *iv = (const OMX_CONFIG_INTRAREFRESHVOPTYPE *)ptr;
        if (iv->IntraRefreshVOP && c->venus_active && c->desc->v_role == VENUS_ROLE_ENCODER)
            (void)venus_ctx_force_keyframe(&c->venus);
        break;
    }
    case OMX_IndexConfigVideoIntraMBRefresh:
    case OMX_IndexConfigCommonOutputCrop:
    case OMX_IndexConfigCommonInputCrop:
        break;
    default:
        ret = OMX_ErrorUnsupportedIndex;
        break;
    }
    pthread_mutex_unlock(&c->mu);
    return ret;
}

static OMX_ERRORTYPE comp_GetExtensionIndex(OMX_HANDLETYPE hComp, OMX_STRING name,
                                            OMX_INDEXTYPE *idx)
{
    (void)hComp;
    if (!name || !idx)
        return OMX_ErrorBadParameter;
    if (strcmp(name, "OMX.OHOS.index.supportBufferType") == 0 ||
        strcmp(name, "OMX.google.android.index.storeMetaDataInBuffers") == 0)
        *idx = Codec_IndexParamSupportBufferType;
    else if (strcmp(name, "OMX.OHOS.index.useBufferType") == 0)
        *idx = Codec_IndexParamUseBufferType;
    else if (strcmp(name, "OMX.OHOS.index.getBufferHandleUsage") == 0)
        *idx = Codec_IndexParamGetBufferHandleUsage;
    else if (strcmp(name, "OMX.OHOS.index.videoPortFormat") == 0)
        *idx = Codec_IndexCodecVideoPortFormat;
    else if (strcmp(name, "OMX.OHOS.index.colorAspects") == 0 ||
             strcmp(name, "OMX.google.android.index.describeColorAspects") == 0)
        *idx = Codec_IndexColorAspects;
    else
        return OMX_ErrorUnsupportedIndex;
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE comp_GetState(OMX_HANDLETYPE hComp, OMX_STATETYPE *pState)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !pState)
        return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->mu);
    *pState = c->state;
    pthread_mutex_unlock(&c->mu);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE register_port_buffer(struct venus_omx_comp *c,
                                          OMX_BUFFERHEADERTYPE **ppHdr,
                                          OMX_U32 port, OMX_PTR appPriv,
                                          OMX_U32 sizeBytes, OMX_U8 *pBuffer,
                                          bool is_allocate)
{
    if (!c || !ppHdr || port > 1u || sizeBytes == 0u)
        return OMX_ErrorBadParameter;

    pthread_mutex_lock(&c->mu);
    struct omx_port_state *ps = &c->ports[port];
    struct omx_buf_slot *slot = NULL;
    for (uint32_t i = 0; i < OMX_MAX_PORT_BUFS; i++) {
        if (!ps->slots[i].in_use) {
            slot = &ps->slots[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&c->mu);
        return OMX_ErrorInsufficientResources;
    }

    memset(slot, 0, sizeof(*slot));
    slot->dma_fd = -1;
    slot->port_index = port;
    slot->hdr.nSize = sizeof(OMX_BUFFERHEADERTYPE);
    slot->hdr.nVersion = omx_spec_ver();
    slot->hdr.nAllocLen = sizeBytes;
    slot->hdr.pAppPrivate = appPriv;
    if (port == 0u)
        slot->hdr.nInputPortIndex = 0u;
    else
        slot->hdr.nOutputPortIndex = 1u;

    if (is_allocate) {
        if (ps->ohos_buf_type == CODEC_BUFFER_TYPE_DMA_MEM_FD) {
            int fd = alloc_sys_dmabuf(sizeBytes);
            if (fd < 0) {
                pthread_mutex_unlock(&c->mu);
                return OMX_ErrorInsufficientResources;
            }
            void *va = mmap(NULL, sizeBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (va == MAP_FAILED) {
                close(fd);
                pthread_mutex_unlock(&c->mu);
                return OMX_ErrorInsufficientResources;
            }
            slot->kind = BUF_KIND_DMABUF_OWNED;
            slot->dma_fd = fd;
            slot->mapped = va;
            slot->mapped_len = sizeBytes;
            slot->hdr.pBuffer = (OMX_U8 *)va;
            if (appPriv)
                ((struct MarbleOmxAppPriv *)appPriv)->fd = fd;
        } else {
            void *mem = calloc(1u, sizeBytes);
            if (!mem) {
                pthread_mutex_unlock(&c->mu);
                return OMX_ErrorInsufficientResources;
            }
            slot->kind = BUF_KIND_HEAP;
            slot->hdr.pBuffer = (OMX_U8 *)mem;
        }
    } else {
        if (ps->ohos_buf_type == CODEC_BUFFER_TYPE_DMA_MEM_FD) {
            int raw_fd = appPriv ? ((struct MarbleOmxAppPriv *)appPriv)->fd
                                 : (pBuffer ? *(int *)pBuffer : -1);
            int dup_fd = (raw_fd >= 0) ? fcntl(raw_fd, F_DUPFD_CLOEXEC, 0) : -1;
            if (dup_fd < 0) {
                pthread_mutex_unlock(&c->mu);
                return OMX_ErrorBadParameter;
            }
            void *va = mmap(NULL, sizeBytes, PROT_READ | PROT_WRITE, MAP_SHARED, dup_fd, 0);
            if (va == MAP_FAILED) {
                close(dup_fd);
                pthread_mutex_unlock(&c->mu);
                return OMX_ErrorInsufficientResources;
            }
            slot->kind = BUF_KIND_DMABUF_IMPORTED;
            slot->dma_fd = dup_fd;
            slot->mapped = va;
            slot->mapped_len = sizeBytes;
            slot->hdr.pBuffer = (OMX_U8 *)va;
        } else if (ps->ohos_buf_type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE) {
            slot->kind = BUF_KIND_DYNAMIC_HANDLE;
            slot->hdr.pBuffer = pBuffer;
        } else if (ps->ohos_buf_type == CODEC_BUFFER_TYPE_HANDLE) {
            slot->kind = BUF_KIND_HANDLE;
            slot->hdr.pBuffer = pBuffer;
        } else {
            slot->kind = BUF_KIND_USER_PTR;
            slot->hdr.pBuffer = pBuffer;
        }
    }

    slot->in_use = true;
    ps->alloc_count++;
    if (port_is_populated(ps)) {
        ps->def.bPopulated = OMX_TRUE;
        omx_log("port populated comp=%s port=%u cnt=%u kind=%u buf_type=0x%x size=%u",
                c->desc->name, port, ps->alloc_count, (uint32_t)slot->kind,
                ps->ohos_buf_type, sizeBytes);
    }
    *ppHdr = &slot->hdr;

    bool emit_st = false, emit_err = false, emit_pe = false;
    OMX_STATETYPE new_st = c->state;
    OMX_ERRORTYPE err_v = OMX_ErrorNone;
    uint32_t pe_port = 0;
    check_pending_transitions_locked(c, &emit_st, &new_st, &emit_err, &err_v,
                                     &emit_pe, &pe_port);
    OMX_CALLBACKTYPE cb = c->cb;
    OMX_PTR app_data = c->app_data;
    pthread_mutex_unlock(&c->mu);

    if (emit_pe && cb.EventHandler)
        cb.EventHandler(&c->omx, app_data, OMX_EventCmdComplete, OMX_CommandPortEnable, pe_port, NULL);
    if (emit_st && cb.EventHandler)
        cb.EventHandler(&c->omx, app_data, OMX_EventCmdComplete, OMX_CommandStateSet, (OMX_U32)new_st, NULL);
    else if (emit_err && cb.EventHandler)
        cb.EventHandler(&c->omx, app_data, OMX_EventError, (OMX_U32)err_v, 0u, NULL);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE comp_UseBuffer(OMX_HANDLETYPE hComp, OMX_BUFFERHEADERTYPE **ppHdr,
                                    OMX_U32 port, OMX_PTR appPriv,
                                    OMX_U32 sizeBytes, OMX_U8 *pBuffer)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    return register_port_buffer(c, ppHdr, port, appPriv, sizeBytes, pBuffer, false);
}

static OMX_ERRORTYPE comp_AllocateBuffer(OMX_HANDLETYPE hComp, OMX_BUFFERHEADERTYPE **ppHdr,
                                         OMX_U32 port, OMX_PTR appPriv, OMX_U32 sizeBytes)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    return register_port_buffer(c, ppHdr, port, appPriv, sizeBytes, NULL, true);
}

static OMX_ERRORTYPE comp_FreeBuffer(OMX_HANDLETYPE hComp, OMX_U32 port, OMX_BUFFERHEADERTYPE *hdr)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || port > 1u || !hdr)
        return OMX_ErrorBadParameter;

    pthread_mutex_lock(&c->mu);
    struct omx_port_state *ps = &c->ports[port];
    struct omx_buf_slot *slot = NULL;
    for (uint32_t i = 0; i < OMX_MAX_PORT_BUFS; i++) {
        if (ps->slots[i].in_use && &ps->slots[i].hdr == hdr) {
            slot = &ps->slots[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&c->mu);
        return OMX_ErrorBadParameter;
    }

    if (slot->kind == BUF_KIND_HEAP) {
        free(slot->hdr.pBuffer);
    } else if (slot->kind == BUF_KIND_DMABUF_OWNED || slot->kind == BUF_KIND_DMABUF_IMPORTED) {
        if (slot->mapped && slot->mapped != MAP_FAILED && slot->mapped_len > 0)
            munmap(slot->mapped, slot->mapped_len);
        if (slot->dma_fd >= 0)
            close(slot->dma_fd);
    }
    memset(slot, 0, sizeof(*slot));
    slot->dma_fd = -1;
    if (ps->alloc_count > 0)
        ps->alloc_count--;
    ps->def.bPopulated = port_is_populated(ps) ? OMX_TRUE : OMX_FALSE;

    bool emit_pd = false;
    if (c->port_disable_pending[port] && ps->alloc_count == 0) {
        c->port_disable_pending[port] = false;
        emit_pd = true;
    }
    bool emit_st = false, emit_err = false, emit_pe = false;
    OMX_STATETYPE new_st = c->state;
    OMX_ERRORTYPE err_v = OMX_ErrorNone;
    uint32_t pe_port = 0;
    check_pending_transitions_locked(c, &emit_st, &new_st, &emit_err, &err_v,
                                     &emit_pe, &pe_port);
    OMX_CALLBACKTYPE cb = c->cb;
    OMX_PTR app_data = c->app_data;
    pthread_mutex_unlock(&c->mu);

    if (emit_pd && cb.EventHandler)
        cb.EventHandler(&c->omx, app_data, OMX_EventCmdComplete, OMX_CommandPortDisable, port, NULL);
    if (emit_st && cb.EventHandler)
        cb.EventHandler(&c->omx, app_data, OMX_EventCmdComplete, OMX_CommandStateSet, (OMX_U32)new_st, NULL);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE comp_EmptyThisBuffer(OMX_HANDLETYPE hComp, OMX_BUFFERHEADERTYPE *hdr)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !hdr)
        return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->mu);
    struct omx_buf_slot *s = find_slot_by_hdr(c, hdr, NULL);
    if (!s || s->port_index != 0u || !c->ports[0].def.bEnabled) {
        pthread_mutex_unlock(&c->mu);
        return OMX_ErrorIncorrectStateOperation;
    }
    s->held_by_comp = true;
    bool ok = q_push(&c->in_q, hdr);
    if (c->venus_active)
        venus_ctx_wakeup(&c->venus);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return ok ? OMX_ErrorNone : OMX_ErrorOverflow;
}

static OMX_ERRORTYPE comp_FillThisBuffer(OMX_HANDLETYPE hComp, OMX_BUFFERHEADERTYPE *hdr)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !hdr)
        return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->mu);
    struct omx_buf_slot *s = find_slot_by_hdr(c, hdr, NULL);
    if (!s || s->port_index != 1u || !c->ports[1].def.bEnabled) {
        pthread_mutex_unlock(&c->mu);
        return OMX_ErrorIncorrectStateOperation;
    }
    if (c->ports[1].ohos_buf_type == CODEC_BUFFER_TYPE_DYNAMIC_HANDLE ||
        s->kind == BUF_KIND_DYNAMIC_HANDLE) {
        /*
         * CodecDynaBuffer::FillThisBuffer refreshes MarbleDynamicBuffer::bufferHandle per call.
         * Inspect the dynamic handle on every output buffer submission; resolve_raw_buf() reads
         * it dynamically again on delivery without caching stale fd/virAddr mappings.
         */
        struct MarbleDynamicBuffer *db = (struct MarbleDynamicBuffer *)hdr->pBuffer;
        struct MarbleBufferHandle *bh = db ? db->bufferHandle : NULL;
        if (!bh) {
            omx_log("FillThisBuffer dynamic handle null comp=%s", c->desc->name);
        }
    }
    s->held_by_comp = true;
    bool ok = q_push(&c->out_q, hdr);
    if (c->venus_active)
        venus_ctx_wakeup(&c->venus);
    pthread_cond_broadcast(&c->cv);
    pthread_mutex_unlock(&c->mu);
    return ok ? OMX_ErrorNone : OMX_ErrorOverflow;
}

static OMX_ERRORTYPE comp_SetCallbacks(OMX_HANDLETYPE hComp, OMX_CALLBACKTYPE *cb, OMX_PTR appData)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !cb)
        return OMX_ErrorBadParameter;
    pthread_mutex_lock(&c->mu);
    c->cb = *cb;
    c->app_data = appData;
    c->omx.pApplicationPrivate = appData;
    pthread_mutex_unlock(&c->mu);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE comp_ComponentRoleEnum(OMX_HANDLETYPE hComp, OMX_U8 *cRole, OMX_U32 nIndex)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c || !cRole)
        return OMX_ErrorBadParameter;
    if (nIndex > 0u)
        return OMX_ErrorNoMore;
    strncpy((char *)cRole, c->desc->role, OMX_MAX_STRINGNAME_SIZE - 1);
    cRole[OMX_MAX_STRINGNAME_SIZE - 1] = '\0';
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE comp_ComponentTunnelRequest(OMX_HANDLETYPE h, OMX_U32 p, OMX_HANDLETYPE tc,
                                                 OMX_U32 tp, OMX_PTR setup)
{
    (void)h; (void)p; (void)tc; (void)tp; (void)setup;
    return OMX_ErrorNotImplemented;
}

static OMX_ERRORTYPE comp_UseEGLImage(OMX_HANDLETYPE h, OMX_BUFFERHEADERTYPE **hdr, OMX_U32 p,
                                      OMX_PTR ap, void *egl)
{
    (void)h; (void)hdr; (void)p; (void)ap; (void)egl;
    return OMX_ErrorNotImplemented;
}

static OMX_ERRORTYPE comp_ComponentDeInit(OMX_HANDLETYPE hComp)
{
    struct venus_omx_comp *c = (struct venus_omx_comp *)((OMX_COMPONENTTYPE *)hComp)->pComponentPrivate;
    if (!c)
        return OMX_ErrorInvalidComponent;
    pthread_mutex_lock(&c->mu);
    c->exiting = true;
    if (c->venus_active)
        venus_ctx_wakeup(&c->venus);
    pthread_cond_broadcast(&c->cv);
    bool joined = c->worker_started;
    pthread_mutex_unlock(&c->mu);

    if (joined) {
        pthread_join(c->worker, NULL);
        c->worker_started = false;
    }

    pthread_mutex_lock(&c->mu);
    stop_venus_session_locked(c);
    for (uint32_t p = 0; p < 2; p++) {
        for (uint32_t i = 0; i < OMX_MAX_PORT_BUFS; i++) {
            struct omx_buf_slot *slot = &c->ports[p].slots[i];
            if (!slot->in_use)
                continue;
            if (slot->kind == BUF_KIND_HEAP)
                free(slot->hdr.pBuffer);
            else if (slot->kind == BUF_KIND_DMABUF_OWNED || slot->kind == BUF_KIND_DMABUF_IMPORTED) {
                if (slot->mapped && slot->mapped != MAP_FAILED)
                    munmap(slot->mapped, slot->mapped_len);
                if (slot->dma_fd >= 0)
                    close(slot->dma_fd);
            }
            slot->in_use = false;
        }
        c->ports[p].alloc_count = 0;
    }
    pthread_mutex_unlock(&c->mu);
    return OMX_ErrorNone;
}

static void init_port_defaults(struct venus_omx_comp *c, uint32_t p)
{
    struct omx_port_state *ps = &c->ports[p];
    memset(ps, 0, sizeof(*ps));
    for (uint32_t i = 0; i < OMX_MAX_PORT_BUFS; i++)
        ps->slots[i].dma_fd = -1;

    ps->def.nSize = sizeof(OMX_PARAM_PORTDEFINITIONTYPE);
    ps->def.nVersion = omx_spec_ver();
    ps->def.nPortIndex = p;
    ps->def.eDir = (p == 0u) ? OMX_DirInput : OMX_DirOutput;
    ps->def.nBufferCountMin = 4u;
    ps->def.nBufferCountActual = 8u;
    ps->def.bEnabled = OMX_TRUE;
    ps->def.bPopulated = OMX_FALSE;
    ps->def.eDomain = OMX_PortDomainVideo;
    ps->def.format.video.nFrameWidth = 1280u;
    ps->def.format.video.nFrameHeight = 720u;
    ps->def.format.video.xFramerate = 30u << 16;

    bool coded = (c->desc->v_role == VENUS_ROLE_DECODER && p == 0u) ||
                 (c->desc->v_role == VENUS_ROLE_ENCODER && p == 1u);
    ps->def.format.video.eCompressionFormat = coded ? c->desc->omx_coding : OMX_VIDEO_CodingUnused;
    /* libhcodec UpdateConfiguredFmt expects OHOS GraphicPixelFormat (24/25/28) here */
    ps->def.format.video.eColorFormat = coded
                                        ? OMX_COLOR_FormatUnused
                                        : (OMX_COLOR_FORMATTYPE)OHOS_PIXEL_FMT_YCBCR_420_SP;
    ps->ohos_pix_fmt = OHOS_PIXEL_FMT_YCBCR_420_SP;
    ps->ohos_buf_type = coded ? CODEC_BUFFER_TYPE_AVSHARE_MEM_FD : CODEC_BUFFER_TYPE_HANDLE;
    refresh_port_sizes(c, p);
}

/* Public OpenMAX IL 1.1.2 Core symbols */
OMX_ERRORTYPE OMX_Init(void)
{
    pthread_mutex_lock(&g_core_mu);
    g_init_refs++;
    omx_log("OMX_Init refs=%u", g_init_refs);
    pthread_mutex_unlock(&g_core_mu);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_Deinit(void)
{
    pthread_mutex_lock(&g_core_mu);
    if (g_init_refs > 0)
        g_init_refs--;
    omx_log("OMX_Deinit refs=%u", g_init_refs);
    pthread_mutex_unlock(&g_core_mu);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_ComponentNameEnum(OMX_STRING name, OMX_U32 len, OMX_U32 idx)
{
    if (!name || len == 0u)
        return OMX_ErrorBadParameter;
    if (idx >= NUM_COMPS)
        return OMX_ErrorNoMore;
    strncpy(name, g_comp_table[idx].name, len - 1u);
    name[len - 1u] = '\0';
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_GetHandle(OMX_HANDLETYPE *pHandle, OMX_STRING compName,
                            OMX_PTR pAppData, OMX_CALLBACKTYPE *pCallBacks)
{
    if (!pHandle || !compName)
        return OMX_ErrorBadParameter;
    const struct comp_desc *found = NULL;
    for (size_t i = 0; i < NUM_COMPS; i++) {
        if (strcmp(compName, g_comp_table[i].name) == 0) {
            found = &g_comp_table[i];
            break;
        }
    }
    if (!found)
        return OMX_ErrorComponentNotFound;
    if (!g_test_sys_ops) {
        const char *dev = (found->v_role == VENUS_ROLE_DECODER)
                              ? VENUS_DEV_DECODER
                              : VENUS_DEV_ENCODER;
        if (access(dev, R_OK | W_OK) != 0) {
            omx_log("OMX_GetHandle dev unavailable comp=%s dev=%s errno=%d",
                    found->name, dev, errno);
            return OMX_ErrorHardware;
        }
    }

    struct venus_omx_comp *c = calloc(1u, sizeof(*c));
    if (!c)
        return OMX_ErrorInsufficientResources;

    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    c->desc = found;
    c->state = OMX_StateLoaded;
    c->target_state = OMX_StateLoaded;
    c->bitrate = 4000000u;
    c->bitrate_mode = V4L2_MPEG_VIDEO_BITRATE_MODE_VBR;
    c->i_period = 30u;
    c->profile = -1;
    c->level = -1;
    if (pCallBacks)
        c->cb = *pCallBacks;
    c->app_data = pAppData;

    init_port_defaults(c, 0u);
    init_port_defaults(c, 1u);
    c->crop_rect.nSize = sizeof(OMX_CONFIG_RECTTYPE);
    c->crop_rect.nVersion = omx_spec_ver();
    c->crop_rect.nPortIndex = 1u;
    c->crop_rect.nWidth = 1280u;
    c->crop_rect.nHeight = 720u;

    c->omx.nSize = sizeof(OMX_COMPONENTTYPE);
    c->omx.nVersion = omx_spec_ver();
    c->omx.pComponentPrivate = c;
    c->omx.pApplicationPrivate = pAppData;
    c->omx.GetComponentVersion = comp_GetComponentVersion;
    c->omx.SendCommand = comp_SendCommand;
    c->omx.GetParameter = comp_GetParameter;
    c->omx.SetParameter = comp_SetParameter;
    c->omx.GetConfig = comp_GetConfig;
    c->omx.SetConfig = comp_SetConfig;
    c->omx.GetExtensionIndex = comp_GetExtensionIndex;
    c->omx.GetState = comp_GetState;
    c->omx.ComponentTunnelRequest = comp_ComponentTunnelRequest;
    c->omx.UseBuffer = comp_UseBuffer;
    c->omx.AllocateBuffer = comp_AllocateBuffer;
    c->omx.FreeBuffer = comp_FreeBuffer;
    c->omx.EmptyThisBuffer = comp_EmptyThisBuffer;
    c->omx.FillThisBuffer = comp_FillThisBuffer;
    c->omx.SetCallbacks = comp_SetCallbacks;
    c->omx.ComponentDeInit = comp_ComponentDeInit;
    c->omx.UseEGLImage = comp_UseEGLImage;
    c->omx.ComponentRoleEnum = comp_ComponentRoleEnum;

    if (pthread_create(&c->worker, NULL, omx_worker_main, c) != 0) {
        pthread_cond_destroy(&c->cv);
        pthread_mutex_destroy(&c->mu);
        free(c);
        return OMX_ErrorInsufficientResources;
    }
    c->worker_started = true;
    *pHandle = &c->omx;
    omx_log("OMX_GetHandle comp=%s", found->name);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_FreeHandle(OMX_HANDLETYPE hComponent)
{
    if (!hComponent)
        return OMX_ErrorInvalidComponent;
    OMX_COMPONENTTYPE *omx = (OMX_COMPONENTTYPE *)hComponent;
    struct venus_omx_comp *c = (struct venus_omx_comp *)omx->pComponentPrivate;
    if (!c)
        return OMX_ErrorInvalidComponent;
    omx_log("OMX_FreeHandle comp=%s", c->desc->name);
    (void)comp_ComponentDeInit(hComponent);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    free(c);
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_GetRolesOfComponent(OMX_STRING compName, OMX_U32 *pNumRoles, OMX_U8 **roles)
{
    if (!compName || !pNumRoles)
        return OMX_ErrorBadParameter;
    for (size_t i = 0; i < NUM_COMPS; i++) {
        if (strcmp(compName, g_comp_table[i].name) == 0) {
            if (roles != NULL && *pNumRoles >= 1u && roles[0] != NULL) {
                strncpy((char *)roles[0], g_comp_table[i].role, OMX_MAX_STRINGNAME_SIZE - 1);
                roles[0][OMX_MAX_STRINGNAME_SIZE - 1] = '\0';
            }
            *pNumRoles = 1u;
            return OMX_ErrorNone;
        }
    }
    *pNumRoles = 0u;
    return OMX_ErrorComponentNotFound;
}

OMX_ERRORTYPE OMX_GetComponentsOfRole(OMX_STRING role, OMX_U32 *pNumComps, OMX_U8 **compNames)
{
    if (!role || !pNumComps)
        return OMX_ErrorBadParameter;
    OMX_U32 found = 0u;
    for (size_t i = 0; i < NUM_COMPS; i++) {
        if (strcmp(role, g_comp_table[i].role) == 0) {
            if (compNames != NULL && found < *pNumComps && compNames[found] != NULL) {
                strncpy((char *)compNames[found], g_comp_table[i].name, OMX_MAX_STRINGNAME_SIZE - 1);
                compNames[found][OMX_MAX_STRINGNAME_SIZE - 1] = '\0';
            }
            found++;
        }
    }
    *pNumComps = found;
    return OMX_ErrorNone;
}

OMX_ERRORTYPE OMX_SetupTunnel(OMX_HANDLETYPE hOut, OMX_U32 nOut, OMX_HANDLETYPE hIn, OMX_U32 nIn)
{
    (void)hOut; (void)nOut; (void)hIn; (void)nIn;
    return OMX_ErrorNotImplemented;
}

OMX_ERRORTYPE OMX_GetContentPipe(OMX_HANDLETYPE *hPipe, OMX_STRING szURI)
{
    (void)hPipe; (void)szURI;
    return OMX_ErrorNotImplemented;
}

#ifdef OMX_VENUS_HOST_TEST
#include <assert.h>

struct omx_mock_state {
    enum venus_role role;
    uint32_t w;
    uint32_t h;
    uint32_t new_w;
    uint32_t new_h;
    bool trigger_sc;
    bool sc_pending;
    bool eos_pending;
    bool in_ready[VENUS_MAX_BUFS];
    bool out_ready[VENUS_MAX_BUFS];
    bool out_queued[VENUS_MAX_BUFS];
    uint32_t out_cnt;
    uint32_t frames_pushed;
    uint32_t in_hist_cnt;
    uint32_t in_hist_len[16];
    uint8_t in_hist[16][64];
    int out_fds[VENUS_MAX_BUFS];
    uint8_t out_pattern;
};

struct omx_test_cb_ctx {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool got_idle;
    bool got_exec;
    bool got_loaded;
    bool got_port_change;
    bool got_port_disabled;
    bool got_port_enabled;
    bool got_eos_event;
    bool got_eos_buf;
    uint32_t flush_done_mask;
    uint32_t err_cnt;
    OMX_ERRORTYPE last_err;
    uint32_t ebd_cnt;
    uint32_t fbd_cnt;
    uint32_t last_fbd_len;
};

static OMX_ERRORTYPE test_on_event(OMX_HANDLETYPE h, OMX_PTR ud, OMX_EVENTTYPE ev,
                                   OMX_U32 d1, OMX_U32 d2, OMX_PTR ed)
{
    (void)h; (void)ed;
    struct omx_test_cb_ctx *tc = (struct omx_test_cb_ctx *)ud;
    pthread_mutex_lock(&tc->mu);
    if (ev == OMX_EventCmdComplete) {
        if (d1 == OMX_CommandStateSet && d2 == OMX_StateIdle) tc->got_idle = true;
        if (d1 == OMX_CommandStateSet && d2 == OMX_StateExecuting) tc->got_exec = true;
        if (d1 == OMX_CommandStateSet && d2 == OMX_StateLoaded) tc->got_loaded = true;
        if (d1 == OMX_CommandPortDisable && d2 == 1u) tc->got_port_disabled = true;
        if (d1 == OMX_CommandPortEnable && d2 == 1u) tc->got_port_enabled = true;
        if (d1 == OMX_CommandFlush && d2 < 2u) tc->flush_done_mask |= (1u << d2);
    } else if (ev == OMX_EventPortSettingsChanged && d1 == 1u) {
        tc->got_port_change = true;
    } else if (ev == OMX_EventBufferFlag && (d2 & OMX_BUFFERFLAG_EOS)) {
        tc->got_eos_event = true;
    } else if (ev == OMX_EventError) {
        tc->err_cnt++;
        tc->last_err = (OMX_ERRORTYPE)(OMX_S32)d1;
    }
    pthread_cond_broadcast(&tc->cv);
    pthread_mutex_unlock(&tc->mu);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE test_on_ebd(OMX_HANDLETYPE h, OMX_PTR ud, OMX_BUFFERHEADERTYPE *b)
{
    (void)h; (void)b;
    struct omx_test_cb_ctx *tc = (struct omx_test_cb_ctx *)ud;
    pthread_mutex_lock(&tc->mu);
    tc->ebd_cnt++;
    pthread_cond_broadcast(&tc->cv);
    pthread_mutex_unlock(&tc->mu);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE test_on_fbd(OMX_HANDLETYPE h, OMX_PTR ud, OMX_BUFFERHEADERTYPE *b)
{
    (void)h;
    struct omx_test_cb_ctx *tc = (struct omx_test_cb_ctx *)ud;
    pthread_mutex_lock(&tc->mu);
    tc->fbd_cnt++;
    tc->last_fbd_len = b->nFilledLen;
    if (b->nFlags & OMX_BUFFERFLAG_EOS)
        tc->got_eos_buf = true;
    pthread_cond_broadcast(&tc->cv);
    pthread_mutex_unlock(&tc->mu);
    return OMX_ErrorNone;
}

static int omx_mock_open(void *ud, const char *path, int flags)
{
    struct omx_mock_state *ms = (struct omx_mock_state *)ud;
    (void)flags;
    ms->role = (strcmp(path, VENUS_DEV_DECODER) == 0) ? VENUS_ROLE_DECODER : VENUS_ROLE_ENCODER;
    return 101;
}

static int omx_mock_close(void *ud, int fd) { (void)ud; (void)fd; return 0; }
static int omx_mock_dmabuf_alloc(void *ud, uint32_t sz)
{
    (void)ud;
    int fd = memfd_create("omx_dmabuf", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, (off_t)venus_align_u32(sz, 4096u)) == 0);
    return fd;
}
static int omx_mock_dmabuf_sync(void *ud, int fd, uint64_t fl) { (void)ud; (void)fd; (void)fl; return 0; }
static void *omx_mock_mmap(void *ud, size_t l, int p, int fl, int fd, off_t o) { (void)ud; return mmap(NULL, l, p, fl, fd, o); }
static int omx_mock_munmap(void *ud, void *a, size_t l) { (void)ud; return munmap(a, l); }

static void omx_mock_pump_ready(struct omx_mock_state *ms)
{
    if (ms->sc_pending || ms->out_cnt == 0)
        return;
    uint32_t want = ms->frames_pushed + (ms->eos_pending ? 1u : 0u);
    uint32_t ready = 0;
    for (uint32_t i = 0; i < ms->out_cnt; i++) {
        if (ms->out_ready[i])
            ready++;
    }
    for (uint32_t i = 0; i < ms->out_cnt && ready < want; i++) {
        if (ms->out_queued[i] && !ms->out_ready[i]) {
            ms->out_ready[i] = true;
            ready++;
        }
    }
}

static int omx_mock_ioctl(void *ud, int fd, unsigned long req, void *arg)
{
    struct omx_mock_state *ms = (struct omx_mock_state *)ud;
    (void)fd;
    if (req == VIDIOC_S_FMT) {
        struct v4l2_format *f = (struct v4l2_format *)arg;
        uint32_t w = ms->new_w ? ms->new_w : (ms->w ? ms->w : f->fmt.pix_mp.width);
        uint32_t h = ms->new_h ? ms->new_h : (ms->h ? ms->h : f->fmt.pix_mp.height);
        ms->w = f->fmt.pix_mp.width;
        ms->h = f->fmt.pix_mp.height;
        f->fmt.pix_mp.width = venus_y_stride(V4L2_PIX_FMT_NV12, w);
        f->fmt.pix_mp.height = venus_y_scanlines(V4L2_PIX_FMT_NV12, h);
        f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width;
        f->fmt.pix_mp.plane_fmt[0].sizeimage = venus_raw_buffer_size(V4L2_PIX_FMT_NV12, w, h);
        return 0;
    }
    if (req == VIDIOC_G_FMT) {
        struct v4l2_format *f = (struct v4l2_format *)arg;
        uint32_t w = ms->new_w ? ms->new_w : ms->w;
        uint32_t h = ms->new_h ? ms->new_h : ms->h;
        f->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        f->fmt.pix_mp.width = venus_y_stride(V4L2_PIX_FMT_NV12, w);
        f->fmt.pix_mp.height = venus_y_scanlines(V4L2_PIX_FMT_NV12, h);
        f->fmt.pix_mp.plane_fmt[0].bytesperline = f->fmt.pix_mp.width;
        f->fmt.pix_mp.plane_fmt[0].sizeimage = venus_raw_buffer_size(V4L2_PIX_FMT_NV12, w, h);
        return 0;
    }
    if (req == VIDIOC_G_SELECTION) {
        struct v4l2_selection *s = (struct v4l2_selection *)arg;
        s->r.left = 0; s->r.top = 0;
        s->r.width = ms->new_w ? ms->new_w : ms->w;
        s->r.height = ms->new_h ? ms->new_h : ms->h;
        return 0;
    }
    if (req == VIDIOC_G_CTRL) {
        struct v4l2_control *c = (struct v4l2_control *)arg;
        c->value = 4;
        return 0;
    }
    if (req == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *r = (struct v4l2_requestbuffers *)arg;
        if (r->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE)
            ms->out_cnt = r->count;
        return 0;
    }
    if (req == VIDIOC_QBUF) {
        struct v4l2_buffer *vb = (struct v4l2_buffer *)arg;
        if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (ms->in_hist_cnt < 16u && vb->m.planes != NULL) {
                uint32_t hi = ms->in_hist_cnt++;
                uint32_t bu = vb->m.planes[0].bytesused;
                ms->in_hist_len[hi] = bu;
                uint32_t cap = bu < 64u ? bu : 64u;
                if (cap > 0u && vb->m.planes[0].m.fd >= 0) {
                    ssize_t pr = pread(vb->m.planes[0].m.fd, ms->in_hist[hi], cap, 0);
                    assert(pr == (ssize_t)cap);
                }
            }
            ms->in_ready[vb->index] = true;
            ms->frames_pushed++;
            if (ms->trigger_sc) {
                ms->trigger_sc = false;
                ms->sc_pending = true;
            }
            omx_mock_pump_ready(ms);
        } else {
            if (vb->m.planes != NULL && vb->index < VENUS_MAX_BUFS)
                ms->out_fds[vb->index] = vb->m.planes[0].m.fd;
            ms->out_queued[vb->index] = true;
            omx_mock_pump_ready(ms);
        }
        return 0;
    }
    if (req == VIDIOC_STREAMOFF) {
        enum v4l2_buf_type t = *(enum v4l2_buf_type *)arg;
        if (t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            memset(ms->in_ready, 0, sizeof(ms->in_ready));
            ms->frames_pushed = 0;
            ms->eos_pending = false;
        } else {
            memset(ms->out_ready, 0, sizeof(ms->out_ready));
            memset(ms->out_queued, 0, sizeof(ms->out_queued));
        }
        return 0;
    }
    if (req == VIDIOC_DQBUF) {
        struct v4l2_buffer *vb = (struct v4l2_buffer *)arg;
        if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            for (uint32_t i = 0; i < VENUS_MAX_BUFS; i++) {
                if (ms->in_ready[i]) { ms->in_ready[i] = false; vb->index = i; return 0; }
            }
        } else {
            for (uint32_t i = 0; i < ms->out_cnt; i++) {
                if (ms->out_ready[i]) {
                    ms->out_ready[i] = false;
                    ms->out_queued[i] = false;
                    vb->index = i;
                    if (ms->frames_pushed > 0) {
                        ms->frames_pushed--;
                        if (ms->out_pattern != 0u && ms->out_fds[i] > 0) {
                            uint8_t pat[64];
                            memset(pat, ms->out_pattern, sizeof(pat));
                            ssize_t pw = pwrite(ms->out_fds[i], pat, sizeof(pat), 0);
                            assert(pw == (ssize_t)sizeof(pat));
                        }
                        vb->m.planes[0].bytesused = 512u;
                        vb->flags = (ms->role == VENUS_ROLE_ENCODER) ? V4L2_BUF_FLAG_KEYFRAME : 0u;
                    } else if (ms->eos_pending) {
                        ms->eos_pending = false;
                        vb->m.planes[0].bytesused = 0u;
                        vb->flags = V4L2_BUF_FLAG_LAST;
                    }
                    omx_mock_pump_ready(ms);
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
    if (req == VIDIOC_DECODER_CMD || req == VIDIOC_ENCODER_CMD) {
        ms->eos_pending = true;
        omx_mock_pump_ready(ms);
        return 0;
    }
    return 0;
}

static int omx_mock_poll(void *ud, struct pollfd *fds, nfds_t nfds, int t)
{
    struct omx_mock_state *ms = (struct omx_mock_state *)ud;
    (void)t;
    int r = 0;
    for (nfds_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd == 101) {
            if (ms->sc_pending) { fds[i].revents |= POLLPRI; r++; }
            for (uint32_t j = 0; j < ms->out_cnt; j++) {
                if (ms->out_ready[j]) { fds[i].revents |= POLLIN; r++; break; }
            }
        }
    }
    if (r == 0) usleep(1000);
    return r;
}

int main(void)
{
    assert(OMX_Init() == OMX_ErrorNone);
    char cname[128] = {0};
    uint32_t n_found = 0;
    for (uint32_t i = 0; OMX_ComponentNameEnum(cname, sizeof(cname), i) == OMX_ErrorNone; i++) {
        assert(strncmp(cname, "OMX.marble.video.", 17) == 0);
        n_found++;
    }
    assert(n_found == 4u);

    OMX_U8 role_buf[128] = {0};
    OMX_U8 *roles[1] = {role_buf};
    OMX_U32 rcnt = 1;
    assert(OMX_GetRolesOfComponent("OMX.marble.video.decoder.avc", &rcnt, roles) == OMX_ErrorNone);
    assert(rcnt == 1 && strcmp((char *)role_buf, "video_decoder.avc") == 0);

    /* 1. Decoder test with AVSHARE_MEM_FD input + DYNAMIC_HANDLE output + SOURCE_CHANGE + EOS */
    struct omx_mock_state ms;
    memset(&ms, 0, sizeof(ms));
    ms.w = 640; ms.h = 480;
    ms.new_w = 1280; ms.new_h = 720;
    ms.trigger_sc = true;

    struct venus_sys_ops ops = {
        .userdata = &ms,
        .open_fn = omx_mock_open, .close_fn = omx_mock_close,
        .ioctl_fn = omx_mock_ioctl, .poll_fn = omx_mock_poll,
        .mmap_fn = omx_mock_mmap, .munmap_fn = omx_mock_munmap,
        .dmabuf_alloc_fn = omx_mock_dmabuf_alloc, .dmabuf_sync_fn = omx_mock_dmabuf_sync,
    };
    g_test_sys_ops = &ops;

    struct omx_test_cb_ctx tctx;
    memset(&tctx, 0, sizeof(tctx));
    pthread_mutex_init(&tctx.mu, NULL);
    pthread_cond_init(&tctx.cv, NULL);
    OMX_CALLBACKTYPE cbs = {test_on_event, test_on_ebd, test_on_fbd};

    OMX_HANDLETYPE h = NULL;
    assert(OMX_GetHandle(&h, "OMX.marble.video.decoder.avc", &tctx, &cbs) == OMX_ErrorNone);
    OMX_COMPONENTTYPE *comp = (OMX_COMPONENTTYPE *)h;

    OhosGetBufferHandleUsageParams usage_p = {sizeof(usage_p), omx_spec_ver(), 1u, 0ull};
    assert(comp->GetParameter(h, Codec_IndexParamGetBufferHandleUsage, &usage_p) == OMX_ErrorNone);
    assert(usage_p.usage == MARBLE_USAGE_DEFAULT);

    /* Verify SupportBufferType per port & DecideOutputBufMode (0x6fa00033 unsupported, 0x6fa00035 accepted) */
    OhosSupportBufferType sbt0 = {sizeof(sbt0), omx_spec_ver(), 0u, 0u};
    assert(comp->GetParameter(h, Codec_IndexParamSupportBufferType, &sbt0) == OMX_ErrorNone);
    assert(sbt0.bufferTypes == CODEC_BUFFER_TYPE_AVSHARE_MEM_FD);
    OhosSupportBufferType sbt1 = {sizeof(sbt1), omx_spec_ver(), 1u, 0u};
    assert(comp->GetParameter(h, Codec_IndexParamSupportBufferType, &sbt1) == OMX_ErrorNone);
    assert(sbt1.bufferTypes == (CODEC_BUFFER_TYPE_HANDLE | CODEC_BUFFER_TYPE_DYNAMIC_HANDLE));

    uint8_t mode_probe[20] = {20, 0, 0, 0};
    assert(comp->GetParameter(h, (OMX_INDEXTYPE)0x6FA00033u, mode_probe) == OMX_ErrorUnsupportedIndex);
    usage_p.usage = MARBLE_USAGE_DEFAULT | (1ULL << 15);
    assert(comp->SetParameter(h, Codec_IndexParamSetBufferHandleUsage, &usage_p) == OMX_ErrorNone);
    uint8_t vrr_probe[12] = {12, 0, 0, 0};
    assert(comp->SetParameter(h, Codec_IndexParamVrrOrMvUpload, vrr_probe) == OMX_ErrorNone);

    /* Enumerate & select I420 (28) on decoder output port */
    OhosCodecVideoPortFormatParam cvpf = {sizeof(cvpf), omx_spec_ver(), 1u, 2u, 0u, 0u, 0u};
    assert(comp->GetParameter(h, Codec_IndexCodecVideoPortFormat, &cvpf) == OMX_ErrorNone);
    assert(cvpf.codecColorFormat == OHOS_PIXEL_FMT_YCBCR_420_P);
    assert(comp->SetParameter(h, Codec_IndexCodecVideoPortFormat, &cvpf) == OMX_ErrorNone);

    OhosUseBufferType ubt = {sizeof(ubt), omx_spec_ver(), 1u, CODEC_BUFFER_TYPE_HANDLE};
    assert(comp->SetParameter(h, Codec_IndexParamUseBufferType, &ubt) == OMX_ErrorNone);

    OMX_PARAM_PORTDEFINITIONTYPE pd0 = {.nSize = sizeof(pd0), .nVersion = omx_spec_ver(), .nPortIndex = 0};
    assert(comp->GetParameter(h, OMX_IndexParamPortDefinition, &pd0) == OMX_ErrorNone);
    pd0.format.video.nFrameWidth = 640;
    pd0.format.video.nFrameHeight = 480;
    pd0.nBufferCountActual = 4;
    assert(comp->SetParameter(h, OMX_IndexParamPortDefinition, &pd0) == OMX_ErrorNone);

    OMX_PARAM_PORTDEFINITIONTYPE pd1 = {.nSize = sizeof(pd1), .nVersion = omx_spec_ver(), .nPortIndex = 1};
    assert(comp->GetParameter(h, OMX_IndexParamPortDefinition, &pd1) == OMX_ErrorNone);
    assert(pd1.format.video.nStride == 640); /* align(640, 128) */
    assert((uint32_t)pd1.format.video.eColorFormat == OHOS_PIXEL_FMT_YCBCR_420_P);
    pd1.nBufferCountActual = 4;
    assert(comp->SetParameter(h, OMX_IndexParamPortDefinition, &pd1) == OMX_ErrorNone);

    assert(comp->SendCommand(h, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);

    OMX_BUFFERHEADERTYPE *in_hdrs[4] = {0}, *out_hdrs[4] = {0};
    struct MarbleBufferHandle bhs[4];
    size_t bh_full_sz = venus_align_u32(640u * 480u * 3u / 2u, 4096u) + 4096u;
    size_t bh_pix_end = venus_align_u32(640u * 480u * 3u / 2u, 4096u);
    void *bh_maps[4] = {0};
    for (int i = 0; i < 4; i++) {
        assert(comp->AllocateBuffer(h, &in_hdrs[i], 0u, NULL, pd0.nBufferSize) == OMX_ErrorNone);
        memset(&bhs[i], 0, sizeof(bhs[i]));
        bhs[i].fd = omx_mock_dmabuf_alloc(NULL, (uint32_t)bh_full_sz);
        bh_maps[i] = mmap(NULL, bh_full_sz, PROT_READ | PROT_WRITE, MAP_SHARED, bhs[i].fd, 0);
        memset((uint8_t *)bh_maps[i] + bh_pix_end, 0xA5, 4096u);
        bhs[i].width = 640; bhs[i].stride = 640; bhs[i].height = 480;
        bhs[i].size = (int32_t)bh_full_sz; bhs[i].format = OHOS_PIXEL_FMT_YCBCR_420_P;
        assert(comp->UseBuffer(h, &out_hdrs[i], 1u, NULL, sizeof(bhs[i]), (OMX_U8 *)&bhs[i]) == OMX_ErrorNone);
    }

    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_idle) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    assert(comp->SendCommand(h, OMX_CommandStateSet, OMX_StateExecuting, NULL) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_exec) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    /* Send CSD packet + trigger SOURCE_CHANGE */
    uint8_t sps_pkt[8] = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1e};
    memcpy(in_hdrs[0]->pBuffer, sps_pkt, sizeof(sps_pkt));
    in_hdrs[0]->nFilledLen = sizeof(sps_pkt);
    in_hdrs[0]->nFlags = OMX_BUFFERFLAG_CODECCONFIG;
    assert(comp->EmptyThisBuffer(h, in_hdrs[0]) == OMX_ErrorNone);

    uint8_t idr_pkt[16] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21};
    memcpy(in_hdrs[1]->pBuffer, idr_pkt, sizeof(idr_pkt));
    in_hdrs[1]->nFilledLen = sizeof(idr_pkt);
    in_hdrs[1]->nFlags = OMX_BUFFERFLAG_SYNCFRAME;
    in_hdrs[1]->nTimeStamp = 33333;
    assert(comp->EmptyThisBuffer(h, in_hdrs[1]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_port_change) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    /* Reconfigure output port to 1280x720 with I420 (28) */
    assert(comp->SendCommand(h, OMX_CommandPortDisable, 1u, NULL) == OMX_ErrorNone);
    for (int i = 0; i < 4; i++) {
        munmap(bh_maps[i], bh_full_sz);
        close(bhs[i].fd);
        assert(comp->FreeBuffer(h, 1u, out_hdrs[i]) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_port_disabled) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    pd1.nPortIndex = 1u;
    assert(comp->GetParameter(h, OMX_IndexParamPortDefinition, &pd1) == OMX_ErrorNone);
    assert(pd1.format.video.nFrameWidth == 1280u && pd1.format.video.nStride == 1280);
    assert((uint32_t)pd1.format.video.eColorFormat == OHOS_PIXEL_FMT_YCBCR_420_P);
    assert(OMX_IndexConfigCommonInputCrop == 0x0700000E);
    assert(OMX_IndexConfigCommonOutputCrop == 0x0700000F);
    OMX_CONFIG_RECTTYPE crop_rect = {.nSize = sizeof(crop_rect), .nVersion = omx_spec_ver(), .nPortIndex = 1u};
    assert(comp->GetConfig(h, OMX_IndexConfigCommonOutputCrop, &crop_rect) == OMX_ErrorNone);
    assert(crop_rect.nWidth == 1280u && crop_rect.nHeight == 720u);

    size_t bh2_full_sz = venus_align_u32(1280u * 720u * 3u / 2u, 4096u) + 4096u;
    size_t bh2_pix_end = venus_align_u32(1280u * 720u * 3u / 2u, 4096u);
    assert(comp->SendCommand(h, OMX_CommandPortEnable, 1u, NULL) == OMX_ErrorNone);
    for (int i = 0; i < 4; i++) {
        memset(&bhs[i], 0, sizeof(bhs[i]));
        bhs[i].fd = omx_mock_dmabuf_alloc(NULL, (uint32_t)bh2_full_sz);
        bh_maps[i] = mmap(NULL, bh2_full_sz, PROT_READ | PROT_WRITE, MAP_SHARED, bhs[i].fd, 0);
        memset((uint8_t *)bh_maps[i] + bh2_pix_end, 0xA5, 4096u);
        bhs[i].width = 1280; bhs[i].stride = 1280; bhs[i].height = 720;
        bhs[i].size = (int32_t)bh2_full_sz; bhs[i].format = OHOS_PIXEL_FMT_YCBCR_420_P;
        assert(comp->UseBuffer(h, &out_hdrs[i], 1u, NULL, sizeof(bhs[i]), (OMX_U8 *)&bhs[i]) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_port_enabled) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    /* Push frame + EOS and receive FBD + EOS (out_hdrs[0..2]: Frame1 + Frame2 + EOS) */
    memcpy(in_hdrs[2]->pBuffer, idr_pkt, sizeof(idr_pkt));
    in_hdrs[2]->nFilledLen = sizeof(idr_pkt);
    in_hdrs[2]->nFlags = OMX_BUFFERFLAG_EOS;
    in_hdrs[2]->nTimeStamp = 66666;
    assert(comp->FillThisBuffer(h, out_hdrs[0]) == OMX_ErrorNone);
    assert(comp->FillThisBuffer(h, out_hdrs[1]) == OMX_ErrorNone);
    assert(comp->FillThisBuffer(h, out_hdrs[2]) == OMX_ErrorNone);
    assert(comp->EmptyThisBuffer(h, in_hdrs[2]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_eos_event || tctx.fbd_cnt < 3u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    /* Verify CropStore canary untouched on decoded output buffer */
    for (size_t k = bh2_pix_end; k < bh2_full_sz; k++)
        assert(((uint8_t *)bh_maps[0])[k] == 0xA5);

    assert(comp->SendCommand(h, OMX_CommandFlush, 0xFFFFFFFFu, NULL) == OMX_ErrorNone);
    assert(comp->SendCommand(h, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    assert(comp->SendCommand(h, OMX_CommandStateSet, OMX_StateLoaded, NULL) == OMX_ErrorNone);
    for (int i = 0; i < 4; i++) {
        assert(comp->FreeBuffer(h, 0u, in_hdrs[i]) == OMX_ErrorNone);
        munmap(bh_maps[i], bh2_full_sz);
        close(bhs[i].fd);
        assert(comp->FreeBuffer(h, 1u, out_hdrs[i]) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_loaded) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(OMX_FreeHandle(h) == OMX_ErrorNone);
    pthread_cond_destroy(&tctx.cv);
    pthread_mutex_destroy(&tctx.mu);

    /* 2. Encoder test: OMX.marble.video.encoder.hevc (HANDLE input + AVSHARE_MEM_FD output + EOS) */
    memset(&ms, 0, sizeof(ms));
    ms.w = 640; ms.h = 480;
    memset(&tctx, 0, sizeof(tctx));
    pthread_mutex_init(&tctx.mu, NULL);
    pthread_cond_init(&tctx.cv, NULL);
    OMX_HANDLETYPE eh = NULL;
    assert(OMX_GetHandle(&eh, "OMX.marble.video.encoder.hevc", &tctx, &cbs) == OMX_ErrorNone);
    OMX_COMPONENTTYPE *ec = (OMX_COMPONENTTYPE *)eh;
    OhosSupportBufferType e_sbt1 = {sizeof(e_sbt1), omx_spec_ver(), 1u, 0u};
    assert(ec->GetParameter(eh, Codec_IndexParamSupportBufferType, &e_sbt1) == OMX_ErrorNone);
    assert(e_sbt1.bufferTypes == CODEC_BUFFER_TYPE_AVSHARE_MEM_FD);
    OhosUseBufferType e_ubt = {sizeof(e_ubt), omx_spec_ver(), 0u, CODEC_BUFFER_TYPE_HANDLE};
    assert(ec->SetParameter(eh, Codec_IndexParamUseBufferType, &e_ubt) == OMX_ErrorNone);
    OMX_PARAM_PORTDEFINITIONTYPE ep0 = {.nSize = sizeof(ep0), .nVersion = omx_spec_ver(), .nPortIndex = 0};
    assert(ec->GetParameter(eh, OMX_IndexParamPortDefinition, &ep0) == OMX_ErrorNone);
    ep0.format.video.nFrameWidth = 640;
    ep0.format.video.nFrameHeight = 480;
    ep0.nBufferCountActual = 4;
    assert(ec->SetParameter(eh, OMX_IndexParamPortDefinition, &ep0) == OMX_ErrorNone);
    OMX_PARAM_PORTDEFINITIONTYPE ep1 = {.nSize = sizeof(ep1), .nVersion = omx_spec_ver(), .nPortIndex = 1};
    assert(ec->GetParameter(eh, OMX_IndexParamPortDefinition, &ep1) == OMX_ErrorNone);
    ep1.nBufferCountActual = 4;
    assert(ec->SetParameter(eh, OMX_IndexParamPortDefinition, &ep1) == OMX_ErrorNone);

    OhosCodecVideoParamHevc hvp = {sizeof(hvp), omx_spec_ver(), 1u, 1u, 0x100u, 30u};
    assert(ec->SetParameter(eh, Codec_IndexParamVideoHevc, &hvp) == OMX_ErrorNone);

    assert(ec->SendCommand(eh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    OMX_BUFFERHEADERTYPE *ein_h[4] = {0}, *eout_h[4] = {0};
    struct MarbleBufferHandle ebhs[4];
    size_t ebh_sz = venus_align_u32(640u * 480u * 3u / 2u, 4096u);
    void *ebh_maps[4] = {0};
    for (int i = 0; i < 4; i++) {
        memset(&ebhs[i], 0, sizeof(ebhs[i]));
        ebhs[i].fd = omx_mock_dmabuf_alloc(NULL, (uint32_t)ebh_sz);
        ebh_maps[i] = mmap(NULL, ebh_sz, PROT_READ | PROT_WRITE, MAP_SHARED, ebhs[i].fd, 0);
        memset(ebh_maps[i], 0x40, ebh_sz);
        ebhs[i].width = 640; ebhs[i].stride = 640; ebhs[i].height = 480;
        ebhs[i].size = (int32_t)ebh_sz; ebhs[i].format = OHOS_PIXEL_FMT_YCBCR_420_SP;
        assert(ec->UseBuffer(eh, &ein_h[i], 0u, NULL, sizeof(ebhs[i]), (OMX_U8 *)&ebhs[i]) == OMX_ErrorNone);
        assert(ec->AllocateBuffer(eh, &eout_h[i], 1u, NULL, ep1.nBufferSize) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_idle) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    assert(ec->SendCommand(eh, OMX_CommandStateSet, OMX_StateExecuting, NULL) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_exec) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    ein_h[0]->nFilledLen = (OMX_U32)ebh_sz;
    ein_h[0]->nFlags = OMX_BUFFERFLAG_EOS;
    ein_h[0]->nTimeStamp = 1000;
    assert(ec->FillThisBuffer(eh, eout_h[0]) == OMX_ErrorNone);
    assert(ec->FillThisBuffer(eh, eout_h[1]) == OMX_ErrorNone);
    assert(ec->EmptyThisBuffer(eh, ein_h[0]) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_eos_event || tctx.fbd_cnt < 2u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    assert(ec->SendCommand(eh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    assert(ec->SendCommand(eh, OMX_CommandStateSet, OMX_StateLoaded, NULL) == OMX_ErrorNone);
    for (int i = 0; i < 4; i++) {
        munmap(ebh_maps[i], ebh_sz);
        close(ebhs[i].fd);
        assert(ec->FreeBuffer(eh, 0u, ein_h[i]) == OMX_ErrorNone);
        assert(ec->FreeBuffer(eh, 1u, eout_h[i]) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_loaded) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(OMX_FreeHandle(eh) == OMX_ErrorNone);
    pthread_cond_destroy(&tctx.cv);
    pthread_mutex_destroy(&tctx.mu);

    /* 3. HEVC single-AU + immediate EOS before SOURCE_CHANGE (256x256 -> 256x256, I420) */
    memset(&ms, 0, sizeof(ms));
    ms.w = 256; ms.h = 256; ms.new_w = 256; ms.new_h = 256; ms.trigger_sc = true;
    memset(&tctx, 0, sizeof(tctx));
    pthread_mutex_init(&tctx.mu, NULL);
    pthread_cond_init(&tctx.cv, NULL);
    OMX_HANDLETYPE hh = NULL;
    assert(OMX_GetHandle(&hh, "OMX.marble.video.decoder.hevc", &tctx, &cbs) == OMX_ErrorNone);
    OMX_COMPONENTTYPE *hc = (OMX_COMPONENTTYPE *)hh;
    OhosCodecVideoPortFormatParam hcv = {sizeof(hcv), omx_spec_ver(), 1u, 2u, 0u, 0u, 0u};
    assert(hc->GetParameter(hh, Codec_IndexCodecVideoPortFormat, &hcv) == OMX_ErrorNone);
    assert(hcv.codecColorFormat == OHOS_PIXEL_FMT_YCBCR_420_P);
    assert(hc->SetParameter(hh, Codec_IndexCodecVideoPortFormat, &hcv) == OMX_ErrorNone);

    OMX_PARAM_PORTDEFINITIONTYPE hp0 = {.nSize = sizeof(hp0), .nVersion = omx_spec_ver(), .nPortIndex = 0};
    assert(hc->GetParameter(hh, OMX_IndexParamPortDefinition, &hp0) == OMX_ErrorNone);
    hp0.format.video.nFrameWidth = 256; hp0.format.video.nFrameHeight = 256; hp0.nBufferCountActual = 4;
    assert(hc->SetParameter(hh, OMX_IndexParamPortDefinition, &hp0) == OMX_ErrorNone);
    OMX_PARAM_PORTDEFINITIONTYPE hp1 = {.nSize = sizeof(hp1), .nVersion = omx_spec_ver(), .nPortIndex = 1};
    assert(hc->GetParameter(hh, OMX_IndexParamPortDefinition, &hp1) == OMX_ErrorNone);
    hp1.nBufferCountActual = 4;
    assert(hc->SetParameter(hh, OMX_IndexParamPortDefinition, &hp1) == OMX_ErrorNone);

    assert(hc->SendCommand(hh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    OMX_BUFFERHEADERTYPE *hin_h[4] = {0}, *hout_h[4] = {0};
    struct MarbleBufferHandle hbhs[4];
    size_t hbh_pix = venus_align_u32(256u * 256u * 3u / 2u, 4096u);
    size_t hbh_sz = hbh_pix + 4096u;
    void *hbh_maps[4] = {0};
    for (int i = 0; i < 4; i++) {
        assert(hc->AllocateBuffer(hh, &hin_h[i], 0u, NULL, hp0.nBufferSize) == OMX_ErrorNone);
        memset(&hbhs[i], 0, sizeof(hbhs[i]));
        hbhs[i].fd = omx_mock_dmabuf_alloc(NULL, (uint32_t)hbh_sz);
        hbh_maps[i] = mmap(NULL, hbh_sz, PROT_READ | PROT_WRITE, MAP_SHARED, hbhs[i].fd, 0);
        memset((uint8_t *)hbh_maps[i] + hbh_pix, 0xA5, 4096u);
        hbhs[i].width = 256; hbhs[i].stride = 256; hbhs[i].height = 256;
        hbhs[i].size = (int32_t)hbh_sz; hbhs[i].format = OHOS_PIXEL_FMT_YCBCR_420_P;
        assert(hc->UseBuffer(hh, &hout_h[i], 1u, NULL, sizeof(hbhs[i]), (OMX_U8 *)&hbhs[i]) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_idle) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    assert(hc->SendCommand(hh, OMX_CommandStateSet, OMX_StateExecuting, NULL) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_exec) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    for (int i = 0; i < 4; i++)
        assert(hc->FillThisBuffer(hh, hout_h[i]) == OMX_ErrorNone);

    /* Send 1 IDR AU + separate empty EOS immediately back-to-back before SOURCE_CHANGE */
    memcpy(hin_h[0]->pBuffer, idr_pkt, sizeof(idr_pkt));
    hin_h[0]->nFilledLen = sizeof(idr_pkt);
    hin_h[0]->nFlags = OMX_BUFFERFLAG_SYNCFRAME;
    assert(hc->EmptyThisBuffer(hh, hin_h[0]) == OMX_ErrorNone);
    hin_h[1]->nFilledLen = 0;
    hin_h[1]->nFlags = OMX_BUFFERFLAG_EOS;
    assert(hc->EmptyThisBuffer(hh, hin_h[1]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_eos_event || tctx.fbd_cnt < 2u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(tctx.got_eos_buf && !tctx.got_port_change);
    for (size_t k = hbh_pix; k < hbh_sz; k++)
        assert(((uint8_t *)hbh_maps[0])[k] == 0xA5);

    assert(hc->SendCommand(hh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    assert(hc->SendCommand(hh, OMX_CommandStateSet, OMX_StateLoaded, NULL) == OMX_ErrorNone);
    for (int i = 0; i < 4; i++) {
        assert(hc->FreeBuffer(hh, 0u, hin_h[i]) == OMX_ErrorNone);
        munmap(hbh_maps[i], hbh_sz);
        close(hbhs[i].fd);
        assert(hc->FreeBuffer(hh, 1u, hout_h[i]) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_loaded) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(OMX_FreeHandle(hh) == OMX_ErrorNone);
    pthread_cond_destroy(&tctx.cv);
    pthread_mutex_destroy(&tctx.mu);

    /* 4. Dynamic-handle AVC encoder EOS test (2 real frames + empty EOS with filledLen=16 -> exact 2 data + 1 EOS) */
    memset(&ms, 0, sizeof(ms));
    ms.w = 640; ms.h = 480;
    memset(&tctx, 0, sizeof(tctx));
    pthread_mutex_init(&tctx.mu, NULL);
    pthread_cond_init(&tctx.cv, NULL);
    OMX_HANDLETYPE dh = NULL;
    assert(OMX_GetHandle(&dh, "OMX.marble.video.encoder.avc", &tctx, &cbs) == OMX_ErrorNone);
    OMX_COMPONENTTYPE *dc = (OMX_COMPONENTTYPE *)dh;
    OhosUseBufferType d_ubt = {sizeof(d_ubt), omx_spec_ver(), 0u, CODEC_BUFFER_TYPE_DYNAMIC_HANDLE};
    assert(dc->SetParameter(dh, Codec_IndexParamUseBufferType, &d_ubt) == OMX_ErrorNone);
    OMX_PARAM_PORTDEFINITIONTYPE dp0 = {.nSize = sizeof(dp0), .nVersion = omx_spec_ver(), .nPortIndex = 0};
    assert(dc->GetParameter(dh, OMX_IndexParamPortDefinition, &dp0) == OMX_ErrorNone);
    dp0.format.video.nFrameWidth = 640;
    dp0.format.video.nFrameHeight = 480;
    dp0.nBufferCountActual = 4;
    assert(dc->SetParameter(dh, OMX_IndexParamPortDefinition, &dp0) == OMX_ErrorNone);
    OMX_PARAM_PORTDEFINITIONTYPE dp1 = {.nSize = sizeof(dp1), .nVersion = omx_spec_ver(), .nPortIndex = 1};
    assert(dc->GetParameter(dh, OMX_IndexParamPortDefinition, &dp1) == OMX_ErrorNone);
    dp1.nBufferCountActual = 4;
    assert(dc->SetParameter(dh, OMX_IndexParamPortDefinition, &dp1) == OMX_ErrorNone);

    assert(dc->SendCommand(dh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    OMX_BUFFERHEADERTYPE *din_h[4] = {0}, *dout_h[4] = {0};
    struct MarbleBufferHandle dbhs[4];
    struct MarbleDynamicBuffer ddyn[4];
    size_t dbh_sz = venus_align_u32(640u * 480u * 3u / 2u, 4096u);
    void *dbh_maps[4] = {0};
    for (int i = 0; i < 4; i++) {
        memset(&dbhs[i], 0, sizeof(dbhs[i]));
        dbhs[i].fd = omx_mock_dmabuf_alloc(NULL, (uint32_t)dbh_sz);
        dbh_maps[i] = mmap(NULL, dbh_sz, PROT_READ | PROT_WRITE, MAP_SHARED, dbhs[i].fd, 0);
        memset(dbh_maps[i], 0x55, dbh_sz);
        dbhs[i].width = 640; dbhs[i].stride = 640; dbhs[i].height = 480;
        dbhs[i].size = (int32_t)dbh_sz; dbhs[i].format = OHOS_PIXEL_FMT_YCBCR_420_SP;
        ddyn[i].type = 0;
        ddyn[i].bufferHandle = &dbhs[i];
        assert(dc->UseBuffer(dh, &din_h[i], 0u, NULL, sizeof(ddyn[i]), (OMX_U8 *)&ddyn[i]) == OMX_ErrorNone);
        assert(dc->AllocateBuffer(dh, &dout_h[i], 1u, NULL, dp1.nBufferSize) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_idle) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    assert(dc->SendCommand(dh, OMX_CommandStateSet, OMX_StateExecuting, NULL) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_exec) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    for (int i = 0; i < 4; i++)
        assert(dc->FillThisBuffer(dh, dout_h[i]) == OMX_ErrorNone);

    /* Push 2 real frames + 1 EOS buffer with filledLen=sizeof(MarbleDynamicBuffer) (CodecDynaBuffer behavior) */
    for (int i = 0; i < 2; i++) {
        din_h[i]->nFilledLen = (OMX_U32)sizeof(struct MarbleDynamicBuffer);
        din_h[i]->nFlags = 0u;
        din_h[i]->nTimeStamp = (OMX_TICKS)(i * 33333);
        assert(dc->EmptyThisBuffer(dh, din_h[i]) == OMX_ErrorNone);
    }
    din_h[2]->nFilledLen = (OMX_U32)sizeof(struct MarbleDynamicBuffer);
    din_h[2]->nFlags = OMX_BUFFERFLAG_EOS;
    din_h[2]->nTimeStamp = 66666;
    assert(dc->EmptyThisBuffer(dh, din_h[2]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_eos_event || !tctx.got_eos_buf) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(tctx.fbd_cnt == 3u); /* 2 real frames + 1 EOS, zero phantom frame */

    assert(dc->SendCommand(dh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    assert(dc->SendCommand(dh, OMX_CommandStateSet, OMX_StateLoaded, NULL) == OMX_ErrorNone);
    for (int i = 0; i < 4; i++) {
        munmap(dbh_maps[i], dbh_sz);
        close(dbhs[i].fd);
        assert(dc->FreeBuffer(dh, 0u, din_h[i]) == OMX_ErrorNone);
        assert(dc->FreeBuffer(dh, 1u, dout_h[i]) == OMX_ErrorNone);
    }
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_loaded) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(OMX_FreeHandle(dh) == OMX_ErrorNone);
    pthread_cond_destroy(&tctx.cv);
    pthread_mutex_destroy(&tctx.mu);

    /* 5. Decoder DYNAMIC_HANDLE (Surface mode) + Seek flush preserving CSD + standard OMX YUV420 + ENOSPC error */
    memset(&ms, 0, sizeof(ms));
    ms.w = 640; ms.h = 480;
    ms.new_w = 640; ms.new_h = 480;
    ms.trigger_sc = true;
    ms.out_pattern = 0x7Au;
    memset(&tctx, 0, sizeof(tctx));
    pthread_mutex_init(&tctx.mu, NULL);
    pthread_cond_init(&tctx.cv, NULL);
    OMX_HANDLETYPE mh = NULL;
    assert(OMX_GetHandle(&mh, "OMX.marble.video.decoder.avc", &tctx, &cbs) == OMX_ErrorNone);
    OMX_COMPONENTTYPE *mc = (OMX_COMPONENTTYPE *)mh;

    /* Standard OMX YUV420 color format enumeration & normalization (19 -> I420 28, 21 -> NV12 24) */
    OMX_VIDEO_PARAM_PORTFORMATTYPE vpf = {sizeof(vpf), omx_spec_ver(), 1u, 3u, OMX_VIDEO_CodingUnused, OMX_COLOR_FormatUnused, 0u};
    assert(mc->GetParameter(mh, OMX_IndexParamVideoPortFormat, &vpf) == OMX_ErrorNone);
    assert(vpf.eColorFormat == OMX_COLOR_FormatYUV420SemiPlanar);
    vpf.nIndex = 4u;
    assert(mc->GetParameter(mh, OMX_IndexParamVideoPortFormat, &vpf) == OMX_ErrorNone);
    assert(vpf.eColorFormat == OMX_COLOR_FormatYUV420Planar);
    vpf.eColorFormat = OMX_COLOR_FormatYUV420Planar;
    assert(mc->SetParameter(mh, OMX_IndexParamVideoPortFormat, &vpf) == OMX_ErrorNone);
    OMX_PARAM_PORTDEFINITIONTYPE mp1 = {.nSize = sizeof(mp1), .nVersion = omx_spec_ver(), .nPortIndex = 1};
    assert(mc->GetParameter(mh, OMX_IndexParamPortDefinition, &mp1) == OMX_ErrorNone);
    assert((uint32_t)mp1.format.video.eColorFormat == OHOS_PIXEL_FMT_YCBCR_420_P);
    vpf.eColorFormat = OMX_COLOR_FormatYUV420SemiPlanar;
    assert(mc->SetParameter(mh, OMX_IndexParamVideoPortFormat, &vpf) == OMX_ErrorNone);
    assert(mc->GetParameter(mh, OMX_IndexParamPortDefinition, &mp1) == OMX_ErrorNone);
    assert((uint32_t)mp1.format.video.eColorFormat == OHOS_PIXEL_FMT_YCBCR_420_SP);

    /* Select DYNAMIC_HANDLE on decoder output port 1 */
    OhosUseBufferType m_ubt = {sizeof(m_ubt), omx_spec_ver(), 1u, CODEC_BUFFER_TYPE_DYNAMIC_HANDLE};
    assert(mc->SetParameter(mh, Codec_IndexParamUseBufferType, &m_ubt) == OMX_ErrorNone);
    OMX_PARAM_PORTDEFINITIONTYPE mp0 = {.nSize = sizeof(mp0), .nVersion = omx_spec_ver(), .nPortIndex = 0};
    assert(mc->GetParameter(mh, OMX_IndexParamPortDefinition, &mp0) == OMX_ErrorNone);
    mp0.format.video.nFrameWidth = 640; mp0.format.video.nFrameHeight = 480; mp0.nBufferCountActual = 4;
    assert(mc->SetParameter(mh, OMX_IndexParamPortDefinition, &mp0) == OMX_ErrorNone);
    assert(mc->GetParameter(mh, OMX_IndexParamPortDefinition, &mp1) == OMX_ErrorNone);
    mp1.nBufferCountActual = 4;
    assert(mc->SetParameter(mh, OMX_IndexParamPortDefinition, &mp1) == OMX_ErrorNone);

    assert(mc->SendCommand(mh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    OMX_BUFFERHEADERTYPE *min_h[4] = {0}, *mout_h[4] = {0};
    struct MarbleDynamicBuffer mdyn[4];
    struct MarbleBufferHandle mbhs[4], mbh_swap;
    size_t mbh_sz = venus_align_u32(640u * 480u * 3u / 2u, 4096u);
    void *mbh_maps[4] = {0}, *mbh_swap_map = NULL;
    for (int i = 0; i < 4; i++) {
        assert(mc->AllocateBuffer(mh, &min_h[i], 0u, NULL, mp0.nBufferSize) == OMX_ErrorNone);
        memset(&mbhs[i], 0, sizeof(mbhs[i]));
        mbhs[i].fd = omx_mock_dmabuf_alloc(NULL, (uint32_t)mbh_sz);
        mbh_maps[i] = mmap(NULL, mbh_sz, PROT_READ | PROT_WRITE, MAP_SHARED, mbhs[i].fd, 0);
        memset(mbh_maps[i], 0x11, mbh_sz);
        mbhs[i].width = 640; mbhs[i].stride = 640; mbhs[i].height = 480;
        mbhs[i].size = (int32_t)mbh_sz; mbhs[i].format = OHOS_PIXEL_FMT_YCBCR_420_SP;
        /* Match CodecDynaBuffer::UseBuffer: dynamic->bufferHandle starts NULL until FillThisBuffer */
        mdyn[i].type = 0;
        mdyn[i].bufferHandle = NULL;
        assert(mc->UseBuffer(mh, &mout_h[i], 1u, NULL, sizeof(mdyn[i]), (OMX_U8 *)&mdyn[i]) == OMX_ErrorNone);
    }
    memset(&mbh_swap, 0, sizeof(mbh_swap));
    mbh_swap.fd = omx_mock_dmabuf_alloc(NULL, (uint32_t)mbh_sz);
    mbh_swap_map = mmap(NULL, mbh_sz, PROT_READ | PROT_WRITE, MAP_SHARED, mbh_swap.fd, 0);
    memset(mbh_swap_map, 0x22, mbh_sz);
    mbh_swap.width = 640; mbh_swap.stride = 640; mbh_swap.height = 480;
    mbh_swap.size = (int32_t)mbh_sz; mbh_swap.format = OHOS_PIXEL_FMT_YCBCR_420_SP;

    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_idle) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(mc->SendCommand(mh, OMX_CommandStateSet, OMX_StateExecuting, NULL) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_exec) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    /* Attach dynamic handle to slot 0 & submit FillThisBuffer */
    mdyn[0].bufferHandle = &mbhs[0];
    assert(mc->FillThisBuffer(mh, mout_h[0]) == OMX_ErrorNone);

    /* Send CSD (SPS1) + Frame 1 (IDR1) -> Venus receives SPS1 + IDR1 */
    uint8_t sps1[8] = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x28};
    uint8_t idr1[16] = {0, 0, 0, 1, 0x65, 0x11, 0x22, 0x33};
    memcpy(min_h[0]->pBuffer, sps1, sizeof(sps1));
    min_h[0]->nFilledLen = sizeof(sps1);
    min_h[0]->nFlags = OMX_BUFFERFLAG_CODECCONFIG;
    assert(mc->EmptyThisBuffer(mh, min_h[0]) == OMX_ErrorNone);
    memcpy(min_h[1]->pBuffer, idr1, sizeof(idr1));
    min_h[1]->nFilledLen = sizeof(idr1);
    min_h[1]->nFlags = OMX_BUFFERFLAG_SYNCFRAME;
    min_h[1]->nTimeStamp = 1000;
    assert(mc->EmptyThisBuffer(mh, min_h[1]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (tctx.fbd_cnt < 1u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(ms.in_hist_cnt == 1u && ms.in_hist_len[0] == sizeof(sps1) + sizeof(idr1));
    assert(memcmp(ms.in_hist[0], sps1, sizeof(sps1)) == 0);
    assert(memcmp(ms.in_hist[0] + sizeof(sps1), idr1, sizeof(idr1)) == 0);
    assert(((uint8_t *)mbh_maps[0])[0] == 0x7Au); /* Decoded into mbhs[0] via dynamic handle */

    /* Swap dynamic handle on slot 0 to mbh_swap before Frame 2 (P-frame, no CSD prepend) */
    ms.out_pattern = 0x8Bu;
    mdyn[0].bufferHandle = &mbh_swap;
    assert(mc->FillThisBuffer(mh, mout_h[0]) == OMX_ErrorNone);
    uint8_t pframe[16] = {0, 0, 0, 1, 0x41, 0x99, 0xAA, 0xBB};
    memcpy(min_h[2]->pBuffer, pframe, sizeof(pframe));
    min_h[2]->nFilledLen = sizeof(pframe);
    min_h[2]->nFlags = 0u;
    min_h[2]->nTimeStamp = 2000;
    assert(mc->EmptyThisBuffer(mh, min_h[2]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (tctx.fbd_cnt < 2u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(ms.in_hist_cnt == 2u && ms.in_hist_len[1] == sizeof(pframe));
    assert(memcmp(ms.in_hist[1], pframe, sizeof(pframe)) == 0);
    assert(((uint8_t *)mbh_swap_map)[0] == 0x8Bu); /* Dynamically redirected to mbh_swap */
    assert(((uint8_t *)mbh_maps[0])[0] == 0x7Au);  /* Previous buffer untouched */

    /* Seek flush: MUST preserve SPS1 and re-prepend to next post-seek IDR without resending CODECCONFIG */
    assert(mc->SendCommand(mh, OMX_CommandFlush, 0xFFFFFFFFu, NULL) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (tctx.flush_done_mask != 3u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);

    mdyn[1].bufferHandle = &mbhs[1];
    assert(mc->FillThisBuffer(mh, mout_h[1]) == OMX_ErrorNone);
    uint8_t idr3[16] = {0, 0, 0, 1, 0x65, 0xCC, 0xDD, 0xEE};
    memcpy(min_h[3]->pBuffer, idr3, sizeof(idr3));
    min_h[3]->nFilledLen = sizeof(idr3);
    min_h[3]->nFlags = OMX_BUFFERFLAG_SYNCFRAME;
    min_h[3]->nTimeStamp = 3000;
    assert(mc->EmptyThisBuffer(mh, min_h[3]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (tctx.fbd_cnt < 3u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(ms.in_hist_cnt == 3u && ms.in_hist_len[2] == sizeof(sps1) + sizeof(idr3));
    assert(memcmp(ms.in_hist[2], sps1, sizeof(sps1)) == 0);
    assert(memcmp(ms.in_hist[2] + sizeof(sps1), idr3, sizeof(idr3)) == 0);

    /* New CODECCONFIG replaces old SPS1 */
    uint8_t sps2[10] = {0, 0, 0, 1, 0x67, 0x4D, 0x00, 0x29, 0xFF, 0x01};
    uint8_t idr4[16] = {0, 0, 0, 1, 0x65, 0x44, 0x55, 0x66};
    mdyn[2].bufferHandle = &mbhs[2];
    assert(mc->FillThisBuffer(mh, mout_h[2]) == OMX_ErrorNone);
    memcpy(min_h[0]->pBuffer, sps2, sizeof(sps2));
    min_h[0]->nFilledLen = sizeof(sps2);
    min_h[0]->nFlags = OMX_BUFFERFLAG_CODECCONFIG;
    assert(mc->EmptyThisBuffer(mh, min_h[0]) == OMX_ErrorNone);
    memcpy(min_h[1]->pBuffer, idr4, sizeof(idr4));
    min_h[1]->nFilledLen = sizeof(idr4);
    min_h[1]->nFlags = OMX_BUFFERFLAG_SYNCFRAME;
    min_h[1]->nTimeStamp = 4000;
    assert(mc->EmptyThisBuffer(mh, min_h[1]) == OMX_ErrorNone);

    pthread_mutex_lock(&tctx.mu);
    while (tctx.fbd_cnt < 4u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(ms.in_hist_cnt == 4u && ms.in_hist_len[3] == sizeof(sps2) + sizeof(idr4));
    assert(memcmp(ms.in_hist[3], sps2, sizeof(sps2)) == 0);
    assert(memcmp(ms.in_hist[3] + sizeof(sps2), idr4, sizeof(idr4)) == 0);

    /* ENOSPC input overflow MUST report OMX_EventError(OMX_ErrorOverflow), never silently discard */
    min_h[2]->nFilledLen = 16u * 1024u * 1024u;
    min_h[2]->nFlags = 0u;
    assert(mc->EmptyThisBuffer(mh, min_h[2]) == OMX_ErrorNone);
    pthread_mutex_lock(&tctx.mu);
    while (tctx.err_cnt < 1u) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(tctx.last_err == OMX_ErrorOverflow);
    assert(ms.in_hist_cnt == 4u);

    assert(mc->SendCommand(mh, OMX_CommandStateSet, OMX_StateIdle, NULL) == OMX_ErrorNone);
    assert(mc->SendCommand(mh, OMX_CommandStateSet, OMX_StateLoaded, NULL) == OMX_ErrorNone);
    for (int i = 0; i < 4; i++) {
        assert(mc->FreeBuffer(mh, 0u, min_h[i]) == OMX_ErrorNone);
        munmap(mbh_maps[i], mbh_sz);
        close(mbhs[i].fd);
        assert(mc->FreeBuffer(mh, 1u, mout_h[i]) == OMX_ErrorNone);
    }
    munmap(mbh_swap_map, mbh_sz);
    close(mbh_swap.fd);
    pthread_mutex_lock(&tctx.mu);
    while (!tctx.got_loaded) pthread_cond_wait(&tctx.cv, &tctx.mu);
    pthread_mutex_unlock(&tctx.mu);
    assert(OMX_FreeHandle(mh) == OMX_ErrorNone);
    pthread_cond_destroy(&tctx.cv);
    pthread_mutex_destroy(&tctx.mu);

    assert(OMX_Deinit() == OMX_ErrorNone);
    printf("OMX_VENUS_HOST_TEST_OK\n");
    return 0;
}
#endif
