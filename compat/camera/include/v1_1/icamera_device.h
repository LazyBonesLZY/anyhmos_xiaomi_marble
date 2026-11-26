/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_1_ICAMERADEVICE_H
#define OHOS_HDI_CAMERA_V1_1_ICAMERADEVICE_H

#include <cstdint>
#include <vector>
#include "v1_0/icamera_device.h"
#include "v1_1/istream_operator.h"
#include "v1_1/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_1 {

class ICameraDevice : public V1_0::ICameraDevice {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_1.ICameraDevice");

    virtual ~ICameraDevice() = default;

    virtual int32_t GetStreamOperator_V1_1(const sptr<V1_0::IStreamOperatorCallback> &callbackObj,
                                           sptr<V1_1::IStreamOperator> &streamOperator) = 0;

    virtual int32_t GetDefaultSettings(std::vector<uint8_t> &settings) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_1_ICAMERADEVICE_H */
