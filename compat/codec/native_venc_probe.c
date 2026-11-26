// SPDX-License-Identifier: MIT
/*
 * End-to-end OHOS NDK Hardware Video Encoder + Decoder Loopback Probe for Xiaomi marble (SM7475).
 *
 * Exercises public OHOS AVCodecKit NDK APIs:
 *   libnative_media_venc.so (OH_VideoEncoder_*) -> SA 3011 av_codec_service ->
 *   HDF codec_host -> libOMX_Core.z.so -> Qualcomm SM7475 Venus (/dev/video33),
 * then loops the real hardware-encoded Annex-B bitstream back through:
 *   libnative_media_vdec.so (OH_VideoDecoder_*) -> SA 3011 -> codec_host ->
 *   Venus (/dev/video32) and validates decoded luma fidelity against the source gradient.
 *
 * 100% silent, zero audio, zero display/light side effects.
 */
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "multimedia/player_framework/native_avbuffer.h"
#include "multimedia/player_framework/native_avcapability.h"
#include "multimedia/player_framework/native_avcodec_base.h"
#include "multimedia/player_framework/native_avcodec_videodecoder.h"
#include "multimedia/player_framework/native_avcodec_videoencoder.h"
#include "multimedia/player_framework/native_averrors.h"
#include "multimedia/player_framework/native_avformat.h"

#define MAX_SLOTS 16
#define MAX_OUT_PKTS 32
#define PROBE_TIMEOUT_SEC 12

typedef struct {
    uint32_t index;
    OH_AVBuffer *buffer;
} SlotItem;

typedef struct {
    uint8_t *data;
    uint32_t size;
    uint32_t flags;
    int64_t pts;
} EncPacket;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    SlotItem in_q[MAX_SLOTS];
    int in_head;
    int in_tail;
    int in_count;
    EncPacket pkts[MAX_OUT_PKTS];
    int pkt_count;
    uint64_t total_bytes;
    int keyframes;
    int csd_pkts;
    int data_pkts;
    bool out_eos;
    bool closing;
    int32_t err;
    const char *stage;
    const char *mime;
} EncCtx;

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    SlotItem in_q[MAX_SLOTS];
    int in_head;
    int in_tail;
    int in_count;
    int width;
    int height;
    int stride;
    int slice_h;
    int pix_fmt;
    int frames;
    uint64_t bytes;
    uint64_t first_y_sum;
    uint64_t first_y_cnt;
    bool out_eos;
    bool closing;
    int32_t err;
    const char *stage;
} DecCtx;

static void make_deadline(struct timespec *ts, int sec)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
    ts->tv_sec += sec;
}

static bool has_annexb_startcode(const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i + 3u < len && i < 64u; i++) {
        if (p[i] == 0 && p[i + 1] == 0 &&
            (p[i + 2] == 1 || (i + 4u <= len && p[i + 2] == 0 && p[i + 3] == 1))) {
            return true;
        }
    }
    return false;
}

/* Parse Annex-B NAL unit types in a packet for AVC or HEVC diagnostics. */
static void scan_nals(const char *mime, const uint8_t *p, uint32_t len,
                      bool *has_param, bool *has_idr, int *nal_cnt)
{
    bool is_hevc = (strcmp(mime, "video/hevc") == 0);
    for (uint32_t i = 0; i + 3u < len; i++) {
        uint32_t hdr = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            hdr = i + 3u;
        } else if (i + 4u < len && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) {
            hdr = i + 4u;
            i++;
        } else {
            continue;
        }
        if (hdr >= len) {
            break;
        }
        (*nal_cnt)++;
        if (is_hevc) {
            uint8_t ntype = (uint8_t)((p[hdr] >> 1) & 0x3Fu);
            if (ntype == 32u || ntype == 33u || ntype == 34u) {
                *has_param = true;
            }
            if (ntype == 19u || ntype == 20u || ntype == 21u) {
                *has_idr = true;
            }
        } else {
            uint8_t ntype = (uint8_t)(p[hdr] & 0x1Fu);
            if (ntype == 7u || ntype == 8u) {
                *has_param = true;
            }
            if (ntype == 5u) {
                *has_idr = true;
            }
        }
    }
}

