/*
 * End-to-end OHOS ImageSource DMA HEIC hardware decode probe for SM7475 (marble).
 *
 * Generates valid single (hvc1) and 2x2 grid (grid) HEIC containers using Venus
 * /dev/video33 hardware HEVC encoding, decodes them through OHOS NDK ImageSource
 * (OH_ImageSourceNative_CreatePixelmapUsingAllocator with IMAGE_ALLOCATOR_TYPE_DMA),
 * and validates pixel/quadrant fidelity across RGBA_8888, NV12, and NV21.
 * Copyright (C) 2026 Marble HMOS Bringup Project (clean-room MIT).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HEIF_PROBE_HOST_TEST
#include "venus_v4l2.h"
#include <linux/dma-buf.h>
#include <multimedia/image_framework/image/image_common.h>
#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>
#endif

#define TILE_W 256u
#define TILE_H 256u

struct dyn_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool err;
};

static void db_ensure(struct dyn_buf *b, size_t add)
{
    if (b->err)
        return;
    if (b->len + add <= b->cap)
        return;
    size_t ncap = b->cap ? b->cap * 2u : 4096u;
    while (ncap < b->len + add)
        ncap *= 2u;
    uint8_t *p = (uint8_t *)realloc(b->data, ncap);
    if (!p) {
        b->err = true;
        return;
    }
    b->data = p;
    b->cap = ncap;
}

static void db_u8(struct dyn_buf *b, uint8_t v)
{
    db_ensure(b, 1u);
    if (!b->err)
        b->data[b->len++] = v;
}

static void db_be16(struct dyn_buf *b, uint16_t v)
{
    db_u8(b, (uint8_t)(v >> 8));
    db_u8(b, (uint8_t)(v & 0xFFu));
}

static void db_be32(struct dyn_buf *b, uint32_t v)
{
    db_u8(b, (uint8_t)(v >> 24));
    db_u8(b, (uint8_t)((v >> 16) & 0xFFu));
    db_u8(b, (uint8_t)((v >> 8) & 0xFFu));
    db_u8(b, (uint8_t)(v & 0xFFu));
}

static void db_bytes(struct dyn_buf *b, const void *p, size_t n)
{
    if (!n)
        return;
    db_ensure(b, n);
    if (!b->err) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

static void db_zeros(struct dyn_buf *b, size_t n)
{
    if (!n)
        return;
    db_ensure(b, n);
    if (!b->err) {
        memset(b->data + b->len, 0, n);
        b->len += n;
    }
}

static void db_fourcc(struct dyn_buf *b, const char tag[4])
{
    db_bytes(b, tag, 4u);
}

static void write_be32_at(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static size_t begin_box(struct dyn_buf *b, const char tag[4])
{
    size_t pos = b->len;
    db_be32(b, 0u);
    db_fourcc(b, tag);
    return pos;
}

static size_t begin_fullbox(struct dyn_buf *b, const char tag[4], uint8_t ver, uint32_t flags)
{
    size_t pos = begin_box(b, tag);
    db_be32(b, ((uint32_t)ver << 24) | (flags & 0xFFFFFFu));
    return pos;
}

static void end_box(struct dyn_buf *b, size_t pos)
{
    if (!b->err && pos + 8u <= b->len)
        write_be32_at(b->data + pos, (uint32_t)(b->len - pos));
}

struct hevc_au_parts {
    const uint8_t *vps;
    uint16_t vps_len;
    const uint8_t *sps;
    uint16_t sps_len;
    const uint8_t *pps;
    uint16_t pps_len;
    uint8_t *lenpref_slices;
    size_t lenpref_len;
};

static int split_annexb_hevc(const uint8_t *au, size_t len, struct hevc_au_parts *out)
{
    if (!au || len < 6u || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    struct dyn_buf slices = {NULL, 0u, 0u, false};
    size_t i = 0;
    while (i + 3u < len) {
        size_t sc_len = 0;
        if (i + 4u <= len && au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1)
            sc_len = 4u;
        else if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1)
            sc_len = 3u;
        if (!sc_len) {
            i++;
            continue;
        }
        size_t nalu_start = i + sc_len;
        size_t next = nalu_start;
        while (next + 3u <= len) {
            if (au[next] == 0 && au[next + 1] == 0 &&
                (au[next + 2] == 1 ||
                 (next + 4u <= len && au[next + 2] == 0 && au[next + 3] == 1)))
                break;
            next++;
        }
        if (next + 3u > len)
            next = len;
        if (next > nalu_start) {
            size_t nlen = next - nalu_start;
            uint8_t nal_type = (uint8_t)((au[nalu_start] >> 1) & 0x3Fu);
            if (nal_type == 32u && !out->vps && nlen <= 0xFFFFu) {
                out->vps = au + nalu_start;
                out->vps_len = (uint16_t)nlen;
            } else if (nal_type == 33u && !out->sps && nlen <= 0xFFFFu) {
                out->sps = au + nalu_start;
                out->sps_len = (uint16_t)nlen;
            } else if (nal_type == 34u && !out->pps && nlen <= 0xFFFFu) {
                out->pps = au + nalu_start;
                out->pps_len = (uint16_t)nlen;
            } else {
                db_be32(&slices, (uint32_t)nlen);
                db_bytes(&slices, au + nalu_start, nlen);
            }
        }
        i = next;
    }

    if (slices.err || !out->vps || !out->sps || !out->pps || slices.len < 6u) {
        free(slices.data);
        return -1;
    }
    out->lenpref_slices = slices.data;
    out->lenpref_len = slices.len;
    return 0;
}

static void emit_hvcc_box(struct dyn_buf *b, const struct hevc_au_parts *hdr)
{
    size_t pos = begin_box(b, "hvcC");
    db_u8(b, 1u); /* configurationVersion */

    /* Extract 13 bytes starting at byte 2 of SPS RBSP (unescaping 0x03 after 00 00) */
    uint8_t rbsp[16] = {0};
    size_t rcnt = 0;
    if (hdr->sps && hdr->sps_len > 2u) {
        int zeros = 0;
        for (size_t k = 2u; k < hdr->sps_len && rcnt < sizeof(rbsp); k++) {
            uint8_t c = hdr->sps[k];
            if (zeros == 2 && c == 0x03u) {
                zeros = 0;
                continue;
            }
            rbsp[rcnt++] = c;
            zeros = (c == 0u) ? (zeros + 1) : 0;
        }
    }

    if (rcnt >= 13u) {
        db_u8(b, rbsp[1]);              /* profile_space | tier_flag | profile_idc */
        db_bytes(b, rbsp + 2, 4u);      /* general_profile_compatibility_flags */
        db_bytes(b, rbsp + 6, 6u);      /* general_constraint_indicator_flags */
        db_u8(b, rbsp[12]);             /* general_level_idc */
    } else {
        db_u8(b, 0x01u);
        db_be32(b, 0x60000000u);
        db_u8(b, 0x90u);
        db_zeros(b, 5u);
        db_u8(b, 90u);
    }
    db_be16(b, 0xF000u);     /* min_spatial_segmentation_idc */
    db_u8(b, 0xFCu);         /* parallelismType */
    db_u8(b, 0xFDu);         /* chromaFormat = 1 (4:2:0) */
    db_u8(b, 0xF8u);         /* bitDepthLumaMinus8 = 0 */
    db_u8(b, 0xF8u);         /* bitDepthChromaMinus8 = 0 */
    db_be16(b, 0u);          /* avgFrameRate */
    db_u8(b, 0x0Fu);         /* numTemporalLayers=1, temporalIdNested=1, lengthSizeMinusOne=3 (4 bytes) */
    db_u8(b, 3u);            /* numOfArrays: VPS, SPS, PPS */

    /* VPS (32) */
    db_u8(b, 0xA0u);
    db_be16(b, 1u);
    db_be16(b, hdr->vps_len);
    db_bytes(b, hdr->vps, hdr->vps_len);

    /* SPS (33) */
    db_u8(b, 0xA1u);
    db_be16(b, 1u);
    db_be16(b, hdr->sps_len);
    db_bytes(b, hdr->sps, hdr->sps_len);

    /* PPS (34) */
    db_u8(b, 0xA2u);
    db_be16(b, 1u);
    db_be16(b, hdr->pps_len);
    db_bytes(b, hdr->pps, hdr->pps_len);

    end_box(b, pos);
}

