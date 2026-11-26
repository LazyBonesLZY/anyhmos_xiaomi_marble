/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_NFC_V1_1_NFCTYPES_H
#define OHOS_HDI_NFC_V1_1_NFCTYPES_H

#include <cstdint>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Nfc {
namespace V1_1 {

enum VendorProtocalDiscoveryCfg : int32_t {
    NCI_PROTOCOL_18092_ACTIVE = 0,
    NCI_PROTOCOL_B_PRIME = 1,
    NCI_PROTOCOL_DUAL = 2,
    NCI_PROTOCOL_15693 = 3,
    NCI_PROTOCOL_KOVIO = 4,
    NCI_PROTOCOL_MIFARE = 5,
    NCI_DISCOVERY_TYPE_POLL_KOVIO = 6,
    NCI_DISCOVERY_TYPE_POLL_B_PRIME = 7,
    NCI_DISCOVERY_TYPE_LISTEN_B_PRIME = 8,
    VENDOR_PROPEIETARY_CFG_MAX = 9,
};

/* Exact field order & offsets verified against donor libnfc_stub_1.1.z.so
 * NfcVendorConfigBlockMarshalling:
 *   +0 u32, +4..+10 u8*7, +11 bool, +12 u8, +16 vec<u8>, +40 u8,
 *   +48 vec<u8>, +72 vec<u8>, +96 vec<u8>, +120 u8. */
struct NfcVendorConfig {
    uint32_t isoDepExtApduLength = 0;
    uint8_t defaultOffHostRoute = 0;
    uint8_t defaultOffHostRouteFelica = 0;
    uint8_t defaultSysCodeRoute = 0;
    uint8_t defaultSysCodePwrState = 0;
    uint8_t defaultUnconfiguredRoute = 0;
    uint8_t esePipeId = 0;
    uint8_t simPipeId = 0;
    bool pollBailOutMode = false;
    uint8_t checkAlgorithm = 0;
    std::vector<uint8_t> vendorProtocalDiscoveryCfg;
    uint8_t vendorProtocalDiscoveryCfgSize = 0;
    std::vector<uint8_t> hostWhitelist;
    std::vector<uint8_t> offHostRouteUicc;
    std::vector<uint8_t> offHostRouteEse;
    uint8_t defaultIsoDepRoute = 0;
};

static_assert(offsetof(NfcVendorConfig, isoDepExtApduLength) == 0, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, pollBailOutMode) == 11, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, checkAlgorithm) == 12, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, vendorProtocalDiscoveryCfg) == 16, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, vendorProtocalDiscoveryCfgSize) == 40, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, hostWhitelist) == 48, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, offHostRouteUicc) == 72, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, offHostRouteEse) == 96, "offset mismatch");
static_assert(offsetof(NfcVendorConfig, defaultIsoDepRoute) == 120, "offset mismatch");

} // namespace V1_1
} // namespace Nfc
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_NFC_V1_1_NFCTYPES_H
