/* SPDX-License-Identifier: MIT */
/*
 * marble_camera_hw_capture.c - SM7475 real silicon camera capture CLI.
 *
 * Exercises both SM7475 hardware capture paths via marble_isp_capture:
 *   --tpg              : cam-tpg0 (v1.3) -> CSID -> IFE RDI0 -> DMA-BUF
 *   --imx355           : physical Sony IMX355 (Slot 2) -> CSIPHY0 -> CSID -> IFE RDI0
 *   --imx355-pattern N : IMX355 hardware test pattern (0=real optical, 2=8 color bars)
 *   --dump-raw PATH    : write last raw MIPI frame
 *   --dump-jpeg PATH   : write last demosaiced baseline DCT JFIF JPEG
 */

#define _GNU_SOURCE
#include "marble_isp_capture.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void fill_synthetic_raw10_4624x3472(uint8_t *raw, uint32_t w, uint32_t h, uint32_t stride) {
    /* Paint distinct colored bands + top/bottom border markers to verify aspect-crop & Bayer phase:
     *   y < 300 or y >= h - 300 : bright blue border (cropped out in 16:9, visible in 4:3!)
     *   middle left half        : strong red (R=900, G=160, B=160)
     *   middle right half       : strong green (R=160, G=900, B=160) */
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *row = raw + (size_t)y * stride;
        int in_border = (y < 300u || y >= h - 300u);
        uint32_t y_odd = y & 1u;
        for (uint32_t x = 0; x < w; x += 4u) {
            uint16_t p[4];
            for (int k = 0; k < 4; ++k) {
                uint32_t cx = x + (uint32_t)k;
                int is_r = (y_odd == 0u) && ((cx & 1u) == 0u);
                int is_b = (y_odd == 1u) && ((cx & 1u) == 1u);
                if (in_border) {
                    p[k] = is_b ? 920u : 120u;
                } else if (cx < w / 2u) {
                    p[k] = is_r ? 900u : 150u;
                } else {
                    p[k] = (!is_r && !is_b) ? 900u : 150u;
                }
            }
            uint8_t *d = row + (x >> 2) * 5u;
            d[0] = (uint8_t)(p[0] >> 2);
            d[1] = (uint8_t)(p[1] >> 2);
            d[2] = (uint8_t)(p[2] >> 2);
            d[3] = (uint8_t)(p[3] >> 2);
            d[4] = (uint8_t)((p[0] & 3u) | ((p[1] & 3u) << 2) |
                             ((p[2] & 3u) << 4) | ((p[3] & 3u) << 6));
        }
    }
}

