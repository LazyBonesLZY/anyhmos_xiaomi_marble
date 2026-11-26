/* SPDX-License-Identifier: MIT */
/*
 * marble_camera_host_service_1.2.cpp - Clean-room SM7475 (Xiaomi marble) OHOS Camera
 * HDI V1_2 HDF host driver service.
 *
 * Exports HDF driver entry (`driverDesc`) loaded by `hdf_devhost -i 9 -n camera_host`.
 * Links donor `/vendor/lib64/libcamera_stub_1.2.z.so` so `OHOS::HDI::ObjectCollector`
 * wraps our service in `OHOS::HDI::Camera::V1_2::CameraHostStub`, exposing:
 *   - `GetVersion() == (1, 2)` (unlocks `V1_2::ICameraHost::CastFrom` in SA 3008)
 *   - `SetFlashlight_V1_2(float level)` (vtable slot 15 `+120`) driving real SM7475
 *     QPNP LED sysfs `/sys/class/leds/led:torch_0` (+ `led:torch_1`) & `led:switch_0`
 *   - `SetCallback_V1_2` -> `OnFlashlightStatus_V1_2` (`OFF`/`ON`/`UNAVAILABLE`)
 *   - `OpenCamera` / `OpenCamera_V1_1` / `OpenCamera_V1_2`
 *   - `V1_2::ICameraDevice` (`GetStreamOperator`/`V1_1`/`V1_2`, `GetDefaultSettings`,
 *     `GetStatus`, `Reset`) and `V1_2::IStreamOperator` (`*_V1_1`, `UpdateStreams`,
 *     `ConfirmCapture`)
 *
 * Delegates physical SM7475 camera capture/metadata/stream operations to our
 * VDI 1.0 module (`libmarble_camera_vdi_impl.z.so`) loaded via `HdfLoadVdi()`.
 */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <hdf_base.h>
#include <hdf_device_desc.h>
#include <hdf_log.h>
#include <hdf_sbuf_ipc.h>
#include <device_resource_if.h>
#include <hcs_dm_parser.h>
#include <hdf_load_vdi.h>
#include <object_collector.h>

#include "camera.h"
#include "marble_torch_ctrl.h"
#include "v1_0/icamera_host_vdi.h"
#include "v1_0/icamera_device_vdi.h"
#include "v1_0/istream_operator_vdi.h"
#include "v1_0/ioffline_stream_operator_vdi.h"
#include "v1_0/vdi_types.h"
#include "v1_2/icamera_host.h"
#include "v1_2/icamera_device.h"
#include "v1_2/istream_operator.h"
#include "v1_2/icamera_host_callback.h"
#include "v1_2/istream_operator_callback.h"

namespace OHOS::Camera {

namespace HdiV1_0 = OHOS::HDI::Camera::V1_0;
namespace HdiV1_1 = OHOS::HDI::Camera::V1_1;
namespace HdiV1_2 = OHOS::HDI::Camera::V1_2;
namespace VdiV1_0 = OHOS::VDI::Camera::V1_0;

namespace {

constexpr const char *kCameraHostConfigPath = "/vendor/etc/hdfconfig/camera_host_config.hcb";
constexpr size_t kNameStartPos = 6;

static VdiV1_0::VdiStreamInfo toVdiStreamInfo(const HdiV1_0::StreamInfo &src) {
    VdiV1_0::VdiStreamInfo dst {};
    dst.streamId_ = src.streamId_;
    dst.width_ = src.width_;
    dst.height_ = src.height_;
    dst.format_ = src.format_;
    dst.dataspace_ = src.dataspace_;
    dst.intent_ = static_cast<VdiV1_0::VdiStreamIntent>(src.intent_);
    dst.tunneledMode_ = src.tunneledMode_;
    dst.bufferQueue_ = src.bufferQueue_;
    dst.minFrameDuration_ = src.minFrameDuration_;
    dst.encodeType_ = static_cast<VdiV1_0::VdiEncodeType>(src.encodeType_);
    return dst;
}

static HdiV1_0::StreamAttribute toHdiStreamAttribute(const VdiV1_0::VdiStreamAttribute &src) {
    HdiV1_0::StreamAttribute dst {};
    dst.streamId_ = src.streamId_;
    dst.width_ = src.width_;
    dst.height_ = src.height_;
    dst.overrideFormat_ = src.overrideFormat_;
    dst.overrideDataspace_ = src.overrideDataspace_;
    dst.producerUsage_ = src.producerUsage_;
    dst.producerBufferCount_ = src.producerBufferCount_;
    dst.maxBatchCaptureCount_ = src.maxBatchCaptureCount_;
    dst.maxCaptureCount_ = src.maxCaptureCount_;
    return dst;
}

static VdiV1_0::VdiCaptureInfo toVdiCaptureInfo(const HdiV1_0::CaptureInfo &src) {
    VdiV1_0::VdiCaptureInfo dst {};
    dst.streamIds_ = src.streamIds_;
    dst.captureSetting_ = src.captureSetting_;
    dst.enableShutterCallback_ = src.enableShutterCallback_;
    return dst;
}

class MarbleDeviceCallbackBridge : public VdiV1_0::ICameraDeviceVdiCallback {
public:
    explicit MarbleDeviceCallbackBridge(const sptr<HdiV1_0::ICameraDeviceCallback> &cb)
        : cb_(cb) {}

    int32_t OnError(VdiV1_0::VdiErrorType type, int32_t errorCode) override {
        if (cb_ == nullptr)
            return VdiV1_0::NO_ERROR;
        return cb_->OnError(static_cast<HdiV1_0::ErrorType>(type), errorCode);
    }

