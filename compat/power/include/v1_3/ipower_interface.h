/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_POWER_V1_3_IPOWERINTERFACE_H
#define OHOS_HDI_POWER_V1_3_IPOWERINTERFACE_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include "v1_2/ipower_interface.h"
#include "v1_3/ipower_hdi_callback_ext.h"

namespace OHOS {
namespace HDI {
namespace Power {
namespace V1_3 {

class IPowerInterface : public OHOS::HDI::Power::V1_2::IPowerInterface {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.power.v1_3.IPowerInterface");

    virtual ~IPowerInterface() = default;

    static sptr<OHOS::HDI::Power::V1_3::IPowerInterface> Get(bool isStub = false);
    static sptr<OHOS::HDI::Power::V1_3::IPowerInterface> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t RegisterPowerCallbackExt(
        const sptr<OHOS::HDI::Power::V1_3::IPowerHdiCallbackExt> &ipowerHdiCallback) = 0;

    virtual int32_t UnRegisterPowerCallbackExt(
        const sptr<OHOS::HDI::Power::V1_3::IPowerHdiCallbackExt> &ipowerHdiCallback) = 0;

    int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) override {
        majorVer = 1;
        minorVer = 3;
        return HDF_SUCCESS;
    }

    const std::u16string GetDesc() override {
        return metaDescriptor_;
    }
};

} // namespace V1_3
} // namespace Power
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_POWER_V1_3_IPOWERINTERFACE_H
