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
#include <unistd.h>

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

    printf("MARBLE_HW_CAPTURE_SELF_TEST_OK=1 ae_ok=1 rgb_tl=(%u,%u,%u) rgb_tr=(%u,%u,%u) rgb_bl=(%u,%u,%u) jpeg_bytes=%zu\n",
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
    int af_sweep = 0;
    const char *dump_raw = NULL;
    const char *dump_jpeg = NULL;
    uint32_t out_w = 640, out_h = 480;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--self-test") == 0) {
            self_test = 1;
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
