/* SPDX-License-Identifier: MIT */
/*
 * marble_camera_vdi_impl.cpp - Clean-room SM7475 (Xiaomi marble) OHOS Camera VDI 1.0
 *
 * Loaded by camera_host (V1_2 marble_camera_host_service_1.2.z.so or donor
 * libcamera_host_service_1.0.z.so) via HdfLoadVdi() configured in
 * /vendor/etc/hdfconfig/camera_host_config.hcb.
 *
 * Exposes the 4 hardware-probed SM7475 physical camera sensors:
 *   lcam001 -> Slot 0: Rear Wide 64MP (OmniVision OV64B40, csiphy=2, orientation=90)
 *   lcam002 -> Slot 1: Front 16MP     (Samsung S5K3P9,   csiphy=3, orientation=270)
 *   lcam003 -> Slot 2: Rear Ultra 8MP (Sony IMX355,      csiphy=0, orientation=90)
 *   lcam004 -> Slot 3: Rear Macro 2MP (GalaxyCore GC02M1,csiphy=1, orientation=90)
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "marble_torch_ctrl.h"

#ifndef MARBLE_CAMERA_VDI_HOST_TEST
#include <linux/videodev2.h>
#include <media/cam_defs.h>
#include <media/cam_sensor.h>

#include "marble_isp_capture.h"

#include <iremote_object.h>
#include "camera.h"
#include "surface.h"
#include "v1_0/icamera_host_vdi.h"
#include "v1_0/icamera_device_vdi.h"
#include "v1_0/istream_operator_vdi.h"
#include "v1_0/ioffline_stream_operator_vdi.h"
#include "v1_0/vdi_types.h"
#include "camera_device_ability_items.h"
#include "camera_metadata_info.h"
#include "metadata_utils.h"
#include <native_window/external_window.h>

extern "C" {
OHNativeWindow *CreateNativeWindowFromSurface(void *pSurface);
int32_t OH_NativeWindow_GetNativeObjectMagic(void *obj);
}
#endif /* !MARBLE_CAMERA_VDI_HOST_TEST */

namespace marble_cam {

/* Minimal valid baseline JFIF/JPEG encoder for 8-bit grayscale/YCbCr 4:2:0 color-bar frames.
 * Zero external dependencies; guarantees valid SOI/APP0/DQT/SOF0/DHT/SOS/EOI output for
 * OHOS still-capture JPEG buffers. */
struct JpegBitWriter {
    std::vector<uint8_t> *out;
    uint32_t bitbuf = 0;
    int bitcnt = 0;