static void emit_ispe_box(struct dyn_buf *b, uint32_t w, uint32_t h)
{
    size_t pos = begin_fullbox(b, "ispe", 0u, 0u);
    db_be32(b, w);
    db_be32(b, h);
    end_box(b, pos);
}

static void emit_pixi_box(struct dyn_buf *b)
{
    size_t pos = begin_fullbox(b, "pixi", 0u, 0u);
    db_u8(b, 3u);
    db_u8(b, 8u);
    db_u8(b, 8u);
    db_u8(b, 8u);
    end_box(b, pos);
}

static void emit_infe_box(struct dyn_buf *b, uint16_t item_id, uint32_t flags, const char type4[4])
{
    size_t pos = begin_fullbox(b, "infe", 2u, flags);
    db_be16(b, item_id);
    db_be16(b, 0u); /* item_protection_index */
    db_fourcc(b, type4);
    db_u8(b, 0u);   /* empty item_name */
    end_box(b, pos);
}

static int build_heic_container(const struct hevc_au_parts *hdr,
                                const struct hevc_au_parts *tiles, uint32_t num_tiles,
                                uint32_t tile_w, uint32_t tile_h,
                                bool is_grid, uint32_t cols, uint32_t rows,
                                uint32_t disp_w, uint32_t disp_h,
                                uint8_t **out_buf, size_t *out_len)
{
    if (!hdr || !tiles || num_tiles == 0u || !out_buf || !out_len)
        return -1;
    if (!is_grid && num_tiles != 1u)
        return -1;
    if (is_grid && num_tiles != cols * rows)
        return -1;

    struct dyn_buf b = {NULL, 0u, 0u, false};

    /* ftyp */
    size_t ftyp_pos = begin_box(&b, "ftyp");
    db_fourcc(&b, "heic");
    db_be32(&b, 0u);
    db_fourcc(&b, "mif1");
    db_fourcc(&b, "heic");
    end_box(&b, ftyp_pos);

    /* meta */
    size_t meta_pos = begin_fullbox(&b, "meta", 0u, 0u);

    /* hdlr */
    size_t hdlr_pos = begin_fullbox(&b, "hdlr", 0u, 0u);
    db_be32(&b, 0u);
    db_fourcc(&b, "pict");
    db_zeros(&b, 12u);
    db_u8(&b, 0u);
    end_box(&b, hdlr_pos);

    /* pitm */
    uint16_t primary_id = is_grid ? (uint16_t)(num_tiles + 1u) : 1u;
    size_t pitm_pos = begin_fullbox(&b, "pitm", 0u, 0u);
    db_be16(&b, primary_id);
    end_box(&b, pitm_pos);

    /* iinf */
    size_t iinf_pos = begin_fullbox(&b, "iinf", 0u, 0u);
    db_be16(&b, is_grid ? (uint16_t)(num_tiles + 1u) : 1u);
    for (uint32_t t = 0; t < num_tiles; t++)
        emit_infe_box(&b, (uint16_t)(t + 1u), is_grid ? 1u : 0u, "hvc1");
    if (is_grid)
        emit_infe_box(&b, primary_id, 0u, "grid");
    end_box(&b, iinf_pos);

    /* iref (dimg for grid) */
    if (is_grid) {
        size_t iref_pos = begin_fullbox(&b, "iref", 0u, 0u);
        size_t dimg_pos = begin_box(&b, "dimg");
        db_be16(&b, primary_id);
        db_be16(&b, (uint16_t)num_tiles);
        for (uint32_t t = 0; t < num_tiles; t++)
            db_be16(&b, (uint16_t)(t + 1u));
        end_box(&b, dimg_pos);
        end_box(&b, iref_pos);
    }

    /* iprp (ipco + ipma) */
    size_t iprp_pos = begin_box(&b, "iprp");
    size_t ipco_pos = begin_box(&b, "ipco");
    emit_ispe_box(&b, tile_w, tile_h); /* prop 1 */
    emit_hvcc_box(&b, hdr);            /* prop 2 */
    emit_pixi_box(&b);                 /* prop 3 */
    if (is_grid)
        emit_ispe_box(&b, disp_w, disp_h); /* prop 4 */
    end_box(&b, ipco_pos);

    size_t ipma_pos = begin_fullbox(&b, "ipma", 0u, 0u);
    db_be32(&b, is_grid ? (num_tiles + 1u) : 1u);
    for (uint32_t t = 0; t < num_tiles; t++) {
        db_be16(&b, (uint16_t)(t + 1u));
        db_u8(&b, 3u);
        db_u8(&b, 0x80u | 1u);
        db_u8(&b, 0x80u | 2u);
        db_u8(&b, 0x80u | 3u);
    }
    if (is_grid) {
        db_be16(&b, primary_id);
        db_u8(&b, 2u);
        db_u8(&b, 0x80u | 4u);
        db_u8(&b, 0x80u | 3u);
    }
    end_box(&b, ipma_pos);
    end_box(&b, iprp_pos);

    /* idat (grid item payload = 8-byte ImageGrid) */
    if (is_grid) {
        size_t idat_pos = begin_box(&b, "idat");
        db_u8(&b, 0u); /* version */
        db_u8(&b, 0u); /* flags (output_width/height are 16-bit) */
        db_u8(&b, (uint8_t)(rows - 1u));
        db_u8(&b, (uint8_t)(cols - 1u));
        db_be16(&b, (uint16_t)disp_w);
        db_be16(&b, (uint16_t)disp_h);
        end_box(&b, idat_pos);
    }

    /* iloc (v1): record patch positions for mdat item offsets */
    size_t iloc_patch_off[16];
    size_t iloc_pos = begin_fullbox(&b, "iloc", 1u, 0u);
    db_u8(&b, 0x44u); /* offset_size=4, length_size=4 */
    db_u8(&b, 0x00u); /* base_offset_size=0, index_size=0 */
    db_be16(&b, is_grid ? (uint16_t)(num_tiles + 1u) : 1u);
    for (uint32_t t = 0; t < num_tiles; t++) {
        db_be16(&b, (uint16_t)(t + 1u));
        db_be16(&b, 0u); /* construction_method = 0 (file offset) */
        db_be16(&b, 0u); /* data_reference_index */
        db_be16(&b, 1u); /* extent_count */
        iloc_patch_off[t] = b.len;
        db_be32(&b, 0u);
        db_be32(&b, (uint32_t)tiles[t].lenpref_len);
    }
    if (is_grid) {
        db_be16(&b, primary_id);
        db_be16(&b, 1u); /* construction_method = 1 (idat offset) */
        db_be16(&b, 0u);
        db_be16(&b, 1u);
        db_be32(&b, 0u); /* offset 0 inside idat */
        db_be32(&b, 8u); /* 8-byte ImageGrid */
    }
    end_box(&b, iloc_pos);
    end_box(&b, meta_pos);

    /* mdat */
    size_t mdat_pos = begin_box(&b, "mdat");
    for (uint32_t t = 0; t < num_tiles; t++) {
        uint32_t abs_off = (uint32_t)b.len;
        write_be32_at(b.data + iloc_patch_off[t], abs_off);
        db_bytes(&b, tiles[t].lenpref_slices, tiles[t].lenpref_len);
    }
    end_box(&b, mdat_pos);

    if (b.err) {
        free(b.data);
        return -1;
    }
    *out_buf = b.data;
    *out_len = b.len;
    return 0;
}

