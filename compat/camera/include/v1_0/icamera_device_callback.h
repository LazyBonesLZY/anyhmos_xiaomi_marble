/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_0_ICAMERADEVICECALLBACK_H
#define OHOS_HDI_CAMERA_V1_0_ICAMERADEVICECALLBACK_H

#include <cstdint>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/types.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_0 {

class ICameraDeviceCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_0.ICameraDeviceCallback");

    virtual ~ICameraDeviceCallback() = default;

    virtual int32_t OnError(ErrorType type, int32_t errorCode) = 0;

    virtual int32_t OnResult(uint64_t timestamp, const std::vector<uint8_t> &result) = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_0_ICAMERADEVICECALLBACK_H */
