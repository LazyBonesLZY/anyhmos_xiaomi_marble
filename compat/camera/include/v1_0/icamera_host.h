/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_0_ICAMERAHOST_H
#define OHOS_HDI_CAMERA_V1_0_ICAMERAHOST_H

#include <cstdint>
#include <string>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/types.h"
#include "v1_0/icamera_host_callback.h"
#include "v1_0/icamera_device_callback.h"
#include "v1_0/icamera_device.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_0 {

class ICameraHost : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_0.ICameraHost");

    virtual ~ICameraHost() = default;

    static sptr<ICameraHost> Get(bool isStub = false);
    static sptr<ICameraHost> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t SetCallback(const sptr<ICameraHostCallback> &callbackObj) = 0;

    virtual int32_t GetCameraIds(std::vector<std::string> &cameraIds) = 0;

    virtual int32_t GetCameraAbility(const std::string &cameraId,
                                     std::vector<uint8_t> &cameraAbility) = 0;

    virtual int32_t OpenCamera(const std::string &cameraId,
                               const sptr<ICameraDeviceCallback> &callbackObj,
                               sptr<ICameraDevice> &device) = 0;

    virtual int32_t SetFlashlight(const std::string &cameraId, bool isEnable) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_0_ICAMERAHOST_H */