    int32_t OnResult(uint64_t timestamp, const std::vector<uint8_t> &result) override {
        if (cb_ == nullptr)
            return VdiV1_0::NO_ERROR;
        return cb_->OnResult(timestamp, result);
    }

private:
    sptr<HdiV1_0::ICameraDeviceCallback> cb_;
};

class MarbleStreamCallbackBridge : public VdiV1_0::IStreamOperatorVdiCallback {
public:
    explicit MarbleStreamCallbackBridge(const sptr<HdiV1_0::IStreamOperatorCallback> &cbV10,
                                        const sptr<HdiV1_2::IStreamOperatorCallback> &cbV12 = nullptr)
        : cbV10_(cbV10), cbV12_(cbV12) {}

    int32_t OnCaptureStarted(int32_t captureId, const std::vector<int32_t> &streamIds) override {
        if (cbV12_ != nullptr) {
            std::vector<HdiV1_2::CaptureStartedInfo> infos;
            infos.reserve(streamIds.size());
            for (int32_t sid : streamIds) {
                HdiV1_2::CaptureStartedInfo ci {};
                ci.streamId_ = sid;
                ci.exposureTime_ = 33;
                infos.push_back(ci);
            }
            return cbV12_->OnCaptureStarted_V1_2(captureId, infos);
        }
        if (cbV10_ != nullptr)
            return cbV10_->OnCaptureStarted(captureId, streamIds);
        return VdiV1_0::NO_ERROR;
    }

    int32_t OnCaptureEnded(int32_t captureId,
                           const std::vector<VdiV1_0::VdiCaptureEndedInfo> &infos) override {
        sptr<HdiV1_0::IStreamOperatorCallback> cb = cbV12_ != nullptr
            ? sptr<HdiV1_0::IStreamOperatorCallback>(cbV12_.GetRefPtr())
            : cbV10_;
        if (cb == nullptr)
            return VdiV1_0::NO_ERROR;
        std::vector<HdiV1_0::CaptureEndedInfo> out;
        out.reserve(infos.size());
        for (const auto &i : infos) {
            HdiV1_0::CaptureEndedInfo ei {};
            ei.streamId_ = i.streamId_;
            ei.frameCount_ = i.frameCount_;
            out.push_back(ei);
        }
        return cb->OnCaptureEnded(captureId, out);
    }

    int32_t OnCaptureError(int32_t captureId,
                           const std::vector<VdiV1_0::VdiCaptureErrorInfo> &infos) override {
        sptr<HdiV1_0::IStreamOperatorCallback> cb = cbV12_ != nullptr
            ? sptr<HdiV1_0::IStreamOperatorCallback>(cbV12_.GetRefPtr())
            : cbV10_;
        if (cb == nullptr)
            return VdiV1_0::NO_ERROR;
        std::vector<HdiV1_0::CaptureErrorInfo> out;
        out.reserve(infos.size());
        for (const auto &i : infos) {
            HdiV1_0::CaptureErrorInfo ei {};
            ei.streamId_ = i.streamId_;
            ei.error_ = static_cast<HdiV1_0::StreamError>(i.error_);
            out.push_back(ei);
        }
        return cb->OnCaptureError(captureId, out);
    }

    int32_t OnFrameShutter(int32_t captureId, const std::vector<int32_t> &streamIds,
                           uint64_t timestamp) override {
        sptr<HdiV1_0::IStreamOperatorCallback> cb = cbV12_ != nullptr
            ? sptr<HdiV1_0::IStreamOperatorCallback>(cbV12_.GetRefPtr())
            : cbV10_;
        if (cb == nullptr)
            return VdiV1_0::NO_ERROR;
        return cb->OnFrameShutter(captureId, streamIds, timestamp);
    }

private:
    sptr<HdiV1_0::IStreamOperatorCallback> cbV10_;
    sptr<HdiV1_2::IStreamOperatorCallback> cbV12_;
};

class MarbleStreamOperatorServiceV1_2 : public HdiV1_2::IStreamOperator {
public:
    explicit MarbleStreamOperatorServiceV1_2(const sptr<VdiV1_0::IStreamOperatorVdi> &vdi)
        : vdi_(vdi) {}

    ~MarbleStreamOperatorServiceV1_2() override = default;

    int32_t IsStreamsSupported(HdiV1_0::OperationMode mode,
                               const std::vector<uint8_t> &modeSetting,
                               const std::vector<HdiV1_0::StreamInfo> &infos,
                               HdiV1_0::StreamSupportType &type) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        std::vector<VdiV1_0::VdiStreamInfo> vdiInfos;
        vdiInfos.reserve(infos.size());
        for (const auto &si : infos)
            vdiInfos.push_back(toVdiStreamInfo(si));
        VdiV1_0::VdiStreamSupportType vdiType = VdiV1_0::NOT_SUPPORTED;
        int32_t rc = vdi_->IsStreamsSupported(static_cast<VdiV1_0::VdiOperationMode>(mode),
                                              modeSetting, vdiInfos, vdiType);
        type = static_cast<HdiV1_0::StreamSupportType>(vdiType);
        return rc;
    }

    int32_t CreateStreams(const std::vector<HdiV1_0::StreamInfo> &streamInfos) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        std::vector<VdiV1_0::VdiStreamInfo> vdiInfos;
        vdiInfos.reserve(streamInfos.size());
        for (const auto &si : streamInfos)
            vdiInfos.push_back(toVdiStreamInfo(si));
        return vdi_->CreateStreams(vdiInfos);
    }

