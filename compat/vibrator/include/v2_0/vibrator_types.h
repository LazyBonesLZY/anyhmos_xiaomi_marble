/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_VIBRATOR_V2_0_VIBRATOR_TYPES_H
#define OHOS_HDI_VIBRATOR_V2_0_VIBRATOR_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Vibrator {
namespace V2_0 {

/*
 * Clean-room Vibrator HDI v2.0 types binary-verified against ADY-AL10 donor
 * libvibrator_stub_2.0.z.so, libvibrator_proxy_2.0.z.so, and
 * libvibrator_interface_service_2.0.z.so.
 */

struct DeviceVibratorInfo {
    int32_t deviceId;
    int32_t vibratorId;
};
static_assert(sizeof(DeviceVibratorInfo) == 8, "DeviceVibratorInfo ABI size mismatch");
static_assert(offsetof(DeviceVibratorInfo, deviceId) == 0, "DeviceVibratorInfo::deviceId offset mismatch");
static_assert(offsetof(DeviceVibratorInfo, vibratorId) == 4, "DeviceVibratorInfo::vibratorId offset mismatch");
static_assert(std::is_trivially_copyable<DeviceVibratorInfo>::value, "DeviceVibratorInfo must be POD");

enum HdfVibratorMode : int32_t {
    HDF_VIBRATOR_MODE_ONCE = 0,
    HDF_VIBRATOR_MODE_PRESET = 1,
    HDF_VIBRATOR_MODE_HDHAPTIC = 2,
    HDF_VIBRATOR_MODE_BUTT = 3,
};
static_assert(sizeof(HdfVibratorMode) == 4, "HdfVibratorMode ABI size mismatch");

enum HdfEffectType : int32_t {
    HDF_EFFECT_TYPE_TIME = 0,
    HDF_EFFECT_TYPE_PRIMITIVE = 1,
    HDF_EFFECT_TYPE_BUTT = 2,
};
static_assert(sizeof(HdfEffectType) == 4, "HdfEffectType ABI size mismatch");

struct HdfVibratorInfo {
    bool isSupportIntensity;
    bool isSupportFrequency;
    bool isSupportHdHaptic;
    uint16_t intensityMaxValue;
    uint16_t intensityMinValue;
    int16_t frequencyMaxValue;
    int16_t frequencyMinValue;
    int32_t deviceId;
    int32_t vibratorId;
    int32_t position;
    int32_t isLocal;
    int32_t reserved;
};
static_assert(sizeof(HdfVibratorInfo) == 32, "HdfVibratorInfo ABI size mismatch (donor is 32B)");
static_assert(offsetof(HdfVibratorInfo, isSupportIntensity) == 0, "HdfVibratorInfo::isSupportIntensity offset mismatch");
static_assert(offsetof(HdfVibratorInfo, isSupportFrequency) == 1, "HdfVibratorInfo::isSupportFrequency offset mismatch");
static_assert(offsetof(HdfVibratorInfo, isSupportHdHaptic) == 2, "HdfVibratorInfo::isSupportHdHaptic offset mismatch");
static_assert(offsetof(HdfVibratorInfo, intensityMaxValue) == 4, "HdfVibratorInfo::intensityMaxValue offset mismatch");
static_assert(offsetof(HdfVibratorInfo, intensityMinValue) == 6, "HdfVibratorInfo::intensityMinValue offset mismatch");
static_assert(offsetof(HdfVibratorInfo, frequencyMaxValue) == 8, "HdfVibratorInfo::frequencyMaxValue offset mismatch");
static_assert(offsetof(HdfVibratorInfo, frequencyMinValue) == 10, "HdfVibratorInfo::frequencyMinValue offset mismatch");
static_assert(offsetof(HdfVibratorInfo, deviceId) == 12, "HdfVibratorInfo::deviceId offset mismatch");
static_assert(offsetof(HdfVibratorInfo, vibratorId) == 16, "HdfVibratorInfo::vibratorId offset mismatch");
static_assert(offsetof(HdfVibratorInfo, position) == 20, "HdfVibratorInfo::position offset mismatch");
static_assert(offsetof(HdfVibratorInfo, isLocal) == 24, "HdfVibratorInfo::isLocal offset mismatch");
static_assert(offsetof(HdfVibratorInfo, reserved) == 28, "HdfVibratorInfo::reserved offset mismatch");
static_assert(std::is_trivially_copyable<HdfVibratorInfo>::value, "HdfVibratorInfo must be POD");

struct TimeEffect {
    int32_t delay;
    int32_t time;
    uint16_t intensity;
    int16_t frequency;
    int32_t reserved;
};
static_assert(sizeof(TimeEffect) == 16, "TimeEffect ABI size mismatch (donor is 16B)");
static_assert(offsetof(TimeEffect, delay) == 0, "TimeEffect::delay offset mismatch");
static_assert(offsetof(TimeEffect, time) == 4, "TimeEffect::time offset mismatch");
static_assert(offsetof(TimeEffect, intensity) == 8, "TimeEffect::intensity offset mismatch");
static_assert(offsetof(TimeEffect, frequency) == 10, "TimeEffect::frequency offset mismatch");
static_assert(offsetof(TimeEffect, reserved) == 12, "TimeEffect::reserved offset mismatch");
static_assert(std::is_trivially_copyable<TimeEffect>::value, "TimeEffect must be POD");

struct PrimitiveEffect {
    int32_t delay;
    int32_t effectId;
    uint16_t intensity;
    int32_t reserved;
};
static_assert(sizeof(PrimitiveEffect) == 16, "PrimitiveEffect ABI size mismatch (donor is 16B)");
static_assert(offsetof(PrimitiveEffect, delay) == 0, "PrimitiveEffect::delay offset mismatch");
static_assert(offsetof(PrimitiveEffect, effectId) == 4, "PrimitiveEffect::effectId offset mismatch");
static_assert(offsetof(PrimitiveEffect, intensity) == 8, "PrimitiveEffect::intensity offset mismatch");
static_assert(offsetof(PrimitiveEffect, reserved) == 12, "PrimitiveEffect::reserved offset mismatch");
static_assert(std::is_trivially_copyable<PrimitiveEffect>::value, "PrimitiveEffect must be POD");

union CompositeEffect {
    TimeEffect timeEffect;
    PrimitiveEffect primitiveEffect;
};
static_assert(sizeof(CompositeEffect) == 16, "CompositeEffect ABI size mismatch");
static_assert(std::is_trivially_copyable<CompositeEffect>::value, "CompositeEffect must be POD");

struct HdfCompositeEffect {
    int32_t type;
    std::vector<CompositeEffect> compositeEffects;
};
static_assert(sizeof(HdfCompositeEffect) == 32, "HdfCompositeEffect ABI size mismatch");
static_assert(offsetof(HdfCompositeEffect, type) == 0, "HdfCompositeEffect::type offset mismatch");
static_assert(offsetof(HdfCompositeEffect, compositeEffects) == 8, "HdfCompositeEffect::compositeEffects offset mismatch");

struct HdfEffectInfo {
    int32_t duration;
    bool isSupportEffect;
};
static_assert(sizeof(HdfEffectInfo) == 8, "HdfEffectInfo ABI size mismatch");
static_assert(offsetof(HdfEffectInfo, duration) == 0, "HdfEffectInfo::duration offset mismatch");
static_assert(offsetof(HdfEffectInfo, isSupportEffect) == 4, "HdfEffectInfo::isSupportEffect offset mismatch");
static_assert(std::is_trivially_copyable<HdfEffectInfo>::value, "HdfEffectInfo must be POD");

enum EVENT_TYPE : int32_t {
    CONTINUOUS = 0,
    TRANSIENT = 1,
};
static_assert(sizeof(EVENT_TYPE) == 4, "EVENT_TYPE ABI size mismatch");

struct CurvePoint {
    int32_t time;
    int32_t intensity;
    int32_t frequency;
    int32_t reserved;
};
static_assert(sizeof(CurvePoint) == 16, "CurvePoint ABI size mismatch (donor is 16B)");
static_assert(offsetof(CurvePoint, time) == 0, "CurvePoint::time offset mismatch");
static_assert(offsetof(CurvePoint, intensity) == 4, "CurvePoint::intensity offset mismatch");
static_assert(offsetof(CurvePoint, frequency) == 8, "CurvePoint::frequency offset mismatch");
static_assert(offsetof(CurvePoint, reserved) == 12, "CurvePoint::reserved offset mismatch");
static_assert(std::is_trivially_copyable<CurvePoint>::value, "CurvePoint must be POD");

struct HapticEvent {
    EVENT_TYPE type;
    int32_t time;
    int32_t duration;
    int32_t intensity;
    int32_t frequency;
    int32_t index;
    int32_t pointNum;
    std::vector<CurvePoint> points;
};
static_assert(sizeof(HapticEvent) == 56, "HapticEvent ABI size mismatch");
static_assert(offsetof(HapticEvent, type) == 0, "HapticEvent::type offset mismatch");
static_assert(offsetof(HapticEvent, time) == 4, "HapticEvent::time offset mismatch");
static_assert(offsetof(HapticEvent, duration) == 8, "HapticEvent::duration offset mismatch");
static_assert(offsetof(HapticEvent, intensity) == 12, "HapticEvent::intensity offset mismatch");
static_assert(offsetof(HapticEvent, frequency) == 16, "HapticEvent::frequency offset mismatch");
static_assert(offsetof(HapticEvent, index) == 20, "HapticEvent::index offset mismatch");
static_assert(offsetof(HapticEvent, pointNum) == 24, "HapticEvent::pointNum offset mismatch");
static_assert(offsetof(HapticEvent, points) == 32, "HapticEvent::points offset mismatch");

struct HapticPaket {
    int32_t time;
    int32_t eventNum;
    std::vector<HapticEvent> events;
};
static_assert(sizeof(HapticPaket) == 32, "HapticPaket ABI size mismatch");
static_assert(offsetof(HapticPaket, time) == 0, "HapticPaket::time offset mismatch");
static_assert(offsetof(HapticPaket, eventNum) == 4, "HapticPaket::eventNum offset mismatch");
static_assert(offsetof(HapticPaket, events) == 8, "HapticPaket::events offset mismatch");

struct VibratorPackage {
    int32_t patternNum;
    int32_t packageduration;
    std::vector<HapticPaket> patterns;
};
static_assert(sizeof(VibratorPackage) == 32, "VibratorPackage ABI size mismatch");
static_assert(offsetof(VibratorPackage, patternNum) == 0, "VibratorPackage::patternNum offset mismatch");
static_assert(offsetof(VibratorPackage, packageduration) == 4, "VibratorPackage::packageduration offset mismatch");
static_assert(offsetof(VibratorPackage, patterns) == 8, "VibratorPackage::patterns offset mismatch");

struct HapticCapacity {
    bool isSupportHdHaptic;
    bool isSupportPresetMapping;
    bool isSupportTimeDelay;
    int32_t vibratorId;
    bool reserved0;
    int32_t reserved1;
};
static_assert(sizeof(HapticCapacity) == 16, "HapticCapacity ABI size mismatch");
static_assert(offsetof(HapticCapacity, isSupportHdHaptic) == 0, "HapticCapacity::isSupportHdHaptic offset mismatch");
static_assert(offsetof(HapticCapacity, isSupportPresetMapping) == 1, "HapticCapacity::isSupportPresetMapping offset mismatch");
static_assert(offsetof(HapticCapacity, isSupportTimeDelay) == 2, "HapticCapacity::isSupportTimeDelay offset mismatch");
static_assert(offsetof(HapticCapacity, vibratorId) == 4, "HapticCapacity::vibratorId offset mismatch");
static_assert(offsetof(HapticCapacity, reserved0) == 8, "HapticCapacity::reserved0 offset mismatch");
static_assert(offsetof(HapticCapacity, reserved1) == 12, "HapticCapacity::reserved1 offset mismatch");
static_assert(std::is_trivially_copyable<HapticCapacity>::value, "HapticCapacity must be POD");

struct HdfWaveInformation {
    int32_t vibratorId;
    int32_t waveId;
    float intensity;
    float frequency;
    int32_t duration;
    int32_t reserved;
};
static_assert(sizeof(HdfWaveInformation) == 24, "HdfWaveInformation ABI size mismatch");
static_assert(offsetof(HdfWaveInformation, vibratorId) == 0, "HdfWaveInformation::vibratorId offset mismatch");
static_assert(offsetof(HdfWaveInformation, waveId) == 4, "HdfWaveInformation::waveId offset mismatch");
static_assert(offsetof(HdfWaveInformation, intensity) == 8, "HdfWaveInformation::intensity offset mismatch");
static_assert(offsetof(HdfWaveInformation, frequency) == 12, "HdfWaveInformation::frequency offset mismatch");
static_assert(offsetof(HdfWaveInformation, duration) == 16, "HdfWaveInformation::duration offset mismatch");
static_assert(offsetof(HdfWaveInformation, reserved) == 20, "HdfWaveInformation::reserved offset mismatch");
static_assert(std::is_trivially_copyable<HdfWaveInformation>::value, "HdfWaveInformation must be POD");

struct HdfVibratorPlugInfo {
    int32_t deviceId;
    std::string deviceName;
    int32_t vibratorCnt;
    int32_t status;
};
#if defined(_LIBCPP_VERSION)
static_assert(sizeof(HdfVibratorPlugInfo) == 40, "HdfVibratorPlugInfo OHOS libc++ ABI size mismatch");
static_assert(offsetof(HdfVibratorPlugInfo, deviceId) == 0, "HdfVibratorPlugInfo::deviceId offset mismatch");
static_assert(offsetof(HdfVibratorPlugInfo, deviceName) == 8, "HdfVibratorPlugInfo::deviceName offset mismatch");
static_assert(offsetof(HdfVibratorPlugInfo, vibratorCnt) == 32, "HdfVibratorPlugInfo::vibratorCnt offset mismatch");
static_assert(offsetof(HdfVibratorPlugInfo, status) == 36, "HdfVibratorPlugInfo::status offset mismatch");
#endif

} // namespace V2_0
} // namespace Vibrator
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_VIBRATOR_V2_0_VIBRATOR_TYPES_H
