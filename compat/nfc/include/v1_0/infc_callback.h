/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_NFC_V1_0_INFCCALLBACK_H
#define OHOS_HDI_NFC_V1_0_INFCCALLBACK_H

#include <cstdint>
#include <string>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/nfc_types.h"

namespace OHOS {
namespace HDI {
namespace Nfc {
namespace V1_0 {

class INfcCallback : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.nfc.v1_0.INfcCallback");

    virtual ~INfcCallback() = default;

    virtual int32_t OnData(const std::vector<uint8_t> &data) = 0;

    virtual int32_t OnEvent(OHOS::HDI::Nfc::V1_0::NfcEvent event,
                            OHOS::HDI::Nfc::V1_0::NfcStatus status) = 0;

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
} // namespace Nfc
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_NFC_V1_0_INFCCALLBACK_H
