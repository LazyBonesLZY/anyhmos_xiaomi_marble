/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_1_ICAMERAHOST_H
#define OHOS_HDI_CAMERA_V1_1_ICAMERAHOST_H

#include <cstdint>
#include <string>
#include "v1_0/icamera_host.h"
#include "v1_1/icamera_device.h"
#include "v1_1/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_1 {

class ICameraHost : public V1_0::ICameraHost {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_1.ICameraHost");

    virtual ~ICameraHost() = default;

    static sptr<ICameraHost> Get(bool isStub = false);
    static sptr<ICameraHost> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t OpenCamera_V1_1(const std::string &cameraId,
                                    const sptr<V1_0::ICameraDeviceCallback> &callbackObj,
                                    sptr<V1_1::ICameraDevice> &device) = 0;

    virtual int32_t Prelaunch(const PrelaunchConfig &config) = 0;

    int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) override {
        majorVer = 1;
        minorVer = 1;
        return HDF_SUCCESS;
    }

    bool IsProxy() override {
        return false;
    }

    const std::u16string GetDesc() override {
        return metaDescriptor_;
    }
};

} // namespace V1_1
} // namespace Camera
} // namespace HDI
} // namespace OHOS

#endif /* OHOS_HDI_CAMERA_V1_1_ICAMERAHOST_H */