static uint8_t pattern_y(int x, int y, int w, int h, int frame_idx)
{
    int grad = 48 + ((x * 80) / (w > 1 ? w : 1)) + ((y * 80) / (h > 1 ? h : 1));
    int v = grad + frame_idx * 6;
    if (v > 235) v = 235;
    if (v < 16) v = 16;
    return (uint8_t)v;
}

static int fill_nv12_frame(uint8_t *dst, int cap, int width, int height,
                           int stride, int slice_h, int frame_idx, int *out_mean_y)
{
    if (stride < width) stride = width;
    if (slice_h < height) slice_h = height;
    int y_plane = stride * slice_h;
    int uv_plane = stride * ((height + 1) / 2);
    int need = y_plane + uv_plane;
    if (cap < need) {
        return -1;
    }
    uint64_t sum = 0;
    for (int y = 0; y < height; y++) {
        uint8_t *row = dst + (size_t)y * (size_t)stride;
        for (int x = 0; x < width; x++) {
            uint8_t val = pattern_y(x, y, width, height, frame_idx);
            row[x] = val;
            sum += val;
        }
    }
    uint8_t *uv = dst + (size_t)y_plane;
    int uv_h = (height + 1) / 2;
    for (int y = 0; y < uv_h; y++) {
        uint8_t *row = uv + (size_t)y * (size_t)stride;
        for (int x = 0; x + 1 < width; x += 2) {
            row[x] = 104;
            row[x + 1] = 152;
        }
    }
    if (out_mean_y != NULL) {
        *out_mean_y = (int)(sum / (uint64_t)((uint64_t)width * (uint64_t)height));
    }
    return need;
}

/* ---------------- Encoder callbacks ---------------- */

static void enc_on_error(OH_AVCodec *codec, int32_t errorCode, void *userData)
{
    (void)codec;
    EncCtx *ctx = (EncCtx *)userData;
    pthread_mutex_lock(&ctx->mu);
    if (ctx->err == 0) {
        ctx->err = errorCode != 0 ? errorCode : -1;
        ctx->stage = "enc_cb_error";
    }
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);
}

static void enc_on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format, void *userData)
{
    (void)codec;
    (void)format;
    (void)userData;
}

static void enc_on_need_input(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    (void)codec;
    EncCtx *ctx = (EncCtx *)userData;
    pthread_mutex_lock(&ctx->mu);
    if (!ctx->closing && ctx->in_count < MAX_SLOTS) {
        ctx->in_q[ctx->in_tail].index = index;
        ctx->in_q[ctx->in_tail].buffer = buffer;
        ctx->in_tail = (ctx->in_tail + 1) % MAX_SLOTS;
        ctx->in_count++;
    }
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);
}

static void enc_on_new_output(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    EncCtx *ctx = (EncCtx *)userData;
    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    OH_AVErrCode arc = OH_AVBuffer_GetBufferAttr(buffer, &attr);
    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);

    pthread_mutex_lock(&ctx->mu);
    if (ctx->closing) {
        pthread_mutex_unlock(&ctx->mu);
        (void)OH_VideoEncoder_FreeOutputBuffer(codec, index);
        return;
    }
    if (arc != AV_ERR_OK) {
        if (ctx->err == 0) {
            ctx->err = (int32_t)arc;
            ctx->stage = "enc_out_attr";
        }
    } else {
        if (attr.size > 0 && addr != NULL && ctx->pkt_count < MAX_OUT_PKTS) {
            uint8_t *copy = (uint8_t *)malloc((size_t)attr.size);
            if (copy != NULL) {
                memcpy(copy, addr + attr.offset, (size_t)attr.size);
                EncPacket *ep = &ctx->pkts[ctx->pkt_count++];
                ep->data = copy;
                ep->size = (uint32_t)attr.size;
                ep->flags = attr.flags;
                ep->pts = attr.pts;
                ctx->total_bytes += (uint64_t)attr.size;
                if (attr.flags & AVCODEC_BUFFER_FLAGS_CODEC_DATA) {
                    ctx->csd_pkts++;
                } else {
                    ctx->data_pkts++;
                }
                if (attr.flags & AVCODEC_BUFFER_FLAGS_SYNC_FRAME) {
                    ctx->keyframes++;
                }
            }
        }
        if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
            ctx->out_eos = true;
        }
    }
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);

    OH_AVErrCode frc = OH_VideoEncoder_FreeOutputBuffer(codec, index);
    if (frc != AV_ERR_OK) {
        pthread_mutex_lock(&ctx->mu);
        if (!ctx->closing && ctx->err == 0) {
            ctx->err = (int32_t)frc;
            ctx->stage = "enc_free_out";
        }
        pthread_cond_broadcast(&ctx->cv);
        pthread_mutex_unlock(&ctx->mu);
    }
}

