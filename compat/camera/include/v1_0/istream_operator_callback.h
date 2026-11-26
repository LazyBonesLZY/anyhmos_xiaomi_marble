/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_0_ISTREAMOPERATORCALLBACK_H
#define OHOS_HDI_CAMERA_V1_0_ISTREAMOPERATORCALLBACK_H

#include <cstdint>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_0 {

class IStreamOperatorCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_0.IStreamOperatorCallback");

    virtual ~IStreamOperatorCallback() = default;

    virtual int32_t OnCaptureStarted(int32_t captureId, const std::vector<int32_t> &streamIds) = 0;

    virtual int32_t OnCaptureEnded(int32_t captureId, const std::vector<CaptureEndedInfo> &infos) = 0;

    virtual int32_t OnCaptureError(int32_t captureId, const std::vector<CaptureErrorInfo> &infos) = 0;

    virtual int32_t OnFrameShutter(int32_t captureId, const std::vector<int32_t> &streamIds,
                                   uint64_t timestamp) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_0_ISTREAMOPERATORCALLBACK_H */