    int32_t ReleaseStreams(const std::vector<int32_t> &streamIds) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        return vdi_->ReleaseStreams(streamIds);
    }

    int32_t CommitStreams(HdiV1_0::OperationMode mode,
                          const std::vector<uint8_t> &modeSetting) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        return vdi_->CommitStreams(static_cast<VdiV1_0::VdiOperationMode>(mode), modeSetting);
    }

    int32_t GetStreamAttributes(std::vector<HdiV1_0::StreamAttribute> &attributes) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        std::vector<VdiV1_0::VdiStreamAttribute> vdiAttrs;
        int32_t rc = vdi_->GetStreamAttributes(vdiAttrs);
        attributes.clear();
        attributes.reserve(vdiAttrs.size());
        for (const auto &a : vdiAttrs)
            attributes.push_back(toHdiStreamAttribute(a));
        return rc;
    }

    int32_t AttachBufferQueue(int32_t streamId,
                              const sptr<HdiV1_0::BufferProducerSequenceable> &bufferProducer) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        return vdi_->AttachBufferQueue(streamId, bufferProducer);
    }

    int32_t DetachBufferQueue(int32_t streamId) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        return vdi_->DetachBufferQueue(streamId);
    }

    int32_t Capture(int32_t captureId, const HdiV1_0::CaptureInfo &info,
                    bool isStreaming) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        return vdi_->Capture(captureId, toVdiCaptureInfo(info), isStreaming);
    }

    int32_t CancelCapture(int32_t captureId) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        return vdi_->CancelCapture(captureId);
    }

    int32_t ChangeToOfflineStream(const std::vector<int32_t> &streamIds,
                                  const sptr<HdiV1_0::IStreamOperatorCallback> &callbackObj,
                                  sptr<HdiV1_0::IOfflineStreamOperator> &offlineOperator) override {
        (void)streamIds;
        (void)callbackObj;
        offlineOperator = nullptr;
        return HdiV1_0::METHOD_NOT_SUPPORTED;
    }

    /* V1_1 extensions */
    int32_t IsStreamsSupported_V1_1(HdiV1_1::OperationMode_V1_1 mode,
                                    const std::vector<uint8_t> &modeSetting,
                                    const std::vector<HdiV1_1::StreamInfo_V1_1> &infos,
                                    HdiV1_0::StreamSupportType &type) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        std::vector<VdiV1_0::VdiStreamInfo> vdiInfos;
        vdiInfos.reserve(infos.size());
        for (const auto &si : infos)
            vdiInfos.push_back(toVdiStreamInfo(si.v1_0));
        VdiV1_0::VdiStreamSupportType vdiType = VdiV1_0::NOT_SUPPORTED;
        int32_t rc = vdi_->IsStreamsSupported(static_cast<VdiV1_0::VdiOperationMode>(mode),
                                              modeSetting, vdiInfos, vdiType);
        type = static_cast<HdiV1_0::StreamSupportType>(vdiType);
        return rc;
    }

    int32_t CreateStreams_V1_1(const std::vector<HdiV1_1::StreamInfo_V1_1> &streamInfos) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        std::vector<VdiV1_0::VdiStreamInfo> vdiInfos;
        vdiInfos.reserve(streamInfos.size());
        for (const auto &si : streamInfos)
            vdiInfos.push_back(toVdiStreamInfo(si.v1_0));
        return vdi_->CreateStreams(vdiInfos);
    }

    int32_t CommitStreams_V1_1(HdiV1_1::OperationMode_V1_1 mode,
                               const std::vector<uint8_t> &modeSetting) override {
        if (vdi_ == nullptr)
            return HdiV1_0::INVALID_ARGUMENT;
        return vdi_->CommitStreams(static_cast<VdiV1_0::VdiOperationMode>(mode), modeSetting);
    }

    /* V1_2 extensions */
    int32_t UpdateStreams(const std::vector<HdiV1_1::StreamInfo_V1_1> &streamInfos) override {
        return CreateStreams_V1_1(streamInfos);
    }

    int32_t ConfirmCapture(int32_t captureId) override {
        (void)captureId;
        return HdiV1_0::NO_ERROR;
    }

private:
    sptr<VdiV1_0::IStreamOperatorVdi> vdi_;
};

class MarbleCameraDeviceServiceV1_2 : public HdiV1_2::ICameraDevice {
public:
    using CloseHook = std::function<void(const std::string &, uintptr_t)>;

    MarbleCameraDeviceServiceV1_2(std::string cameraId,
                                  const sptr<VdiV1_0::ICameraDeviceVdi> &vdi,
                                  std::vector<uint8_t> defaultSettings,
                                  CloseHook onClose)
        : cameraId_(std::move(cameraId)),
          vdi_(vdi),
          defaultSettings_(std::move(defaultSettings)),
          onClose_(std::move(onClose)) {}

    ~MarbleCameraDeviceServiceV1_2() override {
        (void)Close();
    }

    int32_t GetStreamOperator(const sptr<HdiV1_0::IStreamOperatorCallback> &callbackObj,
                              sptr<HdiV1_0::IStreamOperator> &streamOperator) override {
        sptr<HdiV1_2::IStreamOperator> op12;
        int32_t rc = getStreamOperatorCommon(callbackObj, nullptr, op12);
        if (rc == HdiV1_0::NO_ERROR)
            streamOperator = op12;
        return rc;
    }

    int32_t UpdateSettings(const std::vector<uint8_t> &settings) override {
        if (vdi_ == nullptr)
            return HdiV1_0::CAMERA_CLOSED;
        return vdi_->UpdateSettings(settings);
    }

    int32_t SetResultMode(HdiV1_0::ResultCallbackMode mode) override {
        if (vdi_ == nullptr)
            return HdiV1_0::CAMERA_CLOSED;
        return vdi_->SetResultMode(static_cast<VdiV1_0::VdiResultCallbackMode>(mode));
    }