static bool enc_wait_input(EncCtx *ctx, SlotItem *out, const struct timespec *deadline)
{
    pthread_mutex_lock(&ctx->mu);
    while (ctx->in_count == 0 && ctx->err == 0 && !ctx->out_eos) {
        int rc = pthread_cond_timedwait(&ctx->cv, &ctx->mu, deadline);
        if (rc == ETIMEDOUT) {
            if (ctx->err == 0) {
                ctx->err = -ETIMEDOUT;
                ctx->stage = "enc_wait_input";
            }
            break;
        }
    }
    bool ok = (ctx->in_count > 0 && ctx->err == 0);
    if (ok) {
        *out = ctx->in_q[ctx->in_head];
        ctx->in_head = (ctx->in_head + 1) % MAX_SLOTS;
        ctx->in_count--;
    }
    pthread_mutex_unlock(&ctx->mu);
    return ok;
}

/* ---------------- Decoder callbacks ---------------- */

static void dec_on_error(OH_AVCodec *codec, int32_t errorCode, void *userData)
{
    (void)codec;
    DecCtx *ctx = (DecCtx *)userData;
    pthread_mutex_lock(&ctx->mu);
    if (ctx->err == 0) {
        ctx->err = errorCode != 0 ? errorCode : -1;
        ctx->stage = "dec_cb_error";
    }
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);
}

static void dec_on_stream_changed(OH_AVCodec *codec, OH_AVFormat *format, void *userData)
{
    (void)codec;
    DecCtx *ctx = (DecCtx *)userData;
    int32_t w = 0, h = 0, s = 0, sh = 0, pf = -1;
    pthread_mutex_lock(&ctx->mu);
    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &w) && w > 0) ctx->width = w;
    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &h) && h > 0) ctx->height = h;
    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_STRIDE, &s) && s > 0) ctx->stride = s;
    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &sh) && sh > 0) ctx->slice_h = sh;
    if (OH_AVFormat_GetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, &pf)) ctx->pix_fmt = pf;
    pthread_mutex_unlock(&ctx->mu);
}

static void dec_on_need_input(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    (void)codec;
    DecCtx *ctx = (DecCtx *)userData;
    pthread_mutex_lock(&ctx->mu);
    if (!ctx->closing && ctx->in_count < MAX_SLOTS) {
        ctx->in_q[ctx->in_tail].index = index;
        ctx->in_q[ctx->in_tail].buffer = buffer;
        ctx->in_tail = (ctx->in_tail + 1) % MAX_SLOTS;
        ctx->in_count++;
    }
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);
}

