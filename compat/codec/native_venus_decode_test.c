/*
 * Live SM7475 hardware Venus V4L2 loopback verifier (/dev/video33 -> /dev/video32).
 * Tests H.264 and HEVC encode + decode over DMA-BUF without any framework dependency.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "venus_v4l2.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/dma-buf.h>

#define TEST_W 256u
#define TEST_H 256u
#define TEST_FRAMES 4u
#define MAX_AUS 16u

struct coded_au {
    uint8_t *data;
    uint32_t len;
    int64_t ts_us;
};

static uint8_t pattern_y(uint32_t x, uint32_t y)
{
    return (uint8_t)(48u + (((x + y) * 160u) / 512u));
}

static void fill_pattern_nv12(uint8_t *buf, uint32_t stride, uint32_t y_lines,
                              uint32_t w, uint32_t h, uint32_t frame_idx)
{
    uint8_t shift = (uint8_t)(frame_idx * 4u);
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = buf + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++)
            row[x] = (uint8_t)(pattern_y(x, y) + shift);
    }
    uint8_t *uv = buf + (size_t)stride * y_lines;
    for (uint32_t y = 0; y < h / 2u; y++) {
        uint8_t *row = uv + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x += 2u) {
            row[x] = 96u;
            row[x + 1u] = 160u;
        }
    }
}

static bool has_annexb_startcode(const uint8_t *p, uint32_t len)
{
    for (uint32_t i = 0; i + 3u < len && i < 64u; i++) {
        if (p[i] == 0 && p[i + 1] == 0 &&
            (p[i + 2] == 1 || (i + 4u <= len && p[i + 2] == 0 && p[i + 3] == 1)))
            return true;
    }
    return false;
}

static int drain_enc_events(struct venus_ctx *ctx, struct coded_au *aus, uint32_t *au_cnt,
                            bool *saw_last)
{
    for (;;) {
        uint32_t idx = 0, bytes = 0, flags = 0;
        int64_t ts = 0;
        int rc = venus_ctx_dq_output(ctx, &idx, &bytes, &ts, &flags);
        if (rc == -EAGAIN)
            break;
        if (rc < 0)
            return rc;
        if (bytes > 0 && *au_cnt < MAX_AUS) {
            (void)venus_dmabuf_sync(ctx, ctx->out_bufs[idx].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
            uint8_t *copy = malloc(bytes);
            if (!copy)
                return -ENOMEM;
            memcpy(copy, ctx->out_bufs[idx].map, bytes);
            (void)venus_dmabuf_sync(ctx, ctx->out_bufs[idx].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            aus[*au_cnt].data = copy;
            aus[*au_cnt].len = bytes;
            aus[*au_cnt].ts_us = ts;
            (*au_cnt)++;
        }
        (void)venus_ctx_requeue_output(ctx, idx);
        if ((flags & V4L2_BUF_FLAG_LAST) || bytes == 0)
            *saw_last = true;
    }
    for (;;) {
        uint32_t in_idx = 0;
        if (venus_ctx_dq_input(ctx, &in_idx) < 0)
            break;
    }
    return 0;
}

static int run_codec_loopback(const char *tag, uint32_t coded_fcc, const char *dump_dir)
{
    struct venus_ctx enc;
    struct coded_au aus[MAX_AUS];
    memset(aus, 0, sizeof(aus));
    uint32_t au_cnt = 0;

    uint32_t lin_stride = TEST_W;
    size_t lin_sz = (size_t)lin_stride * TEST_H * 3u / 2u;
    uint8_t *src_nv12 = malloc(lin_sz);
    uint8_t *dec_nv12 = malloc(lin_sz);
    if (!src_nv12 || !dec_nv12) {
        free(src_nv12);
        free(dec_nv12);
        return -ENOMEM;
    }

    int rc = venus_ctx_open(&enc, VENUS_ROLE_ENCODER, coded_fcc, V4L2_PIX_FMT_NV12,
                            TEST_W, TEST_H, NULL);
    if (rc < 0) {
        printf("VENUS_TEST_%s_ENC_OPEN_ERR rc=%d\n", tag, rc);
        goto out;
    }
    rc = venus_ctx_configure(&enc, TEST_W, TEST_H, 30u, 2000000u, 1u,
                             V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, -1, -1);
    if (rc < 0) {
        printf("VENUS_TEST_%s_ENC_CFG_ERR rc=%d\n", tag, rc);
        venus_ctx_close(&enc);
        goto out;
    }
    rc = venus_ctx_start(&enc);
    if (rc < 0) {
        printf("VENUS_TEST_%s_ENC_START_ERR rc=%d\n", tag, rc);
        venus_ctx_close(&enc);
        goto out;
    }

    bool saw_last = false;
    for (uint32_t f = 0; f < TEST_FRAMES; f++) {
        int slot = -1;
        for (int tries = 0; tries < 50; tries++) {
            slot = venus_ctx_acquire_free_input(&enc);
            if (slot >= 0)
                break;
            uint32_t ev = 0;
            (void)venus_ctx_poll(&enc, 50, &ev);
            (void)drain_enc_events(&enc, aus, &au_cnt, &saw_last);
        }
        if (slot < 0) {
            rc = -ETIMEDOUT;
            printf("VENUS_TEST_%s_ENC_NOSLOT\n", tag);
            venus_ctx_close(&enc);
            goto out;
        }
        fill_pattern_nv12(src_nv12, lin_stride, TEST_H, TEST_W, TEST_H, f);
        (void)venus_dmabuf_sync(&enc, enc.in_bufs[slot].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        rc = venus_linear_src_to_nv12(src_nv12, lin_sz, lin_stride, TEST_H,
                                      (uint8_t *)enc.in_bufs[slot].map,
                                      enc.fmt.uv_offset, enc.fmt.y_stride,
                                      enc.in_bufs[slot].size, TEST_W, TEST_H, false);
        (void)venus_dmabuf_sync(&enc, enc.in_bufs[slot].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        if (rc < 0) {
            printf("VENUS_TEST_%s_ENC_PACK_ERR rc=%d\n", tag, rc);
            venus_ctx_close(&enc);
            goto out;
        }
        rc = venus_ctx_queue_input(&enc, (uint32_t)slot, enc.fmt.in_buf_size,
                                   (int64_t)f * 33333LL, 0u);
        if (rc < 0) {
            printf("VENUS_TEST_%s_ENC_QBUF_ERR rc=%d\n", tag, rc);
            venus_ctx_close(&enc);
            goto out;
        }
    }
    rc = venus_ctx_send_eos(&enc);
    if (rc < 0) {
        printf("VENUS_TEST_%s_ENC_EOS_ERR rc=%d\n", tag, rc);
        venus_ctx_close(&enc);
        goto out;
    }
    for (int i = 0; i < 60 && !saw_last; i++) {
        uint32_t ev = 0;
        (void)venus_ctx_poll(&enc, 50, &ev);
        rc = drain_enc_events(&enc, aus, &au_cnt, &saw_last);
        if (rc < 0)
            break;
        if (ev & VENUS_POLL_EVENT) {
            struct v4l2_event v4ev;
            while (venus_ctx_dq_event(&enc, &v4ev) == 0) {
                if (v4ev.type == V4L2_EVENT_EOS)
                    saw_last = true;
            }
        }
    }
    venus_ctx_close(&enc);

    if (au_cnt == 0 || aus[0].len <= 64u || !has_annexb_startcode(aus[0].data, aus[0].len)) {
        printf("VENUS_TEST_%s_ENC_FAIL aus=%u len0=%u\n",
               tag, au_cnt, au_cnt ? aus[0].len : 0u);
        rc = -EPROTO;
        goto out;
    }
    printf("VENUS_TEST_%s_ENC_OK aus=%u len0=%u\n", tag, au_cnt, aus[0].len);

    if (dump_dir && dump_dir[0]) {
        const char *ext = (coded_fcc == V4L2_PIX_FMT_H264) ? "h264" : "hevc";
        char path[512];
        snprintf(path, sizeof(path), "%s/test.%s", dump_dir, ext);
        FILE *fp = fopen(path, "wb");
        if (!fp || fwrite(aus[0].data, 1, aus[0].len, fp) != aus[0].len) {
            if (fp) fclose(fp);
            printf("VENUS_DUMP_AU_FAIL path=%s\n", path);
            rc = -EIO;
            goto out;
        }
        fclose(fp);
        printf("VENUS_DUMP_AU_OK path=%s len=%u\n", path, aus[0].len);
    }

    /* Now hardware-decode those AUs via /dev/video32 */
    struct venus_ctx dec;
    rc = venus_ctx_open(&dec, VENUS_ROLE_DECODER, coded_fcc, V4L2_PIX_FMT_NV12,
                        TEST_W, TEST_H, NULL);
    if (rc < 0) {
        printf("VENUS_TEST_%s_DEC_OPEN_ERR rc=%d\n", tag, rc);
        goto out;
    }
    rc = venus_ctx_configure(&dec, TEST_W, TEST_H, 30u, 0u, 0u, -1, -1, -1);
    if (rc < 0) {
        printf("VENUS_TEST_%s_DEC_CFG_ERR rc=%d\n", tag, rc);
        venus_ctx_close(&dec);
        goto out;
    }
    rc = venus_ctx_start(&dec);
    if (rc < 0) {
        printf("VENUS_TEST_%s_DEC_START_ERR rc=%d\n", tag, rc);
        venus_ctx_close(&dec);
        goto out;
    }

    uint32_t dec_frames = 0;
    double best_mae_y = 999.0, best_mae_uv = 999.0;
    saw_last = false;

    for (uint32_t i = 0; i < au_cnt; i++) {
        int slot = -1;
        for (int tries = 0; tries < 40; tries++) {
            slot = venus_ctx_acquire_free_input(&dec);
            if (slot >= 0)
                break;
            uint32_t ev = 0;
            (void)venus_ctx_poll(&dec, 50, &ev);
            uint32_t in_idx = 0;
            while (venus_ctx_dq_input(&dec, &in_idx) == 0) {}
        }
        if (slot < 0) {
            rc = -ETIMEDOUT;
            venus_ctx_close(&dec);
            goto out;
        }
        (void)venus_dmabuf_sync(&dec, dec.in_bufs[slot].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
        memcpy(dec.in_bufs[slot].map, aus[i].data, aus[i].len);
        (void)venus_dmabuf_sync(&dec, dec.in_bufs[slot].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        rc = venus_ctx_queue_input(&dec, (uint32_t)slot, aus[i].len, aus[i].ts_us, 0u);
        if (rc < 0) {
            printf("VENUS_TEST_%s_DEC_QBUF_ERR rc=%d\n", tag, rc);
            venus_ctx_close(&dec);
            goto out;
        }
        if (!dec.streaming_out) {
            uint32_t ev = 0;
            (void)venus_ctx_poll(&dec, (i == 0) ? 30 : 0, &ev);
            if (ev & VENUS_POLL_EVENT) {
                struct v4l2_event v4ev;
                while (venus_ctx_dq_event(&dec, &v4ev) == 0) {
                    if (v4ev.type == V4L2_EVENT_SOURCE_CHANGE)
                        (void)venus_ctx_handle_source_change(&dec);
                }
            }
        }
    }
    for (int sc_tries = 0; sc_tries < 40 && !dec.streaming_out; sc_tries++) {
        uint32_t ev = 0;
        (void)venus_ctx_poll(&dec, 25, &ev);
        if (ev & VENUS_POLL_EVENT) {
            struct v4l2_event v4ev;
            while (venus_ctx_dq_event(&dec, &v4ev) == 0) {
                if (v4ev.type == V4L2_EVENT_SOURCE_CHANGE)
                    (void)venus_ctx_handle_source_change(&dec);
            }
        }
    }
    if (!dec.streaming_out) {
        printf("VENUS_TEST_%s_DEC_NO_SOURCE_CHANGE\n", tag);
        venus_ctx_close(&dec);
        rc = -ETIMEDOUT;
        goto out;
    }
    rc = venus_ctx_send_eos(&dec);
    if (rc < 0) {
        printf("VENUS_TEST_%s_DEC_EOS_ERR rc=%d\n", tag, rc);
        venus_ctx_close(&dec);
        goto out;
    }

    for (int iter = 0; iter < 80 && !saw_last; iter++) {
        uint32_t ev = 0;
        (void)venus_ctx_poll(&dec, 50, &ev);
        if (ev & VENUS_POLL_EVENT) {
            struct v4l2_event v4ev;
            while (venus_ctx_dq_event(&dec, &v4ev) == 0) {
                if (v4ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    (void)venus_ctx_handle_source_change(&dec);
                } else if (v4ev.type == V4L2_EVENT_EOS) {
                    saw_last = true;
                }
            }
        }
        for (;;) {
            uint32_t idx = 0, bytes = 0, flags = 0;
            int64_t ts = 0;
            int dq = venus_ctx_dq_output(&dec, &idx, &bytes, &ts, &flags);
            if (dq == -EAGAIN)
                break;
            if (dq < 0) {
                rc = dq;
                break;
            }
            if (bytes > 0) {
                (void)venus_dmabuf_sync(&dec, dec.out_bufs[idx].fd,
                                        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
                rc = venus_nv12_to_linear_dst((const uint8_t *)dec.out_bufs[idx].map,
                                              dec.fmt.uv_offset, dec.fmt.y_stride,
                                              dec_nv12, lin_sz, lin_stride, TEST_H,
                                              dec.fmt.crop_left, dec.fmt.crop_top,
                                              dec.fmt.crop_width, dec.fmt.crop_height, false);
                (void)venus_dmabuf_sync(&dec, dec.out_bufs[idx].fd,
                                        DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                if (rc == 0 && dec_frames == 0) {
                    fill_pattern_nv12(src_nv12, lin_stride, TEST_H, TEST_W, TEST_H, 0u);
                    double sy = 0.0, suv = 0.0;
                    for (uint32_t p = 0; p < TEST_W * TEST_H; p++)
                        sy += fabs((double)dec_nv12[p] - (double)src_nv12[p]);
                    for (uint32_t p = TEST_W * TEST_H; p < lin_sz; p++)
                        suv += fabs((double)dec_nv12[p] - (double)src_nv12[p]);
                    best_mae_y = sy / (double)(TEST_W * TEST_H);
                    best_mae_uv = suv / (double)(TEST_W * TEST_H / 2u);
                }
                dec_frames++;
            }
            (void)venus_ctx_requeue_output(&dec, idx);
            if ((flags & V4L2_BUF_FLAG_LAST) || bytes == 0)
                saw_last = true;
        }
        uint32_t in_idx = 0;
        while (venus_ctx_dq_input(&dec, &in_idx) == 0) {}
    }

    uint32_t cw = dec.fmt.crop_width;
    uint32_t ch = dec.fmt.crop_height;
    uint32_t y_str = dec.fmt.y_stride;
    uint32_t out_sz = dec.fmt.out_buf_size;
    venus_ctx_close(&dec);

    if (dec_frames == 0 || cw != TEST_W || ch != TEST_H ||
        best_mae_y >= 16.0 || best_mae_uv >= 16.0) {
        printf("VENUS_TEST_%s_DEC_FAIL frames=%u crop=%ux%u mae_y=%.2f mae_uv=%.2f ystride=%u size=%u\n",
               tag, dec_frames, cw, ch, best_mae_y, best_mae_uv, y_str, out_sz);
        rc = -EPROTO;
        goto out;
    }
    printf("VENUS_TEST_%s_DEC_OK frames=%u crop=%ux%u mae_y=%.2f mae_uv=%.2f ystride=%u size=%u\n",
           tag, dec_frames, cw, ch, best_mae_y, best_mae_uv, y_str, out_sz);
    rc = 0;

out:
    for (uint32_t i = 0; i < au_cnt; i++)
        free(aus[i].data);
    free(src_nv12);
    free(dec_nv12);
    return rc;
}

int main(int argc, char **argv)
{
    const char *dump_dir = NULL;
    if (argc == 3 && strcmp(argv[1], "--dump-au") == 0)
        dump_dir = argv[2];
    else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--dump-au <dir>]\n", argv[0]);
        return 2;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    int r1 = run_codec_loopback("H264", V4L2_PIX_FMT_H264, dump_dir);
    int r2 = run_codec_loopback("HEVC", V4L2_PIX_FMT_HEVC, dump_dir);
    if (r1 == 0 && r2 == 0) {
        printf("VENUS_LOOPBACK_ALL_OK\n");
        return 0;
    }
    printf("VENUS_LOOPBACK_FAIL r1=%d r2=%d\n", r1, r2);
    return 1;
}
