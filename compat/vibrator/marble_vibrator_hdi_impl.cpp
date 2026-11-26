/* SPDX-License-Identifier: MIT */
/*
 * marble_vibrator_hdi_impl.cpp - Clean-room SM7475 (Xiaomi marble) OHOS Vibrator HDI v2.0.
 *
 * SONAME: libvibrator_interface_service_2.0.z.so (bind-mounted over donor service).
 * Loaded by unmodified donor libvibrator_driver.z.so + libvibrator_stub_2.0.z.so inside
 * HDF host-15 (vibrator_host) via VibratorInterfaceImplGetInstance().
 *
 * Hardware backend:
 *   - Auto-discovers SM7475 qcom-hv-haptics (/dev/input/event* whose EVIOCGNAME contains
 *     "haptics" or "vibrator").
 *   - Strictly uses FF_CONSTANT (0x52) (never FF_RUMBLE, which qcom-hv-haptics rejects with -EINVAL).
 *   - Background worker thread handles multi-segment HapticPaket / VibratorPackage /
 *     HdfCompositeEffect / multi-pulse presets with instant preemption & cancellation.
 */

#include "v2_0/ivibrator_interface.h"
#include "marble_vibrator_backend.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Vibrator {
namespace V2_0 {

namespace {

constexpr int32_t kVibratorNotPeriod = -1;
constexpr int32_t kVibratorNotIntensity = -2;
constexpr int32_t kLocalDeviceId = -1;
constexpr int32_t kLocalVibratorId = 1;
constexpr int32_t kDefaultStartUpTimeMs = 5;

static bool isLocalDeviceMatch(const DeviceVibratorInfo &info) {
    /*
     * Donor OHOS callers pass deviceId == -1 (default local device) or 0 (zero-init struct),
     * and vibratorId == 1, 0, or -1 for the single onboard LRA motor.
     */
    const bool devOk = (info.deviceId == kLocalDeviceId || info.deviceId == 0);
    const bool vibOk = (info.vibratorId == kLocalVibratorId || info.vibratorId == 0 || info.vibratorId == -1);
    return devOk && vibOk;
}

static HdfVibratorInfo makeMarbleVibratorInfo() {
    HdfVibratorInfo info{};
    info.isSupportIntensity = true;
    info.isSupportFrequency = false;
    info.isSupportHdHaptic = true;
    info.intensityMaxValue = marble_vibrator::kMaxIntensity;
    info.intensityMinValue = marble_vibrator::kMinIntensity;
    info.frequencyMaxValue = 0;
    info.frequencyMinValue = 0;
    info.deviceId = kLocalDeviceId;
    info.vibratorId = kLocalVibratorId;
    info.position = 0;
    info.isLocal = 1;
    info.reserved = 0;
    return info;
}

static int32_t mapBackendResult(int rc) {
    if (rc == 0)
        return HDF_SUCCESS;
    if (rc == -EINVAL)
        return HDF_ERR_INVALID_PARAM;
    if (rc == -ENODEV)
        return HDF_DEV_ERR_NO_DEVICE;
    return HDF_FAILURE;
}

} // namespace

class VibratorIfService : public IVibratorInterface {
public:
    VibratorIfService() : controller_(&transport_) {
        (void)controller_.probeDevice();
    }

    ~VibratorIfService() override {
        controller_.shutdown();
    }

    int32_t StartOnce(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t duration) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        return mapBackendResult(controller_.startOnce(duration, marble_vibrator::kDefaultOnceIntensity));
    }

