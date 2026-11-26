/*
 * Clean-room OHOS Codec Image HEIF Decode VDI implementation for SM7475 (marble).
 *
 * Bridges OHOS HDF codec_image_service (ICodecHeifDecodeHwi v2.1) to Qualcomm
 * SM7475 Venus hardware HEVC decoder (/dev/video32) over DMA-BUF.
 * Copyright (C) 2026 Marble HMOS Bringup Project (clean-room MIT).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "venus_v4l2.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/dma-buf.h>
#include <linux/videodev2.h>

/* PixelFormat values from drivers/interface/display/composer/v1_0/DisplayComposerType.idl */
enum {
    OHOS_PIXEL_FMT_RGB_565      = 3,
    OHOS_PIXEL_FMT_RGBX_8888    = 11,
    OHOS_PIXEL_FMT_RGBA_8888    = 12,
    OHOS_PIXEL_FMT_BGRX_8888    = 19,
    OHOS_PIXEL_FMT_BGRA_8888    = 20,
    OHOS_PIXEL_FMT_YCBCR_420_SP = 24, /* NV12 */
    OHOS_PIXEL_FMT_YCRCB_420_SP = 25, /* NV21 */
    OHOS_PIXEL_FMT_YCBCR_420_P  = 28, /* I420 */
};

/* OHOS drivers/hdf_core/interfaces/inner_api/hdi/base/buffer_handle.h */
typedef struct BufferHandle {
    int32_t fd;
    int32_t width;
    int32_t stride;
    int32_t height;
    int32_t size;
    int32_t format;
    uint64_t usage;
    void *virAddr;
    uint64_t phyAddr;
    uint32_t reserveFds;
    uint32_t reserveInts;
    int32_t reserve[0];
} BufferHandle;

/* Minimal ABI-compatible mirror of std::vector<OHOS::sptr<OHOS::Ashmem>> */
struct AshmemVecRaw {
    void **begin;
    void **end;
    void **cap;
};

/* OHOS drivers_interface/codec/image/v2_1/CodecImageType.idl CodecHeifDecInfo */
struct CodecHeifDecInfoRaw {
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint8_t enableGrid;
    uint8_t pad0[3];
    uint32_t cols;
    uint32_t rows;
    uint32_t tileWidth;
    uint32_t tileHeight;
    uint32_t colorSpace;
    uint32_t sampleSize;
    uint32_t reserved;
};

struct ICodecHeifDecodeHwi {
    int32_t (*DoHeifDecode)(const void *inputs, void *output,
                            const struct CodecHeifDecInfoRaw *decInfo);
};

_Static_assert(sizeof(BufferHandle) == 56, "BufferHandle ABI mismatch");
_Static_assert(sizeof(struct AshmemVecRaw) == 24, "AshmemVecRaw ABI mismatch");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, displayWidth) == 0, "displayWidth offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, displayHeight) == 4, "displayHeight offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, enableGrid) == 8, "enableGrid offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, cols) == 12, "cols offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, rows) == 16, "rows offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, tileWidth) == 20, "tileWidth offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, tileHeight) == 24, "tileHeight offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, colorSpace) == 28, "colorSpace offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, sampleSize) == 32, "sampleSize offset");
_Static_assert(offsetof(struct CodecHeifDecInfoRaw, reserved) == 36, "reserved offset");
_Static_assert(sizeof(struct CodecHeifDecInfoRaw) == 40, "CodecHeifDecInfo ABI mismatch");

#define HEIF_VDI_LOG_PATH      "/data/local/tmp/marble-codec/heif-vdi.log"
#define HEIF_MAX_CONCURRENT    2u
#define HEIF_MAX_TILES         256u
#define HEIF_MAX_ASHMEM_BYTES  (64u * 1024u * 1024u)

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_log_budget = 64;
static int g_err_budget = 64;

static pthread_mutex_t g_conc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_conc_cv = PTHREAD_COND_INITIALIZER;
static uint32_t g_active_sessions = 0;

static void heif_log(bool is_err, const char *fmt, ...)
{
    pthread_mutex_lock(&g_log_lock);
    if (is_err) {
        if (g_err_budget <= 0) {
            pthread_mutex_unlock(&g_log_lock);
            return;
        }
        g_err_budget--;
    } else {
        if (g_log_budget <= 0) {
            pthread_mutex_unlock(&g_log_lock);
            return;
        }
        g_log_budget--;
    }
    pthread_mutex_unlock(&g_log_lock);

    FILE *fp = fopen(HEIF_VDI_LOG_PATH, "ae");
    if (!fp)
        return;
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(fp, "[%ld.%03ld pid=%d] ", (long)ts.tv_sec, ts.tv_nsec / 1000000L, (int)getpid());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* --- Bitstream normalization (Annex-B, 4-byte length-prefixed, or hvcC) --- */

static int heif_to_annexb(const uint8_t *src, size_t len, bool allow_hvcc,
                          uint8_t **out_buf, size_t *out_len)
{
    if (!src || len < 4u || !out_buf || !out_len)
        return -1;
    *out_buf = NULL;
    *out_len = 0;

    /* (a) Already Annex-B? Note: a 4-byte length-prefixed HEVC NALU is always >= 2 bytes
     * (2-byte HEVC NALU header), so 00 00 00 01 at offset 0 is unambiguously Annex-B. */
    if ((src[0] == 0x00 && src[1] == 0x00 && src[2] == 0x01) ||
        (len >= 5u && src[0] == 0x00 && src[1] == 0x00 && src[2] == 0x00 && src[3] == 0x01)) {
        uint8_t *copy = malloc(len);
        if (!copy)
            return -1;
        memcpy(copy, src, len);
        *out_buf = copy;
        *out_len = len;
        return 0;
    }

    /* (b) ISO/IEC 14496-15 HEVCDecoderConfigurationRecord (hvcC):
     * configurationVersion == 1, >= 23 bytes, first 4 bytes are NOT a clean NALU length. */
    if (allow_hvcc && len >= 23u && src[0] == 1u) {
        size_t pos = 22u;
        uint8_t num_arrays = src[pos++];
        size_t cap = 0;
        size_t scan = pos;
        bool valid_hvcc = (num_arrays > 0u);
        for (uint8_t a = 0; a < num_arrays && valid_hvcc; a++) {
            if (scan + 3u > len) {
                valid_hvcc = false;
                break;
            }
            scan++; /* array_completeness | reserved | NAL_unit_type */
            uint16_t num_nalus = read_be16(src + scan);
            scan += 2u;
            for (uint16_t n = 0; n < num_nalus; n++) {
                if (scan + 2u > len) {
                    valid_hvcc = false;
                    break;
                }
                uint16_t nal_len = read_be16(src + scan);
                scan += 2u;
                if (nal_len < 2u || scan + (size_t)nal_len > len) {
                    valid_hvcc = false;
                    break;
                }
                cap += 4u + (size_t)nal_len;
                scan += nal_len;
            }
        }
        if (valid_hvcc && scan == len && cap > 0u) {
            uint8_t *dst = malloc(cap);
            if (!dst)
                return -1;
            size_t wpos = 0;
            size_t rpos = 23u;
            for (uint8_t a = 0; a < num_arrays; a++) {
                rpos++;
                uint16_t num_nalus = read_be16(src + rpos);
                rpos += 2u;
                for (uint16_t n = 0; n < num_nalus; n++) {
                    uint16_t nal_len = read_be16(src + rpos);
                    rpos += 2u;
                    dst[wpos++] = 0x00;
                    dst[wpos++] = 0x00;
                    dst[wpos++] = 0x00;
                    dst[wpos++] = 0x01;
                    memcpy(dst + wpos, src + rpos, nal_len);
                    wpos += nal_len;
                    rpos += nal_len;
                }
            }
            *out_buf = dst;
            *out_len = wpos;
            return 0;
        }
    }

    /* (c) 4-byte big-endian length-prefixed NALUs covering exact buffer length */
    size_t pos = 0;
    size_t nalu_cnt = 0;
    while (pos + 4u <= len) {
        uint32_t nal_len = read_be32(src + pos);
        pos += 4u;
        if (nal_len < 2u || (size_t)nal_len > len - pos)
            return -1;
        pos += nal_len;
        nalu_cnt++;
    }
    if (pos != len || nalu_cnt == 0u)
        return -1;

    uint8_t *dst = malloc(len);
    if (!dst)
        return -1;
    memcpy(dst, src, len);
    pos = 0;
    while (pos + 4u <= len) {
        uint32_t nal_len = read_be32(dst + pos);
        dst[pos + 0u] = 0x00;
        dst[pos + 1u] = 0x00;
        dst[pos + 2u] = 0x00;
        dst[pos + 3u] = 0x01;
        pos += 4u + (size_t)nal_len;
    }
    *out_buf = dst;
    *out_len = len;
    return 0;
}

/* --- Ashmem + SurfaceBuffer abstraction (overridable for HEIF_VDI_HOST_TEST) --- */

struct heif_input_slice {
    uint8_t *data;
    size_t len;
};

struct heif_output_target {
    BufferHandle *bh;
    uint8_t *map_ptr;
    size_t map_len;
    size_t dst_pixel_bytes;
    bool mmap_owned;
};

struct heif_runtime_ops {
    int (*read_inputs_fn)(const void *inputs, struct heif_input_slice **out_slices, size_t *out_cnt);
    int (*map_output_fn)(void *output, struct heif_output_target *out);
    void (*unmap_output_fn)(struct heif_output_target *out);
    int (*sync_dmabuf_fn)(int fd, uint64_t flags);
    const struct venus_sys_ops *venus_ops;
};

static int compute_dst_pixel_bytes(const BufferHandle *bh, size_t *out_bytes)
{
    if (!bh || bh->width <= 0 || bh->height <= 0 || bh->stride <= 0 || bh->size <= 0 || !out_bytes)
        return -1;

    uint32_t w = (uint32_t)bh->width;
    uint32_t h = (uint32_t)bh->height;
    uint32_t s = (uint32_t)bh->stride;
    uint64_t bytes = 0;

    switch (bh->format) {
    case OHOS_PIXEL_FMT_RGB_565:
        if ((uint64_t)s < (uint64_t)w * 2ULL)
            return -1;
        bytes = (uint64_t)s * h;
        break;
    case OHOS_PIXEL_FMT_RGBX_8888:
    case OHOS_PIXEL_FMT_RGBA_8888:
    case OHOS_PIXEL_FMT_BGRX_8888:
    case OHOS_PIXEL_FMT_BGRA_8888:
        if ((uint64_t)s < (uint64_t)w * 4ULL)
            return -1;
        bytes = (uint64_t)s * h;
        break;
    case OHOS_PIXEL_FMT_YCBCR_420_SP:
    case OHOS_PIXEL_FMT_YCRCB_420_SP:
    case OHOS_PIXEL_FMT_YCBCR_420_P:
        if (s < 2u || (s & 1u) != 0u || s < w)
            return -1;
        bytes = (uint64_t)s * h + (uint64_t)s * (((uint64_t)h + 1ULL) / 2ULL);
        break;
    default:
        return -1;
    }

    if (bytes == 0ULL || bytes > (uint64_t)bh->size || bytes > (uint64_t)SIZE_MAX)
        return -1;
    *out_bytes = (size_t)bytes;
    return 0;
}

typedef int32_t (*AshmemGetSizeFn)(const void *);
typedef bool (*AshmemMapRoFn)(void *);
typedef const void *(*AshmemReadFn)(void *, int32_t, int32_t);
typedef void (*AshmemUnmapFn)(void *);

static pthread_mutex_t g_ashmem_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    bool initialized;
    void *dl_handle;
    AshmemGetSizeFn get_size;
    AshmemMapRoFn map_ro;
    AshmemReadFn read_from;
    AshmemUnmapFn unmap;
} g_ashmem_api = {0};