#ifdef HEIF_PROBE_HOST_TEST
#include <assert.h>

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int main(void)
{
    /* Minimal synthetic Annex-B HEVC AU with VPS(32), SPS(33), PPS(34), IDR(19) */
    const uint8_t fake_au[] = {
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01,
        0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x00, 0x5A,
        0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xC1, 0x73,
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAA, 0xBB, 0xCC, 0xDD
    };
    struct hevc_au_parts parts[4];
    for (int i = 0; i < 4; i++)
        assert(split_annexb_hevc(fake_au, sizeof(fake_au), &parts[i]) == 0);
    assert(parts[0].lenpref_len == 10u); /* 4-byte length + 6-byte IDR */
    assert(rd_be32(parts[0].lenpref_slices) == 6u);

    uint8_t *single = NULL, *grid = NULL;
    size_t single_len = 0, grid_len = 0;
    assert(build_heic_container(&parts[0], &parts[0], 1u, 256u, 256u, false, 1u, 1u,
                                256u, 256u, &single, &single_len) == 0);
    assert(single_len > 128u && memcmp(single + 4, "ftypheic", 8) == 0);

    /* Walk top-level boxes: ftyp, meta, mdat */
    size_t off = 0;
    uint32_t s0 = rd_be32(single + off);
    assert(memcmp(single + off + 4, "ftyp", 4) == 0);
    off += s0;
    uint32_t s1 = rd_be32(single + off);
    assert(memcmp(single + off + 4, "meta", 4) == 0);
    off += s1;
    uint32_t s2 = rd_be32(single + off);
    assert(memcmp(single + off + 4, "mdat", 4) == 0);
    assert(off + s2 == single_len);
    assert(rd_be32(single + off + 8) == 6u);

    assert(build_heic_container(&parts[0], parts, 4u, 256u, 256u, true, 2u, 2u,
                                400u, 360u, &grid, &grid_len) == 0);
    assert(grid_len > single_len);

    free(single);
    free(grid);
    for (int i = 0; i < 4; i++)
        free(parts[i].lenpref_slices);
    printf("HEIF_PROBE_HOST_TEST_OK\n");
    return 0;
}

