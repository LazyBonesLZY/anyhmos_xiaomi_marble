/* SPDX-License-Identifier: MIT */
/*
 * marble_isp_capture.h - Clean-room SM7475 (Xiaomi marble) real silicon
 * camera capture & ISP/DSP pipeline (RAW10 -> Bayer demosaic -> NV21 / DCT JPEG).
 *
 * Hardware pipelines supported on live SM7475 Spectra / Titan camera-kernel:
 *   - MARBLE_HW_SRC_IMX355: Physical Sony IMX355 (Slot 2, 0x34, 19.2MHz MCLK)
 *                           -> CSIPHY0 (4-lane D-PHY 720Mbps/lane)
 *                           -> CSID -> IFE RDI0 -> IOMMU -> DMA-BUF + cam_sync
 *   - MARBLE_HW_SRC_TPG:    SM7475 hardware cam-tpg (v1.3)
 *                           -> CSID -> IFE RDI0 -> IOMMU -> DMA-BUF + cam_sync
 */
#ifndef MARBLE_ISP_CAPTURE_H
#define MARBLE_ISP_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum marble_hw_src {
    MARBLE_HW_SRC_TPG = 0,
    MARBLE_HW_SRC_IMX355 = 1,  /* Slot 2: Rear Ultra 8MP (0x34), CSIPHY0, 4-lane D-PHY */
    MARBLE_HW_SRC_GC02M1 = 2,  /* Slot 3: Rear Macro 2MP (0x6e), CSIPHY1, 1-lane D-PHY */
    MARBLE_HW_SRC_S5K3P9 = 3,  /* Slot 1: Front 16MP (0x20), CSIPHY3, 4-lane D-PHY */
    MARBLE_HW_SRC_OV64B40 = 4, /* Slot 0: Rear Wide 64MP (0x20), CSIPHY2, 3-trio C-PHY */
} marble_hw_src_t;

typedef enum marble_bayer_order {
    MARBLE_BAYER_RGGB = 0,
    MARBLE_BAYER_GRBG = 1,
    MARBLE_BAYER_BGGR = 2,
    MARBLE_BAYER_GBRG = 3,
} marble_bayer_order_t;

typedef struct marble_hw_config {
    marble_hw_src_t src;
    uint32_t width;          /* 0 -> sensor default (OV64B40:1920x1080/4624x3472, S5K3P9:2304x1728, IMX355:1640x1232/820x616, GC02M1:1600x1200, TPG:640x480) */
    uint32_t height;
    int use_raw8;            /* TPG only: 0=RAW10 (default), 1=RAW8 */
    int test_pattern;        /* 0=real optical sensor, >0=sensor hardware test pattern */
    uint32_t exposure_lines; /* 0 -> closed-loop AE (initial default + live CCI updates) */
    uint32_t analog_gain;    /* 0 -> closed-loop AE (initial default + live CCI updates) */
    int ae_disable;          /* 1 -> disable closed-loop AE even if exposure/gain == 0 */
    uint16_t focus_dac;      /* OV64B40 GT9764 VCM initial DAC (0 -> default hyperfocal 370) */
} marble_hw_config_t;

typedef struct marble_ae_limits {
    uint32_t min_exp;
    uint32_t max_exp;
    uint32_t min_gain_q8;    /* 256 = 1.0x */
    uint32_t max_gain_q8;    /* e.g. 4096 = 16.0x */
    uint32_t black_level;    /* 10-bit black pedestal (typically 64) */
} marble_ae_limits_t;

const char *marble_hw_src_name(marble_hw_src_t src);

typedef struct marble_raw_frame_info {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t raw_bytes;
    uint32_t is_raw8;
    uint32_t req_id;
    uint32_t csum;
    uint32_t non_sentinel;
    uint32_t non_zero;
    uint32_t uniq_bytes;
    uint16_t raw10_min;
    uint16_t raw10_max;
    uint32_t raw10_mean;     /* 10-bit average (0..1023) */
    uint32_t clip_permille;  /* permille (0..1000) of sampled pixels >= 980 */
    uint32_t sharpness;      /* center-ROI contrast/gradient focus metric */
    uint32_t ae_exp_lines;   /* active sensor exposure lines */
    uint32_t ae_gain_q8;     /* active sensor analog gain in Q8 (256=1.0x) */
    uint32_t ae_updates;     /* cumulative live CCI AE register updates applied */
    uint16_t focus_dac;      /* active GT9764 VCM DAC (0 if no actuator) */
    uint8_t actuator_ok;     /* 1 if GT9764 VCM actuator acquired & initialized */
    uint8_t reserved_pad;
} marble_raw_frame_info_t;

