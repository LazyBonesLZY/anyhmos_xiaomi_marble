/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_VIBRATOR_V2_0_IVIBRATOR_INTERFACE_H
#define OHOS_HDI_VIBRATOR_V2_0_IVIBRATOR_INTERFACE_H

#include <cstdint>
#include <string>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v2_0/ivibrator_plug_callback.h"
#include "v2_0/vibrator_types.h"

namespace OHOS {
namespace HDI {
namespace Vibrator {
namespace V2_0 {

class IVibratorInterface : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.vibrator.v2_0.IVibratorInterface");

    virtual ~IVibratorInterface() = default;

    static sptr<IVibratorInterface> Get(bool isStub = false);
    static sptr<IVibratorInterface> Get(const std::string &serviceName, bool isStub = false);

    /* Exact 20 pure-virtual slots verified against donor libvibrator_stub_2.0.z.so (offsets +16..+168). */
    virtual int32_t StartOnce(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t duration) = 0;

    virtual int32_t Start(const DeviceVibratorInfo &deviceVibratorInfo, const std::string &effectType) = 0;

    virtual int32_t Stop(const DeviceVibratorInfo &deviceVibratorInfo, HdfVibratorMode mode) = 0;

    virtual int32_t GetVibratorInfo(std::vector<HdfVibratorInfo> &vibratorInfo) = 0;

    virtual int32_t GetDeviceVibratorInfo(const DeviceVibratorInfo &deviceVibratorInfo,
                                          std::vector<HdfVibratorInfo> &vibratorInfo) = 0;

    virtual int32_t GetVibratorIdSingle(const DeviceVibratorInfo &deviceVibratorInfo,
                                        std::vector<HdfVibratorInfo> &vibratorInfo) = 0;

    virtual int32_t EnableVibratorModulation(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t duration,
                                             uint16_t intensity, int16_t frequency) = 0;

    virtual int32_t EnableCompositeEffect(const DeviceVibratorInfo &deviceVibratorInfo,
                                          const HdfCompositeEffect &effect) = 0;

    virtual int32_t GetEffectInfo(const DeviceVibratorInfo &deviceVibratorInfo, const std::string &effectType,
                                  HdfEffectInfo &effectInfo) = 0;

    virtual int32_t IsVibratorRunning(const DeviceVibratorInfo &deviceVibratorInfo, bool &state) = 0;

    virtual int32_t PlayHapticPattern(const DeviceVibratorInfo &deviceVibratorInfo, const HapticPaket &pkg) = 0;

    virtual int32_t GetHapticCapacity(const DeviceVibratorInfo &deviceVibratorInfo,
                                      HapticCapacity &hapticCapacity) = 0;

    virtual int32_t GetHapticStartUpTime(const DeviceVibratorInfo &deviceVibratorInfo, int32_t mode,
                                         int32_t &startUpTime) = 0;

    virtual int32_t StartByIntensity(const DeviceVibratorInfo &deviceVibratorInfo, const std::string &effectType,
                                     uint16_t intensity) = 0;

    virtual int32_t GetAllWaveInfo(const DeviceVibratorInfo &deviceVibratorInfo,
                                   std::vector<HdfWaveInformation> &info) = 0;

    virtual int32_t RegVibratorPlugCallback(const sptr<IVibratorPlugCallback> &callbackObj) = 0;

    virtual int32_t UnRegVibratorPlugCallback(const sptr<IVibratorPlugCallback> &callbackObj) = 0;

    virtual int32_t PlayPatternBySessionId(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t sessionId,
                                           const HapticPaket &hapticPaket) = 0;

    virtual int32_t PlayPackageBySession(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t sessionId,
                                         const VibratorPackage &vibratorPackage) = 0;

    virtual int32_t StopVibrateBySessionId(const DeviceVibratorInfo &deviceVibratorInfo, uint32_t sessionId) = 0;

    /* Slots 25..27 (+176..+192): GetVersion (2.0), IsProxy (false), GetDesc. */
    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 2;
        minorVer = 0;
        return HDF_SUCCESS;
    }

    virtual bool IsProxy() {
        return false;
    }

    virtual const std::u16string GetDesc() {
        return metaDescriptor_;
    }
};

} // namespace V2_0
} // namespace Vibrator
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_VIBRATOR_V2_0_IVIBRATOR_INTERFACE_H