static int run_benchmark(void) {
    const uint32_t sw = 4624u, sh = 3472u;
    const uint32_t sstride = ((((sw * 5u) / 4u) + 15u) & ~15u); /* 5792 */
    uint8_t *raw = (uint8_t *)calloc(1, (size_t)sstride * sh);
    uint8_t *nv21_960 = (uint8_t *)calloc(1, 1280u * 960u * 3u / 2u);
    uint8_t *nv21_720 = (uint8_t *)calloc(1, 1280u * 720u * 3u / 2u);
    uint8_t *nv21_1080 = (uint8_t *)calloc(1, 1920u * 1080u * 3u / 2u);
    uint8_t *rgb_1440 = (uint8_t *)calloc(1, 1920u * 1440u * 3u);
    if (!raw || !nv21_960 || !nv21_720 || !nv21_1080 || !rgb_1440) {
        fprintf(stderr, "BENCH OOM\n");
        return 1;
    }
    fill_synthetic_raw10_4624x3472(raw, sw, sh, sstride);

    /* Warm up persistent 4-thread worker pool */
    (void)marble_bayer_to_nv21_resize_ex(raw, sw, sh, sstride, 0, 1, MARBLE_BAYER_RGGB,
                                         0, nv21_960, 1280u, 960u, 1280u);

    const int iters = 20;
    uint64_t t0 = monotonic_ns();
    for (int i = 0; i < iters; ++i) {
        if (marble_bayer_to_nv21_resize_ex(raw, sw, sh, sstride, 0, 1, MARBLE_BAYER_RGGB,
                                           0, nv21_960, 1280u, 960u, 1280u) != 0)
            return 2;
    }
    double ms_960 = (double)(monotonic_ns() - t0) / (1e6 * iters);

    t0 = monotonic_ns();
    for (int i = 0; i < iters; ++i) {
        if (marble_bayer_to_nv21_resize_ex(raw, sw, sh, sstride, 0, 1, MARBLE_BAYER_RGGB,
                                           0, nv21_720, 1280u, 720u, 1280u) != 0)
            return 3;
    }
    double ms_720 = (double)(monotonic_ns() - t0) / (1e6 * iters);

    t0 = monotonic_ns();
    for (int i = 0; i < iters; ++i) {
        if (marble_bayer_to_nv21_resize_ex(raw, sw, sh, sstride, 0, 1, MARBLE_BAYER_RGGB,
                                           0, nv21_1080, 1920u, 1080u, 1920u) != 0)
            return 4;
    }
    double ms_1080 = (double)(monotonic_ns() - t0) / (1e6 * iters);

    t0 = monotonic_ns();
    for (int i = 0; i < iters; ++i) {
        if (marble_bayer_to_rgb24_resize_ex(raw, sw, sh, sstride, 0, 1, MARBLE_BAYER_RGGB,
                                            rgb_1440, 1920u, 1440u) != 0)
            return 5;
    }
    double ms_rgb = (double)(monotonic_ns() - t0) / (1e6 * iters);

    printf("MARBLE_BENCH_OK=1 src=4624x3472_RAW10 iters=%d "
           "nv21_1280x960_ms=%.3f nv21_1280x720_ms=%.3f nv21_1920x1080_ms=%.3f rgb24_1920x1440_ms=%.3f\n",
           iters, ms_960, ms_720, ms_1080, ms_rgb);

    free(raw);
    free(nv21_960);
    free(nv21_720);
    free(nv21_1080);
    free(rgb_1440);
    return 0;
}

