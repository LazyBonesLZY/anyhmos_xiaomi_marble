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
    uint32_t width;          /* 0 -> sensor default (OV64B40:4624x3472 [explicit 1920x1080], S5K3P9:2304x1728, IMX355:1640x1232/820x616, GC02M1:1600x1200, TPG:640x480) */
    uint32_t height;
    int use_raw8;            /* TPG only: 0=RAW10 (default), 1=RAW8 */
    int test_pattern;        /* 0=real optical sensor, >0=sensor hardware test pattern */
    uint32_t exposure_lines; /* 0 -> closed-loop AE (initial default + live CCI updates) */
    uint32_t analog_gain;    /* 0 -> closed-loop AE (initial default + live CCI updates) */
    int ae_disable;          /* 1 -> disable closed-loop AE even if exposure/gain == 0 */
    uint16_t focus_dac;      /* OV64B40 GT9764 VCM initial DAC (0 -> enable CDAF autofocus, start hyperfocal 370) */
} marble_hw_config_t;

typedef struct marble_ae_limits {
    uint32_t min_exp;
    uint32_t max_exp;
    uint32_t min_gain_q8;    /* 256 = 1.0x */
    uint32_t max_gain_q8;    /* e.g. 4096 = 16.0x */
    uint32_t black_level;    /* 10-bit black pedestal (typically 64) */
} marble_ae_limits_t;

typedef enum marble_cdaf_phase {
    MARBLE_CDAF_DISABLED = 0,
    MARBLE_CDAF_WARMUP   = 1,
    MARBLE_CDAF_COARSE   = 2,
    MARBLE_CDAF_FINE     = 3,
    MARBLE_CDAF_LOCKED   = 4,
} marble_cdaf_phase_t;

typedef struct marble_cdaf_state {
    marble_cdaf_phase_t phase;
    uint32_t frame_idx;
    uint32_t step_idx;
    uint32_t settle_skip;
    uint32_t best_sharp;
    uint32_t locked_sharp;
    uint32_t drop_frames;
    uint16_t cur_dac;
    uint16_t best_dac;
    uint16_t fine_dacs[3];
} marble_cdaf_state_t;

typedef struct marble_raw_view {
    const uint8_t *raw;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t raw_bytes;
    int is_raw8;
    int is_optical;
    marble_bayer_order_t order;
    uint32_t awb_gain_r;     /* Q8 smoothed session AWB gains (0 -> auto compute) */
    uint32_t awb_gain_g;
    uint32_t awb_gain_b;
    int slot;                /* internal DMA-BUF ring slot index */
} marble_raw_view_t;

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

/* Zero-copy hardware frame acquisition: waits for next DMA-BUF frame, runs AE/CDAF/AWB,
 * and exposes a direct read-only pointer into the completed DMA-BUF slot (no 20MB memcpy).
 * Caller MUST pair every successful call (rc == 0) with marble_hw_session_release_raw_view(). */
int marble_hw_session_acquire_raw_view(marble_hw_session_t *sess,
                                       uint64_t timeout_ms,
                                       marble_raw_frame_info_t *out_info,
                                       marble_raw_view_t *out_view);

void marble_hw_session_release_raw_view(marble_hw_session_t *sess,
                                        marble_raw_view_t *view);

/* Convenience helper: grabs next silicon frame and converts directly to RGB24 (dst_w x dst_h). */
int marble_hw_session_grab_rgb24(marble_hw_session_t *sess,
                                 uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h,
                                 uint64_t timeout_ms,
                                 marble_raw_frame_info_t *out_info);

/* Convenience helper: grabs next silicon frame and converts directly to NV21/NV12. */
int marble_hw_session_grab_nv21(marble_hw_session_t *sess,
                                int is_nv12,
                                uint8_t *dst_yuv, uint32_t dst_w, uint32_t dst_h,
                                uint32_t dst_stride,
                                uint64_t timeout_ms,
                                marble_raw_frame_info_t *out_info);

void marble_hw_session_get_geometry(const marble_hw_session_t *sess,
                                    uint32_t *width, uint32_t *height,
                                    uint32_t *stride, uint32_t *raw_bytes);

