/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_0_ICAMERAHOSTCALLBACK_H
#define OHOS_HDI_CAMERA_V1_0_ICAMERAHOSTCALLBACK_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_0 {

class ICameraHostCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_0.ICameraHostCallback");

    virtual ~ICameraHostCallback() = default;

    virtual int32_t OnCameraStatus(const std::string &cameraId, CameraStatus status) = 0;

    virtual int32_t OnFlashlightStatus(const std::string &cameraId, FlashlightStatus status) = 0;

    virtual int32_t OnCameraEvent(const std::string &cameraId, CameraEvent event) = 0;

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
} // namespace Camera
} // namespace HDI
} // namespace OHOS

#endif /* OHOS_HDI_CAMERA_V1_0_ICAMERAHOSTCALLBACK_H */
