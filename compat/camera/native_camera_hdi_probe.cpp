/* SPDX-License-Identifier: MIT */
/*
 * native_camera_hdi_probe.cpp - Clean-room OHOS Camera HDI 1.0 + SA 3008 end-to-end probe.
 *
 * Exercises real OHOS Binder IPC against unmodified vendor libcamera_host_service_1.0.z.so
 * (running in camera_host host-9) backed by libmarble_camera_vdi_impl.z.so, plus verifies
 * SA 3008 (camera_service) registration in SAMGR.
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

bool isValidBaselineJpeg(const uint8_t *buf, size_t len, size_t *outLen) {
    if (buf == nullptr || len < 64)
        return false;
    if (buf[0] != 0xFF || buf[1] != 0xD8)
        return false;
    bool hasSof0 = false;
    bool hasSos = false;
    for (size_t i = 2; i + 1 < len; ++i) {
        if (buf[i] == 0xFF && buf[i + 1] == 0xC0)
            hasSof0 = true;
        if (buf[i] == 0xFF && buf[i + 1] == 0xDA)
            hasSos = true;
        if (hasSos && buf[i] == 0xFF && buf[i + 1] == 0xD9) {
            if (outLen != nullptr)
                *outLen = i + 2;
            return hasSof0;
        }
    }
    return false;
}

bool hasNonZeroBytes(const uint8_t *buf, size_t len) {
    if (buf == nullptr || len == 0)
        return false;
    size_t n = len < 65536 ? len : 65536;
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] != 0)
            return true;
    }
    return false;
}

} // namespace

#ifdef MARBLE_CAMERA_HDI_PROBE_HOST_TEST
int main() {
    uint8_t fakeJpeg[80] = {};
    fakeJpeg[0] = 0xFF; fakeJpeg[1] = 0xD8;
    fakeJpeg[10] = 0xFF; fakeJpeg[11] = 0xC0;
    fakeJpeg[24] = 0xFF; fakeJpeg[25] = 0xDA;
    fakeJpeg[70] = 0xFF; fakeJpeg[71] = 0xD9;
    size_t jlen = 0;
    if (!isValidBaselineJpeg(fakeJpeg, sizeof(fakeJpeg), &jlen) || jlen != 72) {
        std::fprintf(stderr, "JPEG validator self-test failed\n");
        return 1;
    }
    if (!hasNonZeroBytes(fakeJpeg, sizeof(fakeJpeg))) {
        std::fprintf(stderr, "NonZero self-test failed\n");
        return 1;
    }
    std::printf("MARBLE_CAMERA_HDI_PROBE_SELF_TEST_OK=1 jpeg_len=%zu\n", jlen);
    return 0;
}
#else

#include <dlfcn.h>
#include <pthread.h>
#include <iremote_object.h>
#include <ipc_skeleton.h>
#include <message_option.h>
#include <message_parcel.h>
#include <iservmgr_hdi.h>
#include "camera_metadata_info.h"
#include "metadata_utils.h"
#include <native_image/native_image.h>
#include <native_window/external_window.h>

extern "C" {
OH_NativeImage *OH_ConsumerSurface_Create(void);
int32_t OH_ConsumerSurface_SetDefaultSize(OH_NativeImage *image, int32_t width, int32_t height);
int32_t OH_ConsumerSurface_SetDefaultUsage(OH_NativeImage *image, uint64_t usage);
int32_t OH_NativeImage_AcquireNativeWindowBuffer(OH_NativeImage *image,
                                                 OHNativeWindowBuffer **nativeWindowBuffer,
                                                 int *fenceFd);
int32_t OH_NativeImage_ReleaseNativeWindowBuffer(OH_NativeImage *image,
                                                 OHNativeWindowBuffer *nativeWindowBuffer,
                                                 int fenceFd);
}

namespace {

using OHOS::IPCSkeleton;
using OHOS::IRemoteObject;
using OHOS::MessageOption;
using OHOS::MessageParcel;
using OHOS::sptr;

static const std::u16string kSamgrToken = u"ohos.samgr.accessToken";
static const std::u16string kHostToken = u"ohos.hdi.camera.v1_0.ICameraHost";
static const std::u16string kHostTokenV12 = u"ohos.hdi.camera.v1_2.ICameraHost";
static const std::u16string kDeviceToken = u"ohos.hdi.camera.v1_0.ICameraDevice";
static const std::u16string kStreamOpToken = u"ohos.hdi.camera.v1_0.IStreamOperator";
static const std::u16string kDeviceCbToken = u"ohos.hdi.camera.v1_0.ICameraDeviceCallback";
static const std::u16string kStreamCbToken = u"ohos.hdi.camera.v1_0.IStreamOperatorCallback";

struct OhIpcParcelBridge {
    MessageParcel *msgParcel;
};

using StubCtor = void (*)(void *, std::u16string, bool);
using StubDispatch = int (*)(void *, uint32_t, MessageParcel &, MessageParcel &, MessageOption &);
using SetThreadsFn = bool (*)(int);
using JoinThreadFn = void (*)();

static JoinThreadFn gJoinThread = nullptr;
static std::atomic<int> gDevErrors {0};
static std::atomic<int> gDevResults {0};
static std::atomic<int> gStreamStarted {0};
static std::atomic<int> gStreamEnded {0};
static std::atomic<int> gStreamErrors {0};
static std::atomic<int> gStreamShutter {0};

static int DeviceCallbackDispatch(void *, uint32_t code, MessageParcel &data,
                                  MessageParcel &reply, MessageOption &option) {
    (void)reply;
    (void)option;
    std::u16string tok = data.ReadInterfaceToken();
    if (tok != kDeviceCbToken)
        return -1;
    if (code == 1)
        gDevErrors.fetch_add(1);
    else if (code == 2)
        gDevResults.fetch_add(1);
    return 0;
}

static int StreamCallbackDispatch(void *, uint32_t code, MessageParcel &data,
                                  MessageParcel &reply, MessageOption &option) {
    (void)reply;
    (void)option;
    std::u16string tok = data.ReadInterfaceToken();
    if (tok != kStreamCbToken)
        return -1;
    if (code == 1)
        gStreamStarted.fetch_add(1);
    else if (code == 2)
        gStreamEnded.fetch_add(1);
    else if (code == 3)
        gStreamErrors.fetch_add(1);
    else if (code == 4)
        gStreamShutter.fetch_add(1);
    return 0;
}

/* Donor libipc_single.z.so IPCObjectStub has OnRemoteRequest at vtable byte offset 200 (+0xc8). */
static void *MakeDonorCallbackStub(StubCtor ctor, StubDispatch original,
                                   const std::u16string &descriptor,
                                   StubDispatch replacement, uintptr_t *privateTable) {
    void *object = ::operator new(4096);
    std::memset(object, 0, 4096);
    ctor(object, descriptor, false);
    void **table = *reinterpret_cast<void ***>(object);
    if (table == nullptr || table[200 / 8] != reinterpret_cast<void *>(original)) {
        std::fprintf(stderr, "PROBE_VTABLE_GUARD_FAILED\n");
        return nullptr;
    }
    std::memcpy(privateTable, reinterpret_cast<const char *>(table) - 4 * sizeof(void *),
                128 * sizeof(uintptr_t));
    privateTable[4 + 200 / 8] = reinterpret_cast<uintptr_t>(replacement);
    *reinterpret_cast<void ***>(object) = reinterpret_cast<void **>(privateTable + 4);
    return object;
}

