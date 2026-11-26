/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_VIBRATOR_V2_0_IVIBRATOR_PLUG_CALLBACK_H
#define OHOS_HDI_VIBRATOR_V2_0_IVIBRATOR_PLUG_CALLBACK_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v2_0/vibrator_types.h"

namespace OHOS {
namespace HDI {
namespace Vibrator {
namespace V2_0 {

class IVibratorPlugCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.vibrator.v2_0.IVibratorPlugCallback");

    virtual ~IVibratorPlugCallback() = default;

    virtual int32_t OnVibratorPlugEvent(const HdfVibratorPlugInfo &vibratorPlugInfo) = 0;

    virtual int32_t GetVersion(uint32_t &majorVer, uint32_t &minorVer) {
        majorVer = 2;
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

} // namespace V2_0
} // namespace Vibrator
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_VIBRATOR_V2_0_IVIBRATOR_PLUG_CALLBACK_H
