/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_THERMAL_V1_1_ITHERMALINTERFACE_H
#define OHOS_HDI_THERMAL_V1_1_ITHERMALINTERFACE_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_1/ifan_callback.h"
#include "v1_1/ithermal_callback.h"
#include "v1_1/thermal_types.h"

namespace OHOS {
namespace HDI {
namespace Thermal {
namespace V1_1 {

class IThermalInterface : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.thermal.v1_1.IThermalInterface");

    virtual ~IThermalInterface() = default;

    static sptr<OHOS::HDI::Thermal::V1_1::IThermalInterface> Get(bool isStub = false);
    static sptr<OHOS::HDI::Thermal::V1_1::IThermalInterface> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t SetCpuFreq(int32_t freq) = 0;

    virtual int32_t SetGpuFreq(int32_t freq) = 0;

    virtual int32_t SetBatteryCurrent(int32_t current) = 0;

    virtual int32_t GetThermalZoneInfo(OHOS::HDI::Thermal::V1_1::HdfThermalCallbackInfo &event) = 0;

    virtual int32_t IsolateCpu(int32_t num) = 0;

    virtual int32_t Register(const sptr<OHOS::HDI::Thermal::V1_1::IThermalCallback> &callbackObj) = 0;

    virtual int32_t Unregister() = 0;

    virtual int32_t RegisterFanCallback(const sptr<OHOS::HDI::Thermal::V1_1::IFanCallback> &callbackObj) = 0;

    virtual int32_t UnregisterFanCallback() = 0;

    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 1;
        minorVer = 1;
        return HDF_SUCCESS;
    }

    virtual bool IsProxy() {
        return false;
    }

    virtual const std::u16string GetDesc() {
        return metaDescriptor_;
    }
};

} // namespace V1_1
} // namespace Thermal
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_THERMAL_V1_1_ITHERMALINTERFACE_H