static void dec_on_new_output(OH_AVCodec *codec, uint32_t index, OH_AVBuffer *buffer, void *userData)
{
    DecCtx *ctx = (DecCtx *)userData;
    OH_AVCodecBufferAttr attr;
    memset(&attr, 0, sizeof(attr));
    OH_AVErrCode arc = OH_AVBuffer_GetBufferAttr(buffer, &attr);
    uint8_t *addr = OH_AVBuffer_GetAddr(buffer);

    pthread_mutex_lock(&ctx->mu);
    if (ctx->closing) {
        pthread_mutex_unlock(&ctx->mu);
        (void)OH_VideoDecoder_FreeOutputBuffer(codec, index);
        return;
    }
    if (arc != AV_ERR_OK) {
        if (ctx->err == 0) {
            ctx->err = (int32_t)arc;
            ctx->stage = "dec_out_attr";
        }
    } else {
        if (attr.size > 0 && addr != NULL) {
            ctx->frames++;
            ctx->bytes += (uint64_t)attr.size;
            if (ctx->frames == 1) {
                int w = ctx->width;
                int h = ctx->height;
                int s = ctx->stride >= w ? ctx->stride : w;
                const uint8_t *ybase = addr + attr.offset;
                if ((int64_t)s * (int64_t)h <= (int64_t)attr.size) {
                    uint64_t sum = 0;
                    for (int y = 0; y < h; y++) {
                        const uint8_t *row = ybase + (size_t)y * (size_t)s;
                        for (int x = 0; x < w; x++) {
                            sum += row[x];
                        }
                    }
                    ctx->first_y_sum = sum;
                    ctx->first_y_cnt = (uint64_t)((uint64_t)w * (uint64_t)h);
                }
            }
        }
        if (attr.flags & AVCODEC_BUFFER_FLAGS_EOS) {
            ctx->out_eos = true;
        }
    }
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);

    OH_AVErrCode frc = OH_VideoDecoder_FreeOutputBuffer(codec, index);
    if (frc != AV_ERR_OK) {
        pthread_mutex_lock(&ctx->mu);
        if (!ctx->closing && ctx->err == 0) {
            ctx->err = (int32_t)frc;
            ctx->stage = "dec_free_out";
        }
        pthread_cond_broadcast(&ctx->cv);
        pthread_mutex_unlock(&ctx->mu);
    }
}

static bool dec_wait_input(DecCtx *ctx, SlotItem *out, const struct timespec *deadline)
{
    pthread_mutex_lock(&ctx->mu);
    while (ctx->in_count == 0 && ctx->err == 0 && !ctx->out_eos) {
        int rc = pthread_cond_timedwait(&ctx->cv, &ctx->mu, deadline);
        if (rc == ETIMEDOUT) {
            if (ctx->err == 0) {
                ctx->err = -ETIMEDOUT;
                ctx->stage = "dec_wait_input";
            }
            break;
        }
    }
    bool ok = (ctx->in_count > 0 && ctx->err == 0);
    if (ok) {
        *out = ctx->in_q[ctx->in_head];
        ctx->in_head = (ctx->in_head + 1) % MAX_SLOTS;
        ctx->in_count--;
    }
    pthread_mutex_unlock(&ctx->mu);
    return ok;
}

