/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_NFC_V1_1_INFCINTERFACE_H
#define OHOS_HDI_NFC_V1_1_INFCINTERFACE_H

#include <cstdint>
#include <string>
#include <vector>
#include "v1_0/infc_interface.h"
#include "v1_1/nfc_types.h"

namespace OHOS {
namespace HDI {
namespace Nfc {
namespace V1_1 {

class INfcInterface : public OHOS::HDI::Nfc::V1_0::INfcInterface {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.nfc.v1_1.INfcInterface");

    virtual ~INfcInterface() = default;

    static sptr<OHOS::HDI::Nfc::V1_1::INfcInterface> Get(bool isStub = false);
    static sptr<OHOS::HDI::Nfc::V1_1::INfcInterface> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t GetVendorConfig(OHOS::HDI::Nfc::V1_1::NfcVendorConfig &config,
                                    OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t DoFactoryReset(OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t Shutdown(OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

    virtual int32_t IoctlWithResponse(OHOS::HDI::Nfc::V1_0::NfcCommand cmd,
                                      const std::vector<uint8_t> &data,
                                      std::vector<uint8_t> &response,
                                      OHOS::HDI::Nfc::V1_0::NfcStatus &status) = 0;

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
} // namespace Nfc
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_NFC_V1_1_INFCINTERFACE_H
