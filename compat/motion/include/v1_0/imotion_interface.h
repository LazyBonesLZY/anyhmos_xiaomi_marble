/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_MOTION_V1_0_IMOTION_INTERFACE_H
#define OHOS_HDI_MOTION_V1_0_IMOTION_INTERFACE_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/imotion_callback.h"
#include "v1_0/motion_types.h"

namespace OHOS {
namespace HDI {
namespace Motion {
namespace V1_0 {

class IMotionInterface : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.motion.v1_0.IMotionInterface");

    virtual ~IMotionInterface() = default;

    static sptr<OHOS::HDI::Motion::V1_0::IMotionInterface> Get(bool isStub = false);
    static sptr<OHOS::HDI::Motion::V1_0::IMotionInterface> Get(const std::string &serviceName, bool isStub = false);

    /* Exact vtable slots verified against donor libmotion_stub_1.0.z.so (+16..+64). */
    virtual int32_t EnableMotion(int32_t motionType) = 0;

    virtual int32_t DisableMotion(int32_t motionType) = 0;

    virtual int32_t Register(const sptr<OHOS::HDI::Motion::V1_0::IMotionCallback> &callbackObj) = 0;

    virtual int32_t Unregister(const sptr<OHOS::HDI::Motion::V1_0::IMotionCallback> &callbackObj) = 0;

    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 1;
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

} // namespace V1_0
} // namespace Motion
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_MOTION_V1_0_IMOTION_INTERFACE_H