static int run_self_test(const char *dump_jpeg) {
    /* 1. Verify MIPI RAW10 unpack exact bit positions */
    uint8_t packed[5] = { 0x40, 0x80, 0xC0, 0xFF, 0xE4 };
    uint16_t out[4] = {0};
    marble_unpack_mipi_raw10_row(packed, out, 4);
    if (out[0] != 0x100 || out[1] != 0x201 || out[2] != 0x302 || out[3] != 0x3FF) {
        fprintf(stderr, "RAW10 unpack fail: %x %x %x %x\n", out[0], out[1], out[2], out[3]);
        return 1;
    }

    /* 2. Synthetic 64x48 RGGB RAW10 gradient + color quadrants -> demosaic -> RGB24 -> NV21 + DCT JPEG */
    uint32_t w = 64, h = 48, stride = 80;
    uint8_t *raw = (uint8_t *)calloc(1, stride * h);
    uint8_t *rgb = (uint8_t *)calloc(1, w * h * 3u);
    uint8_t *nv21 = (uint8_t *)calloc(1, w * h * 3u / 2u);
    uint8_t *jpeg = (uint8_t *)malloc(128 * 1024u);
    if (!raw || !rgb || !nv21 || !jpeg) return 2;

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; x += 4) {
            uint16_t p[4];
            for (int k = 0; k < 4; ++k) {
                uint32_t cx = x + (uint32_t)k;
                int is_r = ((y & 1u) == 0u) && ((cx & 1u) == 0u);
                int is_b = ((y & 1u) == 1u) && ((cx & 1u) == 1u);
                if (cx < w / 2 && y < h / 2)      p[k] = is_r ? 900 : 120;      /* Red */
                else if (cx >= w / 2 && y < h / 2) p[k] = (!is_r && !is_b) ? 900 : 120; /* Green */
                else if (cx < w / 2)               p[k] = is_b ? 900 : 120;      /* Blue */
                else                               p[k] = (uint16_t)(160 + cx * 10); /* Grey ramp */
            }
            uint8_t *d = raw + y * stride + (x / 4) * 5;
            d[0] = (uint8_t)(p[0] >> 2);
            d[1] = (uint8_t)(p[1] >> 2);
            d[2] = (uint8_t)(p[2] >> 2);
            d[3] = (uint8_t)(p[3] >> 2);
            d[4] = (uint8_t)((p[0] & 3) | ((p[1] & 3) << 2) | ((p[2] & 3) << 4) | ((p[3] & 3) << 6));
        }
    }

    int rc = marble_bayer_to_rgb24_resize(raw, w, h, stride, 0, 1, rgb, w, h);
    if (rc != 0) {
        fprintf(stderr, "demosaic fail rc=%d\n", rc);
        return 3;
    }
    marble_rgb24_to_nv21(rgb, w, h, 0, nv21, w);
    size_t jlen = marble_encode_baseline_jpeg_rgb24(rgb, w, h, 85, jpeg, 128 * 1024u);
    if (jlen < 512 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 ||
        jpeg[jlen - 2] != 0xFF || jpeg[jlen - 1] != 0xD9) {
        fprintf(stderr, "JPEG encode fail jlen=%zu\n", jlen);
        return 4;
    }
    if (dump_jpeg) {
        int fd = open(dump_jpeg, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            ssize_t nw = write(fd, jpeg, jlen);
            close(fd);
            if (nw != (ssize_t)jlen) return 5;
        }
    }
    /* 3. Verify closed-loop AE convergence across dark, daylight, clipped & settled scenes */
    const marble_ae_limits_t lim = { 16u, 9960u, 256u, 3968u, 64u };
    uint32_t e = 0, g = 0;
    /* (a) Dark scene (mean=72 -> net=8): should ramp exp to max & boost gain */
    int chg_dark = marble_ae_compute_next(&lim, 2000u, 256u, 72u, 0u, &e, &g);
    if (!chg_dark || e <= 2000u) {
        fprintf(stderr, "AE dark fail chg=%d e=%u g=%u\n", chg_dark, e, g);
        return 6;
    }
    /* (b) Bright daylight (mean=920, clip=250 permille) from high indoor exp+gain -> drops gain first! */
    uint32_t e2 = 9600u, g2 = 1536u;
    for (int step = 0; step < 6; ++step)
        marble_ae_compute_next(&lim, e2, g2, 920u, 250u, &e2, &g2);
    if (g2 != 256u || e2 >= 3000u) {
        fprintf(stderr, "AE daylight fail e2=%u g2=%u\n", e2, g2);
        return 7;
    }
    /* (c) Settled inside deadband (mean=284 -> net=220, clip=0) -> holds steady (0) */
    int chg_hold = marble_ae_compute_next(&lim, 4000u, 256u, 284u, 0u, &e, &g);
    if (chg_hold != 0) {
        fprintf(stderr, "AE deadband fail chg=%d\n", chg_hold);
        return 8;
    }
    /* (d) Highlight clip guard (mean=250 inside deadband, clip=180 permille) -> must pull down */
    int chg_clip = marble_ae_compute_next(&lim, 4000u, 512u, 250u, 180u, &e, &g);
    if (!chg_clip || (uint64_t)e * g >= (uint64_t)4000u * 512u) {
        fprintf(stderr, "AE clip guard fail chg=%d e=%u g=%u\n", chg_clip, e, g);
        return 9;
    }

    /* 4. Verify aspect-preserving center crop alignment (all even Bayer boundaries) */
    uint32_t cx0 = 99, cy0 = 99, cw = 0, ch = 0;
    marble_compute_aspect_crop(4624u, 3472u, 1280u, 720u, &cx0, &cy0, &cw, &ch);
    if (cx0 != 0u || cy0 != 436u || cw != 4624u || ch != 2600u ||
        (cx0 & 1u) || (cy0 & 1u) || (cw & 1u) || (ch & 1u)) {
        fprintf(stderr, "Aspect crop 4:3->16:9 fail: (%u,%u %ux%u)\n", cx0, cy0, cw, ch);
        return 10;
    }
    marble_compute_aspect_crop(1920u, 1080u, 640u, 480u, &cx0, &cy0, &cw, &ch);
    if (cx0 != 240u || cy0 != 0u || cw != 1440u || ch != 1080u) {
        fprintf(stderr, "Aspect crop 16:9->4:3 fail: (%u,%u %ux%u)\n", cx0, cy0, cw, ch);
        return 11;
    }

    /* 5. Verify direct RAW10 -> NV21 / NV12 / RGBA match RGB24 reference across all 4 Bayer orders */
    uint8_t *nv_direct = (uint8_t *)calloc(1, w * h * 3u / 2u);
    uint8_t *nv_ref    = (uint8_t *)calloc(1, w * h * 3u / 2u);
    uint8_t *rgba_dir  = (uint8_t *)calloc(1, w * h * 4u);
    uint8_t *rgba_ref  = (uint8_t *)calloc(1, w * h * 4u);
    if (!nv_direct || !nv_ref || !rgba_dir || !rgba_ref)
        return 12;
    for (int ord = 0; ord < 4; ++ord) {
        marble_bayer_order_t bo = (marble_bayer_order_t)ord;
        if (marble_bayer_to_rgb24_resize_ex(raw, w, h, stride, 0, 1, bo, rgb, w, h) != 0)
            return 13;
        for (int is_nv12 = 0; is_nv12 <= 1; ++is_nv12) {
            marble_rgb24_to_nv21(rgb, w, h, is_nv12, nv_ref, w);
            if (marble_bayer_to_nv21_resize_ex(raw, w, h, stride, 0, 1, bo, is_nv12,
                                               nv_direct, w, h, w) != 0)
                return 14;
            for (size_t idx = 0; idx < (size_t)w * h * 3u / 2u; ++idx) {
                int diff = (int)nv_direct[idx] - (int)nv_ref[idx];
                if (diff < -1 || diff > 1) {
                    fprintf(stderr, "NV21/12 parity fail ord=%d nv12=%d idx=%zu got=%u ref=%u\n",
                            ord, is_nv12, idx, nv_direct[idx], nv_ref[idx]);
                    return 15;
                }
            }
        }
        marble_rgb24_to_rgba(rgb, w, h, rgba_ref, w * 4u);
        if (marble_bayer_to_rgba_resize_ex(raw, w, h, stride, 0, 1, bo, rgba_dir, w, h, w * 4u) != 0)
            return 16;
        if (memcmp(rgba_dir, rgba_ref, (size_t)w * h * 4u) != 0) {
            fprintf(stderr, "RGBA direct parity fail ord=%d\n", ord);
            return 17;
        }
    }
    free(nv_direct);
    free(nv_ref);
    free(rgba_dir);
    free(rgba_ref);

    /* Restore RGGB rgb for stdout check */
    (void)marble_bayer_to_rgb24_resize(raw, w, h, stride, 0, 1, rgb, w, h);

    /* 6. Verify 4624x3472 packed MIPI-RAW10 4-thread direct NV21 (4:3 + 16:9 crop) + RGB24 */
    const uint32_t sw4 = 4624u, sh4 = 3472u, st4 = 5792u;
    uint8_t *raw4 = (uint8_t *)calloc(1, (size_t)st4 * sh4);
    uint8_t *nv960 = (uint8_t *)calloc(1, 1280u * 960u * 3u / 2u);
    uint8_t *nv720 = (uint8_t *)calloc(1, 1280u * 720u * 3u / 2u);
    uint8_t *rgb1440 = (uint8_t *)calloc(1, 1920u * 1440u * 3u);
    if (!raw4 || !nv960 || !nv720 || !rgb1440)
        return 18;
    fill_synthetic_raw10_4624x3472(raw4, sw4, sh4, st4);
    if (marble_bayer_to_nv21_resize_ex(raw4, sw4, sh4, st4, 0, 1, MARBLE_BAYER_RGGB,
                                       0, nv960, 1280u, 960u, 1280u) != 0 ||
        marble_bayer_to_nv21_resize_ex(raw4, sw4, sh4, st4, 0, 1, MARBLE_BAYER_RGGB,
                                       0, nv720, 1280u, 720u, 1280u) != 0 ||
        marble_bayer_to_rgb24_resize_ex(raw4, sw4, sh4, st4, 0, 1, MARBLE_BAYER_RGGB,
                                        rgb1440, 1920u, 1440u) != 0) {
        fprintf(stderr, "4624x3472 4-thread convert fail\n");
        return 19;
    }
    /* In 4:3 (1280x960), row 10 is inside top blue border (U > 160, V < 120).
     * In 16:9 (1280x720, y0=436 > 300), row 10 is inside red/green body (left side red: V > 160)! */
    const uint8_t *uv960_top = nv960 + 1280u * 960u + 5u * 1280u;
    const uint8_t *uv720_top = nv720 + 1280u * 720u + 5u * 1280u;
    uint8_t v960_top = uv960_top[200], u960_top = uv960_top[201];
    uint8_t v720_top = uv720_top[200], u720_top = uv720_top[201];
    /* In 1920x1440 RGB24, left middle is red (R > G + 40) and right middle is green (G > R + 40) */
    const uint8_t *px_l = rgb1440 + (720u * 1920u + 400u) * 3u;
    const uint8_t *px_r = rgb1440 + (720u * 1920u + 1500u) * 3u;
    if (!(u960_top > 155u && v960_top < 125u && v720_top > 155u && u720_top < 125u &&
          px_l[0] > px_l[1] + 40u && px_r[1] > px_r[0] + 40u)) {
        fprintf(stderr, "4624x3472 color/crop verify fail: 4:3_uv=(%u,%u) 16:9_uv=(%u,%u) L=(%u,%u,%u) R=(%u,%u,%u)\n",
                u960_top, v960_top, u720_top, v720_top,
                px_l[0], px_l[1], px_l[2], px_r[0], px_r[1], px_r[2]);
        return 20;
    }
    free(raw4);
    free(nv960);
    free(nv720);
    free(rgb1440);

    /* 7. Verify lightweight CDAF state machine: warmup -> coarse -> fine -> lock -> scene-change re-sweep */
    marble_cdaf_state_t af;
    marble_cdaf_init(&af, 1, 370u);
    uint16_t cur_lens = 370u, prev_lens = 370u, next_dac = 0u;
    const int target_peak_dac = 445; /* coarse best=420, fine {395,420,445} -> peak=445! */
    int locked_frame = -1;
    for (int f = 1; f <= 35; ++f) {
        /* 1-frame pipeline latency: sharpness reflects prev_lens */
        int dist = (int)prev_lens - target_peak_dac;
        if (dist < 0) dist = -dist;
        uint32_t sharp = (dist < 300) ? (uint32_t)(5000 - dist * 12) : 1400u;
        prev_lens = cur_lens;
        if (marble_cdaf_step(&af, sharp, &next_dac)) {
            if (f < 3) {
                fprintf(stderr, "CDAF moved too early on frame %d\n", f);
                return 21;
            }
            cur_lens = next_dac;
        }
        if (af.phase == MARBLE_CDAF_LOCKED && locked_frame < 0)
            locked_frame = f;
    }
    if (af.phase != MARBLE_CDAF_LOCKED || cur_lens != 445u || locked_frame < 0) {
        fprintf(stderr, "CDAF lock fail phase=%d cur_lens=%u locked_frame=%d\n",
                (int)af.phase, cur_lens, locked_frame);
        return 22;
    }
    /* Minor jitter (20% drop -> still >= 65%) must NOT unlock */
    for (int f = 0; f < 20; ++f) {
        if (marble_cdaf_step(&af, 4000u, &next_dac)) {
            fprintf(stderr, "CDAF false re-trigger on minor jitter\n");
            return 23;
        }
    }
    /* Sustained >35% sharpness drop for 15 consecutive frames MUST re-trigger coarse sweep */
    int retriggered = 0;
    for (int f = 0; f < 16; ++f) {
        if (marble_cdaf_step(&af, 2000u, &next_dac)) {
            retriggered = 1;
            break;
        }
    }
    if (!retriggered || af.phase != MARBLE_CDAF_COARSE || next_dac != 240u) {
        fprintf(stderr, "CDAF re-trigger fail retrig=%d phase=%d dac=%u\n",
                retriggered, (int)af.phase, next_dac);
        return 24;
    }

    printf("MARBLE_HW_CAPTURE_SELF_TEST_OK=1 ae_ok=1 cdaf_ok=1 crop_ok=1 nv21_direct_ok=1 "
           "rgb_tl=(%u,%u,%u) rgb_tr=(%u,%u,%u) rgb_bl=(%u,%u,%u) jpeg_bytes=%zu\n",
           rgb[(4 * w + 4) * 3 + 0], rgb[(4 * w + 4) * 3 + 1], rgb[(4 * w + 4) * 3 + 2],
           rgb[(4 * w + 48) * 3 + 0], rgb[(4 * w + 48) * 3 + 1], rgb[(4 * w + 48) * 3 + 2],
           rgb[(36 * w + 4) * 3 + 0], rgb[(36 * w + 4) * 3 + 1], rgb[(36 * w + 4) * 3 + 2],
           jlen);
    free(raw);
    free(rgb);
    free(nv21);
    free(jpeg);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    marble_hw_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.src = MARBLE_HW_SRC_TPG;
    int frames = 3;
    int self_test = 0;
    int benchmark = 0;
    int af_sweep = 0;
    const char *dump_raw = NULL;
    const char *dump_jpeg = NULL;
    uint32_t out_w = 640, out_h = 480;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            self_test = 1;
        } else if (strcmp(argv[i], "--benchmark") == 0) {
            benchmark = 1;
        } else if (strcmp(argv[i], "--tpg") == 0) {
            cfg.src = MARBLE_HW_SRC_TPG;
        } else if (strcmp(argv[i], "--ov64b40") == 0) {
            cfg.src = MARBLE_HW_SRC_OV64B40;
        } else if (strcmp(argv[i], "--s5k3p9") == 0) {
            cfg.src = MARBLE_HW_SRC_S5K3P9;
        } else if (strcmp(argv[i], "--imx355") == 0) {
            cfg.src = MARBLE_HW_SRC_IMX355;
        } else if (strcmp(argv[i], "--gc02m1") == 0) {
            cfg.src = MARBLE_HW_SRC_GC02M1;
        } else if (strcmp(argv[i], "--820x616") == 0) {
            cfg.width = 820;
            cfg.height = 616;
        } else if (strcmp(argv[i], "--1920x1080") == 0) {
            cfg.width = 1920;
            cfg.height = 1080;
        } else if (strcmp(argv[i], "--4624x3472") == 0) {
            cfg.width = 4624;
            cfg.height = 3472;
        } else if ((strcmp(argv[i], "--imx355-pattern") == 0 || strcmp(argv[i], "--pattern") == 0) && i + 1 < argc) {
            if (strcmp(argv[i], "--imx355-pattern") == 0)
                cfg.src = MARBLE_HW_SRC_IMX355;
            cfg.test_pattern = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            cfg.exposure_lines = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--gain") == 0 && i + 1 < argc) {
            cfg.analog_gain = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--no-ae") == 0) {
            cfg.ae_disable = 1;
        } else if (strcmp(argv[i], "--focus") == 0 && i + 1 < argc) {
            cfg.focus_dac = (uint16_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--af-sweep") == 0) {
            af_sweep = 1;
            if (frames < 5) frames = 5;
        } else if (strcmp(argv[i], "--raw8") == 0) {
            cfg.use_raw8 = 1;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump-raw") == 0 && i + 1 < argc) {
            dump_raw = argv[++i];
        } else if (strcmp(argv[i], "--dump-jpeg") == 0 && i + 1 < argc) {
            dump_jpeg = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            (void)sscanf(argv[++i], "%ux%u", &cfg.width, &cfg.height);
        } else if (strcmp(argv[i], "--jpeg-size") == 0 && i + 1 < argc) {
            (void)sscanf(argv[++i], "%ux%u", &out_w, &out_h);
        }
    }
    if (self_test)
        return run_self_test(dump_jpeg);
    if (benchmark)
        return run_benchmark();

    const char *mode_name = marble_hw_src_name(cfg.src);
    marble_bayer_order_t order = MARBLE_BAYER_RGGB;
    if (cfg.src == MARBLE_HW_SRC_S5K3P9) order = MARBLE_BAYER_GRBG;
    else if (cfg.src == MARBLE_HW_SRC_GC02M1) order = MARBLE_BAYER_GBRG;

    char err[160] = {0};
    marble_hw_session_t *sess = NULL;
    int rc = marble_hw_session_open(&cfg, &sess, err, sizeof(err));
    if (rc != 0) {
        printf("ERR session_open mode=%s msg=%s\n", mode_name, err);
        return 1;
    }

    uint32_t w = 0, h = 0, stride = 0, raw_bytes = 0;
    marble_hw_session_get_geometry(sess, &w, &h, &stride, &raw_bytes);
    printf("HW_SESSION_OPEN mode=%s pattern=%d size=%ux%u stride=%u raw_bytes=%u frames=%d act=%d\n",
           mode_name, cfg.test_pattern, w, h, stride, raw_bytes, frames,
           marble_hw_session_actuator_ok(sess));

    uint8_t *raw = (uint8_t *)malloc(raw_bytes);
    int captured = 0;
    uint32_t prev_csum = 0;
    int csum_changes = 0;
    size_t last_jpeg_bytes = 0;
    marble_raw_frame_info_t last_info;
    memset(&last_info, 0, sizeof(last_info));
    static const uint16_t k_af_dacs[5] = { 320u, 370u, 450u, 550u, 370u };

    for (int f = 0; f < frames; ++f) {
        if (af_sweep && marble_hw_session_actuator_ok(sess)) {
            uint16_t d = k_af_dacs[f % 5];
            (void)marble_hw_session_set_focus(sess, d);
        }
        marble_raw_frame_info_t info;
        int wrc = marble_hw_session_grab_raw(sess, raw, raw_bytes, 3000u, &info);
        last_info = info;
        printf("FRAME req=%u sync_rc=%d non_sentinel=%u/%u non_zero=%u uniq=%u "
               "raw10_min=%u raw10_max=%u raw10_mean=%u clip=%u sharp=%u "
               "ae_exp=%u ae_gain=%u ae_upd=%u act=%u dac=%u csum=0x%08x\n",
               info.req_id, wrc, info.non_sentinel, info.raw_bytes, info.non_zero,
               info.uniq_bytes, info.raw10_min, info.raw10_max, info.raw10_mean,
               info.clip_permille, info.sharpness, info.ae_exp_lines, info.ae_gain_q8,
               info.ae_updates, (unsigned)info.actuator_ok, (unsigned)info.focus_dac, info.csum);
        if (wrc == 0 && info.non_sentinel > info.raw_bytes / 2u && info.uniq_bytes >= 4u) {
            if (captured > 0 && info.csum != prev_csum)
                csum_changes++;
            prev_csum = info.csum;
            captured++;
            if (f == frames - 1 && raw) {
                if (dump_raw) {
                    int fd = open(dump_raw, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) {
                        ssize_t nw = write(fd, raw, raw_bytes);
                        close(fd);
                        printf("DUMPED_RAW path=%s bytes=%zd\n", dump_raw, nw);
                    }
                }
                if (dump_jpeg) {
                    uint8_t *rgb = (uint8_t *)malloc((size_t)out_w * out_h * 3u);
                    uint8_t *jpg = (uint8_t *)malloc(512 * 1024u);
                    if (rgb && jpg) {
                        int drc = marble_bayer_to_rgb24_resize_ex(
                            raw, w, h, stride, cfg.use_raw8,
                            (cfg.src != MARBLE_HW_SRC_TPG && cfg.test_pattern == 0) ? 1 : 0,
                            order, rgb, out_w, out_h);
                        if (drc == 0) {
                            last_jpeg_bytes = marble_encode_baseline_jpeg_rgb24(
                                rgb, out_w, out_h, 85, jpg, 512 * 1024u);
                            if (last_jpeg_bytes > 0) {
                                int fd = open(dump_jpeg, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                                if (fd >= 0) {
                                    ssize_t nw = write(fd, jpg, last_jpeg_bytes);
                                    close(fd);
                                    printf("DUMPED_JPEG path=%s size=%ux%u bytes=%zd head=%02x%02x%02x%02x tail=%02x%02x\n",
                                           dump_jpeg, out_w, out_h, nw,
                                           jpg[0], jpg[1], jpg[2], jpg[3],
                                           jpg[last_jpeg_bytes - 2], jpg[last_jpeg_bytes - 1]);
                                }
                            }
                        }
                    }
                    free(rgb);
                    free(jpg);
                }
            }
        }
    }

    marble_hw_session_close(sess);
    free(raw);

    printf("MARBLE_HW_CAPTURE_SUMMARY mode=%s pattern=%d frames_ok=%d/%d width=%u height=%u "
           "stride=%u csum_changes=%d ae_exp=%u ae_gain=%u ae_upd=%u act=%u dac=%u sharp=%u jpeg_bytes=%zu\n",
           mode_name, cfg.test_pattern, captured, frames, w, h, stride, csum_changes,
           last_info.ae_exp_lines, last_info.ae_gain_q8, last_info.ae_updates,
           (unsigned)last_info.actuator_ok, (unsigned)last_info.focus_dac,
           last_info.sharpness, last_jpeg_bytes);
    return (captured >= 1) ? 0 : 10;
}