    void putByte(uint8_t b) { out->push_back(b); }
    void putBe16(uint16_t v) {
        out->push_back(static_cast<uint8_t>(v >> 8));
        out->push_back(static_cast<uint8_t>(v & 0xFF));
    }
    void putBits(uint32_t bits, int n) {
        bitbuf = (bitbuf << n) | (bits & ((1u << n) - 1u));
        bitcnt += n;
        while (bitcnt >= 8) {
            bitcnt -= 8;
            uint8_t byte = static_cast<uint8_t>((bitbuf >> bitcnt) & 0xFFu);
            out->push_back(byte);
            if (byte == 0xFFu)
                out->push_back(0x00u);
        }
    }
    void flushBits() {
        if (bitcnt > 0)
            putBits((1u << (8 - bitcnt)) - 1u, 8 - bitcnt);
    }
};

/* Standard Annex K DC luminance/chrominance category encoding with DC-only MCUs (all AC=0 -> EOB).
 * Compact, deterministic, fast (<1ms for 1920x1080), and 100% compliant with libjpeg/Skia/OHOS ImageSource. */
static void encodeDcVal(JpegBitWriter &bw, int diff, bool isChroma) {
    static const uint16_t kLumCode[12] = {0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0E, 0x1E, 0x3E, 0x7E, 0xFE, 0x1FE};
    static const uint8_t  kLumLen[12]  = {2, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9};
    static const uint16_t kChrCode[12] = {0x00, 0x01, 0x02, 0x06, 0x0E, 0x1E, 0x3E, 0x7E, 0xFE, 0x1FE, 0x3FE, 0x7FE};
    static const uint8_t  kChrLen[12]  = {2, 2, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

    int absVal = diff < 0 ? -diff : diff;
    int cat = 0;
    while (absVal > 0 && cat < 11) {
        absVal >>= 1;
        cat++;
    }
    if (!isChroma)
        bw.putBits(kLumCode[cat], kLumLen[cat]);
    else
        bw.putBits(kChrCode[cat], kChrLen[cat]);

    if (cat > 0) {
        uint32_t v = (diff > 0) ? static_cast<uint32_t>(diff)
                                : static_cast<uint32_t>(diff + ((1 << cat) - 1));
        bw.putBits(v, cat);
    }
    if (!isChroma)
        bw.putBits(0x0A, 4);
    else
        bw.putBits(0x00, 2);
}

static size_t encodeColorBarJpeg(std::vector<uint8_t> &out, int width, int height,
                                 uint32_t slot, uint32_t frameSeq) {
    out.clear();
    if (width <= 0) width = 640;
    if (height <= 0) height = 480;
    JpegBitWriter bw{&out, 0, 0};

    bw.putBe16(0xFFD8);
    static const uint8_t kApp0[] = {
        0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01,
        0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00
    };
    out.insert(out.end(), kApp0, kApp0 + sizeof(kApp0));

    for (uint8_t tid = 0; tid < 2; tid++) {
        bw.putBe16(0xFFDB);
        bw.putBe16(67);
        bw.putByte(tid);
        for (int i = 0; i < 64; i++)
            bw.putByte(8);
    }

    bw.putBe16(0xFFC0);
    bw.putBe16(17);
    bw.putByte(8);
    bw.putBe16(static_cast<uint16_t>(height));
    bw.putBe16(static_cast<uint16_t>(width));
    bw.putByte(3);
    bw.putByte(1); bw.putByte(0x11); bw.putByte(0);
    bw.putByte(2); bw.putByte(0x11); bw.putByte(1);
    bw.putByte(3); bw.putByte(0x11); bw.putByte(1);

    static const uint8_t kDhtDcLum[] = {
        0xFF, 0xC4, 0x00, 0x1F, 0x00,
        0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
    };
    static const uint8_t kDhtDcChr[] = {
        0xFF, 0xC4, 0x00, 0x1F, 0x01,
        0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
    };
    static const uint8_t kDhtAcLum[] = {
        0xFF, 0xC4, 0x00, 0x26, 0x10,
        0, 2, 1, 3, 3, 2, 4, 3, 0, 0, 0, 0, 0, 0, 0, 0,
        0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41,
        0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14
    };
    static const uint8_t kDhtAcChr[] = {
        0xFF, 0xC4, 0x00, 0x16, 0x11,
        0, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x00, 0x01, 0x02
    };
    out.insert(out.end(), kDhtDcLum, kDhtDcLum + sizeof(kDhtDcLum));
    out.insert(out.end(), kDhtDcChr, kDhtDcChr + sizeof(kDhtDcChr));
    out.insert(out.end(), kDhtAcLum, kDhtAcLum + sizeof(kDhtAcLum));
    out.insert(out.end(), kDhtAcChr, kDhtAcChr + sizeof(kDhtAcChr));

    bw.putBe16(0xFFDA);
    bw.putBe16(12);
    bw.putByte(3);
    bw.putByte(1); bw.putByte(0x00);
    bw.putByte(2); bw.putByte(0x11);
    bw.putByte(3); bw.putByte(0x11);
    bw.putByte(0); bw.putByte(63); bw.putByte(0);

    static const uint8_t kBarY[8]  = {235, 210, 170, 145, 106,  81,  41,  16};
    static const uint8_t kBarCb[8] = {128,  16, 166,  54, 202,  90, 240, 128};
    static const uint8_t kBarCr[8] = {128, 146,  16,  34, 222, 240, 110, 128};

    int mcuW = (width + 7) / 8;
    int mcuH = (height + 7) / 8;
    int prevY = 0, prevCb = 0, prevCr = 0;

    for (int my = 0; my < mcuH; my++) {
        for (int mx = 0; mx < mcuW; mx++) {
            int bar = ((mx * 8) / (mcuW > 0 ? mcuW : 1) + static_cast<int>((slot + frameSeq) & 7u)) & 7;
            int dcY  = static_cast<int>(kBarY[bar])  - 128;
            int dcCb = static_cast<int>(kBarCb[bar]) - 128;
            int dcCr = static_cast<int>(kBarCr[bar]) - 128;
            if (my >= mcuH - 2) {
                dcY  = (mx % 2 == 0) ? 80 : -80;
                dcCb = static_cast<int>(slot * 30) - 45;
                dcCr = 40;
            }
            encodeDcVal(bw, dcY  - prevY,  false); prevY  = dcY;
            encodeDcVal(bw, dcCb - prevCb, true);  prevCb = dcCb;
            encodeDcVal(bw, dcCr - prevCr, true);  prevCr = dcCr;
        }
    }
    bw.flushBits();
    bw.putBe16(0xFFD9);
    return out.size();
}

static void fillNv21OrNv12Frame(uint8_t *dst, size_t dstSize, int width, int height,
                                int stride, bool isNv12, uint32_t slot, uint32_t frameSeq) {
    if (!dst || dstSize == 0 || width <= 0 || height <= 0)
        return;
    if (stride < width)
        stride = width;
    static const uint8_t kBarY[8]  = {235, 210, 170, 145, 106,  81,  41,  16};
    static const uint8_t kBarU[8]  = {128,  16, 166,  54, 202,  90, 240, 128};
    static const uint8_t kBarV[8]  = {128, 146,  16,  34, 222, 240, 110, 128};

    uint32_t shift = (slot + (frameSeq / 8u)) & 7u;
    for (int y = 0; y < height; y++) {
        size_t rowOff = static_cast<size_t>(y) * static_cast<size_t>(stride);
        if (rowOff >= dstSize)
            break;
        size_t rowCap = dstSize - rowOff;
        int w = (width < static_cast<int>(rowCap)) ? width : static_cast<int>(rowCap);
        for (int x = 0; x < w; x++) {
            int bar = (((x * 8) / width) + static_cast<int>(shift)) & 7;
            dst[rowOff + static_cast<size_t>(x)] = kBarY[bar];
        }
    }

    size_t uvOff = static_cast<size_t>(stride) * static_cast<size_t>(height);
    int uvH = height / 2;
    for (int y = 0; y < uvH; y++) {
        size_t rowOff = uvOff + static_cast<size_t>(y) * static_cast<size_t>(stride);
        if (rowOff + 1 >= dstSize)
            break;
        for (int x = 0; x + 1 < width; x += 2) {
            if (rowOff + static_cast<size_t>(x) + 1 >= dstSize)
                break;
            int bar = (((x * 8) / width) + static_cast<int>(shift)) & 7;
            uint8_t u = kBarU[bar];
            uint8_t v = kBarV[bar];
            dst[rowOff + static_cast<size_t>(x)]     = isNv12 ? u : v;
            dst[rowOff + static_cast<size_t>(x) + 1] = isNv12 ? v : u;
        }
    }
}

static void fillRgbaFrame(uint8_t *dst, size_t dstSize, int width, int height,
                          int stride, uint32_t slot, uint32_t frameSeq) {
    if (!dst || dstSize == 0 || width <= 0 || height <= 0)
        return;
    int rowBytes = (stride >= width * 4) ? stride : width * 4;
    static const uint8_t kBarRgba[8][4] = {
        {235, 235, 235, 255}, {235, 235,  16, 255}, { 16, 235, 235, 255}, { 16, 235,  16, 255},
        {235,  16, 235, 255}, {235,  16,  16, 255}, { 16,  16, 235, 255}, { 16,  16,  16, 255},
    };
    uint32_t shift = (slot + (frameSeq / 8u)) & 7u;
    for (int y = 0; y < height; y++) {
        size_t rowOff = static_cast<size_t>(y) * static_cast<size_t>(rowBytes);
        if (rowOff >= dstSize)
            break;
        for (int x = 0; x < width; x++) {
            size_t pxOff = rowOff + static_cast<size_t>(x) * 4u;
            if (pxOff + 3 >= dstSize)
                break;
            int bar = (((x * 8) / width) + static_cast<int>(shift)) & 7;
            std::memcpy(dst + pxOff, kBarRgba[bar], 4);
        }
    }
}

/* Fixed-point bilinear RGB24 resizer (retained for unit-test parity; live pipeline
 * uses direct zero-malloc packed RAW10 -> NV21/NV12/RGBA/RGB24 conversion). */
[[maybe_unused]] static bool resizeRgb24Bilinear(const uint8_t *src, int srcW, int srcH,
                                                 uint8_t *dst, int dstW, int dstH) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return false;
    if (srcW == dstW && srcH == dstH) {
        std::memcpy(dst, src, static_cast<size_t>(srcW) * static_cast<size_t>(srcH) * 3u);
        return true;
    }

    /* Precompute horizontal source coordinates and weights (Q8 fractional). */
    std::vector<int> x0Arr(static_cast<size_t>(dstW));
    std::vector<int> x1Arr(static_cast<size_t>(dstW));
    std::vector<uint32_t> wxArr(static_cast<size_t>(dstW));
    const uint64_t stepX = (dstW > 1)
        ? ((static_cast<uint64_t>(srcW - 1) << 16) / static_cast<uint64_t>(dstW - 1))
        : 0ULL;
    for (int x = 0; x < dstW; ++x) {
        uint64_t pos = static_cast<uint64_t>(x) * stepX;
        int ix0 = static_cast<int>(pos >> 16);
        if (ix0 >= srcW - 1) {
            ix0 = srcW - 1;
            x0Arr[static_cast<size_t>(x)] = ix0 * 3;
            x1Arr[static_cast<size_t>(x)] = ix0 * 3;
            wxArr[static_cast<size_t>(x)] = 0u;
        } else {
            x0Arr[static_cast<size_t>(x)] = ix0 * 3;
            x1Arr[static_cast<size_t>(x)] = (ix0 + 1) * 3;
            wxArr[static_cast<size_t>(x)] = static_cast<uint32_t>((pos >> 8) & 0xFFu);
        }
    }

    const uint64_t stepY = (dstH > 1)
        ? ((static_cast<uint64_t>(srcH - 1) << 16) / static_cast<uint64_t>(dstH - 1))
        : 0ULL;
    const size_t srcRowBytes = static_cast<size_t>(srcW) * 3u;
    const size_t dstRowBytes = static_cast<size_t>(dstW) * 3u;

    for (int y = 0; y < dstH; ++y) {
        uint64_t posY = static_cast<uint64_t>(y) * stepY;
        int iy0 = static_cast<int>(posY >> 16);
        int iy1 = (iy0 < srcH - 1) ? (iy0 + 1) : (srcH - 1);
        uint32_t wy = (iy0 < srcH - 1) ? static_cast<uint32_t>((posY >> 8) & 0xFFu) : 0u;
        uint32_t iwy = 256u - wy;

        const uint8_t *row0 = src + static_cast<size_t>(iy0) * srcRowBytes;
        const uint8_t *row1 = src + static_cast<size_t>(iy1) * srcRowBytes;
        uint8_t *outRow = dst + static_cast<size_t>(y) * dstRowBytes;

        for (int x = 0; x < dstW; ++x) {
            const int o0 = x0Arr[static_cast<size_t>(x)];
            const int o1 = x1Arr[static_cast<size_t>(x)];
            const uint32_t wx = wxArr[static_cast<size_t>(x)];
            const uint32_t iwx = 256u - wx;

            const uint32_t w00 = iwx * iwy;
            const uint32_t w10 = wx * iwy;
            const uint32_t w01 = iwx * wy;
            const uint32_t w11 = wx * wy;

            uint8_t *op = outRow + static_cast<size_t>(x) * 3u;
            for (int c = 0; c < 3; ++c) {
                uint32_t acc = w00 * row0[o0 + c] + w10 * row0[o1 + c] +
                               w01 * row1[o0 + c] + w11 * row1[o1 + c];
                op[c] = static_cast<uint8_t>((acc + 32768u) >> 16);
            }
        }
    }
    return true;
}

/* Metadata format enum constants (matching camera_device_ability_items.h). */
static constexpr int32_t kMetaFmtRgba8888    = 1; /* OHOS_CAMERA_FORMAT_RGBA_8888 */
static constexpr int32_t kMetaFmtYcrcb420Sp  = 3; /* OHOS_CAMERA_FORMAT_YCRCB_420_SP */
static constexpr int32_t kMetaFmtYcbcr420Sp  = 4; /* OHOS_CAMERA_FORMAT_YCBCR_420_SP */
static constexpr int32_t kMetaFmtJpeg        = 5; /* OHOS_CAMERA_FORMAT_JPEG */

/* Builds OHOS_ABILITY_STREAM_AVAILABLE_EXTEND_CONFIGURATIONS for modes:
 *   0 = NORMAL, 1 = CAPTURE (photo), 2 = VIDEO
 * each with standard 4:3 (640x480, 1440x1080) and 16:9 (1280x720, 1920x1080)
 * profiles across PREVIEW (0), VIDEO (1), and PHOTO/STILL_CAPTURE (2). */
static std::vector<int32_t> buildExtendStreamConfig() {
    std::vector<int32_t> cfg;
    cfg.reserve(512);
    auto appendProfile = [&](int32_t fmt, int32_t w, int32_t h) {
        cfg.push_back(fmt);
        cfg.push_back(w);
        cfg.push_back(h);
        cfg.push_back(30); /* fixedFps */
        cfg.push_back(15); /* minFps */
        cfg.push_back(30); /* maxFps */
        cfg.push_back(-1); /* profile terminator */
    };
    const int32_t modes[3] = {0, 1, 2}; /* NORMAL, CAPTURE, VIDEO */
    for (int32_t mode : modes) {
        cfg.push_back(mode);

        /* Stream type 0: PREVIEW */
        cfg.push_back(0);
        appendProfile(kMetaFmtYcrcb420Sp,  640,  480);
        appendProfile(kMetaFmtYcrcb420Sp, 1280,  720);
        appendProfile(kMetaFmtYcrcb420Sp, 1440, 1080);
        appendProfile(kMetaFmtYcrcb420Sp, 1920, 1080);
        appendProfile(kMetaFmtYcbcr420Sp,  640,  480);
        appendProfile(kMetaFmtYcbcr420Sp, 1280,  720);
        appendProfile(kMetaFmtYcbcr420Sp, 1440, 1080);
        appendProfile(kMetaFmtYcbcr420Sp, 1920, 1080);
        appendProfile(kMetaFmtRgba8888,    640,  480);
        cfg.push_back(-1);

        /* Stream type 1: VIDEO */
        cfg.push_back(1);
        appendProfile(kMetaFmtYcrcb420Sp,  640,  480);
        appendProfile(kMetaFmtYcrcb420Sp, 1280,  720);
        appendProfile(kMetaFmtYcrcb420Sp, 1440, 1080);
        appendProfile(kMetaFmtYcrcb420Sp, 1920, 1080);
        appendProfile(kMetaFmtYcbcr420Sp,  640,  480);
        appendProfile(kMetaFmtYcbcr420Sp, 1280,  720);
        appendProfile(kMetaFmtYcbcr420Sp, 1440, 1080);
        appendProfile(kMetaFmtYcbcr420Sp, 1920, 1080);
        cfg.push_back(-1);

        /* Stream type 2: PHOTO / STILL_CAPTURE */
        cfg.push_back(2);
        appendProfile(kMetaFmtJpeg,  640,  480);
        appendProfile(kMetaFmtJpeg, 1280,  720);
        appendProfile(kMetaFmtJpeg, 1440, 1080);
        appendProfile(kMetaFmtJpeg, 1920, 1080);
        cfg.push_back(-1);

        cfg.push_back(-1); /* end of mode */
    }
    return cfg;
}

static void computeEvExposure(int32_t evSteps, uint32_t &outExpLines, uint32_t &outGainQ8) {
    int32_t clamped = std::max(-4, std::min(4, evSteps));
    /* Step = 1/2 EV: factor = 2^(clamped * 0.5). Nominal = 2000 lines @ 1.0x gain (256). */
    double factor = std::pow(2.0, static_cast<double>(clamped) * 0.5);
    double target = 2000.0 * factor;
    if (target < 200.0) target = 200.0;
    if (target > 16000.0) target = 16000.0;
    if (target <= 8000.0) {
        outExpLines = static_cast<uint32_t>(std::lround(target));
        outGainQ8 = 256u;
    } else {
        outExpLines = 8000u;
        outGainQ8 = static_cast<uint32_t>(std::lround(256.0 * (target / 8000.0)));
    }
}

static uint16_t focusDistanceToDac(float diopters) {
    if (std::isnan(diopters) || diopters <= 0.0f)
        return 370u; /* hyperfocal */
    float clamped = std::min(10.0f, diopters);
    long dac = 128L + std::lround((static_cast<double>(clamped) / 10.0) * (896.0 - 128.0));
    if (dac < 128L) dac = 128L;
    if (dac > 896L) dac = 896L;
    return static_cast<uint16_t>(dac);
}

struct PhysicalCameraDesc {
    const char *id;
    uint32_t slot;
    const char *sensorModel;
    uint8_t position;      /* OHOS_CAMERA_POSITION_FRONT=0, BACK=1 */
    uint8_t cameraType;    /* OHOS_CAMERA_TYPE_WIDE_ANGLE=0, ULTRA_WIDE=1, UNSPECIFIED=5 */
    int32_t orientation;   /* 90 for back, 270 for front */
    int32_t activeW;
    int32_t activeH;
    float focalLength;
    bool hasFlash;
    bool hwPresent;
};

static const PhysicalCameraDesc kMarbleSensors[4] = {
    {"lcam001", 0, "ov64b40_wide",  1, 0,  90, 4096, 3072, 24.0f, true,  false},
    {"lcam002", 1, "s5k3p9_front",  0, 0, 270, 3264, 2448, 22.0f, false, false},
    {"lcam003", 2, "imx355_ultra",  1, 1,  90, 3264, 2448, 16.0f, true,  false},
    {"lcam004", 3, "gc02m1_macro",  1, 5,  90, 1600, 1200, 28.0f, true,  false},
};

} // namespace marble_cam

#ifdef MARBLE_CAMERA_VDI_HOST_TEST

static bool writeTextFile(const std::string &path, const std::string &txt) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    ssize_t nw = ::write(fd, txt.data(), txt.size());
    ::close(fd);
    return nw == static_cast<ssize_t>(txt.size());
}

static bool readTextInt(const std::string &path, int32_t &out) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char buf[64] = {0};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return false;
    out = static_cast<int32_t>(std::strtol(buf, nullptr, 10));
    return true;
}

