/*
 * Clean-room OHOS Codec Image JPEG VDI implementation for SM7475 (marble).
 *
 * Bridges OHOS HDF codec_image_service (ICodecJpegHwi v2.1) to the system's
 * ARM64 NEON-accelerated /system/lib64/chipset-sdk/libturbojpeg.z.so, with
 * DMA-BUF allocation/sync and full JPEG marker reconstruction from
 * CodecJpegDecInfo when the caller supplies post-SOS scan data.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

/* PixelFormat values from drivers/interface/display/composer/v1_0/DisplayComposerType.idl */
enum {
    OHOS_PIXEL_FMT_RGB_565 = 3,
    OHOS_PIXEL_FMT_RGBX_8888 = 11,
    OHOS_PIXEL_FMT_RGBA_8888 = 12,
    OHOS_PIXEL_FMT_RGB_888 = 13,
    OHOS_PIXEL_FMT_BGRX_8888 = 19,
    OHOS_PIXEL_FMT_BGRA_8888 = 20,
    OHOS_PIXEL_FMT_YCBCR_420_SP = 24, /* NV12 */
    OHOS_PIXEL_FMT_YCRCB_420_SP = 25, /* NV21 */
    OHOS_PIXEL_FMT_YCBCR_420_P = 28,  /* I420 */
    OHOS_PIXEL_FMT_YCRCB_420_P = 29,  /* YV12 */
};

/* TurboJPEG ABI constants (libjpeg-turbo) */
enum {
    TJ_PF_RGB = 0,
    TJ_PF_BGR = 1,
    TJ_PF_RGBX = 2,
    TJ_PF_BGRX = 3,
    TJ_PF_RGBA = 7,
    TJ_PF_BGRA = 8,
};

enum {
    TJ_SAMP_444 = 0,
    TJ_SAMP_422 = 1,
    TJ_SAMP_420 = 2,
    TJ_SAMP_GRAY = 3,
    TJ_SAMP_440 = 4,
    TJ_SAMP_411 = 5,
};

struct TjScalingFactor {
    int num;
    int denom;
};

#define TJ_SCALED(dim, sf) (((dim) * (sf).num + (sf).denom - 1) / (sf).denom)

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

/* Minimal ABI-compatible mirror of libc++ (_LIBCPP_ABI_NAMESPACE __h) std::vector<T> */
struct VecHeaderRaw {
    const uint8_t *begin;
    const uint8_t *end;
    const uint8_t *cap;
};

struct CodecJpegCompInfoRaw {
    uint32_t componentId;
    uint32_t componentIndex;
    uint32_t hSampFactor;
    uint32_t vSampFactor;
    uint32_t quantTableNo;
    uint32_t dcTableNo;
    uint32_t acTableNo;
    uint8_t infoFlag;
    uint8_t pad[3];
};

struct CodecJpegHuffTableRaw {
    struct VecHeaderRaw bits;
    struct VecHeaderRaw huffVal;
    uint8_t tableFlag;
    uint8_t pad[7];
};

struct CodecJpegQuantTableRaw {
    struct VecHeaderRaw quantVal;
    uint8_t tableFlag;
    uint8_t pad[7];
};

struct CodecImageRegionRaw {
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;
    uint32_t flag;
    uint32_t rsv;
};

struct CodecJpegDecInfoRaw {
    uint32_t imageWidth;
    uint32_t imageHeight;
    uint32_t dataPrecision;
    uint32_t numComponents;
    uint32_t restartInterval;
    uint8_t arithCode;
    uint8_t progressiveMode;
    uint8_t pad0[2];
    struct VecHeaderRaw compInfo;
    struct VecHeaderRaw dcHuffTbl;
    struct VecHeaderRaw acHuffTbl;
    struct VecHeaderRaw quantTbl;
    struct CodecImageRegionRaw region;
    uint32_t sampleSize;
    uint32_t compressPos;
};

/* OHOS drivers_peripheral/codec/image/vdi/codec_jpeg_vdi.h */
struct ICodecJpegHwi {
    int32_t (*JpegInit)(void);
    int32_t (*JpegDeInit)(void);
    int32_t (*AllocateInBuffer)(BufferHandle **buffer, uint32_t size);
    int32_t (*FreeInBuffer)(BufferHandle *buffer);
    int32_t (*DoJpegDecode)(BufferHandle *inBuffer, BufferHandle *outBuffer,
                            const struct CodecJpegDecInfoRaw *decInfo);
    int32_t (*NotifyPowerOn)(void);
};

_Static_assert(sizeof(BufferHandle) == 56, "BufferHandle ABI mismatch");
_Static_assert(sizeof(struct CodecJpegCompInfoRaw) == 32, "CodecJpegCompInfo ABI mismatch");
_Static_assert(sizeof(struct CodecJpegHuffTableRaw) == 56, "CodecJpegHuffTable ABI mismatch");
_Static_assert(sizeof(struct CodecJpegQuantTableRaw) == 32, "CodecJpegQuantTable ABI mismatch");
_Static_assert(sizeof(struct CodecImageRegionRaw) == 24, "CodecImageRegion ABI mismatch");
_Static_assert(offsetof(struct CodecJpegDecInfoRaw, compInfo) == 24, "compInfo offset mismatch");
_Static_assert(offsetof(struct CodecJpegDecInfoRaw, dcHuffTbl) == 48, "dcHuffTbl offset mismatch");
_Static_assert(offsetof(struct CodecJpegDecInfoRaw, acHuffTbl) == 72, "acHuffTbl offset mismatch");
_Static_assert(offsetof(struct CodecJpegDecInfoRaw, quantTbl) == 96, "quantTbl offset mismatch");
_Static_assert(offsetof(struct CodecJpegDecInfoRaw, region) == 120, "region offset mismatch");
_Static_assert(offsetof(struct CodecJpegDecInfoRaw, sampleSize) == 144, "sampleSize offset mismatch");
_Static_assert(offsetof(struct CodecJpegDecInfoRaw, compressPos) == 148, "compressPos offset mismatch");
_Static_assert(sizeof(struct CodecJpegDecInfoRaw) == 152, "CodecJpegDecInfo ABI mismatch");

typedef void *(*TjInitDecompressFn)(void);
typedef int (*TjDestroyFn)(void *handle);
typedef int (*TjDecompressHeader3Fn)(void *handle, const unsigned char *jpegBuf,
                                     unsigned long jpegSize, int *width, int *height,
                                     int *jpegSubsamp, int *jpegColorspace);
typedef struct TjScalingFactor *(*TjGetScalingFactorsFn)(int *numscalingfactors);
typedef int (*TjDecompress2Fn)(void *handle, const unsigned char *jpegBuf,
                               unsigned long jpegSize, unsigned char *dstBuf,
                               int width, int pitch, int height, int pixelFormat,
                               int flags);
typedef int (*TjDecompressToYUVPlanesFn)(void *handle, const unsigned char *jpegBuf,
                                         unsigned long jpegSize,
                                         unsigned char **dstPlanes, int width,
                                         int *strides, int height, int flags);
typedef char *(*TjGetErrorStr2Fn)(void *handle);

