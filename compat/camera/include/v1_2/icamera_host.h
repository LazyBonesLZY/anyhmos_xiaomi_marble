/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_2_ICAMERAHOST_H
#define OHOS_HDI_CAMERA_V1_2_ICAMERAHOST_H

#include <cstdint>
#include <string>
#include "v1_1/icamera_host.h"
#include "v1_2/icamera_device.h"
#include "v1_2/icamera_host_callback.h"
#include "v1_2/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_2 {

class ICameraHost : public V1_1::ICameraHost {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_2.ICameraHost");

    virtual ~ICameraHost() = default;

    static sptr<ICameraHost> Get(bool isStub = false);
    static sptr<ICameraHost> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t OpenCamera_V1_2(const std::string &cameraId,
                                    const sptr<V1_0::ICameraDeviceCallback> &callbackObj,
                                    sptr<V1_2::ICameraDevice> &device) = 0;

    virtual int32_t NotifyDeviceStateChangeInfo(int32_t notifyType, int32_t deviceState) = 0;

    virtual int32_t SetCallback_V1_2(const sptr<V1_2::ICameraHostCallback> &callbackObj) = 0;

    virtual int32_t SetFlashlight_V1_2(float level) = 0;

    virtual int32_t PreCameraSwitch(const std::string &cameraId) = 0;

    virtual int32_t PrelaunchWithOpMode(const V1_1::PrelaunchConfig &config,
                                        int32_t operationMode) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_2_ICAMERAHOST_H */
