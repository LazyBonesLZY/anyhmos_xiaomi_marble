/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_NFC_V1_0_NFCTYPES_H
#define OHOS_HDI_NFC_V1_0_NFCTYPES_H

#include <cstdint>

namespace OHOS {
namespace HDI {
namespace Nfc {
namespace V1_0 {

enum NfcEvent : int32_t {
    OPEN_CPLT = 0,
    CLOSE_CPLT = 1,
    POST_INIT_CPLT = 2,
    PRE_DISCOVER_CPLT = 3,
    REQUEST_CONTROL = 4,
    RELEASE_CONTROL = 5,
    ERROR = 6,
    HCI_NETWORK_RESET = 7,
};

enum NfcStatus : int32_t {
    OK = 0,
    FAILED = 1,
    ERR_TRANSPORT = 2,
    ERR_CMD_TIMEOUT = 3,
    REFUSED = 4,
};

enum NfcCommand : int32_t {
    CMD_INVALID = 0,
};

} // namespace V1_0
} // namespace Nfc
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_NFC_V1_0_NFCTYPES_H