/* Immediately updates sensor exposure_lines and analog gain (Q8, 256=1.0x) over CCI I2C. */
int marble_hw_session_set_exposure(marble_hw_session_t *sess,
                                   uint32_t exp_lines, uint32_t gain_q8);

/* Moves GT9764 VCM autofocus lens (OV64B40 slot 0) to target DAC (clamped 128..896).
 * Non-blocking (wait_ms=0); disables CDAF while in manual focus position. */
int marble_hw_session_set_focus(marble_hw_session_t *sess, uint16_t dac);

/* Enables or disables GT9764 CDAF autofocus; trigger_rescan!=0 restarts a coarse sweep. */
int marble_hw_session_set_af_mode(marble_hw_session_t *sess, int enable_cdaf, int trigger_rescan);

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

/* Initializes and steps lightweight non-blocking CDAF autofocus state machine.
 * marble_cdaf_step returns 1 if *out_next_dac should be written to GT9764 VCM. */
void marble_cdaf_init(marble_cdaf_state_t *st, int enabled, uint16_t init_dac);
void marble_cdaf_trigger_rescan(marble_cdaf_state_t *st);
int marble_cdaf_step(marble_cdaf_state_t *st, uint32_t sharpness, uint16_t *out_next_dac);

/* Computes centered aspect-preserving crop aligned to 2-pixel Bayer quad boundaries. */
void marble_compute_aspect_crop(uint32_t src_w, uint32_t src_h,
                                uint32_t dst_w, uint32_t dst_h,
                                uint32_t *out_x0, uint32_t *out_y0,
                                uint32_t *out_w, uint32_t *out_h);

void marble_unpack_mipi_raw10_row(const uint8_t *src, uint16_t *dst, uint32_t width);

/* Zero-malloc, aspect-preserving, 4-thread direct packed MIPI-RAW10/RAW8 -> NV21/NV12. */
int marble_bayer_to_nv21_resize_ex(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                   uint32_t src_stride, int is_raw8, int is_optical,
                                   marble_bayer_order_t order, int is_nv12,
                                   uint8_t *dst_yuv, uint32_t dst_w, uint32_t dst_h,
                                   uint32_t dst_stride);

int marble_bayer_to_nv21_resize_awb(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                    uint32_t src_stride, int is_raw8, int is_optical,
                                    marble_bayer_order_t order, int is_nv12,
                                    uint32_t gain_r, uint32_t gain_g, uint32_t gain_b,
                                    uint8_t *dst_yuv, uint32_t dst_w, uint32_t dst_h,
                                    uint32_t dst_stride);

/* Zero-malloc, aspect-preserving, 4-thread direct packed MIPI-RAW10/RAW8 -> RGBA8888. */
int marble_bayer_to_rgba_resize_ex(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                   uint32_t src_stride, int is_raw8, int is_optical,
                                   marble_bayer_order_t order,
                                   uint8_t *dst_rgba, uint32_t dst_w, uint32_t dst_h,
                                   uint32_t dst_stride_bytes);

int marble_bayer_to_rgba_resize_awb(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                    uint32_t src_stride, int is_raw8, int is_optical,
                                    marble_bayer_order_t order,
                                    uint32_t gain_r, uint32_t gain_g, uint32_t gain_b,
                                    uint8_t *dst_rgba, uint32_t dst_w, uint32_t dst_h,
                                    uint32_t dst_stride_bytes);

/* Converts MIPI RAW10 (or RAW8 if is_raw8!=0) Bayer frame into RGB24 resized
 * to (dst_w x dst_h) with aspect-preserving crop, black-level subtraction, AWB & sRGB LUT. */
int marble_bayer_to_rgb24_resize_awb(const uint8_t *raw, uint32_t src_w, uint32_t src_h,
                                     uint32_t src_stride, int is_raw8, int is_optical,
                                     marble_bayer_order_t order,
                                     uint32_t gain_r, uint32_t gain_g, uint32_t gain_b,
                                     uint8_t *dst_rgb, uint32_t dst_w, uint32_t dst_h);

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
