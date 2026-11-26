/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_CAMERA_V1_0_ICAMERADEVICE_H
#define OHOS_HDI_CAMERA_V1_0_ICAMERADEVICE_H

#include <cstdint>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/types.h"
#include "v1_0/istream_operator_callback.h"
#include "v1_0/istream_operator.h"

namespace OHOS {
namespace HDI {
namespace Camera {
namespace V1_0 {

class ICameraDevice : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.camera.v1_0.ICameraDevice");

    virtual ~ICameraDevice() = default;

    virtual int32_t GetStreamOperator(const sptr<IStreamOperatorCallback> &callbackObj,
                                      sptr<IStreamOperator> &streamOperator) = 0;

    virtual int32_t UpdateSettings(const std::vector<uint8_t> &settings) = 0;

    virtual int32_t SetResultMode(ResultCallbackMode mode) = 0;

    virtual int32_t GetEnabledResults(std::vector<int32_t> &results) = 0;

    virtual int32_t EnableResult(const std::vector<int32_t> &results) = 0;

    virtual int32_t DisableResult(const std::vector<int32_t> &results) = 0;

    virtual int32_t Close() = 0;

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

#endif /* OHOS_HDI_CAMERA_V1_0_ICAMERADEVICE_H */