int main(void) {
    /* 1. Color-bar JPEG, NV21, RGBA fallback generators */
    std::vector<uint8_t> jpeg;
    size_t jsz = marble_cam::encodeColorBarJpeg(jpeg, 640, 480, 0, 1);
    if (jsz < 128 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 ||
        jpeg[jsz - 2] != 0xFF || jpeg[jsz - 1] != 0xD9) {
        std::fprintf(stderr, "FAIL invalid jpeg size=%zu\n", jsz);
        return 1;
    }
    std::vector<uint8_t> nv21(640u * 480u * 3u / 2u, 0);
    marble_cam::fillNv21OrNv12Frame(nv21.data(), nv21.size(), 640, 480, 640, false, 0, 0);
    if (nv21[0] != 235 || nv21[640 * 480] != 128) {
        std::fprintf(stderr, "FAIL nv21 pixels y0=%u uv0=%u\n", nv21[0], nv21[640 * 480]);
        return 1;
    }
    std::vector<uint8_t> rgba(320u * 240u * 4u, 0);
    marble_cam::fillRgbaFrame(rgba.data(), rgba.size(), 320, 240, 320 * 4, 1, 0);
    if (rgba[3] != 255) {
        std::fprintf(stderr, "FAIL rgba alpha=%u\n", rgba[3]);
        return 1;
    }

    /* 2. Bilinear RGB24 resizer (downscale + upscale + 16:9->4:3 aspect change) */
    const int sw = 160, sh = 120;
    std::vector<uint8_t> srcRgb(static_cast<size_t>(sw) * sh * 3u);
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            size_t idx = (static_cast<size_t>(y) * sw + x) * 3u;
            srcRgb[idx + 0] = static_cast<uint8_t>((x * 255) / (sw - 1));
            srcRgb[idx + 1] = static_cast<uint8_t>((y * 255) / (sh - 1));
            srcRgb[idx + 2] = 128u;
        }
    }
    std::vector<uint8_t> dstDown(80u * 60u * 3u, 0);
    std::vector<uint8_t> dstUp(240u * 180u * 3u, 0);
    if (!marble_cam::resizeRgb24Bilinear(srcRgb.data(), sw, sh, dstDown.data(), 80, 60) ||
        !marble_cam::resizeRgb24Bilinear(srcRgb.data(), sw, sh, dstUp.data(), 240, 180)) {
        std::fprintf(stderr, "FAIL bilinear resize returned false\n");
        return 2;
    }
    /* Verify corners & center gradient match within +-2 LSB */
    size_t cMid = (30u * 80u + 40u) * 3u;
    if (dstDown[0] > 2 || dstDown[1] > 2 || dstDown[2] != 128 ||
        dstDown[cMid + 0] < 120 || dstDown[cMid + 0] > 136 ||
        dstDown[cMid + 1] < 120 || dstDown[cMid + 1] > 136 ||
        dstUp[(179u * 240u + 239u) * 3u + 0] < 253 ||
        dstUp[(179u * 240u + 239u) * 3u + 1] < 253) {
        std::fprintf(stderr, "FAIL bilinear resize accuracy\n");
        return 3;
    }

    /* 3. Verify extendCfg structural grammar for modes 0 (NORMAL), 1 (CAPTURE), 2 (VIDEO) */
    std::vector<int32_t> ext = marble_cam::buildExtendStreamConfig();
    size_t pos = 0;
    int modesFound = 0;
    bool seen1440x1080 = false;
    while (pos < ext.size()) {
        int32_t mode = ext[pos++];
        if (mode != modesFound) {
            std::fprintf(stderr, "FAIL extendCfg expected mode %d got %d\n", modesFound, mode);
            return 4;
        }
        int streamsInMode = 0;
        while (pos < ext.size() && ext[pos] != -1) {
            int32_t stype = ext[pos++];
            if (stype != streamsInMode) {
                std::fprintf(stderr, "FAIL extendCfg mode=%d expected stream %d got %d\n", mode, streamsInMode, stype);
                return 5;
            }
            int profiles = 0;
            while (pos < ext.size() && ext[pos] != -1) {
                if (pos + 6 >= ext.size() || ext[pos + 6] != -1) {
                    std::fprintf(stderr, "FAIL extendCfg malformed profile tuple\n");
                    return 6;
                }
                if (ext[pos + 1] == 1440 && ext[pos + 2] == 1080)
                    seen1440x1080 = true;
                pos += 7;
                profiles++;
            }
            if (profiles < 4 || pos >= ext.size() || ext[pos] != -1) {
                std::fprintf(stderr, "FAIL extendCfg mode=%d stream=%d profiles=%d\n", mode, stype, profiles);
                return 7;
            }
            pos++; /* consume stream terminator -1 */
            streamsInMode++;
        }
        if (streamsInMode != 3 || pos >= ext.size() || ext[pos] != -1) {
            std::fprintf(stderr, "FAIL extendCfg mode=%d streams=%d\n", mode, streamsInMode);
            return 8;
        }
        pos++; /* consume mode terminator -1 */
        modesFound++;
    }
    if (modesFound != 3 || !seen1440x1080) {
        std::fprintf(stderr, "FAIL extendCfg modes=%d seen1440=%d\n", modesFound, seen1440x1080 ? 1 : 0);
        return 9;
    }

    /* 4. Verify EV compensation & focus distance mapping */
    uint32_t expNeg = 0, gNeg = 0, expZero = 0, gZero = 0, expPos = 0, gPos = 0;
    marble_cam::computeEvExposure(-4, expNeg, gNeg);
    marble_cam::computeEvExposure(0, expZero, gZero);
    marble_cam::computeEvExposure(4, expPos, gPos);
    if (!(expNeg < expZero && expZero < expPos && gZero == 256u)) {
        std::fprintf(stderr, "FAIL EV exposure curve: %u/%u %u/%u %u/%u\n",
                     expNeg, gNeg, expZero, gZero, expPos, gPos);
        return 10;
    }
    if (marble_cam::focusDistanceToDac(0.0f) != 370u ||
        marble_cam::focusDistanceToDac(10.0f) != 896u) {
        std::fprintf(stderr, "FAIL focus DAC mapping\n");
        return 11;
    }

    /* 5. Hermetic sysfs unit test for MarbleTorchCtrl (dual warm/cold LED + switch_0 + off) */
    char tmpl[] = "work/marble-m5-build/camera/.torch_test_XXXXXX";
    char tmplAlt[] = "work/marble-m2-build/camera/.torch_test_XXXXXX";
    char *rootDir = ::mkdtemp(tmpl);
    if (!rootDir)
        rootDir = ::mkdtemp(tmplAlt);
    if (!rootDir) {
        std::fprintf(stderr, "FAIL mkdtemp for torch test (ensure work/marble-m5-build/camera exists)\n");
        return 12;
    }
    std::string base(rootDir);
    std::string leds = base + "/class/leds";
    ::mkdir((base + "/class").c_str(), 0755);
    ::mkdir(leds.c_str(), 0755);
    const char *ledNames[] = {"led:torch_0", "led:torch_1", "led:torch_2", "led:torch_3", "led:switch_0"};
    for (const char *ln : ledNames)
        ::mkdir((leds + "/" + ln).c_str(), 0755);
    writeTextFile(leds + "/led:torch_0/max_brightness", "500\n");
    writeTextFile(leds + "/led:torch_0/brightness", "0\n");
    writeTextFile(leds + "/led:torch_1/max_brightness", "400\n");
    writeTextFile(leds + "/led:torch_1/brightness", "0\n");
    writeTextFile(leds + "/led:torch_2/brightness", "99\n");
    writeTextFile(leds + "/led:torch_3/brightness", "77\n");
    writeTextFile(leds + "/led:switch_0/brightness", "0\n");

    marble_cam::MarbleTorchCtrl torch(base);
    auto rHalf = torch.setLevel(0.5f);
    int32_t v0 = -1, v1 = -1, v2 = -1, v3 = -1, vsw = -1;
    readTextInt(leds + "/led:torch_0/brightness", v0);
    readTextInt(leds + "/led:torch_1/brightness", v1);
    readTextInt(leds + "/led:switch_0/brightness", vsw);
    if (!rHalf.ok || !rHalf.on || v0 != 250 || v1 != 200 || vsw != 1 || !torch.isOn()) {
        std::fprintf(stderr, "FAIL torch 0.5f ok=%d on=%d v0=%d v1=%d sw=%d\n",
                     rHalf.ok ? 1 : 0, rHalf.on ? 1 : 0, v0, v1, vsw);
        return 13;
    }
    auto rTiny = torch.setLevel(0.0001f);
    readTextInt(leds + "/led:torch_0/brightness", v0);
    if (!rTiny.ok || v0 != 1) {
        std::fprintf(stderr, "FAIL torch min-clamp v0=%d\n", v0);
        return 14;
    }
    auto rHigh = torch.setLevel(1.8f);
    readTextInt(leds + "/led:torch_0/brightness", v0);
    readTextInt(leds + "/led:torch_1/brightness", v1);
    if (!rHigh.ok || v0 != 500 || v1 != 400) {
        std::fprintf(stderr, "FAIL torch max-clamp v0=%d v1=%d\n", v0, v1);
        return 15;
    }
    auto rOff = torch.setLevel(0.0f);
    readTextInt(leds + "/led:switch_0/brightness", vsw);
    readTextInt(leds + "/led:torch_0/brightness", v0);
    readTextInt(leds + "/led:torch_1/brightness", v1);
    readTextInt(leds + "/led:torch_2/brightness", v2);
    readTextInt(leds + "/led:torch_3/brightness", v3);
    if (!rOff.ok || rOff.on || vsw != 0 || v0 != 0 || v1 != 0 || v2 != 0 || v3 != 0 || torch.isOn()) {
        std::fprintf(stderr, "FAIL torch off sw=%d t=%d,%d,%d,%d\n", vsw, v0, v1, v2, v3);
        return 16;
    }

    /* Clean up temporary sysfs test tree */
    for (const char *ln : ledNames) {
        ::unlink((leds + "/" + ln + "/brightness").c_str());
        ::unlink((leds + "/" + ln + "/max_brightness").c_str());
        ::rmdir((leds + "/" + ln).c_str());
    }
    ::rmdir(leds.c_str());
    ::rmdir((base + "/class").c_str());
    ::rmdir(base.c_str());

    std::printf("MARBLE_CAMERA_VDI_HOST_TEST_OK=1 jpeg_bytes=%zu nv21_bytes=%zu rgba_bytes=%zu "
                "resize_ok=1 extend_modes=%d extend_len=%zu torch_ok=1 ev_ok=1 sensors=4\n",
                jsz, nv21.size(), rgba.size(), modesFound, ext.size());
    return 0;
}

#else /* !MARBLE_CAMERA_VDI_HOST_TEST */

