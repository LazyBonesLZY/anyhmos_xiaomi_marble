/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_1_ISTREAMOPERATOR_H
#define OHOS_HDI_CAMERA_V1_1_ISTREAMOPERATOR_H

#include <cstdint>
#include <vector>
#include "v1_0/istream_operator.h"
#include "v1_1/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_1 {

class IStreamOperator : public V1_0::IStreamOperator {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_1.IStreamOperator");

    virtual ~IStreamOperator() = default;

    virtual int32_t IsStreamsSupported_V1_1(OperationMode_V1_1 mode,
                                            const std::vector<uint8_t> &modeSetting,
                                            const std::vector<StreamInfo_V1_1> &infos,
                                            V1_0::StreamSupportType &type) = 0;

    virtual int32_t CreateStreams_V1_1(const std::vector<StreamInfo_V1_1> &streamInfos) = 0;

    virtual int32_t CommitStreams_V1_1(OperationMode_V1_1 mode,
                                       const std::vector<uint8_t> &modeSetting) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_1_ISTREAMOPERATOR_H */
