/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_NFC_V1_0_INFCINTERFACE_H
#define OHOS_HDI_NFC_V1_0_INFCINTERFACE_H

#include <cstdint>
#include <string>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v1_0/infc_callback.h"
#include "v1_0/nfc_types.h"

namespace OHOS {
namespace HDI {
namespace Nfc {
namespace V1_0 {

class INfcInterface : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.nfc.v1_0.INfcInterface");

    virtual ~INfcInterface() = default;

    static sptr<OHOS::HDI::Nfc::V1_0::INfcInterface> Get(bool isStub = false);
    static sptr<OHOS::HDI::Nfc::V1_0::INfcInterface> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t Open(const sptr<OHOS::HDI::Nfc::V1_0::INfcCallback> &callbackObj,
                         OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t CoreInitialized(const std::vector<uint8_t> &data,
                                    OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t Prediscover(OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t Write(const std::vector<uint8_t> &data,
                          OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t ControlGranted(OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t PowerCycle(OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t Close(OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t Ioctl(OHOS::HDI::Nfc::V1_0::NfcCommand cmd,
                          const std::vector<uint8_t> &data,
                          OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

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

#endif // OHOS_HDI_NFC_V1_0_INFCINTERFACE_H