struct TurboJpegApi {
    void *dl_handle;
    TjInitDecompressFn init_decompress;
    TjDestroyFn destroy;
    TjDecompressHeader3Fn decompress_header3;
    TjGetScalingFactorsFn get_scaling_factors;
    TjDecompress2Fn decompress2;
    TjDecompressToYUVPlanesFn decompress_to_yuv_planes;
    TjGetErrorStr2Fn get_error_str2;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct TurboJpegApi g_tj = {0};
static int g_init_refs = 0;
static int g_log_budget = 48;
static int g_err_budget = 64;

#define JPEG_VDI_LOG_PATH "/data/local/tmp/marble-codec/jpeg-vdi.log"

static void VdiLog(bool is_err, const char *fmt, ...)
{
    pthread_mutex_lock(&g_lock);
    if (is_err) {
        if (g_err_budget <= 0) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        g_err_budget--;
    } else {
        if (g_log_budget <= 0) {
            pthread_mutex_unlock(&g_lock);
            return;
        }
        g_log_budget--;
    }
    pthread_mutex_unlock(&g_lock);

    FILE *fp = fopen(JPEG_VDI_LOG_PATH, "ae");
    if (!fp) {
        return;
    }
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

#ifndef JPEG_VDI_HOST_TEST
static int LoadTurboJpegLocked(void)
{
    if (g_tj.dl_handle && g_tj.init_decompress && g_tj.destroy &&
        g_tj.decompress_header3 && g_tj.decompress2) {
        return 0;
    }

    static const char *const kPaths[] = {
        "libturbojpeg.z.so",
        "/system/lib64/chipset-sdk/libturbojpeg.z.so",
        "/system/lib64/libturbojpeg.z.so",
    };

    void *h = NULL;
    for (size_t i = 0; i < sizeof(kPaths) / sizeof(kPaths[0]); ++i) {
        h = dlopen(kPaths[i], RTLD_NOW | RTLD_LOCAL);
        if (h) {
            break;
        }
    }
    if (!h) {
        return -1;
    }

    struct TurboJpegApi api = {0};
    api.dl_handle = h;
    api.init_decompress = (TjInitDecompressFn)dlsym(h, "tjInitDecompress");
    api.destroy = (TjDestroyFn)dlsym(h, "tjDestroy");
    api.decompress_header3 = (TjDecompressHeader3Fn)dlsym(h, "tjDecompressHeader3");
    api.get_scaling_factors = (TjGetScalingFactorsFn)dlsym(h, "tjGetScalingFactors");
    api.decompress2 = (TjDecompress2Fn)dlsym(h, "tjDecompress2");
    api.decompress_to_yuv_planes = (TjDecompressToYUVPlanesFn)dlsym(h, "tjDecompressToYUVPlanes");
    api.get_error_str2 = (TjGetErrorStr2Fn)dlsym(h, "tjGetErrorStr2");

    if (!api.init_decompress || !api.destroy || !api.decompress_header3 || !api.decompress2) {
        dlclose(h);
        return -1;
    }

    g_tj = api;
    return 0;
}
#else
static int LoadTurboJpegLocked(void);
#endif

static int EnsureTurboJpegLoaded(struct TurboJpegApi *out)
{
    pthread_mutex_lock(&g_lock);
    int rc = LoadTurboJpegLocked();
    if (rc == 0 && out) {
        *out = g_tj;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

static int32_t VdiJpegInit(void)
{
    pthread_mutex_lock(&g_lock);
    int rc = LoadTurboJpegLocked();
    if (rc == 0) {
        g_init_refs++;
    }
    int refs = g_init_refs;
    pthread_mutex_unlock(&g_lock);

    VdiLog(rc != 0, "JpegInit rc=%d refs=%d", rc, refs);
    return rc == 0 ? 0 : -1;
}

static int32_t VdiJpegDeInit(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_init_refs > 0) {
        g_init_refs--;
    }
    int refs = g_init_refs;
    pthread_mutex_unlock(&g_lock);
    VdiLog(false, "JpegDeInit refs=%d", refs);
    return 0;
}

static int32_t VdiNotifyPowerOn(void)
{
    return 0;
}

static int AllocBackingFd(uint64_t size)
{
    static const char *const kHeaps[] = {
        "/dev/dma_heap/system",
        "/dev/dma_heap/qcom,system",
    };

    for (size_t i = 0; i < sizeof(kHeaps) / sizeof(kHeaps[0]); ++i) {
        int heap = open(kHeaps[i], O_RDONLY | O_CLOEXEC);
        if (heap < 0) {
            continue;
        }
        struct dma_heap_allocation_data req;
        memset(&req, 0, sizeof(req));
        req.len = size;
        req.fd_flags = O_RDWR | O_CLOEXEC;
        int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req);
        close(heap);
        if (rc == 0) {
            return (int)req.fd;
        }
    }

#ifdef __NR_memfd_create
    int mfd = (int)syscall(__NR_memfd_create, "codec_jpeg_in", MFD_CLOEXEC);
    if (mfd >= 0) {
        if (ftruncate(mfd, (off_t)size) == 0) {
            return mfd;
        }
        close(mfd);
    }
#endif
    return -1;
}

static int32_t VdiAllocateInBuffer(BufferHandle **buffer, uint32_t size)
{
    if (!buffer || size == 0 || size > (128u * 1024u * 1024u)) {
        return -1;
    }

    uint64_t aligned = ((uint64_t)size + 4095ULL) & ~4095ULL;
    if (aligned > 0x7fffffffULL) {
        return -1;
    }

    int fd = AllocBackingFd(aligned);
    if (fd < 0) {
        VdiLog(true, "AllocateInBuffer failed size=%u errno=%d", size, errno);
        return -1;
    }

    BufferHandle *h = (BufferHandle *)calloc(1, sizeof(BufferHandle));
    if (!h) {
        close(fd);
        return -1;
    }
    h->fd = fd;
    h->size = (int32_t)aligned;
    h->virAddr = NULL;
    h->phyAddr = 0;

    *buffer = h;
    VdiLog(false, "AllocateInBuffer size=%u aligned=%llu fd=%d", size,
           (unsigned long long)aligned, fd);
    return 0;
}

static int32_t VdiFreeInBuffer(BufferHandle *buffer)
{
    if (!buffer) {
        return -1;
    }
    if (buffer->fd >= 0) {
        close(buffer->fd);
        buffer->fd = -1;
    }
    free(buffer);
    return 0;
}

static void SyncDmaBuf(int fd, bool start, bool write)
{
    if (fd < 0) {
        return;
    }
    struct dma_buf_sync sync;
    sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                 (write ? DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

static int MapBufferHandle(const BufferHandle *bh, int prot, uint8_t **out_ptr,
                           bool *out_mapped, size_t *out_len)
{
    if (!bh || bh->size <= 0 || !out_ptr || !out_mapped || !out_len) {
        return -1;
    }

    size_t len = (size_t)bh->size;
    if (bh->fd >= 0) {
        SyncDmaBuf(bh->fd, true, (prot & PROT_WRITE) != 0);
        void *p = mmap(NULL, len, prot, MAP_SHARED, bh->fd, 0);
        if (p == MAP_FAILED) {
            SyncDmaBuf(bh->fd, false, (prot & PROT_WRITE) != 0);
            return -1;
        }
        *out_ptr = (uint8_t *)p;
        *out_mapped = true;
        *out_len = len;
        return 0;
    }

    if (bh->virAddr != NULL) {
        *out_ptr = (uint8_t *)bh->virAddr;
        *out_mapped = false;
        *out_len = len;
        return 0;
    }

    return -1;
}

static void UnmapBufferHandle(const BufferHandle *bh, uint8_t *ptr, size_t len,
                              bool mapped, int prot)
{
    if (mapped && ptr) {
        munmap(ptr, len);
    }
    if (bh && bh->fd >= 0) {
        SyncDmaBuf(bh->fd, false, (prot & PROT_WRITE) != 0);
    }
}

/* ITU-T.81 zigzag -> natural 8x8 index mapping (matches libjpeg jpeg_natural_order[0..63]) */
static const uint8_t kJpegNaturalOrder[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63,
};

/* ITU-T.81 Annex K standard Huffman tables for fallback if a tableFlag is unset */
static const uint8_t kStdDcBits[2][16] = {
    {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0},
};
static const uint8_t kStdDcVal[2][12] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
};
static const uint8_t kStdAcBits[2][16] = {
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125},
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119},
};
static const uint8_t kStdAcVal[2][162] = {
    {
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
        0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
        0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
        0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
        0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
        0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
        0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
        0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
        0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
        0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
        0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
        0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
        0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
        0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
    },
    {
        0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
        0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
        0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
        0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
        0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
        0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
        0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
        0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
        0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
        0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
        0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
        0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
        0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
        0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
    },
};

struct ByteWriter {
    uint8_t *buf;
    size_t pos;
    size_t cap;
    bool overflow;
};

static void EmitByte(struct ByteWriter *w, uint8_t b)
{
    if (w->pos < w->cap) {
        w->buf[w->pos] = b;
    } else {
        w->overflow = true;
    }
    w->pos++;
}

static void EmitBe16(struct ByteWriter *w, uint16_t v)
{
    EmitByte(w, (uint8_t)(v >> 8));
    EmitByte(w, (uint8_t)(v & 0xFF));
}

static void EmitBytes(struct ByteWriter *w, const uint8_t *src, size_t n)
{
    if (n > 0) {
        if (w->pos <= w->cap && n <= w->cap - w->pos) {
            memcpy(w->buf + w->pos, src, n);
        } else {
            w->overflow = true;
        }
        w->pos += n;
    }
}

static size_t VecCount(const struct VecHeaderRaw *v, size_t elem_size)
{
    if (!v || !v->begin || !v->end || v->end < v->begin || elem_size == 0) {
        return 0;
    }
    uintptr_t bytes = (uintptr_t)(v->end - v->begin);
    return (size_t)(bytes / elem_size);
}

static void EmitDhtTable(struct ByteWriter *w, uint8_t tc, uint8_t th,
                         const uint8_t bits[16], const uint8_t *huff_val,
                         size_t val_len)
{
    uint16_t seg_len = (uint16_t)(2 + 1 + 16 + val_len);
    EmitByte(w, 0xFF);
    EmitByte(w, 0xC4);
    EmitBe16(w, seg_len);
    EmitByte(w, (uint8_t)(((tc & 1u) << 4) | (th & 3u)));
    EmitBytes(w, bits, 16);
    EmitBytes(w, huff_val, val_len);
}

static int BuildReconstructedJpeg(const struct CodecJpegDecInfoRaw *dec,
                                  const uint8_t *scan_data, size_t scan_len,
                                  uint8_t **out_buf, size_t *out_len)
{
    if (!dec || !scan_data || scan_len == 0) {
        return -1;
    }
    if (dec->progressiveMode || dec->arithCode) {
        return -1;
    }
    if (dec->imageWidth == 0 || dec->imageHeight == 0 ||
        dec->imageWidth > 65535u || dec->imageHeight > 65535u) {
        return -1;
    }

    size_t n_comp = VecCount(&dec->compInfo, sizeof(struct CodecJpegCompInfoRaw));
    if (dec->numComponents == 0 || dec->numComponents > 4 || n_comp < dec->numComponents) {
        return -1;
    }
    const struct CodecJpegCompInfoRaw *comps =
        (const struct CodecJpegCompInfoRaw *)dec->compInfo.begin;

    size_t n_qt = VecCount(&dec->quantTbl, sizeof(struct CodecJpegQuantTableRaw));
    const struct CodecJpegQuantTableRaw *qts =
        (const struct CodecJpegQuantTableRaw *)dec->quantTbl.begin;

    size_t n_dc = VecCount(&dec->dcHuffTbl, sizeof(struct CodecJpegHuffTableRaw));
    const struct CodecJpegHuffTableRaw *dcs =
        (const struct CodecJpegHuffTableRaw *)dec->dcHuffTbl.begin;

    size_t n_ac = VecCount(&dec->acHuffTbl, sizeof(struct CodecJpegHuffTableRaw));
    const struct CodecJpegHuffTableRaw *acs =
        (const struct CodecJpegHuffTableRaw *)dec->acHuffTbl.begin;

    /* Max possible header size: SOI(2) + 4*DQT(536) + SOF(20) + DRI(6) + 8*DHT(2400) + SOS(18) < 4096 */
    size_t alloc_cap = 4096u + scan_len + 2u;
    uint8_t *buf = (uint8_t *)malloc(alloc_cap);
    if (!buf) {
        return -1;
    }

    struct ByteWriter w = {buf, 0, alloc_cap, false};

    /* SOI */
    EmitByte(&w, 0xFF);
    EmitByte(&w, 0xD8);

    /* DQT */
    for (size_t q = 0; q < n_qt && q < 4; ++q) {
        if (!qts[q].tableFlag) {
            continue;
        }
        size_t n_val = VecCount(&qts[q].quantVal, sizeof(uint16_t));
        if (n_val < 64) {
            continue;
        }
        const uint16_t *qv = (const uint16_t *)qts[q].quantVal.begin;
        bool is_16bit = false;
        for (int i = 0; i < 64; ++i) {
            if (qv[i] > 255) {
                is_16bit = true;
                break;
            }
        }
        EmitByte(&w, 0xFF);
        EmitByte(&w, 0xDB);
        EmitBe16(&w, (uint16_t)(2 + 1 + (is_16bit ? 128 : 64)));
        EmitByte(&w, (uint8_t)(((is_16bit ? 1u : 0u) << 4) | (q & 3u)));
        for (int z = 0; z < 64; ++z) {
            uint16_t v = qv[kJpegNaturalOrder[z]];
            if (is_16bit) {
                EmitBe16(&w, v);
            } else {
                EmitByte(&w, (uint8_t)v);
            }
        }
    }

    /* SOF0 / SOF1 */
    uint8_t prec = (dec->dataPrecision == 12) ? 12 : 8;
    EmitByte(&w, 0xFF);
    EmitByte(&w, prec > 8 ? 0xC1 : 0xC0);
    EmitBe16(&w, (uint16_t)(8 + 3 * dec->numComponents));
    EmitByte(&w, prec);
    EmitBe16(&w, (uint16_t)dec->imageHeight);
    EmitBe16(&w, (uint16_t)dec->imageWidth);
    EmitByte(&w, (uint8_t)dec->numComponents);
    for (uint32_t c = 0; c < dec->numComponents; ++c) {
        uint8_t hs = (uint8_t)(comps[c].hSampFactor & 0x0Fu);
        uint8_t vs = (uint8_t)(comps[c].vSampFactor & 0x0Fu);
        if (hs == 0) {
            hs = 1;
        }
        if (vs == 0) {
            vs = 1;
        }
        EmitByte(&w, (uint8_t)(comps[c].componentId & 0xFFu));
        EmitByte(&w, (uint8_t)((hs << 4) | vs));
        EmitByte(&w, (uint8_t)(comps[c].quantTableNo & 0x03u));
    }

    /* DRI */
    if (dec->restartInterval > 0 && dec->restartInterval <= 65535u) {
        EmitByte(&w, 0xFF);
        EmitByte(&w, 0xDD);
        EmitBe16(&w, 4);
        EmitBe16(&w, (uint16_t)dec->restartInterval);
    }

    /* DHT: emit present tables plus standard Annex K fallback if referenced by a component */
    bool dc_emitted[4] = {false, false, false, false};
    bool ac_emitted[4] = {false, false, false, false};

    for (size_t t = 0; t < n_dc && t < 4; ++t) {
        if (!dcs[t].tableFlag) {
            continue;
        }
        size_t nb = VecCount(&dcs[t].bits, sizeof(uint8_t));
        size_t nv = VecCount(&dcs[t].huffVal, sizeof(uint8_t));
        if (nb < 16) {
            continue;
        }
        const uint8_t *bits = dcs[t].bits.begin;
        size_t sum = 0;
        for (int k = 0; k < 16; ++k) {
            sum += bits[k];
        }
        if (sum > nv || sum > 256) {
            continue;
        }
        EmitDhtTable(&w, 0, (uint8_t)t, bits, dcs[t].huffVal.begin, sum);
        dc_emitted[t] = true;
    }

    for (size_t t = 0; t < n_ac && t < 4; ++t) {
        if (!acs[t].tableFlag) {
            continue;
        }
        size_t nb = VecCount(&acs[t].bits, sizeof(uint8_t));
        size_t nv = VecCount(&acs[t].huffVal, sizeof(uint8_t));
        if (nb < 16) {
            continue;
        }
        const uint8_t *bits = acs[t].bits.begin;
        size_t sum = 0;
        for (int k = 0; k < 16; ++k) {
            sum += bits[k];
        }
        if (sum > nv || sum > 256) {
            continue;
        }
        EmitDhtTable(&w, 1, (uint8_t)t, bits, acs[t].huffVal.begin, sum);
        ac_emitted[t] = true;
    }

    for (uint32_t c = 0; c < dec->numComponents; ++c) {
        uint8_t dt = (uint8_t)(comps[c].dcTableNo & 3u);
        uint8_t at = (uint8_t)(comps[c].acTableNo & 3u);
        if (!dc_emitted[dt]) {
            uint8_t idx = dt > 1 ? 1 : dt;
            EmitDhtTable(&w, 0, dt, kStdDcBits[idx], kStdDcVal[idx], 12);
            dc_emitted[dt] = true;
        }
        if (!ac_emitted[at]) {
            uint8_t idx = at > 1 ? 1 : at;
            EmitDhtTable(&w, 1, at, kStdAcBits[idx], kStdAcVal[idx], 162);
            ac_emitted[at] = true;
        }
    }

    /* SOS */
    EmitByte(&w, 0xFF);
    EmitByte(&w, 0xDA);
    EmitBe16(&w, (uint16_t)(6 + 2 * dec->numComponents));
    EmitByte(&w, (uint8_t)dec->numComponents);
    for (uint32_t c = 0; c < dec->numComponents; ++c) {
        uint8_t dt = (uint8_t)(comps[c].dcTableNo & 3u);
        uint8_t at = (uint8_t)(comps[c].acTableNo & 3u);
        EmitByte(&w, (uint8_t)(comps[c].componentId & 0xFFu));
        EmitByte(&w, (uint8_t)((dt << 4) | at));
    }
    EmitByte(&w, 0x00); /* Ss */
    EmitByte(&w, 0x3F); /* Se */
    EmitByte(&w, 0x00); /* AhAl */

    /* Entropy-coded scan payload + safety EOI */
    EmitBytes(&w, scan_data, scan_len);
    EmitByte(&w, 0xFF);
    EmitByte(&w, 0xD9);

    if (w.overflow) {
        free(buf);
        return -1;
    }

    *out_buf = buf;
    *out_len = w.pos;
    return 0;
}

static struct TjScalingFactor PickScalingFactor(const struct TurboJpegApi *tj,
                                                uint32_t sample_size)
{
    struct TjScalingFactor best = {1, 1};
    if (sample_size <= 1) {
        return best;
    }

    int num_sf = 0;
    struct TjScalingFactor *sfs = tj->get_scaling_factors ? tj->get_scaling_factors(&num_sf) : NULL;
    if (!sfs || num_sf <= 0) {
        if (sample_size >= 8) {
            best.num = 1;
            best.denom = 8;
        } else if (sample_size >= 4) {
            best.num = 1;
            best.denom = 4;
        } else if (sample_size >= 2) {
            best.num = 1;
            best.denom = 2;
        }
        return best;
    }

    /* Pick largest factor <= 1 / sample_size (e.g., 1/2, 1/4, 1/8) */
    double target = 1.0 / (double)sample_size;
    double best_ratio = 2.0;
    for (int i = 0; i < num_sf; ++i) {
        if (sfs[i].num <= 0 || sfs[i].denom <= 0) {
            continue;
        }
        double r = (double)sfs[i].num / (double)sfs[i].denom;
        if (r + 1e-6 >= target && r < best_ratio) {
            best = sfs[i];
            best_ratio = r;
        }
    }
    if (best_ratio > 1.5) {
        best.num = 1;
        best.denom = 1;
    }
    return best;
}

static int MapOhosFmtToTjPf(int32_t ohos_fmt, int *out_tjpf, int *out_bpp)
{
    switch (ohos_fmt) {
    case OHOS_PIXEL_FMT_RGBA_8888:
        *out_tjpf = TJ_PF_RGBA;
        *out_bpp = 4;
        return 0;
    case OHOS_PIXEL_FMT_RGBX_8888:
        *out_tjpf = TJ_PF_RGBX;
        *out_bpp = 4;
        return 0;
    case OHOS_PIXEL_FMT_BGRA_8888:
        *out_tjpf = TJ_PF_BGRA;
        *out_bpp = 4;
        return 0;
    case OHOS_PIXEL_FMT_BGRX_8888:
        *out_tjpf = TJ_PF_BGRX;
        *out_bpp = 4;
        return 0;
    case OHOS_PIXEL_FMT_RGB_888:
        *out_tjpf = TJ_PF_RGB;
        *out_bpp = 3;
        return 0;
    default:
        return -1;
    }
}

static inline uint8_t ClampU8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void InterleaveUVRow(uint8_t *dst, const uint8_t *u, const uint8_t *v,
                            int count, bool v_first)
{
    int x = 0;
#if defined(__aarch64__)
    for (; x + 16 <= count; x += 16) {
        uint8x16_t vu = vld1q_u8(u + x);
        uint8x16_t vv = vld1q_u8(v + x);
        uint8x16x2_t pair;
        pair.val[0] = v_first ? vv : vu;
        pair.val[1] = v_first ? vu : vv;
        vst2q_u8(dst + (size_t)x * 2, pair);
    }
#endif
    for (; x < count; ++x) {
        if (v_first) {
            dst[2 * x + 0] = v[x];
            dst[2 * x + 1] = u[x];
        } else {
            dst[2 * x + 0] = u[x];
            dst[2 * x + 1] = v[x];
        }
    }
}

static int DecodeRgbOut(const struct TurboJpegApi *tj, void *tj_handle,
                        const uint8_t *jpg, size_t jpg_len,
                        int scaled_w, int scaled_h, int tj_pf, int bpp,
                        const struct CodecJpegDecInfoRaw *dec,
                        struct TjScalingFactor sf,
                        BufferHandle *out_buf, uint8_t *out_ptr)
{
    if (out_buf->width <= 0 || out_buf->height <= 0 || out_buf->stride <= 0) {
        return -1;
    }
    uint32_t out_w = (uint32_t)out_buf->width;
    uint32_t out_h = (uint32_t)out_buf->height;
    size_t out_stride = (size_t)out_buf->stride;
    if (out_stride < (size_t)out_w * (size_t)bpp ||
        (uint64_t)out_stride * (uint64_t)out_h > (uint64_t)out_buf->size) {
        return -1;
    }

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    if (dec->region.flag != 0) {
        crop_x = (uint32_t)(((uint64_t)dec->region.left * (uint64_t)sf.num) / (uint64_t)sf.denom);
        crop_y = (uint32_t)(((uint64_t)dec->region.top * (uint64_t)sf.num) / (uint64_t)sf.denom);
    }

    if (dec->region.flag == 0 && (uint32_t)scaled_w <= out_w && (uint32_t)scaled_h <= out_h) {
        int rc = tj->decompress2(tj_handle, jpg, (unsigned long)jpg_len, out_ptr,
                                 scaled_w, (int)out_stride, scaled_h, tj_pf, 0);
        return rc == 0 ? 0 : -1;
    }

    size_t tmp_pitch = (size_t)scaled_w * (size_t)bpp;
    if (tmp_pitch == 0 || (uint64_t)tmp_pitch * (uint64_t)scaled_h > (256ULL * 1024ULL * 1024ULL)) {
        return -1;
    }
    uint8_t *tmp = (uint8_t *)malloc(tmp_pitch * (size_t)scaled_h);
    if (!tmp) {
        return -1;
    }
    int rc = tj->decompress2(tj_handle, jpg, (unsigned long)jpg_len, tmp,
                             scaled_w, (int)tmp_pitch, scaled_h, tj_pf, 0);
    if (rc != 0) {
        free(tmp);
        return -1;
    }

    if (crop_x >= (uint32_t)scaled_w || crop_y >= (uint32_t)scaled_h) {
        free(tmp);
        return -1;
    }
    uint32_t avail_w = (uint32_t)scaled_w - crop_x;
    uint32_t avail_h = (uint32_t)scaled_h - crop_y;
    uint32_t copy_w = avail_w < out_w ? avail_w : out_w;
    uint32_t copy_h = avail_h < out_h ? avail_h : out_h;
    size_t row_bytes = (size_t)copy_w * (size_t)bpp;

    for (uint32_t y = 0; y < copy_h; ++y) {
        memcpy(out_ptr + (size_t)y * out_stride,
               tmp + (size_t)(crop_y + y) * tmp_pitch + (size_t)crop_x * (size_t)bpp,
               row_bytes);
    }

    free(tmp);
    return 0;
}

static void RgbaToYuv420(const uint8_t *rgba, size_t rgba_pitch,
                         int src_w, int src_h, int32_t out_fmt,
                         uint8_t *out_ptr, uint32_t out_w, uint32_t out_h,
                         size_t out_stride)
{
    uint32_t copy_w = (uint32_t)src_w < out_w ? (uint32_t)src_w : out_w;
    uint32_t copy_h = (uint32_t)src_h < out_h ? (uint32_t)src_h : out_h;
    size_t luma_size = out_stride * (size_t)out_h;

    for (uint32_t y = 0; y < copy_h; ++y) {
        const uint8_t *s = rgba + (size_t)y * rgba_pitch;
        uint8_t *dy = out_ptr + (size_t)y * out_stride;
        for (uint32_t x = 0; x < copy_w; ++x) {
            int r = s[4 * x + 0];
            int g = s[4 * x + 1];
            int b = s[4 * x + 2];
            dy[x] = ClampU8((77 * r + 150 * g + 29 * b + 128) >> 8);
        }
    }

    bool semi = (out_fmt == OHOS_PIXEL_FMT_YCBCR_420_SP ||
                 out_fmt == OHOS_PIXEL_FMT_YCRCB_420_SP);
    bool v_first = (out_fmt == OHOS_PIXEL_FMT_YCRCB_420_SP ||
                    out_fmt == OHOS_PIXEL_FMT_YCRCB_420_P);
    size_t uv_stride = out_stride / 2;
    size_t plane2_size = uv_stride * (((size_t)out_h + 1) / 2);

    uint32_t uv_w = (copy_w + 1) / 2;
    uint32_t uv_h = (copy_h + 1) / 2;
    for (uint32_t uy = 0; uy < uv_h; ++uy) {
        uint32_t sy0 = uy * 2;
        uint32_t sy1 = (sy0 + 1 < copy_h) ? (sy0 + 1) : sy0;
        const uint8_t *row0 = rgba + (size_t)sy0 * rgba_pitch;
        const uint8_t *row1 = rgba + (size_t)sy1 * rgba_pitch;

        for (uint32_t ux = 0; ux < uv_w; ++ux) {
            uint32_t sx0 = ux * 2;
            uint32_t sx1 = (sx0 + 1 < copy_w) ? (sx0 + 1) : sx0;
            int r = (row0[4 * sx0 + 0] + row0[4 * sx1 + 0] +
                     row1[4 * sx0 + 0] + row1[4 * sx1 + 0] + 2) >> 2;
            int g = (row0[4 * sx0 + 1] + row0[4 * sx1 + 1] +
                     row1[4 * sx0 + 1] + row1[4 * sx1 + 1] + 2) >> 2;
            int b = (row0[4 * sx0 + 2] + row0[4 * sx1 + 2] +
                     row1[4 * sx0 + 2] + row1[4 * sx1 + 2] + 2) >> 2;
            uint8_t cb = ClampU8(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
            uint8_t cr = ClampU8(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);

            if (semi) {
                uint8_t *duv = out_ptr + luma_size + (size_t)uy * out_stride + (size_t)ux * 2;
                duv[0] = v_first ? cr : cb;
                duv[1] = v_first ? cb : cr;
            } else {
                uint8_t *p1 = out_ptr + luma_size + (size_t)uy * uv_stride;
                uint8_t *p2 = out_ptr + luma_size + plane2_size + (size_t)uy * uv_stride;
                p1[ux] = v_first ? cr : cb;
                p2[ux] = v_first ? cb : cr;
            }
        }
    }
}

static int DecodeYuv420Out(const struct TurboJpegApi *tj, void *tj_handle,
                           const uint8_t *jpg, size_t jpg_len,
                           int scaled_w, int scaled_h, int subsamp,
                           const struct CodecJpegDecInfoRaw *dec,
                           struct TjScalingFactor sf,
                           BufferHandle *out_buf, uint8_t *out_ptr)
{
    if (out_buf->width <= 0 || out_buf->height <= 0 || out_buf->stride <= 0) {
        return -1;
    }
    uint32_t out_w = (uint32_t)out_buf->width;
    uint32_t out_h = (uint32_t)out_buf->height;
    size_t out_stride = (size_t)out_buf->stride;
    size_t uv_h_out = ((size_t)out_h + 1) / 2;
    size_t luma_out = out_stride * (size_t)out_h;
    size_t need_size = luma_out + out_stride * uv_h_out;
    if (out_stride < (size_t)out_w || (uint64_t)need_size > (uint64_t)out_buf->size) {
        return -1;
    }

    if (subsamp == TJ_SAMP_420 && dec->region.flag == 0 && tj->decompress_to_yuv_planes) {
        int y_stride_tmp = (scaled_w + 15) & ~15;
        int y_h_tmp = (scaled_h + 15) & ~15;
        int uv_stride_tmp = y_stride_tmp / 2;
        int uv_h_tmp = y_h_tmp / 2;
        size_t y_bytes = (size_t)y_stride_tmp * (size_t)y_h_tmp;
        size_t uv_bytes = (size_t)uv_stride_tmp * (size_t)uv_h_tmp;
        uint8_t *tmp = (uint8_t *)malloc(y_bytes + 2 * uv_bytes);
        if (!tmp) {
            return -1;
        }
        uint8_t *planes[3] = {tmp, tmp + y_bytes, tmp + y_bytes + uv_bytes};
        int strides[3] = {y_stride_tmp, uv_stride_tmp, uv_stride_tmp};

        int rc = tj->decompress_to_yuv_planes(tj_handle, jpg, (unsigned long)jpg_len,
                                              planes, scaled_w, strides, scaled_h, 0);
        if (rc == 0) {
            uint32_t copy_w = (uint32_t)scaled_w < out_w ? (uint32_t)scaled_w : out_w;
            uint32_t copy_h = (uint32_t)scaled_h < out_h ? (uint32_t)scaled_h : out_h;
            for (uint32_t y = 0; y < copy_h; ++y) {
                memcpy(out_ptr + (size_t)y * out_stride,
                       planes[0] + (size_t)y * (size_t)y_stride_tmp, copy_w);
            }
            int copy_uv_w = (int)((copy_w + 1) / 2);
            int copy_uv_h = (int)((copy_h + 1) / 2);
            bool semi = (out_buf->format == OHOS_PIXEL_FMT_YCBCR_420_SP ||
                         out_buf->format == OHOS_PIXEL_FMT_YCRCB_420_SP);
            bool v_first = (out_buf->format == OHOS_PIXEL_FMT_YCRCB_420_SP ||
                            out_buf->format == OHOS_PIXEL_FMT_YCRCB_420_P);
            if (semi) {
                for (int uy = 0; uy < copy_uv_h; ++uy) {
                    InterleaveUVRow(out_ptr + luma_out + (size_t)uy * out_stride,
                                    planes[1] + (size_t)uy * (size_t)uv_stride_tmp,
                                    planes[2] + (size_t)uy * (size_t)uv_stride_tmp,
                                    copy_uv_w, v_first);
                }
            } else {
                size_t uv_stride_out = out_stride / 2;
                size_t plane2_off = uv_stride_out * uv_h_out;
                uint8_t *src1 = v_first ? planes[2] : planes[1];
                uint8_t *src2 = v_first ? planes[1] : planes[2];
                for (int uy = 0; uy < copy_uv_h; ++uy) {
                    memcpy(out_ptr + luma_out + (size_t)uy * uv_stride_out,
                           src1 + (size_t)uy * (size_t)uv_stride_tmp, (size_t)copy_uv_w);
                    memcpy(out_ptr + luma_out + plane2_off + (size_t)uy * uv_stride_out,
                           src2 + (size_t)uy * (size_t)uv_stride_tmp, (size_t)copy_uv_w);
                }
            }
            free(tmp);
            return 0;
        }
        free(tmp);
    }

    /* Fallback for 4:4:4 / 4:2:2 / grayscale / cropped YUV decode via RGBX */
    size_t tmp_pitch = (size_t)scaled_w * 4u;
    if (tmp_pitch == 0 || (uint64_t)tmp_pitch * (uint64_t)scaled_h > (256ULL * 1024ULL * 1024ULL)) {
        return -1;
    }
    uint8_t *rgba = (uint8_t *)malloc(tmp_pitch * (size_t)scaled_h);
    if (!rgba) {
        return -1;
    }
    int rc = tj->decompress2(tj_handle, jpg, (unsigned long)jpg_len, rgba,
                             scaled_w, (int)tmp_pitch, scaled_h, TJ_PF_RGBX, 0);
    if (rc != 0) {
        free(rgba);
        return -1;
    }

    uint32_t crop_x = 0;
    uint32_t crop_y = 0;
    if (dec->region.flag != 0) {
        crop_x = ((uint32_t)(((uint64_t)dec->region.left * (uint64_t)sf.num) / (uint64_t)sf.denom)) & ~1u;
        crop_y = ((uint32_t)(((uint64_t)dec->region.top * (uint64_t)sf.num) / (uint64_t)sf.denom)) & ~1u;
    }
    if (crop_x >= (uint32_t)scaled_w || crop_y >= (uint32_t)scaled_h) {
        free(rgba);
        return -1;
    }

    const uint8_t *crop_ptr = rgba + (size_t)crop_y * tmp_pitch + (size_t)crop_x * 4u;
    RgbaToYuv420(crop_ptr, tmp_pitch, scaled_w - (int)crop_x, scaled_h - (int)crop_y,
                 out_buf->format, out_ptr, out_w, out_h, out_stride);
    free(rgba);
    return 0;
}

static int32_t VdiDoJpegDecode(BufferHandle *inBuffer, BufferHandle *outBuffer,
                               const struct CodecJpegDecInfoRaw *decInfo)
{
    if (!inBuffer || !outBuffer || !decInfo) {
        VdiLog(true, "DoJpegDecode null arg");
        return -1;
    }

    struct TurboJpegApi tj = {0};
    if (EnsureTurboJpegLoaded(&tj) != 0) {
        VdiLog(true, "DoJpegDecode libturbojpeg load failed");
        return -1;
    }

    uint8_t *in_base = NULL;
    uint8_t *out_base = NULL;
    bool in_mapped = false;
    bool out_mapped = false;
    size_t in_map_len = 0;
    size_t out_map_len = 0;
    uint8_t *rebuilt = NULL;
    void *tj_handle = NULL;
    int32_t ret = -1;

    if (MapBufferHandle(inBuffer, PROT_READ, &in_base, &in_mapped, &in_map_len) != 0) {
        VdiLog(true, "DoJpegDecode map in failed fd=%d size=%d", inBuffer->fd, inBuffer->size);
        goto cleanup;
    }
    if (MapBufferHandle(outBuffer, PROT_READ | PROT_WRITE, &out_base, &out_mapped, &out_map_len) != 0) {
        VdiLog(true, "DoJpegDecode map out failed fd=%d size=%d", outBuffer->fd, outBuffer->size);
        goto cleanup;
    }

    if (decInfo->compressPos >= in_map_len) {
        VdiLog(true, "DoJpegDecode invalid compressPos=%u size=%zu",
               decInfo->compressPos, in_map_len);
        goto cleanup;
    }

    const uint8_t *scan_ptr = in_base + decInfo->compressPos;
    size_t scan_len = in_map_len - decInfo->compressPos;
    const uint8_t *jpg_ptr = NULL;
    size_t jpg_len = 0;

    if (scan_len >= 2 && scan_ptr[0] == 0xFF && scan_ptr[1] == 0xD8) {
        jpg_ptr = scan_ptr;
        jpg_len = scan_len;
    } else if (decInfo->compressPos > 0 && in_map_len >= 2 &&
               in_base[0] == 0xFF && in_base[1] == 0xD8) {
        jpg_ptr = in_base;
        jpg_len = in_map_len;
    } else {
        if (BuildReconstructedJpeg(decInfo, scan_ptr, scan_len, &rebuilt, &jpg_len) != 0) {
            VdiLog(true, "DoJpegDecode reconstruct failed w=%u h=%u comp=%u prog=%u",
                   decInfo->imageWidth, decInfo->imageHeight,
                   decInfo->numComponents, decInfo->progressiveMode);
            goto cleanup;
        }
        jpg_ptr = rebuilt;
    }

    tj_handle = tj.init_decompress();
    if (!tj_handle) {
        VdiLog(true, "DoJpegDecode tjInitDecompress failed");
        goto cleanup;
    }

    int img_w = 0, img_h = 0, subsamp = 0, colorspace = 0;
    if (tj.decompress_header3(tj_handle, jpg_ptr, (unsigned long)jpg_len,
                              &img_w, &img_h, &subsamp, &colorspace) != 0) {
        const char *err = tj.get_error_str2 ? tj.get_error_str2(tj_handle) : "hdr_err";
        VdiLog(true, "DoJpegDecode hdr3 failed (%s) rebuilt=%d len=%zu",
               err ? err : "unknown", rebuilt != NULL, jpg_len);
        goto cleanup;
    }

    struct TjScalingFactor sf = PickScalingFactor(&tj, decInfo->sampleSize);
    int scaled_w = TJ_SCALED(img_w, sf);
    int scaled_h = TJ_SCALED(img_h, sf);
    if (scaled_w <= 0 || scaled_h <= 0) {
        goto cleanup;
    }

    int tj_pf = 0, bpp = 0;
    if (MapOhosFmtToTjPf(outBuffer->format, &tj_pf, &bpp) == 0) {
        ret = DecodeRgbOut(&tj, tj_handle, jpg_ptr, jpg_len,
                           scaled_w, scaled_h, tj_pf, bpp,
                           decInfo, sf, outBuffer, out_base);
    } else if (outBuffer->format == OHOS_PIXEL_FMT_YCBCR_420_SP ||
               outBuffer->format == OHOS_PIXEL_FMT_YCRCB_420_SP ||
               outBuffer->format == OHOS_PIXEL_FMT_YCBCR_420_P ||
               outBuffer->format == OHOS_PIXEL_FMT_YCRCB_420_P) {
        ret = DecodeYuv420Out(&tj, tj_handle, jpg_ptr, jpg_len,
                              scaled_w, scaled_h, subsamp,
                              decInfo, sf, outBuffer, out_base);
    } else {
        VdiLog(true, "DoJpegDecode unsupported out fmt=%d", outBuffer->format);
        goto cleanup;
    }

    VdiLog(ret != 0,
           "DoJpegDecode ret=%d img=%dx%d out=%dx%d stride=%d fmt=%d sample=%u sf=%d/%d rebuilt=%d",
           ret, img_w, img_h, outBuffer->width, outBuffer->height, outBuffer->stride,
           outBuffer->format, decInfo->sampleSize, sf.num, sf.denom, rebuilt != NULL);

cleanup:
    if (tj_handle) {
        tj.destroy(tj_handle);
    }
    free(rebuilt);
    if (out_base) {
        UnmapBufferHandle(outBuffer, out_base, out_map_len, out_mapped, PROT_READ | PROT_WRITE);
    }
    if (in_base) {
        UnmapBufferHandle(inBuffer, in_base, in_map_len, in_mapped, PROT_READ);
    }
    return ret;
}

static struct ICodecJpegHwi g_jpeg_hwi = {
    .JpegInit = VdiJpegInit,
    .JpegDeInit = VdiJpegDeInit,
    .AllocateInBuffer = VdiAllocateInBuffer,
    .FreeInBuffer = VdiFreeInBuffer,
    .DoJpegDecode = VdiDoJpegDecode,
    .NotifyPowerOn = VdiNotifyPowerOn,
};

#ifndef JPEG_VDI_HOST_TEST
__attribute__((visibility("default")))
#endif
struct ICodecJpegHwi *GetCodecJpegHwi(void)
{
    return &g_jpeg_hwi;
}

#ifdef JPEG_VDI_HOST_TEST
#include <assert.h>

/* Synthetic minimal 2x2 grayscale mock JPEG decoder for host ABI + reconstruction verification */
static unsigned char g_last_hdr_bytes[8192];
static unsigned long g_last_hdr_len = 0;

static void *MockInit(void)
{
    static int dummy = 1;
    return &dummy;
}
static int MockDestroy(void *h)
{
    (void)h;
    return 0;
}
static struct TjScalingFactor *MockScalingFactors(int *n)
{
    static struct TjScalingFactor sfs[] = {{2, 1}, {1, 1}, {1, 2}, {1, 4}, {1, 8}};
    *n = 5;
    return sfs;
}
static int MockHdr3(void *h, const unsigned char *buf, unsigned long len,
                    int *w, int *height, int *subsamp, int *cs)
{
    (void)h;
    if (!buf || len < 4 || buf[0] != 0xFF || buf[1] != 0xD8) {
        return -1;
    }
    /* Walk JPEG markers up to SOS to verify marker integrity */
    size_t pos = 2;
    bool seen_dqt = false, seen_sof = false, seen_dht = false, seen_sos = false;
    while (pos + 1 < len) {
        if (buf[pos] != 0xFF) {
            return -1;
        }
        uint8_t mk = buf[pos + 1];
        pos += 2;
        if (mk == 0xDB) {
            seen_dqt = true;
        } else if (mk == 0xC0) {
            seen_sof = true;
            if (pos + 7 > len) {
                return -1;
            }
            *height = (buf[pos + 3] << 8) | buf[pos + 4];
            *w = (buf[pos + 5] << 8) | buf[pos + 6];
        } else if (mk == 0xC4) {
            seen_dht = true;
        } else if (mk == 0xDA) {
            seen_sos = true;
            break;
        }
        if (pos + 2 > len) {
            return -1;
        }
        uint16_t seglen = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
        if (seglen < 2 || pos + seglen > len) {
            return -1;
        }
        pos += seglen;
    }
    if (!seen_dqt || !seen_sof || !seen_dht || !seen_sos) {
        return -1;
    }
    g_last_hdr_len = len < sizeof(g_last_hdr_bytes) ? len : sizeof(g_last_hdr_bytes);
    memcpy(g_last_hdr_bytes, buf, g_last_hdr_len);
    *subsamp = TJ_SAMP_420;
    *cs = 0;
    return 0;
}
static int MockDecompress2(void *h, const unsigned char *buf, unsigned long len,
                           unsigned char *dst, int w, int pitch, int height,
                           int pf, int flags)
{
    (void)h;
    (void)buf;
    (void)len;
    (void)flags;
    for (int y = 0; y < height; ++y) {
        uint8_t *row = dst + (size_t)y * (size_t)pitch;
        for (int x = 0; x < w; ++x) {
            if (pf == TJ_PF_RGBA || pf == TJ_PF_RGBX) {
                row[4 * x + 0] = (uint8_t)(10 + x);
                row[4 * x + 1] = (uint8_t)(20 + y);
                row[4 * x + 2] = 0x33;
                row[4 * x + 3] = 0xFF;
            } else if (pf == TJ_PF_BGRA || pf == TJ_PF_BGRX) {
                row[4 * x + 0] = 0x33;
                row[4 * x + 1] = (uint8_t)(20 + y);
                row[4 * x + 2] = (uint8_t)(10 + x);
                row[4 * x + 3] = 0xFF;
            }
        }
    }
    return 0;
}
static int MockDecompressYuvPlanes(void *h, const unsigned char *buf, unsigned long len,
                                   unsigned char **planes, int w, int *strides,
                                   int height, int flags)
{
    (void)h;
    (void)buf;
    (void)len;
    (void)flags;
    for (int y = 0; y < height; ++y) {
        memset(planes[0] + (size_t)y * (size_t)strides[0], (uint8_t)(0x40 + y), (size_t)w);
    }
    int uv_w = (w + 1) / 2;
    int uv_h = (height + 1) / 2;
    for (int uy = 0; uy < uv_h; ++uy) {
        memset(planes[1] + (size_t)uy * (size_t)strides[1], 0x55, (size_t)uv_w);
        memset(planes[2] + (size_t)uy * (size_t)strides[2], 0xAA, (size_t)uv_w);
    }
    return 0;
}
static char *MockErrStr(void *h)
{
    (void)h;
    return (char *)"mock_err";
}
static int LoadTurboJpegLocked(void)
{
    g_tj.dl_handle = (void *)1;
    g_tj.init_decompress = MockInit;
    g_tj.destroy = MockDestroy;
    g_tj.decompress_header3 = MockHdr3;
    g_tj.get_scaling_factors = MockScalingFactors;
    g_tj.decompress2 = MockDecompress2;
    g_tj.decompress_to_yuv_planes = MockDecompressYuvPlanes;
    g_tj.get_error_str2 = MockErrStr;
    return 0;
}

int main(void)
{
    struct ICodecJpegHwi *hwi = GetCodecJpegHwi();
    assert(hwi != NULL);
    assert(hwi->JpegInit() == 0);
    assert(hwi->NotifyPowerOn() == 0);

    /* Allocate + map input scan buffer (simulates OHOS TryNormalInBuff post-SOS data) */
    BufferHandle *in_buf = NULL;
    assert(hwi->AllocateInBuffer(&in_buf, 128) == 0);
    assert(in_buf != NULL && in_buf->fd >= 0 && in_buf->size >= 4096);
    uint8_t *in_map = mmap(NULL, (size_t)in_buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, in_buf->fd, 0);
    assert(in_map != MAP_FAILED);
    in_map[0] = 0xAA;
    in_map[1] = 0xBB;
    in_map[2] = 0xFF;
    in_map[3] = 0xD9;
    munmap(in_map, (size_t)in_buf->size);

    /* Construct CodecJpegDecInfoRaw */
    struct CodecJpegCompInfoRaw comps[3] = {
        {.componentId = 1, .componentIndex = 0, .hSampFactor = 2, .vSampFactor = 2, .quantTableNo = 0, .dcTableNo = 0, .acTableNo = 0, .infoFlag = 1},
        {.componentId = 2, .componentIndex = 1, .hSampFactor = 1, .vSampFactor = 1, .quantTableNo = 1, .dcTableNo = 1, .acTableNo = 1, .infoFlag = 1},
        {.componentId = 3, .componentIndex = 2, .hSampFactor = 1, .vSampFactor = 1, .quantTableNo = 1, .dcTableNo = 1, .acTableNo = 1, .infoFlag = 1},
    };
    uint16_t qvals[2][64];
    for (int i = 0; i < 64; ++i) {
        qvals[0][i] = (uint16_t)(i + 1);
        qvals[1][i] = (uint16_t)(i + 2);
    }
    struct CodecJpegQuantTableRaw qts[2] = {
        {.quantVal = {(const uint8_t *)qvals[0], (const uint8_t *)(qvals[0] + 64), (const uint8_t *)(qvals[0] + 64)}, .tableFlag = 1},
        {.quantVal = {(const uint8_t *)qvals[1], (const uint8_t *)(qvals[1] + 64), (const uint8_t *)(qvals[1] + 64)}, .tableFlag = 1},
    };
    struct CodecJpegHuffTableRaw dcs[2] = {
        {.bits = {kStdDcBits[0], kStdDcBits[0] + 16, kStdDcBits[0] + 16},
         .huffVal = {kStdDcVal[0], kStdDcVal[0] + 12, kStdDcVal[0] + 12},
         .tableFlag = 1},
        {.bits = {kStdDcBits[1], kStdDcBits[1] + 16, kStdDcBits[1] + 16},
         .huffVal = {kStdDcVal[1], kStdDcVal[1] + 12, kStdDcVal[1] + 12},
         .tableFlag = 1},
    };
    struct CodecJpegHuffTableRaw acs[2] = {
        {.bits = {kStdAcBits[0], kStdAcBits[0] + 16, kStdAcBits[0] + 16},
         .huffVal = {kStdAcVal[0], kStdAcVal[0] + 162, kStdAcVal[0] + 162},
         .tableFlag = 1},
        {.bits = {kStdAcBits[1], kStdAcBits[1] + 16, kStdAcBits[1] + 16},
         .huffVal = {kStdAcVal[1], kStdAcVal[1] + 162, kStdAcVal[1] + 162},
         .tableFlag = 1},
    };

    struct CodecJpegDecInfoRaw dec;
    memset(&dec, 0, sizeof(dec));
    dec.imageWidth = 16;
    dec.imageHeight = 12;
    dec.dataPrecision = 8;
    dec.numComponents = 3;
    dec.compInfo = (struct VecHeaderRaw){(const uint8_t *)comps, (const uint8_t *)(comps + 3), (const uint8_t *)(comps + 3)};
    dec.dcHuffTbl = (struct VecHeaderRaw){(const uint8_t *)dcs, (const uint8_t *)(dcs + 2), (const uint8_t *)(dcs + 2)};
    dec.acHuffTbl = (struct VecHeaderRaw){(const uint8_t *)acs, (const uint8_t *)(acs + 2), (const uint8_t *)(acs + 2)};
    dec.quantTbl = (struct VecHeaderRaw){(const uint8_t *)qts, (const uint8_t *)(qts + 2), (const uint8_t *)(qts + 2)};
    dec.sampleSize = 2;

    /* 1. RGBA_8888 with sampleSize=2 -> 8x6 output */
    uint8_t out_rgba[6 * 64];
    memset(out_rgba, 0, sizeof(out_rgba));
    BufferHandle out_bh;
    memset(&out_bh, 0, sizeof(out_bh));
    out_bh.fd = -1;
    out_bh.width = 8;
    out_bh.height = 6;
    out_bh.stride = 64;
    out_bh.size = sizeof(out_rgba);
    out_bh.format = OHOS_PIXEL_FMT_RGBA_8888;
    out_bh.virAddr = out_rgba;
    assert(hwi->DoJpegDecode(in_buf, &out_bh, &dec) == 0);
    assert(out_rgba[0] == 10 && out_rgba[1] == 20 && out_rgba[2] == 0x33 && out_rgba[3] == 0xFF);
    assert(out_rgba[64 + 4] == 11 && out_rgba[64 + 5] == 21);

    /* Verify zigzag DQT in reconstructed JPEG: first DQT index 1 should come from kJpegNaturalOrder[1]=1 (qvals[0][1]=2), index 2 from kJpegNaturalOrder[2]=8 (qvals[0][8]=9) */
    assert(g_last_hdr_len > 10 && g_last_hdr_bytes[2] == 0xFF && g_last_hdr_bytes[3] == 0xDB);
    assert(g_last_hdr_bytes[7] == 1 && g_last_hdr_bytes[8] == 2 && g_last_hdr_bytes[9] == 9);

    /* 2. NV21 with sampleSize=1 and crop region (left=4, top=4, w=8, h=4) */
    dec.sampleSize = 1;
    dec.region = (struct CodecImageRegionRaw){.left = 4, .right = 12, .top = 4, .bottom = 8, .flag = 1};
    uint8_t out_nv21[64 * 8];
    memset(out_nv21, 0, sizeof(out_nv21));
    out_bh.width = 8;
    out_bh.height = 4;
    out_bh.stride = 32;
    out_bh.size = sizeof(out_nv21);
    out_bh.format = OHOS_PIXEL_FMT_YCRCB_420_SP;
    out_bh.virAddr = out_nv21;
    assert(hwi->DoJpegDecode(in_buf, &out_bh, &dec) == 0);

    /* 3. NV21 direct planar->semi-planar NEON path (no region crop, 16x12) */
    dec.region.flag = 0;
    uint8_t out_nv21_full[64 * 20];
    memset(out_nv21_full, 0, sizeof(out_nv21_full));
    out_bh.width = 16;
    out_bh.height = 12;
    out_bh.stride = 64;
    out_bh.size = sizeof(out_nv21_full);
    out_bh.format = OHOS_PIXEL_FMT_YCRCB_420_SP;
    out_bh.virAddr = out_nv21_full;
    assert(hwi->DoJpegDecode(in_buf, &out_bh, &dec) == 0);
    assert(out_nv21_full[0] == 0x40);
    /* Y plane = 64*12 = 768; NV21 starts with V=0xAA, U=0x55 */
    assert(out_nv21_full[768 + 0] == 0xAA && out_nv21_full[768 + 1] == 0x55);

    /* 4. NV12 direct path + full JPEG passthrough (starts with FF D8) */
    BufferHandle *in_buf2 = NULL;
    assert(g_last_hdr_len > 0 && g_last_hdr_len <= sizeof(g_last_hdr_bytes));
    assert(hwi->AllocateInBuffer(&in_buf2, (uint32_t)g_last_hdr_len) == 0);
    assert(in_buf2 != NULL && (unsigned long)in_buf2->size >= g_last_hdr_len);
    uint8_t *in_map2 = mmap(NULL, (size_t)in_buf2->size, PROT_READ | PROT_WRITE, MAP_SHARED, in_buf2->fd, 0);
    assert(in_map2 != MAP_FAILED);
    memcpy(in_map2, g_last_hdr_bytes, g_last_hdr_len);
    munmap(in_map2, (size_t)in_buf2->size);
    out_bh.format = OHOS_PIXEL_FMT_YCBCR_420_SP;
    assert(hwi->DoJpegDecode(in_buf2, &out_bh, &dec) == 0);
    assert(out_nv21_full[768 + 0] == 0x55 && out_nv21_full[768 + 1] == 0xAA);

    assert(hwi->FreeInBuffer(in_buf2) == 0);
    assert(hwi->FreeInBuffer(in_buf) == 0);
    assert(hwi->JpegDeInit() == 0);
    printf("JPEG_VDI_HOST_TEST_OK\n");
    return 0;
}
#endif