bool checkSa3008Registered() {
    sptr<IRemoteObject> samgr = IPCSkeleton::GetContextObject();
    if (samgr == nullptr)
        return false;
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kSamgrToken))
        return false;
    if (!data.WriteInt32(3008))
        return false;
    int rc = samgr->SendRequest(2 /* CHECK_SYSTEM_ABILITY_TRANSACTION */, data, reply, opt);
    if (rc != 0)
        return false;
    sptr<IRemoteObject> sa = reply.ReadRemoteObject();
    return sa != nullptr;
}

bool callGetHostVersion(const sptr<IRemoteObject> &host, uint32_t &majorVer, uint32_t &minorVer) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kHostToken))
        return false;
    int rc = host->SendRequest(0 /* CMD_CAMERA_HOST_GET_VERSION */, data, reply, opt);
    if (rc != 0)
        return false;
    return reply.ReadUint32(majorVer) && reply.ReadUint32(minorVer);
}

bool callSetFlashlightV12(const sptr<IRemoteObject> &host, float level) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kHostTokenV12) || !data.WriteFloat(level))
        return false;
    int rc = host->SendRequest(11 /* CMD_CAMERA_HOST_SET_FLASHLIGHT_V1_2 */, data, reply, opt);
    return rc == 0;
}

bool callGetCameraIds(const sptr<IRemoteObject> &host, std::vector<std::string> &outIds) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kHostToken) || !data.WriteBool(false))
        return false;
    int rc = host->SendRequest(2 /* CMD_CAMERA_HOST_GET_CAMERA_IDS */, data, reply, opt);
    if (rc != 0) {
        std::printf("GET_CAMERA_IDS_ERR rc=%d\n", rc);
        return false;
    }
    uint32_t count = 0;
    if (!reply.ReadUint32(count) || count > 32) {
        std::printf("GET_CAMERA_IDS_BAD_COUNT count=%u\n", count);
        return false;
    }
    outIds.clear();
    for (uint32_t i = 0; i < count; ++i) {
        const char *s = reply.ReadCString();
        outIds.emplace_back(s != nullptr ? s : "");
    }
    return true;
}