    int32_t GetEnabledResults(std::vector<int32_t> &results) override {
        if (vdi_ == nullptr)
            return HdiV1_0::CAMERA_CLOSED;
        return vdi_->GetEnabledResults(results);
    }

    int32_t EnableResult(const std::vector<int32_t> &results) override {
        if (vdi_ == nullptr)
            return HdiV1_0::CAMERA_CLOSED;
        return vdi_->EnableResult(results);
    }

    int32_t DisableResult(const std::vector<int32_t> &results) override {
        if (vdi_ == nullptr)
            return HdiV1_0::CAMERA_CLOSED;
        return vdi_->DisableResult(results);
    }

    int32_t Close() override {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true))
            return HdiV1_0::NO_ERROR;
        int32_t rc = HdiV1_0::NO_ERROR;
        if (vdi_ != nullptr)
            rc = vdi_->Close();
        if (onClose_)
            onClose_(cameraId_, reinterpret_cast<uintptr_t>(this));
        return rc;
    }

    /* V1_1 extensions */
    int32_t GetStreamOperator_V1_1(const sptr<HdiV1_0::IStreamOperatorCallback> &callbackObj,
                                   sptr<HdiV1_1::IStreamOperator> &streamOperator) override {
        sptr<HdiV1_2::IStreamOperator> op12;
        int32_t rc = getStreamOperatorCommon(callbackObj, nullptr, op12);
        if (rc == HdiV1_0::NO_ERROR)
            streamOperator = op12;
        return rc;
    }

    int32_t GetDefaultSettings(std::vector<uint8_t> &settings) override {
        settings = defaultSettings_;
        return HdiV1_0::NO_ERROR;
    }

    /* V1_2 extensions */
    int32_t GetStreamOperator_V1_2(const sptr<HdiV1_2::IStreamOperatorCallback> &callbackObj,
                                   sptr<HdiV1_2::IStreamOperator> &streamOperator) override {
        return getStreamOperatorCommon(nullptr, callbackObj, streamOperator);
    }

    int32_t GetStatus(const std::vector<uint8_t> &metaIn,
                      std::vector<uint8_t> &metaOut) override {
        (void)metaIn;
        metaOut.clear();
        return HdiV1_0::NO_ERROR;
    }

    int32_t Reset() override {
        return HdiV1_0::NO_ERROR;
    }

private:
    int32_t getStreamOperatorCommon(const sptr<HdiV1_0::IStreamOperatorCallback> &cbV10,
                                    const sptr<HdiV1_2::IStreamOperatorCallback> &cbV12,
                                    sptr<HdiV1_2::IStreamOperator> &outOp) {
        if (vdi_ == nullptr || closed_.load())
            return HdiV1_0::CAMERA_CLOSED;
        sptr<VdiV1_0::IStreamOperatorVdiCallback> bridge =
            new (std::nothrow) MarbleStreamCallbackBridge(cbV10, cbV12);
        if (bridge == nullptr)
            return HdiV1_0::INSUFFICIENT_RESOURCES;
        sptr<VdiV1_0::IStreamOperatorVdi> opVdi = nullptr;
        int32_t rc = vdi_->GetStreamOperator(bridge, opVdi);
        if (rc != HdiV1_0::NO_ERROR || opVdi == nullptr)
            return (rc != HdiV1_0::NO_ERROR) ? rc : HdiV1_0::INSUFFICIENT_RESOURCES;
        outOp = new (std::nothrow) MarbleStreamOperatorServiceV1_2(opVdi);
        return (outOp != nullptr) ? HdiV1_0::NO_ERROR : HdiV1_0::INSUFFICIENT_RESOURCES;
    }

    std::string cameraId_;
    sptr<VdiV1_0::ICameraDeviceVdi> vdi_;
    std::vector<uint8_t> defaultSettings_;
    CloseHook onClose_;
    std::atomic<bool> closed_ {false};
};

struct CameraIdMapEntry {
    int32_t index = 0;
    std::string currentCameraId;
    std::string vendorCameraId;
    VdiV1_0::ICameraHostVdi *vdi = nullptr;
};

static std::string vdiCameraIdPrefix(const std::string &id) {
    size_t startPos = id.find("&name=");
    if (startPos != std::string::npos) {
        startPos += kNameStartPos;
        size_t endPos = id.find("&id=");
        if (endPos != std::string::npos && endPos > startPos)
            return id.substr(startPos, endPos - startPos) + "/";
    }
    return "lcam00";
}

class MarbleCameraHostServiceV1_2;

class MarbleHostCallbackBridge : public VdiV1_0::ICameraHostVdiCallback {
public:
    explicit MarbleHostCallbackBridge(MarbleCameraHostServiceV1_2 *owner)
        : owner_(owner) {}

    int32_t OnCameraStatus(const std::string &cameraId,
                           VdiV1_0::VdiCameraStatus status) override;
    int32_t OnFlashlightStatus(const std::string &cameraId,
                               VdiV1_0::VdiFlashlightStatus status) override;
    int32_t OnCameraEvent(const std::string &cameraId,
                          VdiV1_0::VdiCameraEvent event) override;
    const sptr<IRemoteObject> Remote() const override { return nullptr; }

private:
    MarbleCameraHostServiceV1_2 *owner_;
};