static int load_ashmem_api(void)
{
    pthread_mutex_lock(&g_ashmem_lock);
    if (g_ashmem_api.initialized && g_ashmem_api.get_size && g_ashmem_api.map_ro &&
        g_ashmem_api.read_from && g_ashmem_api.unmap) {
        pthread_mutex_unlock(&g_ashmem_lock);
        return 0;
    }

    AshmemGetSizeFn f_size = (AshmemGetSizeFn)dlsym(RTLD_DEFAULT, "_ZNK4OHOS6Ashmem13GetAshmemSizeEv");
    AshmemMapRoFn f_map = (AshmemMapRoFn)dlsym(RTLD_DEFAULT, "_ZN4OHOS6Ashmem17MapReadOnlyAshmemEv");
    AshmemReadFn f_read = (AshmemReadFn)dlsym(RTLD_DEFAULT, "_ZN4OHOS6Ashmem14ReadFromAshmemEii");
    AshmemUnmapFn f_unmap = (AshmemUnmapFn)dlsym(RTLD_DEFAULT, "_ZN4OHOS6Ashmem11UnmapAshmemEv");

    if (!f_size || !f_map || !f_read || !f_unmap) {
        if (!g_ashmem_api.dl_handle)
            g_ashmem_api.dl_handle = dlopen("libutils.z.so", RTLD_LAZY | RTLD_LOCAL);
        if (!g_ashmem_api.dl_handle)
            g_ashmem_api.dl_handle = dlopen("/system/lib64/chipset-pub-sdk/libutils.z.so", RTLD_LAZY | RTLD_LOCAL);
        if (g_ashmem_api.dl_handle) {
            if (!f_size)
                f_size = (AshmemGetSizeFn)dlsym(g_ashmem_api.dl_handle, "_ZNK4OHOS6Ashmem13GetAshmemSizeEv");
            if (!f_map)
                f_map = (AshmemMapRoFn)dlsym(g_ashmem_api.dl_handle, "_ZN4OHOS6Ashmem17MapReadOnlyAshmemEv");
            if (!f_read)
                f_read = (AshmemReadFn)dlsym(g_ashmem_api.dl_handle, "_ZN4OHOS6Ashmem14ReadFromAshmemEii");
            if (!f_unmap)
                f_unmap = (AshmemUnmapFn)dlsym(g_ashmem_api.dl_handle, "_ZN4OHOS6Ashmem11UnmapAshmemEv");
        }
    }

    g_ashmem_api.get_size = f_size;
    g_ashmem_api.map_ro = f_map;
    g_ashmem_api.read_from = f_read;
    g_ashmem_api.unmap = f_unmap;
    g_ashmem_api.initialized = true;
    int rc = (f_size && f_map && f_read && f_unmap) ? 0 : -1;
    pthread_mutex_unlock(&g_ashmem_lock);
    if (rc != 0)
        heif_log(true, "load_ashmem_api failed: %s", dlerror() ? dlerror() : "missing symbol");
    return rc;
}

static void free_input_slices(struct heif_input_slice *slices, size_t cnt)
{
    if (!slices)
        return;
    for (size_t i = 0; i < cnt; i++)
        free(slices[i].data);
    free(slices);
}

static int default_read_inputs(const void *inputs, struct heif_input_slice **out_slices, size_t *out_cnt)
{
    const struct AshmemVecRaw *vec = (const struct AshmemVecRaw *)inputs;
    if (!vec || !vec->begin || !vec->end || vec->end <= vec->begin)
        return -1;
    if (load_ashmem_api() != 0)
        return -1;

    size_t cnt = (size_t)(vec->end - vec->begin);
    if (cnt == 0u || cnt > HEIF_MAX_TILES + 1u)
        return -1;

    struct heif_input_slice *slices = calloc(cnt, sizeof(*slices));
    if (!slices)
        return -1;

    for (size_t i = 0; i < cnt; i++) {
        void *ashmem = vec->begin[i];
        if (!ashmem) {
            free_input_slices(slices, cnt);
            return -1;
        }
        int32_t sz = g_ashmem_api.get_size(ashmem);
        if (sz <= 0 || (uint32_t)sz > HEIF_MAX_ASHMEM_BYTES) {
            heif_log(true, "ashmem[%zu] bad size=%d", i, sz);
            free_input_slices(slices, cnt);
            return -1;
        }
        if (!g_ashmem_api.map_ro(ashmem)) {
            heif_log(true, "ashmem[%zu] MapReadOnlyAshmem failed", i);
            free_input_slices(slices, cnt);
            return -1;
        }
        const void *ptr = g_ashmem_api.read_from(ashmem, sz, 0);
        if (!ptr) {
            g_ashmem_api.unmap(ashmem);
            heif_log(true, "ashmem[%zu] ReadFromAshmem failed", i);
            free_input_slices(slices, cnt);
            return -1;
        }
        slices[i].data = malloc((size_t)sz);
        if (!slices[i].data) {
            g_ashmem_api.unmap(ashmem);
            free_input_slices(slices, cnt);
            return -1;
        }
        memcpy(slices[i].data, ptr, (size_t)sz);
        slices[i].len = (size_t)sz;
        g_ashmem_api.unmap(ashmem);
    }

    *out_slices = slices;
    *out_cnt = cnt;
    return 0;
}

static int default_sync_dmabuf(int fd, uint64_t flags)
{
    if (fd < 0)
        return 0;
    struct dma_buf_sync sync = { .flags = flags };
    int rc;
    do {
        rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0 && errno == ENOTTY)
        return 0;
    return rc == 0 ? 0 : -errno;
}

static int default_map_output(void *output, struct heif_output_target *out)
{
    if (!output || !out)
        return -1;
    memset(out, 0, sizeof(*out));

    void **sb_slot = (void **)output;
    void *sb = *sb_slot;
    if (!sb)
        return -1;

    /* OHOS::SurfaceBufferImpl::GetBufferHandle() is at vtable slot 8 (+64 bytes) */
    void *const *vtable = *(void *const *const *)sb;
    if (!vtable || !vtable[8])
        return -1;
    typedef BufferHandle *(*GetBufferHandleFn)(const void *);
    BufferHandle *bh = ((GetBufferHandleFn)vtable[8])(sb);
    if (!bh || bh->fd < 0)
        return -1;

    size_t pixel_bytes = 0;
    if (compute_dst_pixel_bytes(bh, &pixel_bytes) != 0) {
        heif_log(true, "unsupported or invalid output BufferHandle w=%d h=%d stride=%d size=%d fmt=%d",
                 bh->width, bh->height, bh->stride, bh->size, bh->format);
        return -1;
    }

    void *ptr = mmap(NULL, (size_t)bh->size, PROT_READ | PROT_WRITE, MAP_SHARED, bh->fd, 0);
    if (ptr == MAP_FAILED) {
        heif_log(true, "mmap output fd=%d size=%d failed errno=%d", bh->fd, bh->size, errno);
        return -1;
    }

    out->bh = bh;
    out->map_ptr = (uint8_t *)ptr;
    out->map_len = (size_t)bh->size;
    out->dst_pixel_bytes = pixel_bytes;
    out->mmap_owned = true;
    return 0;
}