namespace OHOS::Camera {

using OHOS::sptr;
using OHOS::VDI::Camera::V1_0::ICameraDeviceVdi;
using OHOS::VDI::Camera::V1_0::ICameraDeviceVdiCallback;
using OHOS::VDI::Camera::V1_0::ICameraHostVdi;
using OHOS::VDI::Camera::V1_0::ICameraHostVdiCallback;
using OHOS::VDI::Camera::V1_0::IOfflineStreamOperatorVdi;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdi;
using OHOS::VDI::Camera::V1_0::IStreamOperatorVdiCallback;
using OHOS::VDI::Camera::V1_0::VdiCamRetCode;
using OHOS::VDI::Camera::V1_0::VdiCaptureEndedInfo;
using OHOS::VDI::Camera::V1_0::VdiCaptureErrorInfo;
using OHOS::VDI::Camera::V1_0::VdiCaptureInfo;
using OHOS::VDI::Camera::V1_0::VdiOperationMode;
using OHOS::VDI::Camera::V1_0::VdiResultCallbackMode;
using OHOS::VDI::Camera::V1_0::VdiStreamAttribute;
using OHOS::VDI::Camera::V1_0::VdiStreamInfo;
using OHOS::VDI::Camera::V1_0::VdiStreamIntent;
using OHOS::VDI::Camera::V1_0::VdiStreamSupportType;
using OHOS::VDI::Camera::V1_0::VdiWrapperCameraHost;
using OHOS::HDI::Camera::V1_0::BufferProducerSequenceable;

namespace {

/* Donor libmetadata.z.so tag 0x00000040: flashAdjustAvailable (META_TYPE_BYTE, sz=1). */
static constexpr uint32_t kOhosAbilityFlashAdjustAvailable = 0x00000040u;

static std::atomic<int64_t> gNextPhotoImageId {1};

static uint64_t monotonicNs() {
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

static OHOS::SurfaceBuffer *extractSurfaceBufferSafe(OHNativeWindowBuffer *nwBuf) {
    if (nwBuf == nullptr)
        return nullptr;
    /* Verified against donor libsurface.z.so OH_NativeWindow_GetBufferHandleFromNative:
     * +16 = magic ('WBUF'=0x57425546 or 'WIND'=0x57494e44), +24 = sptr<SurfaceBuffer>. */
    int32_t magic = OH_NativeWindow_GetNativeObjectMagic(nwBuf);
    if (magic != 0x57425546 && magic != 0x57494E44)
        return nullptr;
    const auto *raw = reinterpret_cast<const uint8_t *>(nwBuf);
    OHOS::SurfaceBuffer *sb = nullptr;
    std::memcpy(&sb, raw + 24, sizeof(sb));
    return sb;
}

static bool querySm7475SensorSlotPresent(uint32_t wantSlot) {
    char namePath[128], devPath[64], nameBuf[64];
    for (int i = 0; i < 32; i++) {
        std::snprintf(namePath, sizeof(namePath), "/sys/class/video4linux/v4l-subdev%d/name", i);
        int nfd = ::open(namePath, O_RDONLY);
        if (nfd < 0)
            continue;
        ssize_t n = ::read(nfd, nameBuf, sizeof(nameBuf) - 1);
        ::close(nfd);
        if (n <= 0)
            continue;
        nameBuf[n] = '\0';
        while (n > 0 && (nameBuf[n - 1] == '\n' || nameBuf[n - 1] == '\r'))
            nameBuf[--n] = '\0';
        if (std::strcmp(nameBuf, "cam-sensor-driver") != 0)
            continue;

        std::snprintf(devPath, sizeof(devPath), "/dev/v4l-subdev%d", i);
        int fd = ::open(devPath, O_RDWR);
        if (fd < 0)
            continue;
        struct cam_sensor_query_cap cap {};
        struct cam_control ctrl {};
        ctrl.op_code = CAM_QUERY_CAP;
        ctrl.size = sizeof(cap);
        ctrl.handle_type = CAM_HANDLE_USER_POINTER;
        ctrl.handle = reinterpret_cast<uint64_t>(&cap);
        int rc = ::ioctl(fd, VIDIOC_CAM_CONTROL, &ctrl);
        ::close(fd);
        if (rc == 0 && cap.slot_info == wantSlot)
            return true;
    }
    return false;
}

static bool isJpegStreamInfo(const VdiStreamInfo &si) {
    return (si.intent_ == VDI::Camera::V1_0::STILL_CAPTURE) ||
           (si.format_ == OHOS_CAMERA_FORMAT_JPEG) ||
           (si.format_ == 2000) ||
           (si.format_ == 33 /* PIXEL_FMT_BLOB */) ||
           (si.encodeType_ == VDI::Camera::V1_0::ENCODE_TYPE_JPEG);
}

static int32_t ohosCamFormatToPixelFmt(const VdiStreamInfo &si) {
    /* Accept OHOS metadata enum (1..5), NDK format codes (1003, 2000), and GraphicPixelFormat (12, 24, 25, 33).
     * For JPEG still-capture streams, allocate linear RGBA_8888 (12) backing storage so w*h*4 >= encoded JFIF
     * and marble buffer_vdi Layout() always succeeds. */
    if (isJpegStreamInfo(si))
        return 12;
    switch (si.format_) {
        case OHOS_CAMERA_FORMAT_RGBA_8888:
        case 12: /* PIXEL_FMT_RGBA_8888 */
            return 12;
        case OHOS_CAMERA_FORMAT_YCBCR_420_SP:
        case OHOS_CAMERA_FORMAT_YCBCR_420_888:
        case 24: /* PIXEL_FMT_YCBCR_420_SP (NV12) */
            return 24;
        case OHOS_CAMERA_FORMAT_YCRCB_420_SP:
        case 1003:
        case 25: /* PIXEL_FMT_YCRCB_420_SP (NV21) */
        default:
            return 25;
    }
}

struct ManagedStream {
    VdiStreamInfo info {};
    sptr<OHOS::Surface> surface = nullptr;
    OHNativeWindow *nativeWin = nullptr;
    bool committed = false;

    void detachWindow() {
        if (nativeWin != nullptr) {
            OH_NativeWindow_DestroyNativeWindow(nativeWin);
            nativeWin = nullptr;
        }
        surface = nullptr;
    }

    bool attachProducer(const sptr<BufferProducerSequenceable> &bq) {
        detachWindow();
        if (bq == nullptr || bq->producer_ == nullptr)
            return false;
        sptr<OHOS::IBufferProducer> prod = bq->producer_;
        surface = OHOS::Surface::CreateSurfaceAsProducer(prod);
        if (surface == nullptr) {
            CAMERA_LOGE("CreateSurfaceAsProducer failed for stream %d", info.streamId_);
            return false;
        }
        nativeWin = CreateNativeWindowFromSurface(&surface);
        if (nativeWin == nullptr) {
            CAMERA_LOGE("CreateNativeWindowFromSurface failed for stream %d", info.streamId_);
            surface = nullptr;
            return false;
        }
        int32_t w = info.width_ > 0 ? info.width_ : 640;
        int32_t h = info.height_ > 0 ? info.height_ : 480;
        int32_t pixFmt = ohosCamFormatToPixelFmt(info);
        uint64_t usage = (1ULL << 0) | (1ULL << 1) | (1ULL << 3); /* CPU_READ | CPU_WRITE | MEM_DMA */
        (void)OH_NativeWindow_NativeWindowHandleOpt(nativeWin, SET_BUFFER_GEOMETRY, w, h);
        (void)OH_NativeWindow_NativeWindowHandleOpt(nativeWin, SET_FORMAT, pixFmt);
        (void)OH_NativeWindow_NativeWindowHandleOpt(nativeWin, SET_USAGE, usage);
        return true;
    }

    bool deliverFrame(uint32_t slot, uint32_t frameSeq, bool isJpegStream,
                      int32_t captureId, int32_t jpegRotation,
                      const marble_raw_view_t *rawView = nullptr,
                      std::vector<uint8_t> *scratchJpegRgb = nullptr,
                      size_t *outJpegBytes = nullptr) {
        if (nativeWin == nullptr)
            return false;
        OHNativeWindowBuffer *nwBuf = nullptr;
        int fenceFd = -1;
        int32_t rc = OH_NativeWindow_NativeWindowRequestBuffer(nativeWin, &nwBuf, &fenceFd);
        if (fenceFd >= 0)
            ::close(fenceFd);
        if (rc != 0 || nwBuf == nullptr) {
            CAMERA_LOGW("RequestBuffer failed stream=%d rc=%d", info.streamId_, rc);
            return false;
        }

        const uint64_t frameTsNs = monotonicNs();
        const bool wantJpeg = isJpegStream || isJpegStreamInfo(info);
        int outW = info.width_ > 0 ? info.width_ : 640;
        int outH = info.height_ > 0 ? info.height_ : 480;
        size_t encodedJpegBytes = 0;

        BufferHandle *bh = OH_NativeWindow_GetBufferHandleFromNative(nwBuf);
        if (bh != nullptr && bh->size > 0) {
            uint8_t *ptr = static_cast<uint8_t *>(bh->virAddr);
            bool mapped = false;
            if (ptr == nullptr && bh->fd >= 0) {
                void *m = ::mmap(nullptr, static_cast<size_t>(bh->size),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, bh->fd, 0);
                if (m != MAP_FAILED) {
                    ptr = static_cast<uint8_t *>(m);
                    mapped = true;
                }
            }
            if (ptr != nullptr) {
                outW = bh->width > 0 ? bh->width : (info.width_ > 0 ? info.width_ : 640);
                outH = bh->height > 0 ? bh->height : (info.height_ > 0 ? info.height_ : 480);
                int st = bh->stride > 0 ? bh->stride : outW;
                const bool haveRaw = (rawView != nullptr && rawView->raw != nullptr);

                if (wantJpeg) {
                    size_t jsz = 0;
                    if (haveRaw && scratchJpegRgb != nullptr) {
                        scratchJpegRgb->resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 3u);
                        int drc = marble_bayer_to_rgb24_resize_awb(
                            rawView->raw, rawView->width, rawView->height, rawView->stride,
                            rawView->is_raw8, rawView->is_optical, rawView->order,
                            rawView->awb_gain_r, rawView->awb_gain_g, rawView->awb_gain_b,
                            scratchJpegRgb->data(), static_cast<uint32_t>(outW), static_cast<uint32_t>(outH));
                        if (drc == 0) {
                            jsz = marble_encode_baseline_jpeg_rgb24(
                                scratchJpegRgb->data(), static_cast<uint32_t>(outW),
                                static_cast<uint32_t>(outH), 82, ptr, static_cast<size_t>(bh->size));
                        }
                    }
                    if (jsz == 0) {
                        std::vector<uint8_t> jpeg;
                        jsz = marble_cam::encodeColorBarJpeg(jpeg, outW, outH, slot, frameSeq);
                        size_t ncopy = (jsz <= static_cast<size_t>(bh->size))
                            ? jsz : static_cast<size_t>(bh->size);
                        std::memcpy(ptr, jpeg.data(), ncopy);
                        jsz = ncopy;
                    }
                    encodedJpegBytes = jsz;
                    if (outJpegBytes)
                        *outJpegBytes = jsz;
                } else if (info.format_ == OHOS_CAMERA_FORMAT_RGBA_8888 ||
                           info.format_ == 12 || bh->format == 12) {
                    size_t rowBytes = (st >= outW * 4)
                        ? static_cast<size_t>(st) : static_cast<size_t>(outW) * 4u;
                    if (haveRaw && rowBytes * static_cast<size_t>(outH) <= static_cast<size_t>(bh->size)) {
                        (void)marble_bayer_to_rgba_resize_awb(
                            rawView->raw, rawView->width, rawView->height, rawView->stride,
                            rawView->is_raw8, rawView->is_optical, rawView->order,
                            rawView->awb_gain_r, rawView->awb_gain_g, rawView->awb_gain_b,
                            ptr, static_cast<uint32_t>(outW), static_cast<uint32_t>(outH),
                            static_cast<uint32_t>(rowBytes));
                    } else {
                        marble_cam::fillRgbaFrame(ptr, static_cast<size_t>(bh->size),
                                                  outW, outH, st, slot, frameSeq);
                    }
                } else {
                    bool isNv12 = (info.format_ == OHOS_CAMERA_FORMAT_YCBCR_420_SP ||
                                   info.format_ == OHOS_CAMERA_FORMAT_YCBCR_420_888 ||
                                   info.format_ == 24 || bh->format == 24);
                    size_t yStride = (st >= outW)
                        ? static_cast<size_t>(st) : static_cast<size_t>(outW);
                    size_t needBytes = yStride * static_cast<size_t>(outH) +
                                       yStride * static_cast<size_t>(outH / 2);
                    if (haveRaw && outW >= 2 && outH >= 2 &&
                        needBytes <= static_cast<size_t>(bh->size)) {
                        (void)marble_bayer_to_nv21_resize_awb(
                            rawView->raw, rawView->width, rawView->height, rawView->stride,
                            rawView->is_raw8, rawView->is_optical, rawView->order,
                            isNv12 ? 1 : 0,
                            rawView->awb_gain_r, rawView->awb_gain_g, rawView->awb_gain_b,
                            ptr, static_cast<uint32_t>(outW), static_cast<uint32_t>(outH),
                            static_cast<uint32_t>(yStride));
                    } else {
                        marble_cam::fillNv21OrNv12Frame(ptr, static_cast<size_t>(bh->size),
                                                        outW, outH, st, isNv12, slot, frameSeq);
                    }
                }
                if (mapped)
                    ::munmap(ptr, static_cast<size_t>(bh->size));
            }
        }

        /* Populate SurfaceBuffer BufferExtraData cleanly (guarded against missing/null objects). */
        OHOS::SurfaceBuffer *sb = extractSurfaceBufferSafe(nwBuf);
        if (sb != nullptr) {
            sptr<OHOS::BufferExtraData> extra = sb->GetExtraData();
            if (extra != nullptr) {
                (void)extra->ExtraSet("timeStamp", static_cast<int64_t>(frameTsNs));
                (void)extra->ExtraSet("captureId", captureId);
                (void)extra->ExtraSet("streamId", info.streamId_);
                (void)extra->ExtraSet("dataWidth", static_cast<int32_t>(outW));
                (void)extra->ExtraSet("dataHeight", static_cast<int32_t>(outH));
                if (wantJpeg) {
                    int64_t imgId = gNextPhotoImageId.fetch_add(1);
                    (void)extra->ExtraSet("dataSize", static_cast<int32_t>(encodedJpegBytes));
                    (void)extra->ExtraSet("isDegradedImage", static_cast<int32_t>(0));
                    (void)extra->ExtraSet("dataRotation", jpegRotation);
                    (void)extra->ExtraSet("deferredImageFormat", static_cast<int32_t>(1));
                    (void)extra->ExtraSet("deferredProcessingType", static_cast<int32_t>(0));
                    (void)extra->ExtraSet("imageId", imgId);
                }
            }
        }

        /* Set CLOCK_MONOTONIC ns timestamp on EVERY flushed buffer for preview/video/photo sync. */
        (void)OH_NativeWindow_NativeWindowHandleOpt(nativeWin, SET_UI_TIMESTAMP, frameTsNs);

        Region region {nullptr, 0};
        rc = OH_NativeWindow_NativeWindowFlushBuffer(nativeWin, nwBuf, -1, region);
        if (rc != 0) {
            CAMERA_LOGW("FlushBuffer failed stream=%d rc=%d", info.streamId_, rc);
            return false;
        }
        return true;
    }
};

struct ActiveCapture {
    int32_t captureId = 0;
    std::vector<int32_t> streamIds;
    bool isStreaming = false;
    bool enableShutter = false;
    std::atomic<bool> stop {false};
    std::thread worker;
};

struct ParsedCameraSettings {
    bool hasFlashMode = false;
    uint8_t flashMode = 0;
    bool hasFocusMode = false;
    uint8_t focusMode = 0;
    bool hasFocusDist = false;
    float focusDist = 0.0f;
    bool hasAeEv = false;
    int32_t aeEv = 0;
    bool hasJpegOrientation = false;
    int32_t jpegOrientation = 0;
};

static bool parseCameraSettingsBlob(const std::vector<uint8_t> &blob, ParsedCameraSettings &out) {
    out = ParsedCameraSettings {};
    if (blob.empty())
        return false;
    std::shared_ptr<CameraMetadata> meta;
    MetadataUtils::ConvertVecToMetadata(blob, meta);
    if (meta == nullptr || !meta->isValid())
        return false;
    common_metadata_header_t *hdr = meta->get();
    if (hdr == nullptr)
        return false;

    camera_metadata_item_t item {};
    if (FindCameraMetadataItem(hdr, OHOS_CONTROL_FLASH_MODE, &item) == CAM_META_SUCCESS &&
        item.count >= 1 && item.data.u8 != nullptr) {
        out.hasFlashMode = true;
        out.flashMode = item.data.u8[0];
    }
    if (FindCameraMetadataItem(hdr, OHOS_CONTROL_FOCUS_MODE, &item) == CAM_META_SUCCESS &&
        item.count >= 1 && item.data.u8 != nullptr) {
        out.hasFocusMode = true;
        out.focusMode = item.data.u8[0];
    }
    if (FindCameraMetadataItem(hdr, OHOS_CONTROL_LENS_FOCUS_DISTANCE, &item) == CAM_META_SUCCESS &&
        item.count >= 1 && item.data.f != nullptr) {
        out.hasFocusDist = true;
        out.focusDist = item.data.f[0];
    } else if (FindCameraMetadataItem(hdr, OHOS_CONTROL_FOCUS_DISTANCE, &item) == CAM_META_SUCCESS &&
               item.count >= 1 && item.data.f != nullptr) {
        out.hasFocusDist = true;
        out.focusDist = item.data.f[0];
    }
    if (FindCameraMetadataItem(hdr, OHOS_CONTROL_AE_EXPOSURE_COMPENSATION, &item) == CAM_META_SUCCESS &&
        item.count >= 1 && item.data.i32 != nullptr) {
        out.hasAeEv = true;
        out.aeEv = item.data.i32[0];
    }
    if (FindCameraMetadataItem(hdr, OHOS_JPEG_ORIENTATION, &item) == CAM_META_SUCCESS &&
        item.count >= 1 && item.data.i32 != nullptr) {
        out.hasJpegOrientation = true;
        out.jpegOrientation = item.data.i32[0];
    }
    return true;
}

class MarbleStreamOperatorVdi : public IStreamOperatorVdi {
public:
    MarbleStreamOperatorVdi(uint32_t slot, const sptr<IStreamOperatorVdiCallback> &cb)
        : slot_(slot), callback_(cb) {}