bool callGetCameraAbility(const sptr<IRemoteObject> &host, const std::string &camId,
                          std::vector<uint8_t> &outBlob) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kHostToken) || !data.WriteBool(false) ||
        !data.WriteCString(camId.c_str()))
        return false;
    int rc = host->SendRequest(3 /* CMD_CAMERA_HOST_GET_CAMERA_ABILITY */, data, reply, opt);
    if (rc != 0) {
        std::printf("GET_ABILITY_ERR id=%s rc=%d\n", camId.c_str(), rc);
        return false;
    }
    uint32_t sz = 0;
    if (!reply.ReadUint32(sz) || sz == 0 || sz > 4 * 1024 * 1024) {
        std::printf("GET_ABILITY_BAD_SZ id=%s sz=%u\n", camId.c_str(), sz);
        return false;
    }
    const uint8_t *raw = reply.ReadUnpadBuffer(sz);
    if (raw == nullptr)
        return false;
    outBlob.assign(raw, raw + sz);
    return true;
}

sptr<IRemoteObject> callOpenCamera(const sptr<IRemoteObject> &host, const std::string &camId,
                                   const sptr<IRemoteObject> &devCb) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kHostToken) || !data.WriteCString(camId.c_str()) ||
        !data.WriteRemoteObject(devCb))
        return nullptr;
    int rc = host->SendRequest(4 /* CMD_CAMERA_HOST_OPEN_CAMERA */, data, reply, opt);
    if (rc != 0) {
        std::printf("OPEN_CAMERA_ERR id=%s rc=%d\n", camId.c_str(), rc);
        return nullptr;
    }
    return reply.ReadRemoteObject();
}

sptr<IRemoteObject> callGetStreamOperator(const sptr<IRemoteObject> &dev,
                                          const sptr<IRemoteObject> &streamCb) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kDeviceToken) || !data.WriteRemoteObject(streamCb))
        return nullptr;
    int rc = dev->SendRequest(1 /* CMD_CAMERA_DEVICE_GET_STREAM_OPERATOR */, data, reply, opt);
    if (rc != 0) {
        std::printf("GET_STREAM_OPERATOR_ERR rc=%d\n", rc);
        return nullptr;
    }
    return reply.ReadRemoteObject();
}

bool writeStreamInfo(MessageParcel &data, int32_t streamId, int32_t w, int32_t h,
                     int32_t fmt, int32_t dataspace, uint64_t intent,
                     bool tunneled, int32_t minDur, uint64_t encodeType) {
    return data.WriteInt32(streamId) &&
           data.WriteInt32(w) &&
           data.WriteInt32(h) &&
           data.WriteInt32(fmt) &&
           data.WriteInt32(dataspace) &&
           data.WriteUint64(intent) &&
           data.WriteBool(tunneled) &&
           data.WriteInt32(0) /* null BufferProducerSequenceable; attached via AttachBufferQueue */ &&
           data.WriteInt32(minDur) &&
           data.WriteUint64(encodeType);
}

bool callCreateStreams(const sptr<IRemoteObject> &op) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kStreamOpToken) || !data.WriteUint32(2))
        return false;
    /* Stream 100: Preview 640x480 NV21 (OHOS_CAMERA_FORMAT_YCRCB_420_SP = 1003, PREVIEW = 0) */
    if (!writeStreamInfo(data, 100, 640, 480, 1003, 8, 0, true, 0, 0))
        return false;
    /* Stream 101: Still capture 640x480 JPEG (OHOS_CAMERA_FORMAT_JPEG = 2000, STILL_CAPTURE = 2, JPEG = 3) */
    if (!writeStreamInfo(data, 101, 640, 480, 2000, 8, 2, true, 0, 3))
        return false;
    int rc = op->SendRequest(2 /* CMD_STREAM_OPERATOR_CREATE_STREAMS */, data, reply, opt);
    if (rc != 0) {
        std::printf("CREATE_STREAMS_ERR rc=%d\n", rc);
        return false;
    }
    return true;
}

