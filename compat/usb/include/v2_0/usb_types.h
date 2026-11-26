/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_USB_V2_0_USB_TYPES_H
#define OHOS_HDI_USB_V2_0_USB_TYPES_H

#include <cstddef>
#include <cstdint>

namespace OHOS {
namespace HDI {
namespace Usb {
namespace V2_0 {

/*
 * Exact donor ABI (ADY-AL10 libusb_stub_2.0.z.so & libusb_proxy_2.0.z.so):
 *   USBDeviceInfoBlockMarshalling  -> WriteUnpadBuffer(..., 16)
 *   UsbPortStatusBlockMarshalling  -> WriteUnpadBuffer(..., 16)
 *   UsbPortBlockMarshalling        -> WriteUnpadBuffer(..., 24)
 *   PortInfoBlockMarshalling       -> WriteUnpadBuffer(..., 24)
 *   UsbdPorts::ReportData offsets verified by disassembly:
 *     UsbPort.id (+0)                      -> PortInfo.portId (+0)
 *     UsbPort.supportedModes (+4)          -> PortInfo.supportedModes (+16)
 *     UsbPort.usbPortStatus.currentMode (+8)       -> PortInfo.mode (+12)
 *     UsbPort.usbPortStatus.currentPowerRole (+12) -> PortInfo.powerRole (+4)
 *     UsbPort.usbPortStatus.currentDataRole (+16)  -> PortInfo.dataRole (+8)
 */

enum MarbleUsbPortConstants : int32_t {
    MARBLE_DEFAULT_PORT_ID = 1,

    MARBLE_SUPPORTED_MODES_NONE = 0,
    MARBLE_SUPPORTED_MODES_UFP = 1,
    MARBLE_SUPPORTED_MODES_DFP = 2,
    MARBLE_SUPPORTED_MODES_UFP_DFP = 3,

    MARBLE_POWER_ROLE_NONE = 0,
    MARBLE_POWER_ROLE_SOURCE = 1,
    MARBLE_POWER_ROLE_SINK = 2,

    MARBLE_DATA_ROLE_NONE = 0,
    MARBLE_DATA_ROLE_HOST = 1,
    MARBLE_DATA_ROLE_DEVICE = 2,

    MARBLE_PORT_MODE_NONE = 0,
    MARBLE_PORT_MODE_DEVICE = 1,
    MARBLE_PORT_MODE_HOST = 2,
};

struct USBDeviceInfo {
    int32_t status = 0;
    int32_t busNum = 0;
    int32_t devNum = 0;
    int32_t reserved = 0;
};

struct PortInfo {
    int32_t portId = 0;
    int32_t powerRole = 0;
    int32_t dataRole = 0;
    int32_t mode = 0;
    int32_t supportedModes = 0;
    int32_t reserved = 0;
};

struct UsbPortStatus {
    int32_t currentMode = 0;
    int32_t currentPowerRole = 0;
    int32_t currentDataRole = 0;
    int32_t reserved = 0;
};

struct UsbPort {
    int32_t id = 0;
    int32_t supportedModes = 0;
    UsbPortStatus usbPortStatus {};
};

static_assert(sizeof(USBDeviceInfo) == 16, "USBDeviceInfo ABI mismatch with donor libusb_stub_2.0");
static_assert(sizeof(PortInfo) == 24, "PortInfo ABI mismatch with donor libusb_stub_2.0");
static_assert(sizeof(UsbPortStatus) == 16, "UsbPortStatus ABI mismatch with donor libusb_stub_2.0");
static_assert(sizeof(UsbPort) == 24, "UsbPort ABI mismatch with donor libusb_stub_2.0");
static_assert(offsetof(UsbPort, supportedModes) == 4, "UsbPort.supportedModes offset mismatch");
static_assert(offsetof(UsbPort, usbPortStatus) == 8, "UsbPort.usbPortStatus offset mismatch");
static_assert(offsetof(PortInfo, powerRole) == 4, "PortInfo.powerRole offset mismatch");
static_assert(offsetof(PortInfo, dataRole) == 8, "PortInfo.dataRole offset mismatch");
static_assert(offsetof(PortInfo, mode) == 12, "PortInfo.mode offset mismatch");
static_assert(offsetof(PortInfo, supportedModes) == 16, "PortInfo.supportedModes offset mismatch");

} // namespace V2_0
} // namespace Usb
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_USB_V2_0_USB_TYPES_H