class MarbleCameraHostServiceV1_2 : public HdiV1_2::ICameraHost {
public:
    static sptr<MarbleCameraHostServiceV1_2> GetInstance() {
        std::lock_guard<std::mutex> lk(gInstMu_);
        if (gInstance_ != nullptr)
            return gInstance_;

        std::vector<std::string> vdiLibs;
        loadVdiLibList(vdiLibs);

        std::vector<VdiV1_0::ICameraHostVdi *> vdiList;
        std::vector<struct HdfVdiObject *> loaders;
        for (const auto &lib : vdiLibs) {
            struct HdfVdiObject *loader = HdfLoadVdi(lib.c_str());
            if (loader == nullptr || loader->vdiBase == nullptr) {
                CAMERA_LOGW("HdfLoadVdi(%s) failed", lib.c_str());
                continue;
            }
            if (HdfGetVdiVersion(loader) != 1) {
                CAMERA_LOGW("HdfGetVdiVersion(%s) != 1", lib.c_str());
                HdfCloseVdi(loader);
                continue;
            }
            auto *wrap = reinterpret_cast<VdiV1_0::VdiWrapperCameraHost *>(loader->vdiBase);
            if (wrap->module == nullptr) {
                CAMERA_LOGW("VdiWrapperCameraHost(%s) module is null", lib.c_str());
                HdfCloseVdi(loader);
                continue;
            }
            vdiList.push_back(reinterpret_cast<VdiV1_0::ICameraHostVdi *>(wrap->module));
            loaders.push_back(loader);
            CAMERA_LOGI("Loaded Camera VDI: %s", lib.c_str());
        }

        if (vdiList.empty()) {
            CAMERA_LOGE("No Camera VDI modules could be loaded!");
            return nullptr;
        }

        gInstance_ = new (std::nothrow) MarbleCameraHostServiceV1_2(std::move(vdiList),
                                                                    std::move(loaders));
        return gInstance_;
    }

    ~MarbleCameraHostServiceV1_2() override {
        for (auto *loader : loaders_) {
            if (loader != nullptr)
                HdfCloseVdi(loader);
        }
        loaders_.clear();
    }