static int run_decode_loopback(const char *mime, const EncCtx *enc, int width, int height,
                               int target_mean_y)
{
    DecCtx dctx;
    memset(&dctx, 0, sizeof(dctx));
    pthread_mutex_init(&dctx.mu, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&dctx.cv, &attr);
    pthread_condattr_destroy(&attr);
    dctx.width = width;
    dctx.height = height;
    dctx.stride = width;
    dctx.slice_h = height;
    dctx.pix_fmt = AV_PIXEL_FORMAT_NV12;

    OH_AVCodec *dec = OH_VideoDecoder_CreateByMime(mime);
    bool started = false;
    struct timespec deadline;
    make_deadline(&deadline, PROBE_TIMEOUT_SEC);

    if (dec == NULL) {
        dctx.err = -1;
        dctx.stage = "dec_create";
        goto done;
    }
    OH_AVFormat *fmt = OH_AVFormat_CreateVideoFormat(mime, width, height);
    if (fmt == NULL) {
        dctx.err = -1;
        dctx.stage = "dec_format";
        goto done;
    }
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVErrCode rc = OH_VideoDecoder_Configure(dec, fmt);
    OH_AVFormat_Destroy(fmt);
    if (rc != AV_ERR_OK) {
        dctx.err = (int32_t)rc;
        dctx.stage = "dec_configure";
        goto done;
    }
    OH_AVCodecCallback cb = {
        .onError = dec_on_error,
        .onStreamChanged = dec_on_stream_changed,
        .onNeedInputBuffer = dec_on_need_input,
        .onNewOutputBuffer = dec_on_new_output,
    };
    rc = OH_VideoDecoder_RegisterCallback(dec, cb, &dctx);
    if (rc != AV_ERR_OK) {
        dctx.err = (int32_t)rc;
        dctx.stage = "dec_cb";
        goto done;
    }
    rc = OH_VideoDecoder_Prepare(dec);
    if (rc != AV_ERR_OK) {
        dctx.err = (int32_t)rc;
        dctx.stage = "dec_prepare";
        goto done;
    }
    rc = OH_VideoDecoder_Start(dec);
    if (rc != AV_ERR_OK) {
        dctx.err = (int32_t)rc;
        dctx.stage = "dec_start";
        goto done;
    }
    started = true;

    for (int i = 0; i < enc->pkt_count; i++) {
        SlotItem slot;
        if (!dec_wait_input(&dctx, &slot, &deadline)) {
            goto done;
        }
        int32_t cap = OH_AVBuffer_GetCapacity(slot.buffer);
        uint8_t *dst = OH_AVBuffer_GetAddr(slot.buffer);
        if (dst == NULL || cap < (int32_t)enc->pkts[i].size) {
            dctx.err = -ENOSPC;
            dctx.stage = "dec_in_cap";
            goto done;
        }
        memcpy(dst, enc->pkts[i].data, enc->pkts[i].size);
        OH_AVCodecBufferAttr battr = {
            .pts = enc->pkts[i].pts,
            .size = (int32_t)enc->pkts[i].size,
            .offset = 0,
            .flags = enc->pkts[i].flags & (uint32_t)(AVCODEC_BUFFER_FLAGS_CODEC_DATA |
                                                     AVCODEC_BUFFER_FLAGS_SYNC_FRAME),
        };
        rc = OH_AVBuffer_SetBufferAttr(slot.buffer, &battr);
        if (rc != AV_ERR_OK) {
            dctx.err = (int32_t)rc;
            dctx.stage = "dec_set_attr";
            goto done;
        }
        rc = OH_VideoDecoder_PushInputBuffer(dec, slot.index);
        if (rc != AV_ERR_OK) {
            dctx.err = (int32_t)rc;
            dctx.stage = "dec_push_in";
            goto done;
        }
    }

    /* Send empty EOS */
    {
        SlotItem slot;
        if (!dec_wait_input(&dctx, &slot, &deadline)) {
            goto done;
        }
        OH_AVCodecBufferAttr battr = {.pts = 0, .size = 0, .offset = 0, .flags = AVCODEC_BUFFER_FLAGS_EOS};
        (void)OH_AVBuffer_SetBufferAttr(slot.buffer, &battr);
        rc = OH_VideoDecoder_PushInputBuffer(dec, slot.index);
        if (rc != AV_ERR_OK) {
            dctx.err = (int32_t)rc;
            dctx.stage = "dec_push_eos";
            goto done;
        }
    }

    pthread_mutex_lock(&dctx.mu);
    while (!dctx.out_eos && dctx.err == 0) {
        int wrc = pthread_cond_timedwait(&dctx.cv, &dctx.mu, &deadline);
        if (wrc == ETIMEDOUT) {
            dctx.err = -ETIMEDOUT;
            dctx.stage = "dec_wait_eos";
            break;
        }
    }
    pthread_mutex_unlock(&dctx.mu);

done:
    pthread_mutex_lock(&dctx.mu);
    dctx.closing = true;
    pthread_mutex_unlock(&dctx.mu);
    if (started) {
        (void)OH_VideoDecoder_Stop(dec);
    }
    if (dec != NULL) {
        (void)OH_VideoDecoder_Destroy(dec);
    }

    int mean_y = (dctx.first_y_cnt > 0) ? (int)(dctx.first_y_sum / dctx.first_y_cnt) : -1;
    int y_diff = (mean_y >= 0) ? abs(mean_y - target_mean_y) : 999;
    int ok = (dctx.err == 0 && dctx.out_eos && dctx.frames == enc->pkt_count && mean_y > 0 && y_diff <= 20) ? 1 : 0;
    printf("VENC_LOOPBACK_RESULT mime=%s dec_frames=%d dec_bytes=%" PRIu64
           " mean_y=%d target_y=%d y_diff=%d eos=%d err=%d stage=%s ok=%d\n",
           mime, dctx.frames, dctx.bytes, mean_y, target_mean_y, y_diff,
           dctx.out_eos ? 1 : 0, dctx.err, dctx.stage ? dctx.stage : "ok", ok);
    fflush(stdout);

    pthread_cond_destroy(&dctx.cv);
    pthread_mutex_destroy(&dctx.mu);
    return ok;
}