bool callAttachBufferQueue(const sptr<IRemoteObject> &op, int32_t streamId, OHNativeWindow *win) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kStreamOpToken) || !data.WriteInt32(streamId))
        return false;
    /* WriteStrongParcelable non-null prefix (int32 = 1) + BufferProducerSequenceable::Marshalling
     * which writes producer->AsObject() via OH_NativeWindow_WriteToParcel. */
    if (!data.WriteInt32(1))
        return false;
    OhIpcParcelBridge bridge { &data };
    int32_t wrc = OH_NativeWindow_WriteToParcel(win, reinterpret_cast<OHIPCParcel *>(&bridge));
    if (wrc != 0) {
        std::printf("NATIVE_WINDOW_WRITE_PARCEL_ERR stream=%d rc=%d\n", streamId, wrc);
        return false;
    }
    int rc = op->SendRequest(6 /* CMD_STREAM_OPERATOR_ATTACH_BUFFER_QUEUE */, data, reply, opt);
    if (rc != 0) {
        std::printf("ATTACH_BUFFER_QUEUE_ERR stream=%d rc=%d\n", streamId, rc);
        return false;
    }
    return true;
}

bool callCommitStreams(const sptr<IRemoteObject> &op) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kStreamOpToken) ||
        !data.WriteUint64(0 /* OperationMode::NORMAL */) ||
        !data.WriteUint32(0 /* empty modeSetting */))
        return false;
    int rc = op->SendRequest(4 /* CMD_STREAM_OPERATOR_COMMIT_STREAMS */, data, reply, opt);
    if (rc != 0) {
        std::printf("COMMIT_STREAMS_ERR rc=%d\n", rc);
        return false;
    }
    return true;
}

bool callCapture(const sptr<IRemoteObject> &op, int32_t captureId, int32_t streamId, bool isStreaming) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kStreamOpToken) ||
        !data.WriteInt32(captureId) ||
        !data.WriteUint32(1) ||
        !data.WriteUnpadBuffer(&streamId, sizeof(streamId)) ||
        !data.WriteUint32(0) ||
        !data.WriteBool(true /* enableShutterCallback */) ||
        !data.WriteBool(isStreaming))
        return false;
    int rc = op->SendRequest(8 /* CMD_STREAM_OPERATOR_CAPTURE */, data, reply, opt);
    if (rc != 0) {
        std::printf("CAPTURE_ERR cap=%d stream=%d rc=%d\n", captureId, streamId, rc);
        return false;
    }
    return true;
}

bool callCancelCapture(const sptr<IRemoteObject> &op, int32_t captureId) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kStreamOpToken) || !data.WriteInt32(captureId))
        return false;
    return op->SendRequest(9 /* CMD_STREAM_OPERATOR_CANCEL_CAPTURE */, data, reply, opt) == 0;
}

bool callReleaseStreams(const sptr<IRemoteObject> &op, const std::vector<int32_t> &ids) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kStreamOpToken) ||
        !data.WriteUint32(static_cast<uint32_t>(ids.size())))
        return false;
    if (!ids.empty() && !data.WriteUnpadBuffer(ids.data(), ids.size() * sizeof(int32_t)))
        return false;
    return op->SendRequest(3 /* CMD_STREAM_OPERATOR_RELEASE_STREAMS */, data, reply, opt) == 0;
}

bool callCloseDevice(const sptr<IRemoteObject> &dev) {
    MessageParcel data;
    MessageParcel reply;
    MessageOption opt;
    if (!data.WriteInterfaceToken(kDeviceToken))
        return false;
    return dev->SendRequest(7 /* CMD_CAMERA_DEVICE_CLOSE */, data, reply, opt) == 0;
}

struct ConsumerHolder {
    OH_NativeImage *img = nullptr;
    OHNativeWindow *win = nullptr;
    std::atomic<int> availCount {0};

    static void onFrameAvail(void *ctx) {
        auto *self = static_cast<ConsumerHolder *>(ctx);
        if (self != nullptr)
            self->availCount.fetch_add(1);
    }

    bool init(int32_t w, int32_t h) {
        img = OH_ConsumerSurface_Create();
        if (img == nullptr)
            return false;
        (void)OH_ConsumerSurface_SetDefaultSize(img, w, h);
        (void)OH_ConsumerSurface_SetDefaultUsage(img, (1ULL << 0) | (1ULL << 1) | (1ULL << 3));
        OH_OnFrameAvailableListener listener { this, &ConsumerHolder::onFrameAvail };
        (void)OH_NativeImage_SetOnFrameAvailableListener(img, listener);
        win = OH_NativeImage_AcquireNativeWindow(img);
        return win != nullptr;
    }