#else /* Target device OHOS NDK + Venus HW encoder probe */

static void fill_solid_nv12(uint8_t *dst, uint32_t stride, uint32_t y_lines,
                            uint32_t w, uint32_t h, uint8_t y, uint8_t u, uint8_t v)
{
    for (uint32_t r = 0; r < h; r++)
        memset(dst + (size_t)r * stride, y, w);
    uint8_t *uv = dst + (size_t)stride * y_lines;
    for (uint32_t r = 0; r < h / 2u; r++) {
        uint8_t *row = uv + (size_t)r * stride;
        for (uint32_t c = 0; c < w; c += 2u) {
            row[c + 0u] = u;
            row[c + 1u] = v;
        }
    }
}

static int encode_hevc_idr_tile(uint8_t y_val, uint8_t u_val, uint8_t v_val,
                                uint8_t **out_au, uint32_t *out_len)
{
    struct venus_ctx enc;
    memset(&enc, 0, sizeof(enc));
    enc.fd = -1;
    enc.wake_efd = -1;

    uint32_t lin_stride = TILE_W;
    size_t lin_sz = (size_t)lin_stride * TILE_H * 3u / 2u;
    uint8_t *src_nv12 = (uint8_t *)malloc(lin_sz);
    if (!src_nv12)
        return -ENOMEM;
    fill_solid_nv12(src_nv12, lin_stride, TILE_H, TILE_W, TILE_H, y_val, u_val, v_val);

    int rc = venus_ctx_open(&enc, VENUS_ROLE_ENCODER, V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_NV12,
                            TILE_W, TILE_H, NULL);
    if (rc < 0) {
        free(src_nv12);
        return rc;
    }
    rc = venus_ctx_configure(&enc, TILE_W, TILE_H, 30u, 2000000u, 1u,
                             V4L2_MPEG_VIDEO_BITRATE_MODE_VBR, -1, -1);
    if (rc < 0 || (rc = venus_ctx_start(&enc)) < 0) {
        venus_ctx_close(&enc);
        free(src_nv12);
        return rc;
    }

    int slot = venus_ctx_acquire_free_input(&enc);
    if (slot < 0) {
        venus_ctx_close(&enc);
        free(src_nv12);
        return -EBUSY;
    }

    (void)venus_dmabuf_sync(&enc, enc.in_bufs[slot].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    rc = venus_linear_src_to_nv12(src_nv12, lin_sz, lin_stride, TILE_H,
                                  (uint8_t *)enc.in_bufs[slot].map,
                                  enc.fmt.uv_offset, enc.fmt.y_stride,
                                  enc.in_bufs[slot].size, TILE_W, TILE_H, false);
    (void)venus_dmabuf_sync(&enc, enc.in_bufs[slot].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
    free(src_nv12);
    if (rc < 0) {
        venus_ctx_close(&enc);
        return rc;
    }

    rc = venus_ctx_queue_input(&enc, (uint32_t)slot, enc.fmt.in_buf_size, 0LL, 0u);
    if (rc == 0)
        rc = venus_ctx_send_eos(&enc);
    if (rc < 0) {
        venus_ctx_close(&enc);
        return rc;
    }

    uint8_t *au = NULL;
    uint32_t au_len = 0;
    bool saw_last = false;
    for (int i = 0; i < 60 && !saw_last; i++) {
        uint32_t ev = 0;
        (void)venus_ctx_poll(&enc, 50, &ev);
        for (;;) {
            uint32_t idx = 0, bytes = 0, flags = 0;
            int64_t ts = 0;
            int dq = venus_ctx_dq_output(&enc, &idx, &bytes, &ts, &flags);
            if (dq == -EAGAIN)
                break;
            if (dq < 0) {
                rc = dq;
                break;
            }
            if (bytes > 0u && !au) {
                (void)venus_dmabuf_sync(&enc, enc.out_bufs[idx].fd,
                                        DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
                au = (uint8_t *)malloc(bytes);
                if (au) {
                    memcpy(au, enc.out_bufs[idx].map, bytes);
                    au_len = bytes;
                }
                (void)venus_dmabuf_sync(&enc, enc.out_bufs[idx].fd,
                                        DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            }
            (void)venus_ctx_requeue_output(&enc, idx);
            if ((flags & V4L2_BUF_FLAG_LAST) || bytes == 0u)
                saw_last = true;
        }
        uint32_t in_idx = 0;
        while (venus_ctx_dq_input(&enc, &in_idx) == 0) {}
        if (ev & VENUS_POLL_EVENT) {
            struct v4l2_event v4ev;
            while (venus_ctx_dq_event(&enc, &v4ev) == 0) {
                if (v4ev.type == V4L2_EVENT_EOS)
                    saw_last = true;
            }
        }
    }

    venus_ctx_close(&enc);
    if (!au || au_len < 16u) {
        free(au);
        return -EPROTO;
    }
    *out_au = au;
    *out_len = au_len;
    return 0;
}

static bool write_file_blob(const char *path, const uint8_t *data, size_t len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return false;
    bool ok = (fwrite(data, 1, len, fp) == len);
    fclose(fp);
    return ok;
}

enum color_tag {
    CLR_RED = 0,
    CLR_GREEN = 1,
    CLR_BLUE = 2,
    CLR_YELLOW = 3,
};

static bool check_rgba_patch(const uint8_t *px, uint32_t w, uint32_t x0, uint32_t y0,
                             uint32_t pw, uint32_t ph, enum color_tag expected)
{
    uint64_t sr = 0, sg = 0, sb = 0, cnt = 0;
    for (uint32_t y = y0; y < y0 + ph; y++) {
        for (uint32_t x = x0; x < x0 + pw; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4u;
            sr += p[0];
            sg += p[1];
            sb += p[2];
            cnt++;
        }
    }
    if (!cnt)
        return false;
    uint32_t r = (uint32_t)(sr / cnt), g = (uint32_t)(sg / cnt), b = (uint32_t)(sb / cnt);
    switch (expected) {
    case CLR_RED:    return r > 180u && g < 80u && b < 80u;
    case CLR_GREEN:  return g > 180u && r < 80u && b < 80u;
    case CLR_BLUE:   return b > 180u && r < 80u && g < 80u;
    case CLR_YELLOW: return r > 180u && g > 180u && b < 80u;
    default:         return false;
    }
}

static bool check_yuv_patch(const uint8_t *buf, uint32_t w, uint32_t h, int32_t fmt,
                            uint32_t x0, uint32_t y0, uint32_t pw, uint32_t ph,
                            enum color_tag expected)
{
    const uint8_t *uv_plane = buf + (size_t)w * h;
    uint64_t sy = 0, su = 0, sv = 0, yc = 0, uvc = 0;
    bool is_nv21 = (fmt == PIXEL_FORMAT_NV21);
    for (uint32_t y = y0; y < y0 + ph; y++) {
        for (uint32_t x = x0; x < x0 + pw; x++) {
            sy += buf[(size_t)y * w + x];
            yc++;
        }
    }
    for (uint32_t y = y0 / 2u; y < (y0 + ph) / 2u; y++) {
        for (uint32_t x = (x0 & ~1u); x < ((x0 + pw) & ~1u); x += 2u) {
            uint8_t c0 = uv_plane[(size_t)y * w + x + 0u];
            uint8_t c1 = uv_plane[(size_t)y * w + x + 1u];
            su += is_nv21 ? c1 : c0;
            sv += is_nv21 ? c0 : c1;
            uvc++;
        }
    }
    if (!yc || !uvc)
        return false;
    uint32_t y = (uint32_t)(sy / yc), u = (uint32_t)(su / uvc), v = (uint32_t)(sv / uvc);
    switch (expected) {
    case CLR_RED:    return v > 200u && u < 120u;
    case CLR_GREEN:  return u < 90u && v < 70u;
    case CLR_BLUE:   return u > 200u && v < 140u;
    case CLR_YELLOW: return y > 180u && u < 60u && v > 125u;
    default:         return false;
    }
}

static int run_decode_case(const char *name, const uint8_t *heic, size_t heic_len,
                           uint32_t exp_w, uint32_t exp_h, int32_t req_fmt, bool is_grid)
{
    OH_ImageSourceNative *src = NULL;
    OH_ImageSource_Info *sinfo = NULL;
    OH_DecodingOptions *opts = NULL;
    OH_PixelmapNative *pm = NULL;
    OH_Pixelmap_ImageInfo *pinfo = NULL;
    uint8_t *pixels = NULL;

    uint32_t iw = 0, ih = 0;
    uint32_t pw = 0, ph = 0, pstride = 0, bcount = 0;
    int32_t pfmt = 0;
    size_t need_bytes = 0, read_sz = 0;
    bool ok = false;

    Image_ErrorCode rc = OH_ImageSourceNative_CreateFromData((uint8_t *)heic, heic_len, &src);
    if (rc != IMAGE_SUCCESS || !src)
        goto fail;

    rc = OH_ImageSourceInfo_Create(&sinfo);
    if (rc != IMAGE_SUCCESS)
        goto fail;
    rc = OH_ImageSourceNative_GetImageInfo(src, 0, sinfo);
    if (rc != IMAGE_SUCCESS ||
        OH_ImageSourceInfo_GetWidth(sinfo, &iw) != IMAGE_SUCCESS ||
        OH_ImageSourceInfo_GetHeight(sinfo, &ih) != IMAGE_SUCCESS ||
        iw != exp_w || ih != exp_h)
        goto fail;

    if (OH_DecodingOptions_Create(&opts) != IMAGE_SUCCESS ||
        OH_DecodingOptions_SetPixelFormat(opts, req_fmt) != IMAGE_SUCCESS)
        goto fail;

    rc = OH_ImageSourceNative_CreatePixelmapUsingAllocator(src, opts, IMAGE_ALLOCATOR_TYPE_DMA, &pm);
    if (rc != IMAGE_SUCCESS || !pm)
        goto fail;

    if (OH_PixelmapImageInfo_Create(&pinfo) != IMAGE_SUCCESS ||
        OH_PixelmapNative_GetImageInfo(pm, pinfo) != IMAGE_SUCCESS ||
        OH_PixelmapImageInfo_GetWidth(pinfo, &pw) != IMAGE_SUCCESS ||
        OH_PixelmapImageInfo_GetHeight(pinfo, &ph) != IMAGE_SUCCESS ||
        OH_PixelmapImageInfo_GetRowStride(pinfo, &pstride) != IMAGE_SUCCESS ||
        OH_PixelmapImageInfo_GetPixelFormat(pinfo, &pfmt) != IMAGE_SUCCESS)
        goto fail;

    need_bytes = (req_fmt == PIXEL_FORMAT_RGBA_8888)
        ? ((size_t)pw * ph * 4u)
        : ((size_t)pw * ph * 3u / 2u);
    if (OH_PixelmapNative_GetByteCount(pm, &bcount) == IMAGE_SUCCESS && bcount > need_bytes)
        need_bytes = (size_t)bcount;
    pixels = (uint8_t *)malloc(need_bytes);
    if (!pixels)
        goto fail;
    read_sz = need_bytes;
    rc = OH_PixelmapNative_ReadPixels(pm, pixels, &read_sz);
    if (rc != IMAGE_SUCCESS || pw != exp_w || ph != exp_h || pfmt != req_fmt)
        goto fail;
    if (!is_grid) {
        ok = (req_fmt == PIXEL_FORMAT_RGBA_8888)
            ? check_rgba_patch(pixels, pw, pw / 2u - 16u, ph / 2u - 16u, 32u, 32u, CLR_RED)
            : check_yuv_patch(pixels, pw, ph, req_fmt, pw / 2u - 16u, ph / 2u - 16u, 32u, 32u, CLR_RED);
    } else {
        uint32_t q1x = 256u + ((pw - 256u) / 2u > 16u ? (pw - 256u) / 2u - 16u : 0u);
        uint32_t q2y = 256u + ((ph - 256u) / 2u > 16u ? (ph - 256u) / 2u - 16u : 0u);
        if (req_fmt == PIXEL_FORMAT_RGBA_8888) {
            ok = check_rgba_patch(pixels, pw, 112u, 112u, 32u, 32u, CLR_RED) &&
                 check_rgba_patch(pixels, pw, q1x,  112u, 32u, 32u, CLR_GREEN) &&
                 check_rgba_patch(pixels, pw, 112u, q2y,  32u, 32u, CLR_BLUE) &&
                 check_rgba_patch(pixels, pw, q1x,  q2y,  32u, 32u, CLR_YELLOW);
        } else {
            ok = check_yuv_patch(pixels, pw, ph, req_fmt, 112u, 112u, 32u, 32u, CLR_RED) &&
                 check_yuv_patch(pixels, pw, ph, req_fmt, q1x,  112u, 32u, 32u, CLR_GREEN) &&
                 check_yuv_patch(pixels, pw, ph, req_fmt, 112u, q2y,  32u, 32u, CLR_BLUE) &&
                 check_yuv_patch(pixels, pw, ph, req_fmt, q1x,  q2y,  32u, 32u, CLR_YELLOW);
        }
    }

    printf("HEIF_PROBE_CASE name=%s fmt=%d w=%u h=%u stride=%u rc=%d ok=%d\n",
           name, req_fmt, pw, ph, pstride, (int)rc, ok ? 1 : 0);

    free(pixels);
    if (pinfo) OH_PixelmapImageInfo_Release(pinfo);
    if (pm) OH_PixelmapNative_Release(pm);
    if (opts) OH_DecodingOptions_Release(opts);
    if (sinfo) OH_ImageSourceInfo_Release(sinfo);
    if (src) OH_ImageSourceNative_Release(src);
    return ok ? 0 : -1;

fail:
    printf("HEIF_PROBE_CASE name=%s fmt=%d w=0 h=0 rc=%d ok=0\n", name, req_fmt, (int)rc);
    free(pixels);
    if (pinfo) OH_PixelmapImageInfo_Release(pinfo);
    if (pm) OH_PixelmapNative_Release(pm);
    if (opts) OH_DecodingOptions_Release(opts);
    if (sinfo) OH_ImageSourceInfo_Release(sinfo);
    if (src) OH_ImageSourceNative_Release(src);
    return -1;
}

static int run_file_case(const char *path)
{
    OH_ImageSourceNative *src = NULL;
    OH_ImageSource_Info *sinfo = NULL;
    OH_DecodingOptions *opts = NULL;
    OH_PixelmapNative *pm = NULL;
    OH_Pixelmap_ImageInfo *pinfo = NULL;

    uint32_t pw = 0, ph = 0, pstride = 0;

    Image_ErrorCode rc = OH_ImageSourceNative_CreateFromUri((char *)path, strlen(path), &src);
    if (rc != IMAGE_SUCCESS || !src)
        goto fail;
    if (OH_ImageSourceInfo_Create(&sinfo) != IMAGE_SUCCESS ||
        OH_ImageSourceNative_GetImageInfo(src, 0, sinfo) != IMAGE_SUCCESS)
        goto fail;
    if (OH_DecodingOptions_Create(&opts) != IMAGE_SUCCESS ||
        OH_DecodingOptions_SetPixelFormat(opts, PIXEL_FORMAT_RGBA_8888) != IMAGE_SUCCESS)
        goto fail;
    rc = OH_ImageSourceNative_CreatePixelmapUsingAllocator(src, opts, IMAGE_ALLOCATOR_TYPE_DMA, &pm);
    if (rc != IMAGE_SUCCESS || !pm)
        goto fail;
    if (OH_PixelmapImageInfo_Create(&pinfo) != IMAGE_SUCCESS ||
        OH_PixelmapNative_GetImageInfo(pm, pinfo) != IMAGE_SUCCESS ||
        OH_PixelmapImageInfo_GetWidth(pinfo, &pw) != IMAGE_SUCCESS ||
        OH_PixelmapImageInfo_GetHeight(pinfo, &ph) != IMAGE_SUCCESS ||
        OH_PixelmapImageInfo_GetRowStride(pinfo, &pstride) != IMAGE_SUCCESS)
        goto fail;

    printf("HEIF_FILE_CASE path=%s w=%u h=%u stride=%u rc=%d ok=1\n",
           path, pw, ph, pstride, (int)rc);
    OH_PixelmapImageInfo_Release(pinfo);
    OH_PixelmapNative_Release(pm);
    OH_DecodingOptions_Release(opts);
    OH_ImageSourceInfo_Release(sinfo);
    OH_ImageSourceNative_Release(src);
    return 0;

fail:
    printf("HEIF_FILE_CASE path=%s rc=%d ok=0\n", path, (int)rc);
    if (pinfo) OH_PixelmapImageInfo_Release(pinfo);
    if (pm) OH_PixelmapNative_Release(pm);
    if (opts) OH_DecodingOptions_Release(opts);
    if (sinfo) OH_ImageSourceInfo_Release(sinfo);
    if (src) OH_ImageSourceNative_Release(src);
    return -1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc == 3 && strcmp(argv[1], "--file") == 0)
        return run_file_case(argv[2]) == 0 ? 0 : 1;
    if (argc != 1) {
        fprintf(stderr, "Usage: %s [--file <path>]\n", argv[0]);
        return 2;
    }

    static const uint8_t colors[4][3] = {
        { 76u,  88u, 248u}, /* Red */
        {150u,  48u,  28u}, /* Green */
        { 36u, 248u, 110u}, /* Blue */
        {220u,  10u, 146u}, /* Yellow */
    };
    uint8_t *aus[4] = {0};
    uint32_t au_lens[4] = {0};
    struct hevc_au_parts parts[4];
    memset(parts, 0, sizeof(parts));

    for (uint32_t i = 0; i < 4u; i++) {
        if (encode_hevc_idr_tile(colors[i][0], colors[i][1], colors[i][2],
                                 &aus[i], &au_lens[i]) != 0 ||
            split_annexb_hevc(aus[i], au_lens[i], &parts[i]) != 0) {
            fprintf(stderr, "HEIF_PROBE_ENC_FAIL tile=%u\n", i);
            for (uint32_t j = 0; j < 4u; j++) {
                free(parts[j].lenpref_slices);
                free(aus[j]);
            }
            return 1;
        }
    }

    uint8_t *heic_single = NULL, *heic_grid = NULL, *heic_crop = NULL;
    size_t len_single = 0, len_grid = 0, len_crop = 0;
    int rc = 0, passed = 0;

    if (build_heic_container(&parts[0], &parts[0], 1u, TILE_W, TILE_H, false, 1u, 1u,
                             TILE_W, TILE_H, &heic_single, &len_single) != 0 ||
        build_heic_container(&parts[0], parts, 4u, TILE_W, TILE_H, true, 2u, 2u,
                             512u, 512u, &heic_grid, &len_grid) != 0 ||
        build_heic_container(&parts[0], parts, 4u, TILE_W, TILE_H, true, 2u, 2u,
                             400u, 360u, &heic_crop, &len_crop) != 0) {
        rc = 1;
        goto out;
    }

    (void)write_file_blob("/data/local/tmp/marble-codec/test_single.heic", heic_single, len_single);
    (void)write_file_blob("/data/local/tmp/marble-codec/test_grid.heic", heic_grid, len_grid);
    (void)write_file_blob("/data/local/tmp/marble-codec/test_grid_crop.heic", heic_crop, len_crop);
    if (run_decode_case("single", heic_single, len_single, 256u, 256u, PIXEL_FORMAT_RGBA_8888, false) == 0) passed++;
    if (run_decode_case("single", heic_single, len_single, 256u, 256u, PIXEL_FORMAT_NV12, false) == 0) passed++;
    if (run_decode_case("single", heic_single, len_single, 256u, 256u, PIXEL_FORMAT_NV21, false) == 0) passed++;
    if (run_decode_case("grid",   heic_grid,   len_grid,   512u, 512u, PIXEL_FORMAT_RGBA_8888, true) == 0) passed++;
    if (run_decode_case("grid",   heic_grid,   len_grid,   512u, 512u, PIXEL_FORMAT_NV12, true) == 0) passed++;
    if (run_decode_case("grid",   heic_grid,   len_grid,   512u, 512u, PIXEL_FORMAT_NV21, true) == 0) passed++;
    if (run_decode_case("grid_crop", heic_crop, len_crop,  400u, 360u, PIXEL_FORMAT_RGBA_8888, true) == 0) passed++;

    if (passed == 7) {
        printf("HEIF_PROBE_ALL_OK cases=7\n");
        rc = 0;
    } else {
        printf("HEIF_PROBE_FAIL passed=%d/7\n", passed);
        rc = 1;
    }

out:
    free(heic_single);
    free(heic_grid);
    free(heic_crop);
    for (uint32_t i = 0; i < 4u; i++) {
        free(parts[i].lenpref_slices);
        free(aus[i]);
    }
    return rc;
}
#endif