    int32_t SetCallback(const sptr<HdiV1_0::ICameraHostCallback> &callbackObj) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            cbV10_ = callbackObj;
        }
        registerVdiCallbacks();
        return HdiV1_0::NO_ERROR;
    }

    int32_t GetCameraIds(std::vector<std::string> &cameraIds) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (camMap_.empty())
            refreshCamMapLocked();
        cameraIds.clear();
        for (const auto &e : camMap_)
            cameraIds.push_back(e.currentCameraId);
        return HdiV1_0::NO_ERROR;
    }

    int32_t GetCameraAbility(const std::string &cameraId,
                             std::vector<uint8_t> &cameraAbility) override {
        VdiV1_0::ICameraHostVdi *vdi = nullptr;
        std::string vendorId;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!resolveCameraLocked(cameraId, vdi, vendorId))
                return HdiV1_0::INVALID_ARGUMENT;
        }
        return vdi->GetCameraAbility(vendorId, cameraAbility);
    }

    int32_t OpenCamera(const std::string &cameraId,
                       const sptr<HdiV1_0::ICameraDeviceCallback> &callbackObj,
                       sptr<HdiV1_0::ICameraDevice> &device) override {
        sptr<HdiV1_2::ICameraDevice> dev12;
        int32_t rc = openCameraCommon(cameraId, callbackObj, dev12);
        if (rc == HdiV1_0::NO_ERROR)
            device = dev12;
        return rc;
    }

    int32_t SetFlashlight(const std::string &cameraId, bool isEnable) override {
        bool busy = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            VdiV1_0::ICameraHostVdi *vdi = nullptr;
            std::string vendorId;
            if (!resolveCameraLocked(cameraId, vdi, vendorId))
                return HdiV1_0::INVALID_ARGUMENT;
            if (isEnable && !openDeviceTokens_.empty())
                busy = true;
        }
        if (busy) {
            emitFlashlightStatus(cameraId, HdiV1_0::FLASHLIGHT_UNAVAILABLE);
            return HdiV1_0::CAMERA_BUSY;
        }
        auto res = marble_cam::globalTorchCtrl().setLevel(isEnable ? 1.0f : 0.0f);
        CAMERA_LOGI("SetFlashlight(%s, %d) ok=%d b0=%d b1=%d sw=%d",
                    cameraId.c_str(), isEnable ? 1 : 0, res.ok ? 1 : 0,
                    res.torch0Brightness, res.torch1Brightness, res.switch0Value);
        HdiV1_0::FlashlightStatus st = (isEnable && res.on)
            ? HdiV1_0::FLASHLIGHT_ON : HdiV1_0::FLASHLIGHT_OFF;
        emitFlashlightStatus(cameraId, st);
        return HdiV1_0::NO_ERROR;
    }

    /* V1_1 extensions */
    int32_t OpenCamera_V1_1(const std::string &cameraId,
                            const sptr<HdiV1_0::ICameraDeviceCallback> &callbackObj,
                            sptr<HdiV1_1::ICameraDevice> &device) override {
        sptr<HdiV1_2::ICameraDevice> dev12;
        int32_t rc = openCameraCommon(cameraId, callbackObj, dev12);
        if (rc == HdiV1_0::NO_ERROR)
            device = dev12;
        return rc;
    }

    int32_t Prelaunch(const HdiV1_1::PrelaunchConfig &config) override {
        CAMERA_LOGI("Prelaunch camera=%s", config.cameraId.c_str());
        return HdiV1_0::NO_ERROR;
    }

    /* V1_2 extensions */
    int32_t OpenCamera_V1_2(const std::string &cameraId,
                            const sptr<HdiV1_0::ICameraDeviceCallback> &callbackObj,
                            sptr<HdiV1_2::ICameraDevice> &device) override {
        return openCameraCommon(cameraId, callbackObj, device);
    }

    int32_t NotifyDeviceStateChangeInfo(int32_t notifyType, int32_t deviceState) override {
        CAMERA_LOGI("NotifyDeviceStateChangeInfo type=%d state=%d", notifyType, deviceState);
        return HdiV1_0::NO_ERROR;
    }

    int32_t SetCallback_V1_2(const sptr<HdiV1_2::ICameraHostCallback> &callbackObj) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            cbV12_ = callbackObj;
        }
        registerVdiCallbacks();
        return HdiV1_0::NO_ERROR;
    }

    int32_t SetFlashlight_V1_2(float level) override {
        bool wantOn = (level > 0.0f);
        bool busy = false;
        size_t openCnt = 0;
        std::string primaryCam;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (camMap_.empty())
                refreshCamMapLocked();
            primaryCam = camMap_.empty() ? "lcam001" : camMap_[0].currentCameraId;
            openCnt = openDeviceTokens_.size();
            if (wantOn && openCnt > 0)
                busy = true;
        }
        if (busy) {
            CAMERA_LOGW("SetFlashlight_V1_2(%.3f) rejected: %zu camera(s) open",
                        static_cast<double>(level), openCnt);
            emitFlashlightStatus(primaryCam, HdiV1_0::FLASHLIGHT_UNAVAILABLE);
            return HdiV1_2::CAMERA_BUSY;
        }
        auto res = marble_cam::globalTorchCtrl().setLevel(level);
        CAMERA_LOGI("SetFlashlight_V1_2(level=%.3f) ok=%d on=%d b0=%d b1=%d sw=%d",
                    static_cast<double>(level), res.ok ? 1 : 0, res.on ? 1 : 0,
                    res.torch0Brightness, res.torch1Brightness, res.switch0Value);
        HdiV1_0::FlashlightStatus st = (wantOn && res.on)
            ? HdiV1_0::FLASHLIGHT_ON : HdiV1_0::FLASHLIGHT_OFF;
        emitFlashlightStatus(primaryCam, st);
        return HdiV1_0::NO_ERROR;
    }

    int32_t PreCameraSwitch(const std::string &cameraId) override {
        CAMERA_LOGI("PreCameraSwitch camera=%s", cameraId.c_str());
        return HdiV1_0::NO_ERROR;
    }

    int32_t PrelaunchWithOpMode(const HdiV1_1::PrelaunchConfig &config,
                                int32_t operationMode) override {
        CAMERA_LOGI("PrelaunchWithOpMode camera=%s opMode=%d",
                    config.cameraId.c_str(), operationMode);
        return HdiV1_0::NO_ERROR;
    }

    /* Callbacks forwarded from VDI */
    void onVdiCameraStatus(const std::string &vendorId, VdiV1_0::VdiCameraStatus status) {
        sptr<HdiV1_0::ICameraHostCallback> cb;
        std::string curId;
        {
            std::lock_guard<std::mutex> lk(mu_);
            curId = vendorToCurrentLocked(vendorId);
            cb = activeV10CallbackLocked();
        }
        if (cb != nullptr)
            (void)cb->OnCameraStatus(curId, static_cast<HdiV1_0::CameraStatus>(status));
    }

    void onVdiFlashlightStatus(const std::string &vendorId, VdiV1_0::VdiFlashlightStatus status) {
        std::string curId;
        {
            std::lock_guard<std::mutex> lk(mu_);
            curId = vendorToCurrentLocked(vendorId);
        }
        emitFlashlightStatus(curId, static_cast<HdiV1_0::FlashlightStatus>(status));
    }

    void onVdiCameraEvent(const std::string &vendorId, VdiV1_0::VdiCameraEvent event) {
        sptr<HdiV1_0::ICameraHostCallback> cb;
        std::string curId;
        {
            std::lock_guard<std::mutex> lk(mu_);
            curId = vendorToCurrentLocked(vendorId);
            cb = activeV10CallbackLocked();
        }
        if (cb != nullptr)
            (void)cb->OnCameraEvent(curId, static_cast<HdiV1_0::CameraEvent>(event));
    }