static int run_encoder_case(const char *mime, int width, int height, int num_frames,
                            int *out_enc_ok, int *out_loop_ok)
{
    *out_enc_ok = 0;
    *out_loop_ok = 0;

    OH_AVCapability *cap = OH_AVCodec_GetCapabilityByCategory(mime, true, HARDWARE);
    const char *cap_name = cap ? OH_AVCapability_GetName(cap) : "none";
    int cap_hw = cap ? (OH_AVCapability_IsHardware(cap) ? 1 : 0) : 0;
    printf("VENC_CAP mime=%s name=%s hw=%d\n", mime, cap_name ? cap_name : "?", cap_hw);
    fflush(stdout);

    EncCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mime = mime;
    pthread_mutex_init(&ctx.mu, NULL);
    pthread_condattr_t cattr;
    pthread_condattr_init(&cattr);
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    pthread_cond_init(&ctx.cv, &cattr);
    pthread_condattr_destroy(&cattr);

    OH_AVCodec *enc = OH_VideoEncoder_CreateByMime(mime);
    bool started = false;
    int stride = width;
    int slice_h = height;
    int target_mean_y = 128;
    struct timespec deadline;
    make_deadline(&deadline, PROBE_TIMEOUT_SEC);

    if (enc == NULL) {
        ctx.err = -1;
        ctx.stage = "enc_create";
        goto done;
    }

    OH_AVFormat *fmt = OH_AVFormat_CreateVideoFormat(mime, width, height);
    if (fmt == NULL) {
        ctx.err = -1;
        ctx.stage = "enc_format";
        goto done;
    }
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_NV12);
    OH_AVFormat_SetLongValue(fmt, OH_MD_KEY_BITRATE, 2000000);
    OH_AVFormat_SetDoubleValue(fmt, OH_MD_KEY_FRAME_RATE, 30.0);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_VIDEO_ENCODE_BITRATE_MODE, VBR);
    OH_AVFormat_SetIntValue(fmt, OH_MD_KEY_I_FRAME_INTERVAL, 1000);

    OH_AVErrCode rc = OH_VideoEncoder_Configure(enc, fmt);
    OH_AVFormat_Destroy(fmt);
    if (rc != AV_ERR_OK) {
        ctx.err = (int32_t)rc;
        ctx.stage = "enc_configure";
        goto done;
    }

    OH_AVFormat *in_desc = OH_VideoEncoder_GetInputDescription(enc);
    if (in_desc != NULL) {
        int32_t s = 0, sh = 0, pf = -1;
        if (OH_AVFormat_GetIntValue(in_desc, OH_MD_KEY_VIDEO_STRIDE, &s) && s >= width) {
            stride = s;
        }
        if (OH_AVFormat_GetIntValue(in_desc, OH_MD_KEY_VIDEO_SLICE_HEIGHT, &sh) && sh >= height) {
            slice_h = sh;
        }
        (void)OH_AVFormat_GetIntValue(in_desc, OH_MD_KEY_PIXEL_FORMAT, &pf);
        printf("VENC_INPUT_DESC mime=%s w=%d h=%d stride=%d slice_h=%d pix_fmt=%d\n",
               mime, width, height, stride, slice_h, pf);
        fflush(stdout);
        OH_AVFormat_Destroy(in_desc);
    }

    OH_AVCodecCallback cb = {
        .onError = enc_on_error,
        .onStreamChanged = enc_on_stream_changed,
        .onNeedInputBuffer = enc_on_need_input,
        .onNewOutputBuffer = enc_on_new_output,
    };
    rc = OH_VideoEncoder_RegisterCallback(enc, cb, &ctx);
    if (rc != AV_ERR_OK) {
        ctx.err = (int32_t)rc;
        ctx.stage = "enc_cb";
        goto done;
    }
    rc = OH_VideoEncoder_Prepare(enc);
    if (rc != AV_ERR_OK) {
        ctx.err = (int32_t)rc;
        ctx.stage = "enc_prepare";
        goto done;
    }
    rc = OH_VideoEncoder_Start(enc);
    if (rc != AV_ERR_OK) {
        ctx.err = (int32_t)rc;
        ctx.stage = "enc_start";
        goto done;
    }
    started = true;

    for (int f = 0; f < num_frames; f++) {
        if (f == 2) {
            /* Exercise dynamic parameter update (request I-frame) mid-stream. */
            OH_AVFormat *dyn = OH_AVFormat_Create();
            if (dyn != NULL) {
                OH_AVFormat_SetIntValue(dyn, OH_MD_KEY_REQUEST_I_FRAME, 1);
                (void)OH_VideoEncoder_SetParameter(enc, dyn);
                OH_AVFormat_Destroy(dyn);
            }
        }
        SlotItem slot;
        if (!enc_wait_input(&ctx, &slot, &deadline)) {
            goto done;
        }
        int32_t cap_sz = OH_AVBuffer_GetCapacity(slot.buffer);
        uint8_t *dst = OH_AVBuffer_GetAddr(slot.buffer);
        if (dst == NULL) {
            ctx.err = -EINVAL;
            ctx.stage = "enc_in_addr";
            goto done;
        }
        int mean_y = 0;
        int filled = fill_nv12_frame(dst, cap_sz, width, height, stride, slice_h, f,
                                     f == 0 ? &mean_y : NULL);
        if (filled < 0) {
            ctx.err = -ENOSPC;
            ctx.stage = "enc_in_cap";
            goto done;
        }
        if (f == 0) {
            target_mean_y = mean_y;
        }
        OH_AVCodecBufferAttr battr = {
            .pts = (int64_t)f * 33333LL,
            .size = filled,
            .offset = 0,
            .flags = AVCODEC_BUFFER_FLAGS_NONE,
        };
        rc = OH_AVBuffer_SetBufferAttr(slot.buffer, &battr);
        if (rc != AV_ERR_OK) {
            ctx.err = (int32_t)rc;
            ctx.stage = "enc_set_attr";
            goto done;
        }
        rc = OH_VideoEncoder_PushInputBuffer(enc, slot.index);
        if (rc != AV_ERR_OK) {
            ctx.err = (int32_t)rc;
            ctx.stage = "enc_push_in";
            goto done;
        }
    }

    /* Send empty EOS buffer */
    {
        SlotItem slot;
        if (!enc_wait_input(&ctx, &slot, &deadline)) {
            goto done;
        }
        OH_AVCodecBufferAttr battr = {
            .pts = (int64_t)num_frames * 33333LL,
            .size = 0,
            .offset = 0,
            .flags = AVCODEC_BUFFER_FLAGS_EOS,
        };
        (void)OH_AVBuffer_SetBufferAttr(slot.buffer, &battr);
        rc = OH_VideoEncoder_PushInputBuffer(enc, slot.index);
        if (rc != AV_ERR_OK) {
            ctx.err = (int32_t)rc;
            ctx.stage = "enc_push_eos";
            goto done;
        }
    }

    pthread_mutex_lock(&ctx.mu);
    while (!ctx.out_eos && ctx.err == 0) {
        int wrc = pthread_cond_timedwait(&ctx.cv, &ctx.mu, &deadline);
        if (wrc == ETIMEDOUT) {
            ctx.err = -ETIMEDOUT;
            ctx.stage = "enc_wait_eos";
            break;
        }
    }
    pthread_mutex_unlock(&ctx.mu);