typedef struct marble_hw_session marble_hw_session_t;

/* Opens and starts a live SM7475 hardware capture session (4-slot DMA-BUF ring). */
int marble_hw_session_open(const marble_hw_config_t *cfg,
                           marble_hw_session_t **out_sess,
                           char *err_buf, size_t err_sz);

/* Waits for next completed silicon frame, copies raw payload into out_raw (if non-NULL,
 * capacity >= stride*height), fills *out_info, and re-queues the DMA-BUF slot. */
int marble_hw_session_grab_raw(marble_hw_session_t *sess,
                               uint8_t *out_raw, size_t out_raw_cap,
                               uint64_t timeout_ms,
                               marble_raw_frame_info_t *out_info);

/* Convenience helper: grabs next silicon frame and converts directly to RGB24 (dst_w x dst_h). */
int marble_hw_session_grab_rgb24(marble_hw_session_t *sess,
                                 uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h,
                                 uint64_t timeout_ms,
                                 marble_raw_frame_info_t *out_info);

void marble_hw_session_get_geometry(const marble_hw_session_t *sess,
                                    uint32_t *width, uint32_t *height,
                                    uint32_t *stride, uint32_t *raw_bytes);

/* Immediately updates sensor exposure_lines and analog gain (Q8, 256=1.0x) over CCI I2C. */
int marble_hw_session_set_exposure(marble_hw_session_t *sess,
                                   uint32_t exp_lines, uint32_t gain_q8);

/* Moves GT9764 VCM autofocus lens (OV64B40 slot 0) to target DAC (clamped 128..896). */
int marble_hw_session_set_focus(marble_hw_session_t *sess, uint16_t dac);

int marble_hw_session_actuator_ok(const marble_hw_session_t *sess);

/* Cleanly stops sensor/TPG, CSIPHY, ISP, parks & powers down actuator, releases all DMA-BUFs. */
void marble_hw_session_close(marble_hw_session_t *sess);

/* ---- Pure host-testable 3A / DSP / Baseline DCT JPEG functions ---- */

/* Computes next damped closed-loop AE (exposure_lines, gain_q8) from 10-bit frame stats.
 * Returns 1 if an I2C update should be applied, 0 if within target deadband or unchanged. */
int marble_ae_compute_next(const marble_ae_limits_t *lim,
                           uint32_t cur_exp, uint32_t cur_gain_q8,
                           uint32_t raw10_mean, uint32_t clip_permille,
                           uint32_t *out_exp, uint32_t *out_gain_q8);

void marble_unpack_mipi_raw10_row(const uint8_t *src, uint16_t *dst, uint32_t width);

/* Converts MIPI RAW10 (or RAW8 if is_raw8!=0) Bayer frame into RGB24 resized
 * to (dst_w x dst_h) with black-level subtraction, grey-world AWB, auto-gain & gamma. */
int marble_bayer_to_rgb24_resize_ex(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                    uint32_t src_stride, int is_raw8, int is_optical,
                                    marble_bayer_order_t order,
                                    uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h);

int marble_bayer_to_rgb24_resize(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                 uint32_t src_stride, int is_raw8, int is_optical,
                                 uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h);

/* Converts RGB24 (w x h) to NV21 (Y plane + interleaved VU plane, total w*h*3/2 bytes)
 * or NV12 if nv12 != 0. */
void marble_rgb24_to_nv21(const uint8_t *rgb, uint32_t w, uint32_t h,
                          int nv12, uint8_t *dst_yuv, uint32_t dst_stride);

/* Converts RGB24 (w x h) to RGBA8888. */
void marble_rgb24_to_rgba(const uint8_t *rgb, uint32_t w, uint32_t h,
                          uint8_t *dst_rgba, uint32_t dst_stride_bytes);

/* Full 2D 8x8 Baseline DCT + Annex K DC+AC Huffman YCbCr 4:2:0 JFIF JPEG encoder.
 * Writes encoded JPEG into out_buf (capacity out_cap). Returns encoded byte count,
 * or 0 on error/overflow. */
size_t marble_encode_baseline_jpeg_rgb24(const uint8_t *rgb, uint32_t w, uint32_t h,
                                         int quality, uint8_t *out_buf, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* MARBLE_ISP_CAPTURE_H */
