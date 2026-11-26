/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_POWER_V1_2_IPOWERHDICALLBACK_H
#define OHOS_HDI_POWER_V1_2_IPOWERHDICALLBACK_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>

namespace OHOS {
namespace HDI {
namespace Power {
namespace V1_2 {

class IPowerHdiCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.power.v1_2.IPowerHdiCallback");

    virtual ~IPowerHdiCallback() = default;

    virtual int32_t OnSuspend() = 0;

    virtual int32_t OnWakeup() = 0;

    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 1;
        minorVer = 2;
        return HDF_SUCCESS;
    }

    virtual bool IsProxy() {
        return false;
    }

    virtual const std::u16string GetDesc() {
        return metaDescriptor_;
    }
};

} // namespace V1_2
} // namespace Power
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_POWER_V1_2_IPOWERHDICALLBACK_H