    int32_t Start(const DeviceVibratorInfo &deviceVibratorInfo, const std::string &effectType) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo) || effectType.empty())
            return HDF_ERR_INVALID_PARAM;
        marble_vibrator::PresetResolution preset = marble_vibrator::resolvePresetEffect(effectType);
        if (preset.isNull)
            return mapBackendResult(controller_.stopAll());
        return mapBackendResult(controller_.startSegments(preset.segments, 0));
    }

    int32_t Stop(const DeviceVibratorInfo &deviceVibratorInfo, HdfVibratorMode mode) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        if (mode < HDF_VIBRATOR_MODE_ONCE || mode > HDF_VIBRATOR_MODE_BUTT)
            return HDF_ERR_INVALID_PARAM;
        return mapBackendResult(controller_.stopAll());
    }

    int32_t GetVibratorInfo(std::vector<HdfVibratorInfo> &vibratorInfo) override {
        vibratorInfo.clear();
        vibratorInfo.push_back(makeMarbleVibratorInfo());
        return HDF_SUCCESS;
    }

    int32_t GetDeviceVibratorInfo(const DeviceVibratorInfo &deviceVibratorInfo,
                                  std::vector<HdfVibratorInfo> &vibratorInfo) override {
        vibratorInfo.clear();
        if (deviceVibratorInfo.deviceId != kLocalDeviceId && deviceVibratorInfo.deviceId != 0)
            return HDF_ERR_NOT_SUPPORT;
        vibratorInfo.push_back(makeMarbleVibratorInfo());
        return HDF_SUCCESS;
    }

    int32_t GetVibratorIdSingle(const DeviceVibratorInfo &deviceVibratorInfo,
                                std::vector<HdfVibratorInfo> &vibratorInfo) override {
        vibratorInfo.clear();
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_NOT_SUPPORT;
        vibratorInfo.push_back(makeMarbleVibratorInfo());
        return HDF_SUCCESS;
    }

    int32_t EnableVibratorModulation(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t duration,
                                     uint16_t intensity, int16_t frequency) override {
        (void)frequency;
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        if (duration == 0)
            return kVibratorNotPeriod;
        if (intensity < marble_vibrator::kMinIntensity || intensity > marble_vibrator::kMaxIntensity)
            return kVibratorNotIntensity;
        return mapBackendResult(controller_.startOnce(duration, intensity));
    }

    int32_t EnableCompositeEffect(const DeviceVibratorInfo &deviceVibratorInfo,
                                  const HdfCompositeEffect &effect) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        std::vector<marble_vibrator::VibratorSegment> segs;
        if (!marble_vibrator::buildSegmentsFromComposite(effect, segs))
            return HDF_ERR_INVALID_PARAM;
        return mapBackendResult(controller_.startSegments(segs, 0));
    }

    int32_t GetEffectInfo(const DeviceVibratorInfo &deviceVibratorInfo, const std::string &effectType,
                          HdfEffectInfo &effectInfo) override {
        effectInfo.duration = 0;
        effectInfo.isSupportEffect = false;
        if (!isLocalDeviceMatch(deviceVibratorInfo) || effectType.empty())
            return HDF_ERR_INVALID_PARAM;

        marble_vibrator::PresetResolution preset = marble_vibrator::resolvePresetEffect(effectType);
        if (preset.known) {
            effectInfo.isSupportEffect = true;
            effectInfo.duration = static_cast<int32_t>(preset.totalDurationMs);
        } else {
            effectInfo.isSupportEffect = false;
            effectInfo.duration = 0;
        }
        return HDF_SUCCESS;
    }

    int32_t IsVibratorRunning(const DeviceVibratorInfo &deviceVibratorInfo, bool &state) override {
        state = false;
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        state = controller_.isRunning();
        return HDF_SUCCESS;
    }

    int32_t PlayHapticPattern(const DeviceVibratorInfo &deviceVibratorInfo, const HapticPaket &pkg) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        std::vector<marble_vibrator::VibratorSegment> segs;
        if (!marble_vibrator::buildSegmentsFromPaket(pkg, segs))
            return HDF_ERR_INVALID_PARAM;
        return mapBackendResult(controller_.startSegments(segs, 0));
    }

    int32_t GetHapticCapacity(const DeviceVibratorInfo &deviceVibratorInfo,
                              HapticCapacity &hapticCapacity) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        hapticCapacity.isSupportHdHaptic = true;
        hapticCapacity.isSupportPresetMapping = true;
        hapticCapacity.isSupportTimeDelay = true;
        hapticCapacity.vibratorId = kLocalVibratorId;
        hapticCapacity.reserved0 = false;
        hapticCapacity.reserved1 = 0;
        return HDF_SUCCESS;
    }

    int32_t GetHapticStartUpTime(const DeviceVibratorInfo &deviceVibratorInfo, int32_t mode,
                                 int32_t &startUpTime) override {
        (void)mode;
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        startUpTime = kDefaultStartUpTimeMs;
        return HDF_SUCCESS;
    }

    int32_t StartByIntensity(const DeviceVibratorInfo &deviceVibratorInfo, const std::string &effectType,
                             uint16_t intensity) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo) || effectType.empty())
            return HDF_ERR_INVALID_PARAM;
        if (intensity < marble_vibrator::kMinIntensity || intensity > marble_vibrator::kMaxIntensity)
            return HDF_ERR_INVALID_PARAM;

        marble_vibrator::PresetResolution preset = marble_vibrator::resolvePresetEffect(effectType);
        if (preset.isNull)
            return mapBackendResult(controller_.stopAll());
        for (auto &seg : preset.segments)
            seg.intensity = intensity;
        return mapBackendResult(controller_.startSegments(preset.segments, 0));
    }

    int32_t GetAllWaveInfo(const DeviceVibratorInfo &deviceVibratorInfo,
                           std::vector<HdfWaveInformation> &info) override {
        info.clear();
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        static const HdfWaveInformation kWaves[] = {
            {kLocalVibratorId, 1, 0.45f, 170.0f, 20, 0},
            {kLocalVibratorId, 2, 0.65f, 170.0f, 15, 0},
            {kLocalVibratorId, 3, 0.85f, 170.0f, 25, 0},
            {kLocalVibratorId, 4, 0.95f, 170.0f, 50, 0},
            {kLocalVibratorId, 5, 0.75f, 170.0f, 40, 0},
            {kLocalVibratorId, 6, 0.80f, 170.0f, 80, 0},
        };
        info.assign(kWaves, kWaves + (sizeof(kWaves) / sizeof(kWaves[0])));
        return HDF_SUCCESS;
    }

    int32_t RegVibratorPlugCallback(const sptr<IVibratorPlugCallback> &callbackObj) override {
        if (callbackObj == nullptr)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(cbMu_);
        if (std::find(plugCbs_.begin(), plugCbs_.end(), callbackObj) == plugCbs_.end())
            plugCbs_.push_back(callbackObj);
        return HDF_SUCCESS;
    }

    int32_t UnRegVibratorPlugCallback(const sptr<IVibratorPlugCallback> &callbackObj) override {
        if (callbackObj == nullptr)
            return HDF_ERR_INVALID_PARAM;
        std::lock_guard<std::mutex> lk(cbMu_);
        plugCbs_.erase(std::remove(plugCbs_.begin(), plugCbs_.end(), callbackObj), plugCbs_.end());
        return HDF_SUCCESS;
    }

    int32_t PlayPatternBySessionId(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t sessionId,
                                   const HapticPaket &hapticPaket) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        std::vector<marble_vibrator::VibratorSegment> segs;
        if (!marble_vibrator::buildSegmentsFromPaket(hapticPaket, segs))
            return HDF_ERR_INVALID_PARAM;
        return mapBackendResult(controller_.startSegments(segs, sessionId));
    }

    int32_t PlayPackageBySession(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t sessionId,
                                 const VibratorPackage &vibratorPackage) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        std::vector<marble_vibrator::VibratorSegment> segs;
        if (!marble_vibrator::buildSegmentsFromPackage(vibratorPackage, segs))
            return HDF_ERR_INVALID_PARAM;
        return mapBackendResult(controller_.startSegments(segs, sessionId));
    }

    int32_t StopVibrateBySessionId(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t sessionId) override {
        if (!isLocalDeviceMatch(deviceVibratorInfo))
            return HDF_ERR_INVALID_PARAM;
        return mapBackendResult(controller_.stopBySessionId(sessionId));
    }

private:
    marble_vibrator::LinuxEvdevTransport transport_;
    marble_vibrator::VibratorController controller_;
    std::mutex cbMu_;
    std::vector<sptr<IVibratorPlugCallback>> plugCbs_;
};

extern "C" IVibratorInterface *VibratorInterfaceImplGetInstance(void) {
    return new (std::nothrow) VibratorIfService();
}

} // namespace V2_0
} // namespace Vibrator
} // namespace HDI
} // namespace OHOS