static void default_unmap_output(struct heif_output_target *out)
{
    if (!out)
        return;
    if (out->mmap_owned && out->map_ptr && out->map_len > 0u)
        munmap(out->map_ptr, out->map_len);
    out->map_ptr = NULL;
    out->map_len = 0;
    out->mmap_owned = false;
}

static const struct heif_runtime_ops g_default_runtime_ops = {
    .read_inputs_fn = default_read_inputs,
    .map_output_fn = default_map_output,
    .unmap_output_fn = default_unmap_output,
    .sync_dmabuf_fn = default_sync_dmabuf,
    .venus_ops = NULL,
};

/* --- Pixel format conversion & subsampled tile blit --- */

static inline uint8_t clamp_u8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static inline void yuv_to_rgb(int y, int u, int v, bool full_range, bool bt709,
                              uint8_t *r_out, uint8_t *g_out, uint8_t *b_out)
{
    int c, d = u - 128, e = v - 128;
    int r, g, b;
    if (full_range) {
        c = y;
        if (bt709) {
            r = c + ((403 * e + 128) >> 8);
            g = c - ((48 * d + 120 * e + 128) >> 8);
            b = c + ((475 * d + 128) >> 8);
        } else {
            r = c + ((359 * e + 128) >> 8);
            g = c - ((88 * d + 183 * e + 128) >> 8);
            b = c + ((454 * d + 128) >> 8);
        }
    } else {
        c = y - 16;
        if (bt709) {
            r = (298 * c + 459 * e + 128) >> 8;
            g = (298 * c - 55 * d - 136 * e + 128) >> 8;
            b = (298 * c + 541 * d + 128) >> 8;
        } else {
            r = (298 * c + 409 * e + 128) >> 8;
            g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            b = (298 * c + 516 * d + 128) >> 8;
        }
    }
    *r_out = clamp_u8(r);
    *g_out = clamp_u8(g);
    *b_out = clamp_u8(b);
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

static int heif_blit_nv12_tile(const uint8_t *src_nv12, uint32_t src_uv_off,
                               uint32_t src_stride, uint32_t crop_left, uint32_t crop_top,
                               uint32_t src_w, uint32_t src_h, size_t src_max_bytes,
                               struct heif_output_target *dst,
                               uint32_t dst_tile_x0, uint32_t dst_tile_y0,
                               uint32_t disp_w, uint32_t disp_h,
                               uint32_t eff_disp_w, uint32_t eff_disp_h,
                               uint32_t sample, bool full_range, bool bt709)
{
    if (!src_nv12 || !dst || !dst->bh || !dst->map_ptr || src_w == 0u || src_h == 0u ||
        sample == 0u || dst_tile_x0 >= disp_w || dst_tile_y0 >= disp_h)
        return 0; /* tile completely outside clipped display region */

    uint32_t need_y_rows = crop_top + src_h;
    uint32_t need_uv_rows = (crop_top + src_h + 1u) / 2u;
    size_t req_y_end = (size_t)(need_y_rows - 1u) * src_stride + crop_left + src_w;
    size_t req_uv_end = (size_t)src_uv_off + (size_t)(need_uv_rows - 1u) * src_stride +
                        ((crop_left + src_w + 1u) & ~1u);
    if (req_y_end > src_max_bytes || req_uv_end > src_max_bytes)
        return -1;

    uint32_t tile_x1 = min_u32(disp_w, dst_tile_x0 + src_w);
    uint32_t tile_y1 = min_u32(disp_h, dst_tile_y0 + src_h);

    uint32_t ox_begin = (dst_tile_x0 + sample - 1u) / sample;
    uint32_t ox_end = min_u32(eff_disp_w, (tile_x1 + sample - 1u) / sample);
    uint32_t oy_begin = (dst_tile_y0 + sample - 1u) / sample;
    uint32_t oy_end = min_u32(eff_disp_h, (tile_y1 + sample - 1u) / sample);
    if (ox_begin >= ox_end || oy_begin >= oy_end)
        return 0;

    uint8_t *dst_ptr = dst->map_ptr;
    size_t dst_cap = dst->dst_pixel_bytes;
    uint32_t dst_stride = (uint32_t)dst->bh->stride;
    uint32_t dst_h = (uint32_t)dst->bh->height;
    int32_t fmt = dst->bh->format;

    const uint8_t *src_y_base = src_nv12;
    const uint8_t *src_uv_base = src_nv12 + src_uv_off;

    if (fmt == OHOS_PIXEL_FMT_YCBCR_420_SP || fmt == OHOS_PIXEL_FMT_YCRCB_420_SP ||
        fmt == OHOS_PIXEL_FMT_YCBCR_420_P) {
        /* Copy Y plane within [oy_begin, oy_end) x [ox_begin, ox_end) */
        for (uint32_t oy = oy_begin; oy < oy_end; oy++) {
            uint32_t sy = oy * sample - dst_tile_y0 + crop_top;
            const uint8_t *srow = src_y_base + (size_t)sy * src_stride + crop_left;
            size_t d_off = (size_t)oy * dst_stride + ox_begin;
            if (d_off + (ox_end - ox_begin) > dst_cap)
                return -1;
            uint8_t *drow = dst_ptr + (size_t)oy * dst_stride;
            if (sample == 1u) {
                memcpy(drow + ox_begin, srow + (ox_begin * sample - dst_tile_x0),
                       ox_end - ox_begin);
            } else {
                for (uint32_t ox = ox_begin; ox < ox_end; ox++)
                    drow[ox] = srow[ox * sample - dst_tile_x0];
            }
        }

        /* Chroma step in full-res pixels is 2 * sample */
        uint32_t eff_uv_w = (eff_disp_w + 1u) / 2u;
        uint32_t eff_uv_h = (eff_disp_h + 1u) / 2u;
        uint32_t step2 = 2u * sample;
        uint32_t cox_begin = (dst_tile_x0 + step2 - 1u) / step2;
        uint32_t cox_end = min_u32(eff_uv_w, (tile_x1 + step2 - 1u) / step2);
        uint32_t coy_begin = (dst_tile_y0 + step2 - 1u) / step2;
        uint32_t coy_end = min_u32(eff_uv_h, (tile_y1 + step2 - 1u) / step2);
        if (cox_begin >= cox_end || coy_begin >= coy_end)
            return 0;

        size_t dst_uv_off = (size_t)dst_stride * dst_h;
        if (fmt == OHOS_PIXEL_FMT_YCBCR_420_P) {
            uint32_t uv_stride = dst_stride / 2u;
            uint32_t uv_h = (dst_h + 1u) / 2u;
            uint8_t *u_dst = dst_ptr + dst_uv_off;
            uint8_t *v_dst = u_dst + (size_t)uv_stride * uv_h;
            for (uint32_t coy = coy_begin; coy < coy_end; coy++) {
                uint32_t sy = coy * step2 - dst_tile_y0 + crop_top;
                const uint8_t *suv = src_uv_base + (size_t)(sy / 2u) * src_stride;
                size_t row_off = (size_t)coy * uv_stride;
                if (dst_uv_off + (size_t)uv_stride * uv_h + row_off + cox_end > dst_cap)
                    return -1;
                for (uint32_t cox = cox_begin; cox < cox_end; cox++) {
                    uint32_t sx = cox * step2 - dst_tile_x0 + crop_left;
                    uint32_t uvx = (sx & ~1u);
                    u_dst[row_off + cox] = suv[uvx + 0u];
                    v_dst[row_off + cox] = suv[uvx + 1u];
                }
            }
        } else {
            bool swap_uv = (fmt == OHOS_PIXEL_FMT_YCRCB_420_SP);
            for (uint32_t coy = coy_begin; coy < coy_end; coy++) {
                uint32_t sy = coy * step2 - dst_tile_y0 + crop_top;
                const uint8_t *suv = src_uv_base + (size_t)(sy / 2u) * src_stride;
                size_t d_off = dst_uv_off + (size_t)coy * dst_stride;
                if (d_off + (size_t)cox_end * 2u > dst_cap)
                    return -1;
                uint8_t *drow = dst_ptr + d_off;
                if (!swap_uv && sample == 1u && (crop_left & 1u) == 0u) {
                    uint32_t tx0 = cox_begin * 2u - dst_tile_x0 + crop_left;
                    memcpy(drow + (size_t)cox_begin * 2u, suv + tx0,
                           (size_t)(cox_end - cox_begin) * 2u);
                } else {
                    for (uint32_t cox = cox_begin; cox < cox_end; cox++) {
                        uint32_t sx = cox * step2 - dst_tile_x0 + crop_left;
                        uint32_t uvx = (sx & ~1u);
                        drow[cox * 2u + 0u] = suv[uvx + (swap_uv ? 1u : 0u)];
                        drow[cox * 2u + 1u] = suv[uvx + (swap_uv ? 0u : 1u)];
                    }
                }
            }
        }
        return 0;
    }

    /* RGB outputs: RGB565 (3), RGBX/RGBA (11/12), BGRX/BGRA (19/20) */
    uint32_t bpp = (fmt == OHOS_PIXEL_FMT_RGB_565) ? 2u : 4u;
    bool bgr = (fmt == OHOS_PIXEL_FMT_BGRX_8888 || fmt == OHOS_PIXEL_FMT_BGRA_8888);

    for (uint32_t oy = oy_begin; oy < oy_end; oy++) {
        uint32_t sy = oy * sample - dst_tile_y0 + crop_top;
        const uint8_t *sy_row = src_y_base + (size_t)sy * src_stride;
        const uint8_t *suv_row = src_uv_base + (size_t)(sy / 2u) * src_stride;
        size_t row_off = (size_t)oy * dst_stride;
        if (row_off + (size_t)ox_end * bpp > dst_cap)
            return -1;
        uint8_t *drow = dst_ptr + row_off;

        for (uint32_t ox = ox_begin; ox < ox_end; ox++) {
            uint32_t sx = ox * sample - dst_tile_x0 + crop_left;
            uint32_t uvx = (sx & ~1u);
            uint8_t r, g, b;
            yuv_to_rgb((int)sy_row[sx], (int)suv_row[uvx + 0u], (int)suv_row[uvx + 1u],
                       full_range, bt709, &r, &g, &b);
            if (fmt == OHOS_PIXEL_FMT_RGB_565) {
                uint16_t px = (uint16_t)(((uint16_t)(r >> 3) << 11) |
                                         ((uint16_t)(g >> 2) << 5) |
                                         (uint16_t)(b >> 3));
                uint8_t *p = drow + (size_t)ox * 2u;
                p[0] = (uint8_t)(px & 0xFFu);
                p[1] = (uint8_t)(px >> 8);
            } else {
                uint8_t *p = drow + (size_t)ox * 4u;
                p[0] = bgr ? b : r;
                p[1] = g;
                p[2] = bgr ? r : b;
                p[3] = 0xFFu;
            }
        }
    }
    return 0;
}

/* --- Venus HEVC tile decode core --- */

static int poll_for_source_change(struct venus_ctx *vctx, uint64_t timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    while (!vctx->streaming_out) {
        uint64_t now = monotonic_ms();
        if (now >= deadline)
            return -ETIMEDOUT;
        int wait_ms = (int)min_u32((uint32_t)(deadline - now), 50u);
        uint32_t ev = 0;
        int rc = venus_ctx_poll(vctx, wait_ms, &ev);
        if (rc < 0)
            return rc;
        if (ev & VENUS_POLL_EVENT) {
            struct v4l2_event v4ev;
            while (venus_ctx_dq_event(vctx, &v4ev) == 0) {
                if (v4ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    rc = venus_ctx_handle_source_change(vctx);
                    if (rc < 0)
                        return rc;
                } else if (v4ev.type == V4L2_EVENT_EOS) {
                    return -EPIPE;
                }
            }
        }
        uint32_t in_idx = 0;
        while (venus_ctx_dq_input(vctx, &in_idx) == 0) {}
        if (ev & VENUS_POLL_ERR)
            return -EIO;
    }
    return 0;
}

static int do_heif_decode_with_ops(const void *inputs, void *output,
                                   const struct CodecHeifDecInfoRaw *decInfo,
                                   const struct heif_runtime_ops *ops)
{
    if (!inputs || !output || !decInfo || !ops || !ops->read_inputs_fn ||
        !ops->map_output_fn || !ops->unmap_output_fn || !ops->sync_dmabuf_fn)
        return -1;

    uint32_t disp_w = decInfo->displayWidth;
    uint32_t disp_h = decInfo->displayHeight;
    uint32_t sample = decInfo->sampleSize == 0u ? 1u : decInfo->sampleSize;
    if (disp_w == 0u || disp_h == 0u)
        return -1;
    if (sample != 1u && sample != 2u && sample != 4u && sample != 8u && sample != 16u)
        return -1;

    bool enable_grid = (decInfo->enableGrid != 0u);
    uint32_t cols = enable_grid ? decInfo->cols : 1u;
    uint32_t rows = enable_grid ? decInfo->rows : 1u;
    uint32_t tile_w = enable_grid ? decInfo->tileWidth : disp_w;
    uint32_t tile_h = enable_grid ? decInfo->tileHeight : disp_h;
    if (cols == 0u || rows == 0u || cols > HEIF_MAX_TILES || rows > HEIF_MAX_TILES ||
        (uint64_t)cols * rows > HEIF_MAX_TILES)
        return -1;
    uint32_t num_tiles = cols * rows;
    if (tile_w < 96u || tile_h < 96u || tile_w > 8192u || tile_h > 8192u)
        return -1;

    struct heif_output_target dst;
    if (ops->map_output_fn(output, &dst) != 0)
        return -1;

    uint32_t eff_disp_w = min_u32((uint32_t)dst.bh->width, (disp_w + sample - 1u) / sample);
    uint32_t eff_disp_h = min_u32((uint32_t)dst.bh->height, (disp_h + sample - 1u) / sample);
    if (eff_disp_w == 0u || eff_disp_h == 0u) {
        ops->unmap_output_fn(&dst);
        return -1;
    }

    struct heif_input_slice *raw_slices = NULL;
    size_t raw_cnt = 0;
    if (ops->read_inputs_fn(inputs, &raw_slices, &raw_cnt) != 0) {
        ops->unmap_output_fn(&dst);
        return -1;
    }

    size_t tile_start_idx = 0;
    uint8_t *hdr_annexb = NULL;
    size_t hdr_len = 0;

    if (raw_cnt == (size_t)num_tiles + 1u) {
        if (heif_to_annexb(raw_slices[0].data, raw_slices[0].len, true, &hdr_annexb, &hdr_len) != 0) {
            heif_log(true, "header slice convert failed len=%zu", raw_slices[0].len);
            free_input_slices(raw_slices, raw_cnt);
            ops->unmap_output_fn(&dst);
            return -1;
        }
        tile_start_idx = 1u;
    } else if (raw_cnt == (size_t)num_tiles) {
        tile_start_idx = 0u;
    } else {
        heif_log(true, "unexpected input slice count=%zu num_tiles=%u", raw_cnt, num_tiles);
        free_input_slices(raw_slices, raw_cnt);
        ops->unmap_output_fn(&dst);
        return -1;
    }

    struct heif_input_slice *annexb_tiles = calloc(num_tiles, sizeof(*annexb_tiles));
    if (!annexb_tiles) {
        free(hdr_annexb);
        free_input_slices(raw_slices, raw_cnt);
        ops->unmap_output_fn(&dst);
        return -1;
    }
    for (uint32_t t = 0; t < num_tiles; t++) {
        const struct heif_input_slice *src = &raw_slices[tile_start_idx + t];
        bool allow_hvcc = (tile_start_idx == 0u && t == 0u);
        if (heif_to_annexb(src->data, src->len, allow_hvcc,
                           &annexb_tiles[t].data, &annexb_tiles[t].len) != 0) {
            heif_log(true, "tile %u convert failed len=%zu", t, src->len);
            free_input_slices(annexb_tiles, num_tiles);
            free(hdr_annexb);
            free_input_slices(raw_slices, raw_cnt);
            ops->unmap_output_fn(&dst);
            return -1;
        }
    }
    free_input_slices(raw_slices, raw_cnt);
    raw_slices = NULL;

    /* Limit concurrent Venus HEIF sessions */
    pthread_mutex_lock(&g_conc_mu);
    while (g_active_sessions >= HEIF_MAX_CONCURRENT)
        pthread_cond_wait(&g_conc_cv, &g_conc_mu);
    g_active_sessions++;
    pthread_mutex_unlock(&g_conc_mu);

    int rc = -1;
    uint32_t tiles_decoded = 0;
    struct venus_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    vctx.fd = -1;
    vctx.wake_efd = -1;

    if (venus_ctx_open(&vctx, VENUS_ROLE_DECODER, V4L2_PIX_FMT_HEVC, V4L2_PIX_FMT_NV12,
                       tile_w, tile_h, ops->venus_ops) < 0) {
        heif_log(true, "venus_ctx_open failed tile=%ux%u", tile_w, tile_h);
        goto cleanup;
    }
    if (venus_ctx_configure(&vctx, tile_w, tile_h, 30u, 0u, 0u, -1, -1, -1) < 0) {
        heif_log(true, "venus_ctx_configure failed tile=%ux%u", tile_w, tile_h);
        goto cleanup;
    }
    if (venus_ctx_start(&vctx) < 0) {
        heif_log(true, "venus_ctx_start failed");
        goto cleanup;
    }

    size_t first_len = hdr_len + annexb_tiles[0].len;
    if (first_len == 0u || first_len > vctx.fmt.in_buf_size) {
        heif_log(true, "tile 0 too large: %zu > %u", first_len, vctx.fmt.in_buf_size);
        goto cleanup;
    }
    int slot0 = venus_ctx_acquire_free_input(&vctx);
    if (slot0 < 0)
        goto cleanup;
    (void)venus_dmabuf_sync(&vctx, vctx.in_bufs[slot0].fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
    uint8_t *in_map0 = (uint8_t *)vctx.in_bufs[slot0].map;
    if (hdr_len > 0u)
        memcpy(in_map0, hdr_annexb, hdr_len);
    memcpy(in_map0 + hdr_len, annexb_tiles[0].data, annexb_tiles[0].len);
    (void)venus_dmabuf_sync(&vctx, vctx.in_bufs[slot0].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

    if (venus_ctx_queue_input(&vctx, (uint32_t)slot0, (uint32_t)first_len, 0LL,
                              V4L2_BUF_FLAG_KEYFRAME) < 0) {
        heif_log(true, "queue tile 0 failed");
        goto cleanup;
    }

    if (poll_for_source_change(&vctx, 2000u) < 0 || !vctx.streaming_out) {
        heif_log(true, "timeout waiting SOURCE_CHANGE tile=%ux%u", tile_w, tile_h);
        goto cleanup;
    }

    /* Inspect post-SOURCE_CHANGE colorspace and quantization */
    bool full_range = true;
    bool bt709 = false;
    struct v4l2_format cap_fmt;
    memset(&cap_fmt, 0, sizeof(cap_fmt));
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (vctx.ops.ioctl_fn &&
        vctx.ops.ioctl_fn(vctx.ops.userdata, vctx.fd, VIDIOC_G_FMT, &cap_fmt) == 0) {
        if (cap_fmt.fmt.pix_mp.quantization == V4L2_QUANTIZATION_LIM_RANGE)
            full_range = false;
        else if (cap_fmt.fmt.pix_mp.quantization == V4L2_QUANTIZATION_FULL_RANGE ||
                 cap_fmt.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
            full_range = true;
        if (cap_fmt.fmt.pix_mp.colorspace == V4L2_COLORSPACE_REC709)
            bt709 = true;
    }

    /* Prepare destination write sync bracket */
    (void)ops->sync_dmabuf_fn(dst.bh->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);

    uint32_t next_tile = 1u;
    bool saw_last = false;
    bool seen[HEIF_MAX_TILES];
    memset(seen, 0, sizeof(seen));
    uint64_t budget_ms = (uint64_t)num_tiles * 250ULL;
    if (budget_ms < 5000ULL)
        budget_ms = 5000ULL;
    uint64_t deadline = monotonic_ms() + budget_ms;

    while (!saw_last && (tiles_decoded < num_tiles || vctx.eos_sent)) {
        uint32_t dq_in = 0;
        while (venus_ctx_dq_input(&vctx, &dq_in) == 0) {}

        while (next_tile < num_tiles) {
            int slot = venus_ctx_acquire_free_input(&vctx);
            if (slot < 0)
                break;
            size_t tlen = annexb_tiles[next_tile].len;
            if (tlen == 0u || tlen > vctx.fmt.in_buf_size) {
                heif_log(true, "tile %u bad len=%zu > %u", next_tile, tlen, vctx.fmt.in_buf_size);
                goto sync_end;
            }
            (void)venus_dmabuf_sync(&vctx, vctx.in_bufs[slot].fd,
                                    DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            memcpy(vctx.in_bufs[slot].map, annexb_tiles[next_tile].data, tlen);
            (void)venus_dmabuf_sync(&vctx, vctx.in_bufs[slot].fd,
                                    DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            if (venus_ctx_queue_input(&vctx, (uint32_t)slot, (uint32_t)tlen,
                                      (int64_t)next_tile, V4L2_BUF_FLAG_KEYFRAME) < 0) {
                heif_log(true, "queue tile %u failed", next_tile);
                goto sync_end;
            }
            next_tile++;
        }

        if (next_tile == num_tiles && !vctx.eos_sent) {
            int eos_rc = venus_ctx_send_eos(&vctx);
            if (eos_rc < 0 && eos_rc != -EAGAIN) {
                heif_log(true, "venus_ctx_send_eos rc=%d", eos_rc);
                goto sync_end;
            }
        }

        for (;;) {
            uint32_t out_idx = 0, bytes = 0, flags = 0;
            int64_t ts = -1;
            int dq = venus_ctx_dq_output(&vctx, &out_idx, &bytes, &ts, &flags);
            if (dq == -EAGAIN)
                break;
            if (dq < 0)
                goto sync_end;

            if (bytes > 0u) {
                uint32_t tidx = UINT32_MAX;
                if (ts >= 0 && ts < (int64_t)num_tiles && !seen[ts]) {
                    tidx = (uint32_t)ts;
                } else {
                    for (uint32_t k = 0; k < num_tiles; k++) {
                        if (!seen[k]) {
                            tidx = k;
                            break;
                        }
                    }
                }
                if (tidx < num_tiles) {
                    seen[tidx] = true;
                    uint32_t row = tidx / cols;
                    uint32_t col = tidx % cols;
                    uint32_t cw = vctx.fmt.crop_width ? min_u32(tile_w, vctx.fmt.crop_width) : tile_w;
                    uint32_t ch = vctx.fmt.crop_height ? min_u32(tile_h, vctx.fmt.crop_height) : tile_h;
                    (void)venus_dmabuf_sync(&vctx, vctx.out_bufs[out_idx].fd,
                                            DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
                    int brc = heif_blit_nv12_tile((const uint8_t *)vctx.out_bufs[out_idx].map,
                                                  vctx.fmt.uv_offset, vctx.fmt.y_stride,
                                                  vctx.fmt.crop_left, vctx.fmt.crop_top,
                                                  cw, ch, vctx.out_bufs[out_idx].size,
                                                  &dst, col * tile_w, row * tile_h,
                                                  disp_w, disp_h, eff_disp_w, eff_disp_h,
                                                  sample, full_range, bt709);
                    (void)venus_dmabuf_sync(&vctx, vctx.out_bufs[out_idx].fd,
                                            DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                    if (brc != 0) {
                        (void)venus_ctx_requeue_output(&vctx, out_idx);
                        heif_log(true, "blit tile %u failed fmt=%d", tidx, dst.bh->format);
                        goto sync_end;
                    }
                    tiles_decoded++;
                }
            }
            if (flags & V4L2_BUF_FLAG_LAST) {
                saw_last = true;
                break;
            }
            (void)venus_ctx_requeue_output(&vctx, out_idx);
            if (!vctx.eos_sent && tiles_decoded >= num_tiles)
                break;
        }

        if (saw_last || (!vctx.eos_sent && tiles_decoded >= num_tiles))
            break;

        uint64_t now = monotonic_ms();
        if (now >= deadline) {
            heif_log(true, "stream deadline reached decoded=%u/%u", tiles_decoded, num_tiles);
            break;
        }
        uint32_t ev = 0;
        int prc = venus_ctx_poll(&vctx, (int)min_u32((uint32_t)(deadline - now), 50u), &ev);
        if (prc < 0)
            break;
        if (ev & VENUS_POLL_EVENT) {
            struct v4l2_event v4ev;
            while (venus_ctx_dq_event(&vctx, &v4ev) == 0) {
                if (v4ev.type == V4L2_EVENT_SOURCE_CHANGE)
                    (void)venus_ctx_handle_source_change(&vctx);
                else if (v4ev.type == V4L2_EVENT_EOS)
                    saw_last = true;
            }
        }
        if (ev & VENUS_POLL_ERR)
            break;
    }

    if (tiles_decoded == num_tiles)
        rc = 0;

sync_end:
    (void)ops->sync_dmabuf_fn(dst.bh->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);

cleanup:
    venus_ctx_close(&vctx);
    pthread_mutex_lock(&g_conc_mu);
    if (g_active_sessions > 0u)
        g_active_sessions--;
    pthread_cond_signal(&g_conc_cv);
    pthread_mutex_unlock(&g_conc_mu);

    free_input_slices(annexb_tiles, num_tiles);
    free(hdr_annexb);
    ops->unmap_output_fn(&dst);

    heif_log(rc != 0,
             "DoHeifDecode ret=%d disp=%ux%u grid=%u cols=%u rows=%u tile=%ux%u sample=%u fmt=%d tiles=%u/%u",
             rc, disp_w, disp_h, (unsigned)enable_grid, cols, rows, tile_w, tile_h,
             sample, dst.bh ? dst.bh->format : -1, tiles_decoded, num_tiles);
    return rc;
}

static int32_t heif_vdi_do_decode(const void *inputs, void *output,
                                  const struct CodecHeifDecInfoRaw *decInfo)
{
    return do_heif_decode_with_ops(inputs, output, decInfo, &g_default_runtime_ops);
}

static struct ICodecHeifDecodeHwi g_heif_decode_hwi = {
    .DoHeifDecode = heif_vdi_do_decode,
};

struct ICodecHeifDecodeHwi *GetCodecHeifDecodeHwi(void)
{
    return &g_heif_decode_hwi;
}

#ifdef HEIF_VDI_HOST_TEST
#include <assert.h>

struct mock_ashmem_item {
    const uint8_t *data;
    size_t len;
};

struct mock_frame {
    uint8_t *nv12;
    size_t nv12_len;
    int64_t ts_us;
};

struct mock_state {
    uint32_t width;
    uint32_t height;
    uint32_t y_stride;
    uint32_t uv_offset;
    uint32_t raw_size;
    uint32_t colorspace;
    uint32_t quantization;
    uint32_t in_count;
    uint32_t out_count;
    uint32_t cap_queued[VENUS_MAX_BUFS];
    bool cap_avail[VENUS_MAX_BUFS];
    bool in_queued[VENUS_MAX_BUFS];
    int in_fds[VENUS_MAX_BUFS];
    int out_fds[VENUS_MAX_BUFS];
    int in_fd_base;
    int out_fd_base;
    int next_dmabuf_fd;
    uint8_t *fd_maps[64];
    struct mock_frame frames[8];
    uint32_t queued_tile_cnt;
    uint32_t emitted_cnt;
    bool source_change_pending;
    bool source_change_sent;
    bool streamon_in;
    bool streamon_out;
    bool eos_sent;
    bool last_flag_dequeued;
    bool saw_raw_slice_without_hdr;
    bool truncated_mode;

    uint32_t dmabuf_sync_calls;
    BufferHandle mock_bh;
    uint8_t *mock_out_buf;
    size_t mock_out_total;
};

static int mock_open(void *ud, const char *path, int flags)
{
    (void)ud; (void)path; (void)flags;
    return 100;
}

static int mock_close(void *ud, int fd)
{
    struct mock_state *st = (struct mock_state *)ud;
    if (fd >= 200 && fd < 264 && st->fd_maps[fd - 200]) {
        free(st->fd_maps[fd - 200]);
        st->fd_maps[fd - 200] = NULL;
    }
    return 0;
}

static int mock_dmabuf_alloc(void *ud, uint32_t size)
{
    struct mock_state *st = (struct mock_state *)ud;
    int fd = st->next_dmabuf_fd++;
    assert(fd >= 200 && fd < 264);
    uint8_t *buf = calloc(1, size);
    assert(buf != NULL);
    st->fd_maps[fd - 200] = buf;
    return fd;
}

static void *mock_mmap(void *ud, size_t len, int prot, int flags, int fd, off_t off)
{
    (void)len; (void)prot; (void)flags; (void)off;
    struct mock_state *st = (struct mock_state *)ud;
    if (fd >= 200 && fd < 264 && st->fd_maps[fd - 200])
        return st->fd_maps[fd - 200];
    return MAP_FAILED;
}

static int mock_munmap(void *ud, void *addr, size_t len)
{
    (void)ud; (void)addr; (void)len;
    return 0;
}

static int mock_dmabuf_sync(void *ud, int fd, uint64_t flags)
{
    (void)fd; (void)flags;
    struct mock_state *st = (struct mock_state *)ud;
    st->dmabuf_sync_calls++;
    return 0;
}

static int mock_ioctl(void *ud, int fd, unsigned long req, void *arg)
{
    (void)fd;
    struct mock_state *st = (struct mock_state *)ud;
    if (req == VIDIOC_QUERYCAP) {
        struct v4l2_capability *cap = (struct v4l2_capability *)arg;
        cap->capabilities = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
        return 0;
    }
    if (req == VIDIOC_S_FMT || req == VIDIOC_G_FMT) {
        struct v4l2_format *fmt = (struct v4l2_format *)arg;
        if (fmt->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            fmt->fmt.pix_mp.width = st->width;
            fmt->fmt.pix_mp.height = st->height;
            fmt->fmt.pix_mp.plane_fmt[0].sizeimage = 1024u * 1024u;
            return 0;
        }
        if (fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            fmt->fmt.pix_mp.width = st->width;
            fmt->fmt.pix_mp.height = st->height;
            fmt->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
            fmt->fmt.pix_mp.colorspace = st->colorspace;
            fmt->fmt.pix_mp.quantization = st->quantization;
            fmt->fmt.pix_mp.plane_fmt[0].bytesperline = st->y_stride;
            fmt->fmt.pix_mp.plane_fmt[0].sizeimage = st->raw_size;
            return 0;
        }
    }
    if (req == VIDIOC_G_SELECTION) {
        struct v4l2_selection *sel = (struct v4l2_selection *)arg;
        sel->r.left = 0;
        sel->r.top = 0;
        sel->r.width = st->width;
        sel->r.height = st->height;
        return 0;
    }
    if (req == VIDIOC_G_EXT_CTRLS) {
        struct v4l2_ext_controls *ctrls = (struct v4l2_ext_controls *)arg;
        if (ctrls->count > 0)
            ctrls->controls[0].value = 4;
        return 0;
    }
    if (req == VIDIOC_SUBSCRIBE_EVENT)
        return 0;
    if (req == VIDIOC_REQBUFS) {
        struct v4l2_requestbuffers *rb = (struct v4l2_requestbuffers *)arg;
        if (rb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
            st->in_count = rb->count;
        else
            st->out_count = rb->count;
        return 0;
    }
    if (req == VIDIOC_STREAMON) {
        uint32_t t = *(uint32_t *)arg;
        if (t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
            st->streamon_in = true;
        else
            st->streamon_out = true;
        return 0;
    }
    if (req == VIDIOC_STREAMOFF) {
        uint32_t t = *(uint32_t *)arg;
        if (t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            if (st->eos_sent && st->streamon_out)
                assert(st->last_flag_dequeued);
            st->streamon_in = false;
        } else {
            st->streamon_out = false;
        }
        return 0;
    }
    if (req == VIDIOC_QBUF) {
        struct v4l2_buffer *vb = (struct v4l2_buffer *)arg;
        int bfd = vb->m.planes[0].m.fd;
        if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            st->in_fds[vb->index] = bfd;
            uint32_t bytes = vb->m.planes[0].bytesused;
            int64_t ts = (int64_t)vb->timestamp.tv_sec * 1000000LL + vb->timestamp.tv_usec;
            const uint8_t *payload = (bfd >= 200 && bfd < 264) ? st->fd_maps[bfd - 200] : NULL;
            assert(payload != NULL && bytes >= 4 && payload[0] == 0 && payload[1] == 0 &&
                   payload[2] == 0 && payload[3] == 1);
            if (ts > 0) {
                /* Verify tiles 1..N-1 do not start with VPS (NAL type 32 -> 0x40) */
                assert(((payload[4] >> 1) & 0x3Fu) != 32u);
                st->saw_raw_slice_without_hdr = true;
            }
            st->in_queued[vb->index] = true;
            if (!st->source_change_sent)
                st->source_change_pending = true;
            st->queued_tile_cnt++;
        } else {
            st->out_fds[vb->index] = bfd;
            st->cap_avail[vb->index] = true;
        }
        return 0;
    }
    if (req == VIDIOC_DQEVENT) {
        struct v4l2_event *ev = (struct v4l2_event *)arg;
        if (st->source_change_pending && !st->source_change_sent) {
            ev->type = V4L2_EVENT_SOURCE_CHANGE;
            ev->u.src_change.changes = V4L2_EVENT_SRC_CH_RESOLUTION;
            st->source_change_pending = false;
            st->source_change_sent = true;
            return 0;
        }
        return -ENOENT;
    }
    if (req == VIDIOC_DECODER_CMD) {
        struct v4l2_decoder_cmd *cmd = (struct v4l2_decoder_cmd *)arg;
        if (cmd->cmd == V4L2_DEC_CMD_STOP) {
            assert(st->streamon_out);
            st->eos_sent = true;
        }
        return 0;
    }
    if (req == VIDIOC_DQBUF) {
        struct v4l2_buffer *vb = (struct v4l2_buffer *)arg;
        if (vb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            for (uint32_t i = 0; i < st->in_count; i++) {
                if (st->in_queued[i]) {
                    st->in_queued[i] = false;
                    vb->index = i;
                    return 0;
                }
            }
            return -EAGAIN;
        }
        if (vb->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            uint32_t limit = st->truncated_mode ? (st->queued_tile_cnt > 0 ? st->queued_tile_cnt - 1u : 0u)
                                                : st->queued_tile_cnt;
            if (st->emitted_cnt < limit) {
                for (uint32_t i = 0; i < st->out_count; i++) {
                    if (st->cap_avail[i]) {
                        st->cap_avail[i] = false;
                        struct mock_frame *fr = &st->frames[st->emitted_cnt++];
                        int obfd = st->out_fds[i];
                        assert(obfd >= 200 && obfd < 264 && st->fd_maps[obfd - 200] != NULL);
                        memcpy(st->fd_maps[obfd - 200], fr->nv12, fr->nv12_len);
                        vb->index = i;
                        vb->m.planes[0].bytesused = (uint32_t)fr->nv12_len;
                        vb->timestamp.tv_sec = fr->ts_us / 1000000LL;
                        vb->timestamp.tv_usec = fr->ts_us % 1000000LL;
                        vb->flags = 0u;
                        return 0;
                    }
                }
            }
            if (st->emitted_cnt >= limit && st->eos_sent && !st->last_flag_dequeued) {
                for (uint32_t i = 0; i < st->out_count; i++) {
                    if (st->cap_avail[i]) {
                        st->cap_avail[i] = false;
                        vb->index = i;
                        vb->m.planes[0].bytesused = 0u;
                        vb->flags = V4L2_BUF_FLAG_LAST;
                        st->last_flag_dequeued = true;
                        return 0;
                    }
                }
            }
            return -EAGAIN;
        }
    }
    return 0;
}

static int mock_poll(void *ud, struct pollfd *pfds, nfds_t nfds, int timeout_ms)
{
    (void)timeout_ms;
    struct mock_state *st = (struct mock_state *)ud;
    if (nfds > 0) {
        pfds[0].revents = POLLOUT;
        if (st->source_change_pending)
            pfds[0].revents |= POLLPRI;
        uint32_t limit = st->truncated_mode ? (st->queued_tile_cnt > 0 ? st->queued_tile_cnt - 1u : 0u)
                                            : st->queued_tile_cnt;
        if (st->streamon_out && (st->emitted_cnt < limit || (st->eos_sent && !st->last_flag_dequeued)))
            pfds[0].revents |= POLLIN;
    }
    return 1;
}

static int mock_read_inputs(const void *inputs, struct heif_input_slice **out_slices, size_t *out_cnt)
{
    const struct AshmemVecRaw *vec = (const struct AshmemVecRaw *)inputs;
    size_t cnt = (size_t)(vec->end - vec->begin);
    struct heif_input_slice *slices = calloc(cnt, sizeof(*slices));
    assert(slices != NULL);
    for (size_t i = 0; i < cnt; i++) {
        const struct mock_ashmem_item *item = (const struct mock_ashmem_item *)vec->begin[i];
        slices[i].data = malloc(item->len);
        assert(slices[i].data != NULL);
        memcpy(slices[i].data, item->data, item->len);
        slices[i].len = item->len;
    }
    *out_slices = slices;
    *out_cnt = cnt;
    return 0;
}

static struct mock_state *g_cur_mock = NULL;

static int mock_map_output(void *output, struct heif_output_target *out)
{
    (void)output;
    struct mock_state *st = g_cur_mock;
    size_t pbytes = 0;
    if (compute_dst_pixel_bytes(&st->mock_bh, &pbytes) != 0)
        return -1;
    out->bh = &st->mock_bh;
    out->map_ptr = st->mock_out_buf;
    out->map_len = st->mock_out_total;
    out->dst_pixel_bytes = pbytes;
    out->mmap_owned = false;
    return 0;
}

static void mock_unmap_output(struct heif_output_target *out)
{
    (void)out;
}

static int mock_sync_cb(int fd, uint64_t flags)
{
    (void)fd; (void)flags;
    g_cur_mock->dmabuf_sync_calls++;
    return 0;
}

static uint8_t *alloc_solid_nv12(uint32_t w, uint32_t h, uint32_t stride,
                                 uint32_t uv_off, uint32_t total,
                                 uint8_t y, uint8_t u, uint8_t v)
{
    uint8_t *buf = calloc(1, total);
    assert(buf != NULL);
    for (uint32_t r = 0; r < h; r++)
        memset(buf + (size_t)r * stride, y, w);
    uint8_t *uv = buf + uv_off;
    for (uint32_t r = 0; r < h / 2u; r++) {
        for (uint32_t c = 0; c < w; c += 2u) {
            uv[(size_t)r * stride + c + 0u] = u;
            uv[(size_t)r * stride + c + 1u] = v;
        }
    }
    return buf;
}

static void setup_mock_dst(struct mock_state *st, int32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t bpp = (fmt == OHOS_PIXEL_FMT_RGB_565) ? 2u :
                   ((fmt == OHOS_PIXEL_FMT_YCBCR_420_SP || fmt == OHOS_PIXEL_FMT_YCRCB_420_SP ||
                     fmt == OHOS_PIXEL_FMT_YCBCR_420_P) ? 1u : 4u);
    uint32_t stride = venus_align_u32(w * bpp, 128u);
    uint64_t pix = (bpp == 1u)
        ? (uint64_t)stride * h + (uint64_t)stride * ((h + 1u) / 2u)
        : (uint64_t)stride * ((h + 3u) & ~3u);
    uint64_t total = ((pix + 4095ULL) & ~4095ULL) + 4096ULL;

    free(st->mock_out_buf);
    st->mock_out_buf = calloc(1, (size_t)total);
    assert(st->mock_out_buf != NULL);
    /* Fill trailing 4096-byte CropStore page with 0xA5 canary */
    memset(st->mock_out_buf + total - 4096u, 0xA5, 4096u);

    memset(&st->mock_bh, 0, sizeof(st->mock_bh));
    st->mock_bh.fd = 77;
    st->mock_bh.width = (int32_t)w;
    st->mock_bh.height = (int32_t)h;
    st->mock_bh.stride = (int32_t)stride;
    st->mock_bh.size = (int32_t)total;
    st->mock_bh.format = fmt;
    st->mock_out_total = (size_t)total;
}

static void verify_canary(const struct mock_state *st)
{
    const uint8_t *tail = st->mock_out_buf + st->mock_out_total - 4096u;
    for (size_t i = 0; i < 4096u; i++)
        assert(tail[i] == 0xA5u);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct ICodecHeifDecodeHwi *hwi = GetCodecHeifDecodeHwi();
    assert(hwi != NULL && hwi->DoHeifDecode != NULL);

    /* Test heif_to_annexb across Annex-B, 4-byte BE length-prefixed, and hvcC */
    const uint8_t annex_in[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C};
    uint8_t *out_ab = NULL;
    size_t out_ab_len = 0;
    assert(heif_to_annexb(annex_in, sizeof(annex_in), true, &out_ab, &out_ab_len) == 0);
    assert(out_ab_len == sizeof(annex_in) && memcmp(out_ab, annex_in, out_ab_len) == 0);
    free(out_ab);

    const uint8_t lenpref_in[] = {0x00, 0x00, 0x00, 0x03, 0x26, 0x01, 0xAF};
    assert(heif_to_annexb(lenpref_in, sizeof(lenpref_in), false, &out_ab, &out_ab_len) == 0);
    assert(out_ab_len == sizeof(lenpref_in) && out_ab[3] == 0x01 && out_ab[4] == 0x26);
    free(out_ab);

    /* Minimal valid hvcC with 1 array containing 1 3-byte VPS NALU */
    const uint8_t hvcc_in[22 + 1 + 1 + 2 + 2 + 3] = {
        [0] = 1, [21] = 0xFF, [22] = 1,
        [23] = 0xA0, [24] = 0, [25] = 1, [26] = 0, [27] = 3,
        [28] = 0x40, [29] = 0x01, [30] = 0x0C
    };
    assert(heif_to_annexb(hvcc_in, sizeof(hvcc_in), true, &out_ab, &out_ab_len) == 0);
    assert(out_ab_len == 7 && out_ab[3] == 0x01 && out_ab[4] == 0x40 && out_ab[6] == 0x0C);
    free(out_ab);

    /* Setup single + 2x2 grid mock Venus session */
    const uint8_t hdr_ab[] = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C}; /* VPS NAL type 32 */
    const uint8_t idr_lenpref[] = {0x00, 0x00, 0x00, 0x04, 0x26, 0x01, 0xAA, 0xBB}; /* IDR NAL type 19 */
    const struct mock_ashmem_item item_hdr = { hdr_ab, sizeof(hdr_ab) };
    const struct mock_ashmem_item item_idr = { idr_lenpref, sizeof(idr_lenpref) };
    void *single_vec_ptrs[2] = { (void *)&item_hdr, (void *)&item_idr };
    struct AshmemVecRaw single_vec = { single_vec_ptrs, single_vec_ptrs + 2, single_vec_ptrs + 2 };

    struct mock_state st;
    memset(&st, 0, sizeof(st));
    st.width = 128u;
    st.height = 128u;
    st.y_stride = venus_y_stride(V4L2_PIX_FMT_NV12, 128u);
    uint32_t y_lines = venus_y_scanlines(V4L2_PIX_FMT_NV12, 128u);
    st.uv_offset = st.y_stride * y_lines;
    st.raw_size = venus_raw_buffer_size(V4L2_PIX_FMT_NV12, 128u, 128u);
    st.colorspace = V4L2_COLORSPACE_SMPTE170M;
    st.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    st.in_fd_base = 200;
    st.out_fd_base = 210;
    st.next_dmabuf_fd = 200;

    struct venus_sys_ops vops = {
        .userdata = &st,
        .open_fn = mock_open,
        .close_fn = mock_close,
        .ioctl_fn = mock_ioctl,
        .poll_fn = mock_poll,
        .mmap_fn = mock_mmap,
        .munmap_fn = mock_munmap,
        .dmabuf_alloc_fn = mock_dmabuf_alloc,
        .dmabuf_sync_fn = mock_dmabuf_sync,
    };
    struct heif_runtime_ops rops = {
        .read_inputs_fn = mock_read_inputs,
        .map_output_fn = mock_map_output,
        .unmap_output_fn = mock_unmap_output,
        .sync_dmabuf_fn = mock_sync_cb,
        .venus_ops = &vops,
    };
    g_cur_mock = &st;

    /* Test single decode into NV12, NV21, I420, RGBA8888, BGRA8888, RGB565 */
    /* Full-range Red ~(Y=76, U=85, V=255) */
    uint8_t *red_nv12 = alloc_solid_nv12(128u, 128u, st.y_stride, st.uv_offset, st.raw_size, 76u, 85u, 255u);
    const int32_t fmts[] = {
        OHOS_PIXEL_FMT_YCBCR_420_SP,
        OHOS_PIXEL_FMT_YCRCB_420_SP,
        OHOS_PIXEL_FMT_YCBCR_420_P,
        OHOS_PIXEL_FMT_RGBA_8888,
        OHOS_PIXEL_FMT_BGRA_8888,
        OHOS_PIXEL_FMT_RGB_565,
    };
    for (size_t fi = 0; fi < sizeof(fmts) / sizeof(fmts[0]); fi++) {
        st.next_dmabuf_fd = 200;
        st.queued_tile_cnt = 0;
        st.emitted_cnt = 0;
        st.source_change_pending = false;
        st.source_change_sent = false;
        st.streamon_in = false;
        st.streamon_out = false;
        st.eos_sent = false;
        st.last_flag_dequeued = false;
        st.dmabuf_sync_calls = 0;
        st.frames[0].nv12 = red_nv12;
        st.frames[0].nv12_len = st.raw_size;
        st.frames[0].ts_us = 0;
        setup_mock_dst(&st, fmts[fi], 128u, 128u);

        struct CodecHeifDecInfoRaw info = {
            .displayWidth = 128u, .displayHeight = 128u, .enableGrid = 0u,
            .cols = 1u, .rows = 1u, .tileWidth = 128u, .tileHeight = 128u,
            .sampleSize = 1u,
        };
        void *out_sb = (void *)0x1;
        assert(do_heif_decode_with_ops(&single_vec, &out_sb, &info, &rops) == 0);
        assert(st.dmabuf_sync_calls >= 2u);
        verify_canary(&st);

        uint32_t dstr = (uint32_t)st.mock_bh.stride;
        if (fmts[fi] == OHOS_PIXEL_FMT_YCBCR_420_SP) {
            assert(st.mock_out_buf[0] == 76u);
            assert(st.mock_out_buf[dstr * 128u + 0] == 85u);
            assert(st.mock_out_buf[dstr * 128u + 1] == 255u);
        } else if (fmts[fi] == OHOS_PIXEL_FMT_YCRCB_420_SP) {
            assert(st.mock_out_buf[dstr * 128u + 0] == 255u);
            assert(st.mock_out_buf[dstr * 128u + 1] == 85u);
        } else if (fmts[fi] == OHOS_PIXEL_FMT_YCBCR_420_P) {
            size_t u_off = (size_t)dstr * 128u;
            size_t v_off = u_off + (size_t)(dstr / 2u) * 64u;
            assert(st.mock_out_buf[u_off] == 85u && st.mock_out_buf[v_off] == 255u);
        } else if (fmts[fi] == OHOS_PIXEL_FMT_RGBA_8888) {
            assert(st.mock_out_buf[0] > 230u && st.mock_out_buf[1] < 20u &&
                   st.mock_out_buf[2] < 20u && st.mock_out_buf[3] == 255u);
        } else if (fmts[fi] == OHOS_PIXEL_FMT_BGRA_8888) {
            assert(st.mock_out_buf[2] > 230u && st.mock_out_buf[1] < 20u &&
                   st.mock_out_buf[0] < 20u && st.mock_out_buf[3] == 255u);
        } else if (fmts[fi] == OHOS_PIXEL_FMT_RGB_565) {
            uint16_t px = (uint16_t)(st.mock_out_buf[0] | ((uint16_t)st.mock_out_buf[1] << 8));
            assert((px >> 11) >= 28u && ((px >> 5) & 0x3Fu) < 6u && (px & 0x1Fu) < 4u);
        }
    }

    /* Test 2x2 grid placement, cropped display (200x180), and sampleSize=2 -> 100x90 */
    uint8_t *t0 = red_nv12;
    uint8_t *t1 = alloc_solid_nv12(128u, 128u, st.y_stride, st.uv_offset, st.raw_size, 150u, 44u, 21u);   /* Green */
    uint8_t *t2 = alloc_solid_nv12(128u, 128u, st.y_stride, st.uv_offset, st.raw_size, 29u, 255u, 107u);  /* Blue */
    uint8_t *t3 = alloc_solid_nv12(128u, 128u, st.y_stride, st.uv_offset, st.raw_size, 226u, 0u, 149u);   /* Yellow */

    void *grid_vec_ptrs[5] = { (void *)&item_hdr, (void *)&item_idr, (void *)&item_idr,
                               (void *)&item_idr, (void *)&item_idr };
    struct AshmemVecRaw grid_vec = { grid_vec_ptrs, grid_vec_ptrs + 5, grid_vec_ptrs + 5 };

    /* Full 256x256 grid -> RGBA8888 */
    st.next_dmabuf_fd = 200;
    st.queued_tile_cnt = 0; st.emitted_cnt = 0;
    st.source_change_pending = false; st.source_change_sent = false;
    st.streamon_in = false; st.streamon_out = false; st.eos_sent = false; st.last_flag_dequeued = false;
    st.saw_raw_slice_without_hdr = false;
    st.frames[0] = (struct mock_frame){ t0, st.raw_size, 0 };
    st.frames[1] = (struct mock_frame){ t1, st.raw_size, 1 };
    st.frames[2] = (struct mock_frame){ t2, st.raw_size, 2 };
    st.frames[3] = (struct mock_frame){ t3, st.raw_size, 3 };
    setup_mock_dst(&st, OHOS_PIXEL_FMT_RGBA_8888, 256u, 256u);

    struct CodecHeifDecInfoRaw grid_info = {
        .displayWidth = 256u, .displayHeight = 256u, .enableGrid = 1u,
        .cols = 2u, .rows = 2u, .tileWidth = 128u, .tileHeight = 128u, .sampleSize = 1u,
    };
    void *out_sb = (void *)0x1;
    assert(do_heif_decode_with_ops(&grid_vec, &out_sb, &grid_info, &rops) == 0);
    assert(st.saw_raw_slice_without_hdr);
    verify_canary(&st);
    uint32_t dstr = (uint32_t)st.mock_bh.stride;
    const uint8_t *q0 = st.mock_out_buf + (size_t)32 * dstr + (size_t)32 * 4u;
    const uint8_t *q1 = st.mock_out_buf + (size_t)32 * dstr + (size_t)160 * 4u;
    const uint8_t *q2 = st.mock_out_buf + (size_t)160 * dstr + (size_t)32 * 4u;
    const uint8_t *q3 = st.mock_out_buf + (size_t)160 * dstr + (size_t)160 * 4u;
    assert(q0[0] > 200u && q0[1] < 30u && q0[2] < 30u);
    assert(q1[1] > 200u && q1[0] < 30u && q1[2] < 30u);
    assert(q2[2] > 200u && q2[0] < 30u && q2[1] < 30u);
    assert(q3[0] > 200u && q3[1] > 200u && q3[2] < 30u);

    /* Cropped + subsampled 200x180 (sampleSize=2 -> 100x90) */
    st.next_dmabuf_fd = 200;
    st.queued_tile_cnt = 0; st.emitted_cnt = 0;
    st.source_change_pending = false; st.source_change_sent = false;
    st.streamon_in = false; st.streamon_out = false; st.eos_sent = false; st.last_flag_dequeued = false;
    setup_mock_dst(&st, OHOS_PIXEL_FMT_RGBA_8888, 100u, 90u);
    struct CodecHeifDecInfoRaw crop_info = {
        .displayWidth = 200u, .displayHeight = 180u, .enableGrid = 1u,
        .cols = 2u, .rows = 2u, .tileWidth = 128u, .tileHeight = 128u, .sampleSize = 2u,
    };
    assert(do_heif_decode_with_ops(&grid_vec, &out_sb, &crop_info, &rops) == 0);
    verify_canary(&st);
    dstr = (uint32_t)st.mock_bh.stride;
    q0 = st.mock_out_buf + (size_t)16 * dstr + (size_t)16 * 4u;
    q1 = st.mock_out_buf + (size_t)16 * dstr + (size_t)80 * 4u;
    q2 = st.mock_out_buf + (size_t)75 * dstr + (size_t)16 * 4u;
    q3 = st.mock_out_buf + (size_t)75 * dstr + (size_t)80 * 4u;
    assert(q0[0] > 200u && q0[1] < 30u);
    assert(q1[1] > 200u && q1[0] < 30u);
    assert(q2[2] > 200u && q2[1] < 30u);
    assert(q3[0] > 200u && q3[1] > 200u && q3[2] < 30u);

    /* Negative tests: NULL inputs, unsupported pixel format 35, truncated tiles */
    assert(hwi->DoHeifDecode(NULL, &out_sb, &grid_info) != 0);
    assert(hwi->DoHeifDecode(&single_vec, NULL, &grid_info) != 0);
    assert(hwi->DoHeifDecode(&single_vec, &out_sb, NULL) != 0);
    st.mock_bh.format = 35;
    assert(do_heif_decode_with_ops(&single_vec, &out_sb, &crop_info, &rops) != 0);

    st.next_dmabuf_fd = 200;
    st.queued_tile_cnt = 0; st.emitted_cnt = 0;
    st.source_change_pending = false; st.source_change_sent = false;
    st.streamon_in = false; st.streamon_out = false; st.eos_sent = false; st.last_flag_dequeued = false;
    st.truncated_mode = true;
    setup_mock_dst(&st, OHOS_PIXEL_FMT_RGBA_8888, 256u, 256u);
    assert(do_heif_decode_with_ops(&grid_vec, &out_sb, &grid_info, &rops) != 0);

    free(red_nv12); free(t1); free(t2); free(t3);
    free(st.mock_out_buf);
    printf("HEIF_VDI_HOST_TEST_OK\n");
    return 0;
}
#endif