    ~ConsumerHolder() {
        if (win != nullptr) {
            OH_NativeWindow_DestroyNativeWindow(win);
            win = nullptr;
        }
        if (img != nullptr) {
            OH_NativeImage_Destroy(&img);
            img = nullptr;
        }
    }
};

struct CameraCaptureReport {
    std::string id;
    bool openOk = false;
    int previewFrames = 0;
    bool previewNonZero = false;
    int previewCsumChanges = 0;
    bool jpegOk = false;
    size_t jpegBytes = 0;
    bool closeOk = false;
    std::string hwSrc = "none";
    int hwFrames = 0;
    int raw10Mean = 0;
};

static uint32_t fnvBufCsum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    size_t step = (n > 8192) ? (n / 4096) : 1;
    for (size_t i = 0; i < n; i += step) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void readHwStatusFile(CameraCaptureReport &rep) {
    int fd = ::open("/data/vendor/camera/last_capture_status.txt", O_RDONLY);
    if (fd < 0)
        return;
    char buf[256] = {0};
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0)
        return;
    char srcBuf[64] = {0};
    unsigned slot = 0, hwf = 0, req = 0, rmin = 0, rmax = 0, rmean = 0, csum = 0;
    size_t jb = 0;
    if (std::sscanf(buf, "slot=%u src=%63s hw_frames=%u req_id=%u raw10_min=%u raw10_max=%u raw10_mean=%u csum=0x%x jpeg_bytes=%zu",
                    &slot, srcBuf, &hwf, &req, &rmin, &rmax, &rmean, &csum, &jb) >= 3) {
        rep.hwSrc = srcBuf;
        rep.hwFrames = static_cast<int>(hwf);
        rep.raw10Mean = static_cast<int>(rmean);
    }
}

