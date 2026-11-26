/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_2_ICAMERADEVICE_H
#define OHOS_HDI_CAMERA_V1_2_ICAMERADEVICE_H

#include <cstdint>
#include <vector>
#include "v1_1/icamera_device.h"
#include "v1_2/istream_operator_callback.h"
#include "v1_2/istream_operator.h"
#include "v1_2/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_2 {

class ICameraDevice : public V1_1::ICameraDevice {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_2.ICameraDevice");

    virtual ~ICameraDevice() = default;

    virtual int32_t GetStreamOperator_V1_2(const sptr<V1_2::IStreamOperatorCallback> &callbackObj,
                                           sptr<V1_2::IStreamOperator> &streamOperator) = 0;

    virtual int32_t GetStatus(const std::vector<uint8_t> &metaIn,
                              std::vector<uint8_t> &metaOut) = 0;

    virtual int32_t Reset() = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_2_ICAMERADEVICE_H */