private:
    MarbleCameraHostServiceV1_2(std::vector<VdiV1_0::ICameraHostVdi *> vdiList,
                                std::vector<struct HdfVdiObject *> loaders)
        : vdiList_(std::move(vdiList)), loaders_(std::move(loaders)) {
        std::lock_guard<std::mutex> lk(mu_);
        refreshCamMapLocked();
    }

    static void loadVdiLibList(std::vector<std::string> &outLibs) {
        outLibs.clear();
        ReleaseHcsTree();
        const struct DeviceResourceIface *res = DeviceResourceGetIfaceInstance(HDF_CONFIG_SOURCE);
        if (res != nullptr) {
            SetHcsBlobPath(kCameraHostConfigPath);
            const struct DeviceResourceNode *root = res->GetRootNode();
            if (root != nullptr) {
                int32_t count = res->GetElemNum(root, "vdiLibList");
                for (int32_t i = 0; i < count; ++i) {
                    const char *name = nullptr;
                    if (res->GetStringArrayElem(root, "vdiLibList", i, &name, nullptr) == HDF_SUCCESS &&
                        name != nullptr && name[0] != '\0') {
                        outLibs.emplace_back(name);
                    }
                }
            }
        }
        if (outLibs.empty()) {
            outLibs.emplace_back("libmarble_camera_vdi_impl.z.so");
            outLibs.emplace_back("libcamera_external_usb_camera.so");
        }
    }

    void registerVdiCallbacks() {
        std::lock_guard<std::mutex> lk(mu_);
        if (vdiCbRegistered_)
            return;
        sptr<VdiV1_0::ICameraHostVdiCallback> bridge =
            new (std::nothrow) MarbleHostCallbackBridge(this);
        if (bridge == nullptr)
            return;
        for (auto *vdi : vdiList_) {
            if (vdi != nullptr)
                (void)vdi->SetCallback(bridge);
        }
        vdiCbRegistered_ = true;
    }

    void refreshCamMapLocked() {
        camMap_.clear();
        int32_t idx = 1;
        for (auto *vdi : vdiList_) {
            if (vdi == nullptr)
                continue;
            std::vector<std::string> ids;
            if (vdi->GetCameraIds(ids) != VdiV1_0::NO_ERROR)
                continue;
            for (const auto &vid : ids) {
                CameraIdMapEntry e;
                e.index = idx;
                e.currentCameraId = vdiCameraIdPrefix(vid) + std::to_string(idx);
                e.vendorCameraId = vid;
                e.vdi = vdi;
                camMap_.push_back(std::move(e));
                idx++;
            }
        }
    }

    bool resolveCameraLocked(const std::string &cameraId,
                             VdiV1_0::ICameraHostVdi *&outVdi,
                             std::string &outVendorId) {
        if (camMap_.empty())
            refreshCamMapLocked();
        for (const auto &e : camMap_) {
            if (e.currentCameraId == cameraId) {
                outVdi = e.vdi;
                outVendorId = e.vendorCameraId;
                return outVdi != nullptr;
            }
        }
        return false;
    }

    std::string vendorToCurrentLocked(const std::string &vendorId) const {
        for (const auto &e : camMap_) {
            if (e.vendorCameraId == vendorId)
                return e.currentCameraId;
        }
        return vendorId;
    }

    sptr<HdiV1_0::ICameraHostCallback> activeV10CallbackLocked() const {
        if (cbV10_ != nullptr)
            return cbV10_;
        if (cbV12_ != nullptr)
            return static_cast<HdiV1_0::ICameraHostCallback *>(cbV12_.GetRefPtr());
        return nullptr;
    }

    void emitFlashlightStatus(const std::string &cameraId, HdiV1_0::FlashlightStatus st) {
        sptr<HdiV1_2::ICameraHostCallback> cb12;
        sptr<HdiV1_0::ICameraHostCallback> cb10;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cb12 = cbV12_;
            cb10 = cbV10_;
        }
        if (cb12 != nullptr)
            (void)cb12->OnFlashlightStatus_V1_2(st);
        if (cb10 != nullptr)
            (void)cb10->OnFlashlightStatus(cameraId, st);
    }

    int32_t openCameraCommon(const std::string &cameraId,
                             const sptr<HdiV1_0::ICameraDeviceCallback> &callbackObj,
                             sptr<HdiV1_2::ICameraDevice> &device) {
        VdiV1_0::ICameraHostVdi *vdi = nullptr;
        std::string vendorId;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!resolveCameraLocked(cameraId, vdi, vendorId))
                return HdiV1_0::INVALID_ARGUMENT;
        }

        /* Turn off torch before opening camera and emit FLASHLIGHT_UNAVAILABLE. */
        if (marble_cam::globalTorchCtrl().isOn())
            (void)marble_cam::globalTorchCtrl().setLevel(0.0f);
        emitFlashlightStatus(cameraId, HdiV1_0::FLASHLIGHT_UNAVAILABLE);

        sptr<VdiV1_0::ICameraDeviceVdiCallback> cbBridge =
            new (std::nothrow) MarbleDeviceCallbackBridge(callbackObj);
        if (cbBridge == nullptr)
            return HdiV1_0::INSUFFICIENT_RESOURCES;

        sptr<VdiV1_0::ICameraDeviceVdi> devVdi = nullptr;
        int32_t rc = vdi->OpenCamera(vendorId, cbBridge, devVdi);
        if (rc != HdiV1_0::NO_ERROR || devVdi == nullptr) {
            emitFlashlightStatus(cameraId, HdiV1_0::FLASHLIGHT_OFF);
            return (rc != HdiV1_0::NO_ERROR) ? rc : HdiV1_0::INSUFFICIENT_RESOURCES;
        }

        std::vector<uint8_t> abilityVec;
        (void)vdi->GetCameraAbility(vendorId, abilityVec);

        sptr<MarbleCameraDeviceServiceV1_2> devSvc = new (std::nothrow) MarbleCameraDeviceServiceV1_2(
            cameraId, devVdi, std::move(abilityVec),
            [this](const std::string &cid, uintptr_t token) {
                onDeviceClosed(cid, token);
            });
        if (devSvc == nullptr) {
            (void)devVdi->Close();
            emitFlashlightStatus(cameraId, HdiV1_0::FLASHLIGHT_OFF);
            return HdiV1_0::INSUFFICIENT_RESOURCES;
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            openDeviceTokens_.insert(reinterpret_cast<uintptr_t>(devSvc.GetRefPtr()));
        }
        device = devSvc;
        return HdiV1_0::NO_ERROR;
    }

    void onDeviceClosed(const std::string &cameraId, uintptr_t token) {
        bool allClosed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            openDeviceTokens_.erase(token);
            allClosed = openDeviceTokens_.empty();
        }
        if (allClosed) {
            if (marble_cam::globalTorchCtrl().isOn())
                (void)marble_cam::globalTorchCtrl().setLevel(0.0f);
            emitFlashlightStatus(cameraId, HdiV1_0::FLASHLIGHT_OFF);
        }
    }

    static std::mutex gInstMu_;
    static sptr<MarbleCameraHostServiceV1_2> gInstance_;

    std::mutex mu_;
    std::vector<VdiV1_0::ICameraHostVdi *> vdiList_;
    std::vector<struct HdfVdiObject *> loaders_;
    std::vector<CameraIdMapEntry> camMap_;
    sptr<HdiV1_0::ICameraHostCallback> cbV10_;
    sptr<HdiV1_2::ICameraHostCallback> cbV12_;
    std::set<uintptr_t> openDeviceTokens_;
    bool vdiCbRegistered_ = false;
};