static CameraCaptureReport runSingleCameraCapture(const sptr<IRemoteObject> &host,
                                                  const std::string &camId,
                                                  const sptr<IRemoteObject> &devCb,
                                                  const sptr<IRemoteObject> &streamCb,
                                                  const std::string &dumpJpegPath) {
    CameraCaptureReport rep;
    rep.id = camId;
    (void)::unlink("/data/vendor/camera/last_capture_status.txt");

    sptr<IRemoteObject> dev = callOpenCamera(host, camId, devCb);
    if (dev == nullptr)
        return rep;
    rep.openOk = true;
    std::printf("CAMERA_OPEN id=%s ok=1\n", camId.c_str());

    sptr<IRemoteObject> op = callGetStreamOperator(dev, streamCb);
    if (op == nullptr) {
        rep.closeOk = callCloseDevice(dev);
        return rep;
    }

    ConsumerHolder previewSurf;
    ConsumerHolder jpegSurf;
    if (!previewSurf.init(640, 480) || !jpegSurf.init(640, 480)) {
        std::printf("CONSUMER_SURFACE_INIT_ERR id=%s\n", camId.c_str());
        rep.closeOk = callCloseDevice(dev);
        return rep;
    }

    if (!callCreateStreams(op) ||
        !callAttachBufferQueue(op, 100, previewSurf.win) ||
        !callAttachBufferQueue(op, 101, jpegSurf.win) ||
        !callCommitStreams(op)) {
        rep.closeOk = callCloseDevice(dev);
        return rep;
    }

    /* 1) Continuous NV21 preview capture (3 frames) */
    uint32_t prevCsum = 0;
    if (callCapture(op, 1, 100, true)) {
        for (int tries = 0; tries < 100 && rep.previewFrames < 3; ++tries) {
            OHNativeWindowBuffer *nwBuf = nullptr;
            int fenceFd = -1;
            int32_t arc = OH_NativeImage_AcquireNativeWindowBuffer(previewSurf.img, &nwBuf, &fenceFd);
            if (fenceFd >= 0)
                ::close(fenceFd);
            if (arc == 0 && nwBuf != nullptr) {
                BufferHandle *bh = OH_NativeWindow_GetBufferHandleFromNative(nwBuf);
                if (bh != nullptr && bh->size > 0 && bh->fd >= 0) {
                    void *p = ::mmap(nullptr, static_cast<size_t>(bh->size), PROT_READ, MAP_SHARED, bh->fd, 0);
                    if (p != MAP_FAILED) {
                        const uint8_t *bp = static_cast<const uint8_t *>(p);
                        if (hasNonZeroBytes(bp, static_cast<size_t>(bh->size)))
                            rep.previewNonZero = true;
                        uint32_t cs = fnvBufCsum(bp, static_cast<size_t>(bh->size));
                        if (rep.previewFrames > 0 && cs != prevCsum)
                            rep.previewCsumChanges++;
                        prevCsum = cs;
                        ::munmap(p, static_cast<size_t>(bh->size));
                    }
                }
                (void)OH_NativeImage_ReleaseNativeWindowBuffer(previewSurf.img, nwBuf, -1);
                rep.previewFrames++;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
        }
        (void)callCancelCapture(op, 1);
    }

    /* 2) Single-shot still baseline DCT JPEG capture */
    if (callCapture(op, 2, 101, false)) {
        for (int tries = 0; tries < 100 && !rep.jpegOk; ++tries) {
            OHNativeWindowBuffer *nwBuf = nullptr;
            int fenceFd = -1;
            int32_t arc = OH_NativeImage_AcquireNativeWindowBuffer(jpegSurf.img, &nwBuf, &fenceFd);
            if (fenceFd >= 0)
                ::close(fenceFd);
            if (arc == 0 && nwBuf != nullptr) {
                BufferHandle *bh = OH_NativeWindow_GetBufferHandleFromNative(nwBuf);
                if (bh != nullptr && bh->size > 0 && bh->fd >= 0) {
                    void *p = ::mmap(nullptr, static_cast<size_t>(bh->size), PROT_READ, MAP_SHARED, bh->fd, 0);
                    if (p != MAP_FAILED) {
                        rep.jpegOk = isValidBaselineJpeg(static_cast<const uint8_t *>(p),
                                                         static_cast<size_t>(bh->size), &rep.jpegBytes);
                        if (rep.jpegOk && !dumpJpegPath.empty()) {
                            int dfd = ::open(dumpJpegPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                            if (dfd >= 0) {
                                (void)::write(dfd, p, rep.jpegBytes);
                                ::close(dfd);
                            }
                        }
                        ::munmap(p, static_cast<size_t>(bh->size));
                    }
                }
                (void)OH_NativeImage_ReleaseNativeWindowBuffer(jpegSurf.img, nwBuf, -1);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    readHwStatusFile(rep);
    (void)callReleaseStreams(op, {100, 101});
    rep.closeOk = callCloseDevice(dev);

    std::printf("CAMERA_RUN id=%s open=%d preview_frames=%d preview_nonzero=%d preview_csum_changes=%d "
                "jpeg_ok=%d jpeg_bytes=%zu hw_src=%s hw_frames=%d raw10_mean=%d close=%d\n",
                rep.id.c_str(), rep.openOk ? 1 : 0, rep.previewFrames,
                rep.previewNonZero ? 1 : 0, rep.previewCsumChanges,
                rep.jpegOk ? 1 : 0, rep.jpegBytes,
                rep.hwSrc.c_str(), rep.hwFrames, rep.raw10Mean, rep.closeOk ? 1 : 0);
    return rep;
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string dumpJpegPath;
    std::string dumpOptJpegPath;
    std::string dumpDir;
    std::string onlyCam;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-jpeg") == 0 && i + 1 < argc) {
            dumpJpegPath = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-opt-jpeg") == 0 && i + 1 < argc) {
            dumpOptJpegPath = argv[++i];
        } else if (std::strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            dumpDir = argv[++i];
        } else if (std::strcmp(argv[i], "--camera") == 0 && i + 1 < argc) {
            onlyCam = argv[++i];
        } else if (std::strcmp(argv[i], "--self-test") == 0) {
            uint8_t fakeJpeg[80] = {};
            fakeJpeg[0] = 0xFF; fakeJpeg[1] = 0xD8;
            fakeJpeg[10] = 0xFF; fakeJpeg[11] = 0xC0;
            fakeJpeg[24] = 0xFF; fakeJpeg[25] = 0xDA;
            fakeJpeg[70] = 0xFF; fakeJpeg[71] = 0xD9;
            size_t jlen = 0;
            bool ok = isValidBaselineJpeg(fakeJpeg, sizeof(fakeJpeg), &jlen) && jlen == 72;
            std::printf("MARBLE_CAMERA_HDI_PROBE_SELF_TEST_OK=%d jpeg_len=%zu\n", ok ? 1 : 0, jlen);
            return ok ? 0 : 1;
        }
    }

    void *ipcLib = dlopen("libipc_single.z.so", RTLD_NOW | RTLD_GLOBAL);
    auto stubCtor = ipcLib ? reinterpret_cast<StubCtor>(dlsym(
        ipcLib, "_ZN4OHOS13IPCObjectStubC1ENSt3__h12basic_stringIDsNS1_11char_traitsIDsEENS1_9allocatorIDsEEEEb")) : nullptr;
    auto stubOrig = ipcLib ? reinterpret_cast<StubDispatch>(dlsym(
        ipcLib, "_ZN4OHOS13IPCObjectStub15OnRemoteRequestEjRNS_13MessageParcelES2_RNS_13MessageOptionE")) : nullptr;
    auto setThreads = ipcLib ? reinterpret_cast<SetThreadsFn>(dlsym(
        ipcLib, "_ZN4OHOS11IPCSkeleton19SetMaxWorkThreadNumEi")) : nullptr;
    auto joinThread = ipcLib ? reinterpret_cast<JoinThreadFn>(dlsym(
        ipcLib, "_ZN4OHOS11IPCSkeleton14JoinWorkThreadEv")) : nullptr;
    if (!stubCtor || !stubOrig || !setThreads || !joinThread) {
        std::printf("IPC_SYMBOL_ERR=1\n");
        return 1;
    }
    (void)setThreads(4);
    gJoinThread = joinThread;
    pthread_t ipcTid;
    if (pthread_create(&ipcTid, nullptr, [](void *) -> void * { gJoinThread(); return nullptr; }, nullptr) == 0) {
        pthread_detach(ipcTid);
    }

    bool sa3008Ok = checkSa3008Registered();
    std::printf("SA3008_REGISTERED=%d\n", sa3008Ok ? 1 : 0);

    auto servmgr = OHOS::HDI::ServiceManager::V1_0::IServiceManager::Get();
    if (servmgr == nullptr) {
        std::printf("HDI_SERVMGR_ERR=null\n");
        return 1;
    }
    sptr<IRemoteObject> host = servmgr->GetService("camera_service");
    if (host == nullptr) {
        std::printf("CAMERA_HOST_ERR=service_not_found\n");
        return 2;
    }
    std::printf("CAMERA_HOST_SERVICE=connected\n");

    uint32_t majorVer = 0, minorVer = 0;
    bool verOk = callGetHostVersion(host, majorVer, minorVer);
    bool torchV12Ok = false;
    if (verOk && (majorVer > 1u || (majorVer == 1u && minorVer >= 2u))) {
        bool onOk = callSetFlashlightV12(host, 0.5f);
        ::usleep(80000);
        bool offOk = callSetFlashlightV12(host, 0.0f);
        torchV12Ok = onOk && offOk;
    }
    std::printf("CAMERA_HOST_VERSION ok=%d major=%u minor=%u torch_v12_ok=%d\n",
                verOk ? 1 : 0, majorVer, minorVer, torchV12Ok ? 1 : 0);

    std::vector<std::string> camIds;
    if (!callGetCameraIds(host, camIds)) {
        return 3;
    }
    std::printf("CAMERA_IDS count=%zu", camIds.size());
    for (const auto &id : camIds)
        std::printf(" %s", id.c_str());
    std::printf("\n");

    int abilitiesOk = 0;
    for (const auto &id : camIds) {
        std::vector<uint8_t> blob;
        if (callGetCameraAbility(host, id, blob)) {
            std::shared_ptr<OHOS::Camera::CameraMetadata> meta;
            OHOS::Camera::MetadataUtils::ConvertVecToMetadata(blob, meta);
            if (meta != nullptr && meta->isValid()) {
                abilitiesOk++;
                std::printf("CAMERA_ABILITY id=%s bytes=%zu valid=1\n", id.c_str(), blob.size());
            } else {
                std::printf("CAMERA_ABILITY id=%s bytes=%zu valid=0\n", id.c_str(), blob.size());
            }
        }
    }

    if (camIds.empty()) {
        std::printf("MARBLE_CAMERA_HDI_PROBE_SUMMARY sa3008=%d host=1 cam_count=0 abilities_ok=0 open=0 preview_frames=0 jpeg_ok=0 jpeg_bytes=0 started=0 shutter=0 ended=0 close=0 tpg_hw=0 imx355_hw=0\n",
                    sa3008Ok ? 1 : 0);
        return 4;
    }

    alignas(16) static uintptr_t devTable[128];
    alignas(16) static uintptr_t streamTable[128];
    void *devCbRaw = MakeDonorCallbackStub(stubCtor, stubOrig, kDeviceCbToken, &DeviceCallbackDispatch, devTable);
    void *streamCbRaw = MakeDonorCallbackStub(stubCtor, stubOrig, kStreamCbToken, &StreamCallbackDispatch, streamTable);
    if (devCbRaw == nullptr || streamCbRaw == nullptr)
        return 5;
    sptr<IRemoteObject> devCb(reinterpret_cast<IRemoteObject *>(devCbRaw));
    sptr<IRemoteObject> streamCb(reinterpret_cast<IRemoteObject *>(streamCbRaw));

    std::vector<std::string> targets;
    if (!onlyCam.empty()) {
        targets.push_back(onlyCam);
    } else {
        /* Batch G4: exercise ALL 4 physical optical cameras sequentially over Binder + DMA-BUF */
        targets = camIds;
    }

    bool allRunsOk = true;
    int totalPreviewFrames = 0;
    bool anyPreviewNonZero = false;
    bool allJpegOk = true;
    size_t lastJpegBytes = 0;
    bool allCloseOk = true;
    bool ov64b40HwOk = false;
    bool s5k3p9HwOk = false;
    bool imx355HwOk = false;
    bool gc02m1HwOk = false;
    bool tpgHwOk = false;

    for (const auto &tid : targets) {
        std::string jpath;
        if (!dumpDir.empty()) {
            jpath = dumpDir + "/" + tid + ".jpg";
        } else if (tid == "lcam003" && !dumpOptJpegPath.empty()) {
            jpath = dumpOptJpegPath;
        } else {
            jpath = dumpJpegPath;
        }
        CameraCaptureReport rep = runSingleCameraCapture(host, tid, devCb, streamCb, jpath);
        totalPreviewFrames += rep.previewFrames;
        if (rep.previewNonZero) anyPreviewNonZero = true;
        if (!rep.jpegOk) allJpegOk = false;
        if (rep.jpegBytes > 0) lastJpegBytes = rep.jpegBytes;
        if (!rep.closeOk) allCloseOk = false;
        if (rep.hwSrc == "ov64b40_optical" && rep.hwFrames >= 3) ov64b40HwOk = true;
        if (rep.hwSrc == "s5k3p9_optical"  && rep.hwFrames >= 3) s5k3p9HwOk = true;
        if (rep.hwSrc == "imx355_optical"  && rep.hwFrames >= 3) imx355HwOk = true;
        if (rep.hwSrc == "gc02m1_optical"  && rep.hwFrames >= 3) gc02m1HwOk = true;
        if (rep.hwSrc == "sm7475_tpg"      && rep.hwFrames >= 3) tpgHwOk = true;
        if (!rep.openOk || rep.previewFrames < 3 || !rep.previewNonZero || !rep.jpegOk || !rep.closeOk)
            allRunsOk = false;
    }
    int opticalCams = (ov64b40HwOk ? 1 : 0) + (s5k3p9HwOk ? 1 : 0) +
                      (imx355HwOk ? 1 : 0) + (gc02m1HwOk ? 1 : 0);

    std::printf("MARBLE_CAMERA_HDI_PROBE_SUMMARY sa3008=%d host=1 host_ver=%u.%u torch_v12_ok=%d cam_count=%zu abilities_ok=%d open=%d "
                "preview_frames=%d preview_nonzero=%d jpeg_ok=%d jpeg_bytes=%zu started=%d shutter=%d "
                "ended=%d errors=%d close=%d ov64b40_hw=%d s5k3p9_hw=%d imx355_hw=%d gc02m1_hw=%d "
                "tpg_hw=%d optical_cams=%d\n",
                sa3008Ok ? 1 : 0, majorVer, minorVer, torchV12Ok ? 1 : 0, camIds.size(), abilitiesOk, allRunsOk ? 1 : 0,
                totalPreviewFrames, anyPreviewNonZero ? 1 : 0, allJpegOk ? 1 : 0, lastJpegBytes,
                gStreamStarted.load(), gStreamShutter.load(),
                gStreamEnded.load(), gStreamErrors.load(), allCloseOk ? 1 : 0,
                ov64b40HwOk ? 1 : 0, s5k3p9HwOk ? 1 : 0, imx355HwOk ? 1 : 0, gc02m1HwOk ? 1 : 0,
                tpgHwOk ? 1 : 0, opticalCams);

    bool allGood = (camIds.size() == 4) && (abilitiesOk == 4) && allRunsOk &&
                   allJpegOk && allCloseOk &&
                   (!onlyCam.empty() || (opticalCams == 4));
    std::fflush(stdout);
    _exit(allGood ? 0 : 10);
}
#endif
