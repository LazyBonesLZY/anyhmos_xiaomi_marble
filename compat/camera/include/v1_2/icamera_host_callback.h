/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_2_ICAMERAHOSTCALLBACK_H
#define OHOS_HDI_CAMERA_V1_2_ICAMERAHOSTCALLBACK_H

#include <cstdint>
#include "v1_0/icamera_host_callback.h"
#include "v1_2/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_2 {

class ICameraHostCallback : public V1_0::ICameraHostCallback {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_2.ICameraHostCallback");

    virtual ~ICameraHostCallback() = default;

    virtual int32_t OnFlashlightStatus_V1_2(V1_0::FlashlightStatus status) = 0;

    int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) override {
        majorVer = 1;
        minorVer = 2;
        return HDF_SUCCESS;
    }

    bool IsProxy() override {
        return false;
    }

    const std::u16string GetDesc() override {
        return metaDescriptor_;
    }
};

} // namespace V1_2
} // namespace Camera
} // namespace HDI
} // namespace OHOS

#endif /* OHOS_HDI_CAMERA_V1_2_ICAMERAHOSTCALLBACK_H */