done:
    pthread_mutex_lock(&ctx.mu);
    ctx.closing = true;
    pthread_mutex_unlock(&ctx.mu);
    if (started) {
        (void)OH_VideoEncoder_Stop(enc);
    }
    if (enc != NULL) {
        (void)OH_VideoEncoder_Destroy(enc);
    }

    bool annexb_ok = (ctx.pkt_count > 0);
    bool has_param = false;
    bool has_idr = false;
    int total_nals = 0;
    for (int i = 0; i < ctx.pkt_count; i++) {
        bool ab = has_annexb_startcode(ctx.pkts[i].data, ctx.pkts[i].size);
        if (!ab) annexb_ok = false;
        scan_nals(mime, ctx.pkts[i].data, ctx.pkts[i].size, &has_param, &has_idr, &total_nals);
        printf("VENC_OUT mime=%s pkt=%d bytes=%u flags=0x%x pts=%" PRId64 " annexb=%d\n",
               mime, i, ctx.pkts[i].size, ctx.pkts[i].flags, ctx.pkts[i].pts, ab ? 1 : 0);
    }
    int want_kf = (num_frames >= 3) ? 2 : 1;
    int enc_ok = (cap_hw == 1 && ctx.err == 0 && ctx.out_eos && ctx.pkt_count == num_frames &&
                  ctx.total_bytes > 0 && annexb_ok && has_param && has_idr && ctx.keyframes >= want_kf) ? 1 : 0;
    printf("VENC_ENCODE_RESULT mime=%s in_frames=%d out_pkts=%d csd=%d data=%d keyframes=%d"
           " total_bytes=%" PRIu64 " nals=%d param=%d idr=%d annexb=%d eos=%d err=%d stage=%s ok=%d\n",
           mime, num_frames, ctx.pkt_count, ctx.csd_pkts, ctx.data_pkts, ctx.keyframes,
           ctx.total_bytes, total_nals, has_param ? 1 : 0, has_idr ? 1 : 0, annexb_ok ? 1 : 0,
           ctx.out_eos ? 1 : 0, ctx.err, ctx.stage ? ctx.stage : "ok", enc_ok);
    fflush(stdout);
    *out_enc_ok = enc_ok;

    if (enc_ok) {
        *out_loop_ok = run_decode_loopback(mime, &ctx, width, height, target_mean_y);
    }

    for (int i = 0; i < ctx.pkt_count; i++) {
        free(ctx.pkts[i].data);
    }
    pthread_cond_destroy(&ctx.cv);
    pthread_mutex_destroy(&ctx.mu);
    return (enc_ok && *out_loop_ok) ? 0 : 2;
}

