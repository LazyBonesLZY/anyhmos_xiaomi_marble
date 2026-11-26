/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_2_ISTREAMOPERATOR_H
#define OHOS_HDI_CAMERA_V1_2_ISTREAMOPERATOR_H

#include <cstdint>
#include <vector>
#include "v1_1/istream_operator.h"
#include "v1_2/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_2 {

class IStreamOperator : public V1_1::IStreamOperator {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_2.IStreamOperator");

    virtual ~IStreamOperator() = default;

    virtual int32_t UpdateStreams(const std::vector<V1_1::StreamInfo_V1_1> &streamInfos) = 0;

    virtual int32_t ConfirmCapture(int32_t captureId) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_2_ISTREAMOPERATOR_H */
