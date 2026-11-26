/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_POWER_V1_3_IPOWERHDICALLBACKEXT_H
#define OHOS_HDI_POWER_V1_3_IPOWERHDICALLBACKEXT_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>

namespace OHOS {
namespace HDI {
namespace Power {
namespace V1_3 {

class IPowerHdiCallbackExt : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.power.v1_3.IPowerHdiCallbackExt");

    virtual ~IPowerHdiCallbackExt() = default;

    virtual int32_t OnSuspendWithTag(const std::string &tag) = 0;

    virtual int32_t OnWakeupWithTag(const std::string &tag) = 0;

    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 1;
        minorVer = 3;
        return HDF_SUCCESS;
    }

    virtual bool IsProxy() {
        return false;
    }

    virtual const std::u16string GetDesc() {
        return metaDescriptor_;
    }
};

} // namespace V1_3
} // namespace Power
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_POWER_V1_3_IPOWERHDICALLBACKEXT_H
