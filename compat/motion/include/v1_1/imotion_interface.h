/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_MOTION_V1_1_IMOTION_INTERFACE_H
#define OHOS_HDI_MOTION_V1_1_IMOTION_INTERFACE_H

#include <cstdint>
#include <string>
#include <vector>
#include <hdf_base.h>
#include "v1_0/imotion_callback.h"
#include "v1_0/imotion_interface.h"
#include "v1_1/motion_types.h"

namespace OHOS {
namespace HDI {
namespace Motion {
namespace V1_1 {

using OHOS::HDI::Motion::V1_0::HdfMotionEvent;
using OHOS::HDI::Motion::V1_0::IMotionCallback;

class IMotionInterface : public OHOS::HDI::Motion::V1_0::IMotionInterface {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.motion.v1_1.IMotionInterface");

    virtual ~IMotionInterface() = default;

    static sptr<OHOS::HDI::Motion::V1_1::IMotionInterface> Get(bool isStub = false);
    static sptr<OHOS::HDI::Motion::V1_1::IMotionInterface> Get(const std::string &serviceName, bool isStub = false);
    static sptr<OHOS::HDI::Motion::V1_1::IMotionInterface> CastFrom(
        const sptr<OHOS::HDI::Motion::V1_0::IMotionInterface> &parentObject);

    /* Exact vtable slot +72 verified against donor libmotion_stub_1.1.z.so. */
    virtual int32_t SetMotionConfig(int32_t motionType, const std::vector<uint8_t> &data) = 0;

    /* Reuses V1_0 vtable slot +48 (GetVersion) and +64 (GetDesc). */
    int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) override {
        majorVer = 1;
        minorVer = 1;
        return HDF_SUCCESS;
    }

    const std::u16string GetDesc() override {
        return metaDescriptor_;
    }
};

} // namespace V1_1
} // namespace Motion
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_MOTION_V1_1_IMOTION_INTERFACE_H
