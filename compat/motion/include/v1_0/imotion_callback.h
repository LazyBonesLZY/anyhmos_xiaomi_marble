/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_MOTION_V1_0_IMOTION_CALLBACK_H
#define OHOS_HDI_MOTION_V1_0_IMOTION_CALLBACK_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/motion_types.h"

namespace OHOS {
namespace HDI {
namespace Motion {
namespace V1_0 {

class IMotionCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.motion.v1_0.IMotionCallback");

    virtual ~IMotionCallback() = default;

    /* Verified against donor libmotion_stub_1.0.z.so MotionCallbackProxy vtable (+16..+40). */
    virtual int32_t OnDataEvent(const OHOS::HDI::Motion::V1_0::HdfMotionEvent &event) = 0;

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

#endif // OHOS_HDI_MOTION_V1_0_IMOTION_CALLBACK_H