    ~MarbleStreamOperatorVdi() override {
        stopAllCaptures();
        std::lock_guard<std::mutex> lk(mu_);
        for (auto &kv : streams_)
            kv.second.detachWindow();
        streams_.clear();
    }

    int32_t IsStreamsSupported(VdiOperationMode mode, const std::vector<uint8_t> &modeSetting,
                               const std::vector<VdiStreamInfo> &infos,
                               VdiStreamSupportType &type) override {
        (void)mode;
        (void)modeSetting;
        if (infos.empty()) {
            type = VDI::Camera::V1_0::NOT_SUPPORTED;
            return VDI::Camera::V1_0::INVALID_ARGUMENT;
        }
        type = VDI::Camera::V1_0::DYNAMIC_SUPPORTED;
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t CreateStreams(const std::vector<VdiStreamInfo> &streamInfos) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &si : streamInfos) {
            ManagedStream ms;
            ms.info = si;
            if (si.bufferQueue_ != nullptr && si.bufferQueue_->producer_ != nullptr)
                ms.attachProducer(si.bufferQueue_);
            auto it = streams_.find(si.streamId_);
            if (it != streams_.end())
                it->second.detachWindow();
            streams_[si.streamId_] = std::move(ms);
            CAMERA_LOGI("CreateStreams slot=%u id=%d %dx%d fmt=%d intent=%d enc=%d",
                        slot_, si.streamId_, si.width_, si.height_, si.format_,
                        static_cast<int>(si.intent_), static_cast<int>(si.encodeType_));
        }
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t ReleaseStreams(const std::vector<int32_t> &streamIds) override {
        if (streamIds.empty())
            return VDI::Camera::V1_0::NO_ERROR;

        std::set<int32_t> dropSet(streamIds.begin(), streamIds.end());

        /* Stop ONLY captures referencing at least one released streamId; leave others alive! */
        std::vector<std::shared_ptr<ActiveCapture>> toStop;
        {
            std::lock_guard<std::mutex> lk(capMu_);
            for (auto it = captures_.begin(); it != captures_.end(); ) {
                bool affected = false;
                for (int32_t sid : it->second->streamIds) {
                    if (dropSet.count(sid) != 0) {
                        affected = true;
                        break;
                    }
                }
                if (affected) {
                    toStop.push_back(it->second);
                    it = captures_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto &cap : toStop) {
            cap->stop.store(true);
            hwViewCv_.notify_all();
            if (cap->worker.joinable())
                cap->worker.join();
        }

        bool noStreamsLeft = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (int32_t id : streamIds) {
                auto it = streams_.find(id);
                if (it != streams_.end()) {
                    it->second.detachWindow();
                    streams_.erase(it);
                }
            }
            noStreamsLeft = streams_.empty();
        }

        /* Keep hwSess_ alive while other streams remain active! */
        if (noStreamsLeft) {
            bool noCapturesLeft = false;
            {
                std::lock_guard<std::mutex> lk(capMu_);
                noCapturesLeft = captures_.empty();
            }
            if (noCapturesLeft) {
                std::lock_guard<std::mutex> hlk(hwMu_);
                closeHwSessionLocked();
            }
        }
        CAMERA_LOGI("ReleaseStreams slot=%u dropped=%zu stopped_caps=%zu remaining_streams_empty=%d",
                    slot_, streamIds.size(), toStop.size(), noStreamsLeft ? 1 : 0);
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t CommitStreams(VdiOperationMode mode, const std::vector<uint8_t> &modeSetting) override {
        (void)mode;
        if (!modeSetting.empty()) {
            ParsedCameraSettings ps;
            if (parseCameraSettingsBlob(modeSetting, ps))
                applyParsedSettings(ps);
        }
        std::lock_guard<std::mutex> lk(mu_);
        for (auto &kv : streams_)
            kv.second.committed = true;
        CAMERA_LOGI("CommitStreams slot=%u mode=%d streams=%zu",
                    slot_, static_cast<int>(mode), streams_.size());
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t GetStreamAttributes(std::vector<VdiStreamAttribute> &attributes) override {
        std::lock_guard<std::mutex> lk(mu_);
        attributes.clear();
        for (const auto &kv : streams_) {
            VdiStreamAttribute attr {};
            attr.streamId_ = kv.second.info.streamId_;
            attr.width_ = kv.second.info.width_;
            attr.height_ = kv.second.info.height_;
            attr.overrideFormat_ = kv.second.info.format_;
            attr.overrideDataspace_ = kv.second.info.dataspace_;
            attr.producerUsage_ = 0x0B; /* CPU_READ | CPU_WRITE | MEM_DMA */
            attr.producerBufferCount_ = 4;
            attr.maxBatchCaptureCount_ = 1;
            attr.maxCaptureCount_ = 1;
            attributes.push_back(attr);
        }
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t AttachBufferQueue(int32_t streamId,
                              const sptr<BufferProducerSequenceable> &bufferProducer) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = streams_.find(streamId);
        if (it == streams_.end())
            return VDI::Camera::V1_0::INVALID_ARGUMENT;
        if (!it->second.attachProducer(bufferProducer))
            return VDI::Camera::V1_0::INVALID_ARGUMENT;
        CAMERA_LOGI("AttachBufferQueue slot=%u stream=%d OK", slot_, streamId);
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t DetachBufferQueue(int32_t streamId) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = streams_.find(streamId);
        if (it != streams_.end())
            it->second.detachWindow();
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t Capture(int32_t captureId, const VdiCaptureInfo &info, bool isStreaming) override {
        if (info.streamIds_.empty())
            return VDI::Camera::V1_0::INVALID_ARGUMENT;

        if (!info.captureSetting_.empty()) {
            ParsedCameraSettings ps;
            if (parseCameraSettingsBlob(info.captureSetting_, ps))
                applyParsedSettings(ps);
        }

        auto cap = std::make_shared<ActiveCapture>();
        cap->captureId = captureId;
        cap->streamIds = info.streamIds_;
        cap->isStreaming = isStreaming;
        cap->enableShutter = info.enableShutterCallback_;

        {
            std::lock_guard<std::mutex> lk(capMu_);
            auto it = captures_.find(captureId);
            if (it != captures_.end()) {
                it->second->stop.store(true);
                hwViewCv_.notify_all();
                if (it->second->worker.joinable())
                    it->second->worker.join();
                captures_.erase(it);
            }
            captures_[captureId] = cap;
        }

        cap->worker = std::thread([this, cap]() {
            sptr<IStreamOperatorVdiCallback> cb = getCallback();
            if (cb != nullptr)
                (void)cb->OnCaptureStarted(cap->captureId, cap->streamIds);

            std::map<int32_t, int32_t> frameCounts;
            for (int32_t sid : cap->streamIds)
                frameCounts[sid] = 0;

            uint32_t seq = 0;
            int failStreak = 0;
            std::vector<uint8_t> jpegRgbScratch;
            do {
                if (cap->stop.load())
                    break;

                bool haveHwFrame = false;
                marble_raw_frame_info_t rawInfo {};
                marble_raw_view_t rawView {};
                rawView.slot = -1;
                std::string srcTag;
                uint32_t hwCountSnapshot = 0;
                {
                    std::unique_lock<std::mutex> hlk(hwMu_);
                    hwViewCv_.wait(hlk, [&]() {
                        return hwViewsHeld_ < 2 || cap->stop.load();
                    });
                    if (cap->stop.load())
                        break;
                    if (ensureHwSessionLocked()) {
                        int grc = marble_hw_session_acquire_raw_view(
                            hwSess_, 1500u, &rawInfo, &rawView);
                        if (grc == 0) {
                            haveHwFrame = true;
                            hwViewsHeld_++;
                            hwFramesOk_++;
                            hwCountSnapshot = hwFramesOk_;
                            srcTag = hwSrcName_;
                        } else {
                            CAMERA_LOGW("HW acquire_raw_view failed slot=%u rc=%d, falling back", slot_, grc);
                            srcTag = "synthetic_fallback";
                        }
                    } else {
                        srcTag = hwSrcName_;
                    }
                }

                int delivered = 0;
                size_t lastJpegBytes = 0;
                int32_t jpegRot = jpegOrientation_.load();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    for (int32_t sid : cap->streamIds) {
                        auto sit = streams_.find(sid);
                        if (sit == streams_.end())
                            continue;
                        bool isJpeg = isJpegStreamInfo(sit->second.info);
                        size_t jsz = 0;
                        if (sit->second.deliverFrame(slot_, seq, isJpeg,
                                                     cap->captureId, jpegRot,
                                                     haveHwFrame ? &rawView : nullptr,
                                                     &jpegRgbScratch, &jsz)) {
                            frameCounts[sid]++;
                            delivered++;
                            if (jsz > 0) lastJpegBytes = jsz;
                        }
                    }
                }

                if (haveHwFrame) {
                    std::lock_guard<std::mutex> hlk(hwMu_);
                    if (hwSess_ != nullptr)
                        marble_hw_session_release_raw_view(hwSess_, &rawView);
                    if (hwViewsHeld_ > 0)
                        hwViewsHeld_--;
                    hwViewCv_.notify_all();
                }

                const bool shouldLogTelemetry = haveHwFrame
                    ? (hwCountSnapshot == 1u || (hwCountSnapshot % 30u) == 0u || lastJpegBytes > 0u)
                    : (seq == 0u || (seq % 30u) == 0u || lastJpegBytes > 0u);
                if (delivered > 0 && shouldLogTelemetry) {
                    writeHwTelemetry(slot_, srcTag.c_str(), hwCountSnapshot,
                                     haveHwFrame ? &rawInfo : nullptr, lastJpegBytes);
                }
                int32_t cbRc = 0;
                cb = getCallback();
                if (cb != nullptr && (cap->enableShutter || !cap->isStreaming || seq == 0))
                    cbRc = cb->OnFrameShutter(cap->captureId, cap->streamIds, monotonicNs());
                if (delivered == 0 && cbRc != 0) {
                    if (++failStreak >= 10) {
                        CAMERA_LOGW("Capture slot=%u cap=%d stopping after %d failed cycles (client dead)",
                                    slot_, cap->captureId, failStreak);
                        break;
                    }
                } else {
                    failStreak = 0;
                }
                seq++;
                if (!cap->isStreaming)
                    break;
                if (!haveHwFrame)
                    std::this_thread::sleep_for(std::chrono::milliseconds(33));
            } while (!cap->stop.load());

            cb = getCallback();
            if (cb != nullptr) {
                std::vector<VdiCaptureEndedInfo> ended;
                for (int32_t sid : cap->streamIds) {
                    VdiCaptureEndedInfo ei {};
                    ei.streamId_ = sid;
                    ei.frameCount_ = frameCounts[sid];
                    ended.push_back(ei);
                }
                (void)cb->OnCaptureEnded(cap->captureId, ended);
            }
        });

        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t CancelCapture(int32_t captureId) override {
        std::shared_ptr<ActiveCapture> cap;
        {
            std::lock_guard<std::mutex> lk(capMu_);
            auto it = captures_.find(captureId);
            if (it == captures_.end())
                return VDI::Camera::V1_0::NO_ERROR;
            cap = it->second;
            captures_.erase(it);
        }
        if (cap != nullptr) {
            cap->stop.store(true);
            hwViewCv_.notify_all();
            if (cap->worker.joinable())
                cap->worker.join();
        }
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t ChangeToOfflineStream(const std::vector<int32_t> &streamIds,
                                  const sptr<IStreamOperatorVdiCallback> &callbackObj,
                                  sptr<IOfflineStreamOperatorVdi> &offlineOperator) override {
        (void)streamIds;
        (void)callbackObj;
        offlineOperator = nullptr;
        return VDI::Camera::V1_0::METHOD_NOT_SUPPORTED;
    }

    /* Non-virtual helpers (preserve exact 256B VDI vtable ABI!). */
    void setCallback(const sptr<IStreamOperatorVdiCallback> &cb) {
        std::lock_guard<std::mutex> lk(cbMu_);
        callback_ = cb;
    }

    sptr<IStreamOperatorVdiCallback> getCallback() {
        std::lock_guard<std::mutex> lk(cbMu_);
        return callback_;
    }

    void applyParsedSettings(const ParsedCameraSettings &ps) {
        if (ps.hasJpegOrientation)
            jpegOrientation_.store(ps.jpegOrientation);

        if (ps.hasFlashMode) {
            if (ps.flashMode == OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN) {
                (void)marble_cam::globalTorchCtrl().setLevel(1.0f);
            } else if (ps.flashMode == OHOS_CAMERA_FLASH_MODE_CLOSE) {
                (void)marble_cam::globalTorchCtrl().setLevel(0.0f);
            }
        }

        if (ps.hasFocusMode || ps.hasFocusDist || ps.hasAeEv) {
            std::lock_guard<std::mutex> hlk(hwMu_);
            if (hwSess_ != nullptr) {
                if (ps.hasFocusMode || ps.hasFocusDist) {
                    bool isManual = (ps.hasFocusMode && ps.focusMode == OHOS_CAMERA_FOCUS_MODE_MANUAL) ||
                                    (ps.hasFocusDist && !ps.hasFocusMode);
                    if (isManual) {
                        uint16_t dac = ps.hasFocusDist
                            ? marble_cam::focusDistanceToDac(ps.focusDist) : 370u;
                        (void)marble_hw_session_set_focus(hwSess_, dac);
                    } else if (ps.hasFocusMode) {
                        int trigger = (ps.focusMode == OHOS_CAMERA_FOCUS_MODE_AUTO) ? 1 : 0;
                        (void)marble_hw_session_set_af_mode(hwSess_, 1, trigger);
                    }
                }
                if (ps.hasAeEv) {
                    uint32_t expLines = 2000u, gainQ8 = 256u;
                    marble_cam::computeEvExposure(ps.aeEv, expLines, gainQ8);
                    (void)marble_hw_session_set_exposure(hwSess_, expLines, gainQ8);
                }
            }
        }
    }

    void stopAllCaptures() {
        std::map<int32_t, std::shared_ptr<ActiveCapture>> local;
        {
            std::lock_guard<std::mutex> lk(capMu_);
            local.swap(captures_);
        }
        for (auto &kv : local) {
            kv.second->stop.store(true);
            hwViewCv_.notify_all();
            if (kv.second->worker.joinable())
                kv.second->worker.join();
        }
        std::lock_guard<std::mutex> hlk(hwMu_);
        closeHwSessionLocked();
    }

private:
    bool ensureHwSessionLocked() {
        if (hwSess_ != nullptr)
            return true;
        if (hwOpenAttempted_)
            return false;
        hwOpenAttempted_ = true;

        marble_hw_config_t cfg {};
        switch (slot_) {
            case 0u:
                cfg.src = MARBLE_HW_SRC_OV64B40;
                cfg.width = 4624u;
                cfg.height = 3472u;
                hwSrcName_ = "ov64b40_optical";
                break;
            case 1u:
                cfg.src = MARBLE_HW_SRC_S5K3P9;
                cfg.width = 2304u;
                cfg.height = 1728u;
                hwSrcName_ = "s5k3p9_optical";
                break;
            case 2u:
                cfg.src = MARBLE_HW_SRC_IMX355;
                cfg.width = 1640u;
                cfg.height = 1232u;
                hwSrcName_ = "imx355_optical";
                break;
            case 3u:
                cfg.src = MARBLE_HW_SRC_GC02M1;
                cfg.width = 1600u;
                cfg.height = 1200u;
                hwSrcName_ = "gc02m1_optical";
                break;
            default:
                cfg.src = MARBLE_HW_SRC_TPG;
                cfg.width = 640u;
                cfg.height = 480u;
                hwSrcName_ = "sm7475_tpg";
                break;
        }
        cfg.test_pattern = 0;

        char err[160] = {0};
        int rc = marble_hw_session_open(&cfg, &hwSess_, err, sizeof(err));
        if (rc == 0 && hwSess_ != nullptr) {
            CAMERA_LOGI("HW_SESSION_OPEN slot=%u src=%s ok=1", slot_, hwSrcName_.c_str());
            return true;
        }
        CAMERA_LOGW("HW_SESSION_OPEN slot=%u src=%s failed (%s), retrying SM7475 TPG0",
                    slot_, hwSrcName_.c_str(), err);

        if (cfg.src != MARBLE_HW_SRC_TPG) {
            marble_hw_config_t tpgCfg {};
            tpgCfg.src = MARBLE_HW_SRC_TPG;
            tpgCfg.width = 640u;
            tpgCfg.height = 480u;
            rc = marble_hw_session_open(&tpgCfg, &hwSess_, err, sizeof(err));
            if (rc == 0 && hwSess_ != nullptr) {
                hwSrcName_ = "sm7475_tpg";
                CAMERA_LOGI("HW_SESSION_OPEN slot=%u src=sm7475_tpg fallback_ok=1", slot_);
                return true;
            }
        }
        CAMERA_LOGW("HW_SESSION_OPEN slot=%u fallback=synthetic err=%s", slot_, err);
        hwSess_ = nullptr;
        hwSrcName_ = "synthetic_fallback";
        return false;
    }

    void closeHwSessionLocked() {
        if (hwSess_ != nullptr) {
            CAMERA_LOGI("HW_SESSION_CLOSE slot=%u src=%s hw_frames=%u",
                        slot_, hwSrcName_.c_str(), hwFramesOk_);
            marble_hw_session_close(hwSess_);
            hwSess_ = nullptr;
        }
        hwOpenAttempted_ = false;
        hwFramesOk_ = 0;
        hwViewsHeld_ = 0;
    }

    static void writeHwTelemetry(uint32_t slot, const char *src, uint32_t hwFrames,
                                 const marble_raw_frame_info_t *ri, size_t jpegBytes) {
        char buf[320];
        int n = std::snprintf(
            buf, sizeof(buf),
            "slot=%u src=%s hw_frames=%u req_id=%u raw10_min=%u raw10_max=%u raw10_mean=%u "
            "csum=0x%08x jpeg_bytes=%zu ae_exp=%u ae_gain=%u ae_upd=%u act_ok=%u focus_dac=%u sharp=%u\n",
            slot, src ? src : "unknown", hwFrames,
            ri ? ri->req_id : 0u,
            ri ? static_cast<unsigned>(ri->raw10_min) : 0u,
            ri ? static_cast<unsigned>(ri->raw10_max) : 0u,
            ri ? ri->raw10_mean : 0u,
            ri ? ri->csum : 0u,
            jpegBytes,
            ri ? ri->ae_exp_lines : 0u,
            ri ? ri->ae_gain_q8 : 0u,
            ri ? ri->ae_updates : 0u,
            ri ? static_cast<unsigned>(ri->actuator_ok) : 0u,
            ri ? static_cast<unsigned>(ri->focus_dac) : 0u,
            ri ? ri->sharpness : 0u);
        if (n <= 0)
            return;
        const char *tmpPath = "/data/vendor/camera/last_capture_status.tmp";
        const char *dstPath = "/data/vendor/camera/last_capture_status.txt";
        int fd = ::open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            (void)::write(fd, buf, static_cast<size_t>(n));
            ::close(fd);
            (void)::rename(tmpPath, dstPath);
        }
    }

    uint32_t slot_;
    std::mutex cbMu_;
    sptr<IStreamOperatorVdiCallback> callback_;
    std::mutex mu_;
    std::map<int32_t, ManagedStream> streams_;
    std::mutex capMu_;
    std::map<int32_t, std::shared_ptr<ActiveCapture>> captures_;
    std::mutex hwMu_;
    std::condition_variable hwViewCv_;
    int hwViewsHeld_ = 0;
    marble_hw_session_t *hwSess_ = nullptr;
    bool hwOpenAttempted_ = false;
    std::string hwSrcName_ = "none";
    uint32_t hwFramesOk_ = 0;
    std::atomic<int32_t> jpegOrientation_ {0};
};

class MarbleCameraDeviceVdi : public ICameraDeviceVdi {
public:
    MarbleCameraDeviceVdi(const std::string &cameraId, uint32_t slot,
                          const sptr<ICameraDeviceVdiCallback> &cb)
        : cameraId_(cameraId), slot_(slot), callback_(cb), opened_(true) {}

    ~MarbleCameraDeviceVdi() override {
        (void)Close();
    }

    int32_t GetStreamOperator(const sptr<IStreamOperatorVdiCallback> &callbackObj,
                              sptr<IStreamOperatorVdi> &streamOperator) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_)
            return VDI::Camera::V1_0::CAMERA_CLOSED;
        if (streamOp_ == nullptr) {
            streamOp_ = new MarbleStreamOperatorVdi(slot_, callbackObj);
        } else if (callbackObj != nullptr) {
            streamOp_->setCallback(callbackObj);
        }
        streamOperator = streamOp_;
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t UpdateSettings(const std::vector<uint8_t> &settings) override {
        sptr<MarbleStreamOperatorVdi> op;
        sptr<ICameraDeviceVdiCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!opened_)
                return VDI::Camera::V1_0::CAMERA_CLOSED;
            op = streamOp_;
            cb = callback_;
        }
        if (!settings.empty()) {
            ParsedCameraSettings ps;
            if (parseCameraSettingsBlob(settings, ps)) {
                if (op != nullptr) {
                    op->applyParsedSettings(ps);
                } else if (ps.hasFlashMode) {
                    if (ps.flashMode == OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN)
                        (void)marble_cam::globalTorchCtrl().setLevel(1.0f);
                    else if (ps.flashMode == OHOS_CAMERA_FLASH_MODE_CLOSE)
                        (void)marble_cam::globalTorchCtrl().setLevel(0.0f);
                }
            }
            if (cb != nullptr)
                (void)cb->OnResult(monotonicNs(), settings);
        }
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t SetResultMode(VdiResultCallbackMode mode) override {
        resultMode_ = mode;
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t GetEnabledResults(std::vector<int32_t> &results) override {
        std::lock_guard<std::mutex> lk(mu_);
        results = enabledResults_;
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t EnableResult(const std::vector<int32_t> &results) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (int32_t r : results) {
            bool found = false;
            for (int32_t e : enabledResults_)
                if (e == r) found = true;
            if (!found)
                enabledResults_.push_back(r);
        }
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t DisableResult(const std::vector<int32_t> &results) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<int32_t> next;
        for (int32_t e : enabledResults_) {
            bool drop = false;
            for (int32_t r : results)
                if (r == e) drop = true;
            if (!drop)
                next.push_back(e);
        }
        enabledResults_.swap(next);
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t Close() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!opened_)
            return VDI::Camera::V1_0::NO_ERROR;
        opened_ = false;
        if (streamOp_ != nullptr) {
            streamOp_->stopAllCaptures();
            streamOp_ = nullptr;
        }
        callback_ = nullptr;
        CAMERA_LOGI("Close camera %s (slot=%u)", cameraId_.c_str(), slot_);
        return VDI::Camera::V1_0::NO_ERROR;
    }

private:
    std::string cameraId_;
    uint32_t slot_;
    sptr<ICameraDeviceVdiCallback> callback_;
    sptr<MarbleStreamOperatorVdi> streamOp_;
    std::mutex mu_;
    VdiResultCallbackMode resultMode_ = VDI::Camera::V1_0::PER_FRAME;
    std::vector<int32_t> enabledResults_;
    bool opened_ = false;
};

static bool buildAbilityVec(const marble_cam::PhysicalCameraDesc &desc,
                            std::vector<uint8_t> &outVec) {
    auto meta = std::make_shared<CameraMetadata>(96, 8192);
    if (meta == nullptr || !meta->isValid())
        return false;

    uint8_t connType = OHOS_CAMERA_CONNECTION_TYPE_BUILTIN;
    uint8_t flashAvail = desc.hasFlash ? OHOS_CAMERA_FLASH_TRUE : OHOS_CAMERA_FLASH_FALSE;
    uint8_t flashAdjustAvail = desc.hasFlash ? 1u : 0u;
    uint8_t flashModes[] = {OHOS_CAMERA_FLASH_MODE_CLOSE, OHOS_CAMERA_FLASH_MODE_OPEN,
                            OHOS_CAMERA_FLASH_MODE_AUTO, OHOS_CAMERA_FLASH_MODE_ALWAYS_OPEN};
    uint8_t muteModes[] = {OHOS_CAMERA_MUTE_MODE_OFF, OHOS_CAMERA_MUTE_MODE_SOLID_COLOR_BLACK};
    uint8_t cameraModes[] = {0, 1, 2}; /* NORMAL, CAPTURE, VIDEO */
    uint8_t aeModes[] = {OHOS_CAMERA_AE_MODE_OFF, OHOS_CAMERA_AE_MODE_ON};
    uint8_t abModes[] = {OHOS_CAMERA_AE_ANTIBANDING_MODE_OFF, OHOS_CAMERA_AE_ANTIBANDING_MODE_AUTO};
    uint8_t awbModes[] = {OHOS_CAMERA_AWB_MODE_OFF, OHOS_CAMERA_AWB_MODE_AUTO};
    uint8_t exposureModes[] = {OHOS_CAMERA_EXPOSURE_MODE_MANUAL,
                               OHOS_CAMERA_EXPOSURE_MODE_CONTINUOUS_AUTO,
                               OHOS_CAMERA_EXPOSURE_MODE_LOCKED,
                               OHOS_CAMERA_EXPOSURE_MODE_AUTO};
    uint8_t meterModes[] = {OHOS_CAMERA_SPOT_METERING,
                            OHOS_CAMERA_REGION_METERING,
                            OHOS_CAMERA_OVERALL_METERING,
                            OHOS_CAMERA_CENTER_WEIGHTED_METERING};
    uint8_t aeLockModes[] = {OHOS_CAMERA_AE_LOCK_OFF, OHOS_CAMERA_AE_LOCK_ON};
    uint8_t awbLockAvail = 1u;
    uint8_t focusModes[] = {OHOS_CAMERA_FOCUS_MODE_MANUAL,
                            OHOS_CAMERA_FOCUS_MODE_CONTINUOUS_AUTO,
                            OHOS_CAMERA_FOCUS_MODE_AUTO};
    uint8_t stabModes[] = {OHOS_CAMERA_VIDEO_STABILIZATION_OFF};
    uint8_t prelaunch = 0;
    uint8_t mirrorSupp = OHOS_CAMERA_MIRROR_OFF;
    uint8_t jpegQuality = OHOS_CAMERA_JPEG_LEVEL_HIGH;
    int32_t jpegOrientation = 0;
    int32_t jpegMaxSize = desc.activeW * desc.activeH;
    int32_t jpegThumbSizes[] = {0, 0, 160, 120, 320, 240};
    int32_t sensitivityRange[] = {100, 3200};
    int32_t orientVariable = 0;
    int32_t fpsRanges[] = {15, 30, 30, 30};
    int32_t activeArr[] = {0, 0, desc.activeW, desc.activeH};
    int32_t pixelArr[] = {desc.activeW, desc.activeH};
    float physSize[] = {5.12f, 3.84f};
    float zoomRange[] = {1.0f, 4.0f};
    int32_t aeCompRange[] = {-4, 4};
    camera_rational_t aeCompStep = {1, 2};
    int32_t orient = desc.orientation;

    /* Basic stream configurations: [format, width, height] triples (4:3 + 16:9). */
    const int32_t basicCfg[] = {
        OHOS_CAMERA_FORMAT_YCRCB_420_SP,  640,  480,
        OHOS_CAMERA_FORMAT_YCRCB_420_SP, 1280,  720,
        OHOS_CAMERA_FORMAT_YCRCB_420_SP, 1440, 1080,
        OHOS_CAMERA_FORMAT_YCRCB_420_SP, 1920, 1080,
        OHOS_CAMERA_FORMAT_YCBCR_420_SP,  640,  480,
        OHOS_CAMERA_FORMAT_YCBCR_420_SP, 1280,  720,
        OHOS_CAMERA_FORMAT_YCBCR_420_SP, 1440, 1080,
        OHOS_CAMERA_FORMAT_YCBCR_420_SP, 1920, 1080,
        OHOS_CAMERA_FORMAT_RGBA_8888,     640,  480,
        OHOS_CAMERA_FORMAT_RGBA_8888,    1280,  720,
        OHOS_CAMERA_FORMAT_JPEG,          640,  480,
        OHOS_CAMERA_FORMAT_JPEG,         1280,  720,
        OHOS_CAMERA_FORMAT_JPEG,         1440, 1080,
        OHOS_CAMERA_FORMAT_JPEG,         1920, 1080,
    };

    std::vector<int32_t> extendCfg = marble_cam::buildExtendStreamConfig();

    (void)meta->addEntry(OHOS_ABILITY_CAMERA_POSITION, &desc.position, 1);
    (void)meta->addEntry(OHOS_ABILITY_CAMERA_TYPE, &desc.cameraType, 1);
    (void)meta->addEntry(OHOS_ABILITY_CAMERA_CONNECTION_TYPE, &connType, 1);
    (void)meta->addEntry(OHOS_SENSOR_ORIENTATION, &orient, 1);
    (void)meta->addEntry(OHOS_ABILITY_SENSOR_ORIENTATION_VARIABLE, &orientVariable, 1);
    (void)meta->addEntry(OHOS_SENSOR_INFO_ACTIVE_ARRAY_SIZE, activeArr, 4);
    (void)meta->addEntry(OHOS_SENSOR_INFO_PIXEL_ARRAY_SIZE, pixelArr, 2);
    (void)meta->addEntry(OHOS_SENSOR_INFO_PHYSICAL_SIZE, physSize, 2);
    (void)meta->addEntry(OHOS_SENSOR_INFO_SENSITIVITY_RANGE, sensitivityRange, 2);
    (void)meta->addEntry(OHOS_ABILITY_FOCAL_LENGTH, &desc.focalLength, 1);
    (void)meta->addEntry(OHOS_ABILITY_FPS_RANGES, fpsRanges, 4);
    (void)meta->addEntry(OHOS_ABILITY_FLASH_AVAILABLE, &flashAvail, 1);
    (void)meta->addEntry(kOhosAbilityFlashAdjustAvailable, &flashAdjustAvail, 1);
    if (desc.hasFlash)
        (void)meta->addEntry(OHOS_ABILITY_FLASH_MODES, flashModes, sizeof(flashModes));
    (void)meta->addEntry(OHOS_ABILITY_MUTE_MODES, muteModes, sizeof(muteModes));
    (void)meta->addEntry(OHOS_ABILITY_CAMERA_MODES, cameraModes, sizeof(cameraModes));
    (void)meta->addEntry(OHOS_ABILITY_ZOOM_RATIO_RANGE, zoomRange, 2);
    (void)meta->addEntry(OHOS_ABILITY_PRELAUNCH_AVAILABLE, &prelaunch, 1);
    (void)meta->addEntry(OHOS_CONTROL_CAPTURE_MIRROR_SUPPORTED, &mirrorSupp, 1);
    (void)meta->addEntry(OHOS_CONTROL_AE_AVAILABLE_MODES, aeModes, sizeof(aeModes));
    (void)meta->addEntry(OHOS_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES, abModes, sizeof(abModes));
    (void)meta->addEntry(OHOS_CONTROL_AWB_AVAILABLE_MODES, awbModes, sizeof(awbModes));
    (void)meta->addEntry(OHOS_ABILITY_EXPOSURE_MODES, exposureModes, sizeof(exposureModes));
    (void)meta->addEntry(OHOS_ABILITY_METER_MODES, meterModes, sizeof(meterModes));
    (void)meta->addEntry(OHOS_ABILITY_AE_LOCK, aeLockModes, sizeof(aeLockModes));
    (void)meta->addEntry(OHOS_ABILITY_AWB_LOCK, &awbLockAvail, 1);
    (void)meta->addEntry(OHOS_CONTROL_AE_COMPENSATION_RANGE, aeCompRange, 2);
    (void)meta->addEntry(OHOS_CONTROL_AE_COMPENSATION_STEP, &aeCompStep, 1);
    (void)meta->addEntry(OHOS_ABILITY_AE_COMPENSATION_RANGE, aeCompRange, 2);
    (void)meta->addEntry(OHOS_ABILITY_AE_COMPENSATION_STEP, &aeCompStep, 1);
    (void)meta->addEntry(OHOS_ABILITY_FOCUS_MODES, focusModes, sizeof(focusModes));
    (void)meta->addEntry(OHOS_ABILITY_VIDEO_STABILIZATION_MODES, stabModes, sizeof(stabModes));
    (void)meta->addEntry(OHOS_JPEG_ORIENTATION, &jpegOrientation, 1);
    (void)meta->addEntry(OHOS_JPEG_QUALITY, &jpegQuality, 1);
    (void)meta->addEntry(OHOS_JPEG_MAX_SIZE, &jpegMaxSize, 1);
    (void)meta->addEntry(OHOS_JPEG_AVAILABLE_THUMBNAIL_SIZES,
                         jpegThumbSizes, sizeof(jpegThumbSizes) / sizeof(jpegThumbSizes[0]));
    uint8_t detectTypes[] = {OHOS_CAMERA_HUMAN_FACE_DETECT};
    (void)meta->addEntry(OHOS_ABILITY_STATISTICS_DETECT_TYPE, detectTypes, sizeof(detectTypes));
    (void)meta->addEntry(OHOS_ABILITY_STREAM_AVAILABLE_BASIC_CONFIGURATIONS,
                         basicCfg, sizeof(basicCfg) / sizeof(basicCfg[0]));
    (void)meta->addEntry(OHOS_ABILITY_STREAM_AVAILABLE_EXTEND_CONFIGURATIONS,
                         extendCfg.data(), extendCfg.size());

    outVec.clear();
    return MetadataUtils::ConvertMetadataToVec(meta, outVec);
}

class MarbleCameraHostVdi : public ICameraHostVdi {
public:
    MarbleCameraHostVdi() = default;
    ~MarbleCameraHostVdi() override {
        (void)CloseAllCameras();
    }

    void Init() {
        std::lock_guard<std::mutex> lk(mu_);
        cameras_.clear();
        for (size_t i = 0; i < 4; i++) {
            marble_cam::PhysicalCameraDesc d = marble_cam::kMarbleSensors[i];
            d.hwPresent = querySm7475SensorSlotPresent(d.slot);
            if (d.hwPresent) {
                cameras_.push_back(d);
                CAMERA_LOGI("Discovered SM7475 camera %s slot=%u sensor=%s pos=%u orient=%d",
                            d.id, d.slot, d.sensorModel, d.position, d.orientation);
            }
        }
        CAMERA_LOGI("MarbleCameraHostVdi Init complete: hw_cameras=%zu", cameras_.size());
    }

    int32_t SetCallback(const sptr<ICameraHostVdiCallback> &callbackObj) override {
        std::lock_guard<std::mutex> lk(mu_);
        hostCallback_ = callbackObj;
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t GetCameraIds(std::vector<std::string> &cameraIds) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (cameras_.empty()) {
            for (size_t i = 0; i < 4; i++) {
                marble_cam::PhysicalCameraDesc d = marble_cam::kMarbleSensors[i];
                d.hwPresent = querySm7475SensorSlotPresent(d.slot);
                if (d.hwPresent)
                    cameras_.push_back(d);
            }
        }
        cameraIds.clear();
        for (const auto &c : cameras_)
            cameraIds.push_back(c.id);
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t GetCameraAbility(const std::string &cameraId,
                             std::vector<uint8_t> &cameraAbility) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &c : cameras_) {
            if (cameraId == c.id) {
                if (!buildAbilityVec(c, cameraAbility))
                    return VDI::Camera::V1_0::INSUFFICIENT_RESOURCES;
                return VDI::Camera::V1_0::NO_ERROR;
            }
        }
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }

    int32_t OpenCamera(const std::string &cameraId,
                       const sptr<ICameraDeviceVdiCallback> &callbackObj,
                       sptr<ICameraDeviceVdi> &device) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto &c : cameras_) {
            if (cameraId == c.id) {
                if (marble_cam::globalTorchCtrl().isOn())
                    (void)marble_cam::globalTorchCtrl().setLevel(0.0f);
                auto prev = openDevices_.find(cameraId);
                if (prev != openDevices_.end() && prev->second != nullptr) {
                    (void)prev->second->Close();
                    openDevices_.erase(prev);
                }
                sptr<MarbleCameraDeviceVdi> dev = new MarbleCameraDeviceVdi(cameraId, c.slot, callbackObj);
                openDevices_[cameraId] = dev;
                device = dev;
                CAMERA_LOGI("OpenCamera %s (slot=%u, %s) OK", cameraId.c_str(), c.slot, c.sensorModel);
                return VDI::Camera::V1_0::NO_ERROR;
            }
        }
        return VDI::Camera::V1_0::INVALID_ARGUMENT;
    }

