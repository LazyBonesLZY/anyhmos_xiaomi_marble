/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_THERMAL_V1_1_ITHERMALCALLBACK_H
#define OHOS_HDI_THERMAL_V1_1_ITHERMALCALLBACK_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_1/thermal_types.h"

namespace OHOS {
namespace HDI {
namespace Thermal {
namespace V1_1 {

class IThermalCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.thermal.v1_1.IThermalCallback");

    virtual ~IThermalCallback() = default;

    virtual int32_t OnThermalDataEvent(const OHOS::HDI::Thermal::V1_1::HdfThermalCallbackInfo &event) = 0;

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

#endif // OHOS_HDI_THERMAL_V1_1_ITHERMALCALLBACK_H
