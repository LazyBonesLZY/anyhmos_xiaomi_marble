/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_2_ISTREAMOPERATORCALLBACK_H
#define OHOS_HDI_CAMERA_V1_2_ISTREAMOPERATORCALLBACK_H

#include <cstdint>
#include <vector>
#include "v1_0/istream_operator_callback.h"
#include "v1_2/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_2 {

class IStreamOperatorCallback : public V1_0::IStreamOperatorCallback {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_2.IStreamOperatorCallback");

    virtual ~IStreamOperatorCallback() = default;

    virtual int32_t OnCaptureStarted_V1_2(int32_t captureId,
                                          const std::vector<CaptureStartedInfo> &infos) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_2_ISTREAMOPERATORCALLBACK_H */