    int32_t SetFlashlight(const std::string &cameraId, bool isEnable) override {
        sptr<ICameraHostVdiCallback> cb;
        {
            std::lock_guard<std::mutex> lk(mu_);
            bool found = false;
            for (const auto &c : cameras_) {
                if (cameraId == c.id) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return VDI::Camera::V1_0::INVALID_ARGUMENT;
            cb = hostCallback_;
        }
        auto res = marble_cam::globalTorchCtrl().setLevel(isEnable ? 1.0f : 0.0f);
        if (cb != nullptr) {
            (void)cb->OnFlashlightStatus(
                cameraId, (isEnable && res.on) ? VDI::Camera::V1_0::FLASHLIGHT_ON
                                               : VDI::Camera::V1_0::FLASHLIGHT_OFF);
        }
        return VDI::Camera::V1_0::NO_ERROR;
    }

    int32_t CloseAllCameras() override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto &kv : openDevices_) {
            if (kv.second != nullptr)
                (void)kv.second->Close();
        }
        openDevices_.clear();
        if (marble_cam::globalTorchCtrl().isOn())
            (void)marble_cam::globalTorchCtrl().setLevel(0.0f);
        return VDI::Camera::V1_0::NO_ERROR;
    }

private:
    std::mutex mu_;
    sptr<ICameraHostVdiCallback> hostCallback_;
    std::vector<marble_cam::PhysicalCameraDesc> cameras_;
    std::map<std::string, sptr<MarbleCameraDeviceVdi>> openDevices_;
};