std::mutex MarbleCameraHostServiceV1_2::gInstMu_;
sptr<MarbleCameraHostServiceV1_2> MarbleCameraHostServiceV1_2::gInstance_ = nullptr;

int32_t MarbleHostCallbackBridge::OnCameraStatus(const std::string &cameraId,
                                                 VdiV1_0::VdiCameraStatus status) {
    if (owner_ != nullptr)
        owner_->onVdiCameraStatus(cameraId, status);
    return VdiV1_0::NO_ERROR;
}

int32_t MarbleHostCallbackBridge::OnFlashlightStatus(const std::string &cameraId,
                                                     VdiV1_0::VdiFlashlightStatus status) {
    if (owner_ != nullptr)
        owner_->onVdiFlashlightStatus(cameraId, status);
    return VdiV1_0::NO_ERROR;
}

int32_t MarbleHostCallbackBridge::OnCameraEvent(const std::string &cameraId,
                                                VdiV1_0::VdiCameraEvent event) {
    if (owner_ != nullptr)
        owner_->onVdiCameraEvent(cameraId, event);
    return VdiV1_0::NO_ERROR;
}

struct HdfCameraHostHost {
    struct IDeviceIoService ioService;
    OHOS::sptr<OHOS::IRemoteObject> stub;
};

static int32_t CameraHostDriverDispatch(struct HdfDeviceIoClient *client, int cmdId,
                                        struct HdfSBuf *data, struct HdfSBuf *reply) {
    auto *host = CONTAINER_OF(client->device->service, struct HdfCameraHostHost, ioService);
    OHOS::MessageParcel *dataParcel = nullptr;
    OHOS::MessageParcel *replyParcel = nullptr;
    OHOS::MessageOption option;

    if (SbufToParcel(data, &dataParcel) != HDF_SUCCESS) {
        CAMERA_LOGE("CameraHostDriverDispatch: invalid data sbuf");
        return HDF_ERR_INVALID_PARAM;
    }
    if (SbufToParcel(reply, &replyParcel) != HDF_SUCCESS) {
        CAMERA_LOGE("CameraHostDriverDispatch: invalid reply sbuf");
        return HDF_ERR_INVALID_PARAM;
    }
    return host->stub->SendRequest(static_cast<uint32_t>(cmdId), *dataParcel, *replyParcel, option);
}

static int HdfCameraHostDriverInit(struct HdfDeviceObject *deviceObject) {
    CAMERA_LOGI("Marble CameraHost V1_2 DriverInit");
    if (deviceObject == nullptr)
        return HDF_FAILURE;
    if (!HdfDeviceSetClass(deviceObject, DEVICE_CLASS_CAMERA)) {
        CAMERA_LOGE("HdfDeviceSetClass(DEVICE_CLASS_CAMERA) failed");
        return HDF_FAILURE;
    }
    return HDF_SUCCESS;
}

static int HdfCameraHostDriverBind(struct HdfDeviceObject *deviceObject) {
    CAMERA_LOGI("Marble CameraHost V1_2 DriverBind");
    if (deviceObject == nullptr)
        return HDF_FAILURE;

    auto *host = new (std::nothrow) HdfCameraHostHost();
    if (host == nullptr)
        return HDF_FAILURE;

    host->ioService.Dispatch = CameraHostDriverDispatch;
    host->ioService.Open = nullptr;
    host->ioService.Release = nullptr;

    OHOS::sptr<HdiV1_2::ICameraHost> serviceImpl = MarbleCameraHostServiceV1_2::GetInstance();
    if (serviceImpl == nullptr) {
        CAMERA_LOGE("MarbleCameraHostServiceV1_2::GetInstance failed");
        delete host;
        return HDF_FAILURE;
    }

    host->stub = OHOS::HDI::ObjectCollector::GetInstance().GetOrNewObject(
        serviceImpl, HdiV1_2::ICameraHost::GetDescriptor());
    if (host->stub == nullptr) {
        CAMERA_LOGE("ObjectCollector::GetOrNewObject(v1_2.ICameraHost) failed");
        delete host;
        return HDF_FAILURE;
    }

    deviceObject->service = &host->ioService;
    CAMERA_LOGI("Marble CameraHost V1_2 bound successfully (version 1.2)");
    return HDF_SUCCESS;
}

static void HdfCameraHostDriverRelease(struct HdfDeviceObject *deviceObject) {
    CAMERA_LOGI("Marble CameraHost V1_2 DriverRelease");
    if (deviceObject == nullptr || deviceObject->service == nullptr)
        return;
    auto *host = CONTAINER_OF(deviceObject->service, struct HdfCameraHostHost, ioService);
    delete host;
    deviceObject->service = nullptr;
}

static struct HdfDriverEntry g_marbleCameraHostV12DriverEntry = {
    .moduleVersion = 1,
    .moduleName = "camera_service",
    .Bind = HdfCameraHostDriverBind,
    .Init = HdfCameraHostDriverInit,
    .Release = HdfCameraHostDriverRelease,
};

} // namespace
} // namespace OHOS::Camera

extern "C" {
HDF_INIT(OHOS::Camera::g_marbleCameraHostV12DriverEntry);
}