int main(int argc, char **argv)
{
    int width = 640;
    int height = 480;
    int frames = 4;
    const char *only_mime = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mime") == 0 && i + 1 < argc) {
            only_mime = argv[++i];
        }
    }

    int avc_enc = 0, avc_loop = 0, hevc_enc = 0, hevc_loop = 0;
    int all_ok = 1;
    if (only_mime == NULL || strcmp(only_mime, "video/avc") == 0) {
        if (run_encoder_case("video/avc", width, height, frames, &avc_enc, &avc_loop) != 0) {
            all_ok = 0;
        }
    } else {
        avc_enc = 1;
        avc_loop = 1;
    }
    if (only_mime == NULL || strcmp(only_mime, "video/hevc") == 0) {
        if (run_encoder_case("video/hevc", width, height, frames, &hevc_enc, &hevc_loop) != 0) {
            all_ok = 0;
        }
    } else {
        hevc_enc = 1;
        hevc_loop = 1;
    }
    printf("MARBLE_VENC_PROBE_SUMMARY width=%d height=%d frames=%d avc_enc=%d avc_loop=%d hevc_enc=%d hevc_loop=%d all_ok=%d\n",
           width, height, frames, avc_enc, avc_loop, hevc_enc, hevc_loop, all_ok);
    return all_ok ? 0 : 2;
}