static int CreateMarbleCameraHostVdi(struct HdfVdiBase *vdiBase) {
    auto *wrapper = reinterpret_cast<struct VdiWrapperCameraHost *>(vdiBase);
    auto *impl = new (std::nothrow) MarbleCameraHostVdi();
    if (impl == nullptr)
        return HDF_FAILURE;
    impl->Init();
    wrapper->module = impl;
    return HDF_SUCCESS;
}

static int DestroyMarbleCameraHostVdi(struct HdfVdiBase *vdiBase) {
    auto *wrapper = reinterpret_cast<struct VdiWrapperCameraHost *>(vdiBase);
    auto *impl = reinterpret_cast<MarbleCameraHostVdi *>(wrapper->module);
    delete impl;
    wrapper->module = nullptr;
    return HDF_SUCCESS;
}

static struct VdiWrapperCameraHost g_marbleVdiCameraHost = {
    .base = {
        .moduleVersion = 1,
        .moduleName = "MarbleCameraHostVdi",
        .CreateVdiInstance = CreateMarbleCameraHostVdi,
        .DestoryVdiInstance = DestroyMarbleCameraHostVdi,
    },
    .module = nullptr,
};

} // namespace
} // namespace OHOS::Camera

extern "C" {
HDF_VDI_INIT(OHOS::Camera::g_marbleVdiCameraHost);
}

#endif /* MARBLE_CAMERA_VDI_HOST_TEST */
