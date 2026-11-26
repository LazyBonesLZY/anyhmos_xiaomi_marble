/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_USB_V2_0_IUSBD_SUBSCRIBER_H
#define OHOS_HDI_USB_V2_0_IUSBD_SUBSCRIBER_H

#include <cstdint>
#include <string>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v2_0/usb_types.h"

namespace OHOS {
namespace HDI {
namespace Usb {
namespace V2_0 {

class IUsbdSubscriber : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.usb.v2_0.IUsbdSubscriber");

    virtual ~IUsbdSubscriber() = default;

    virtual int32_t DeviceEvent(const USBDeviceInfo &info) = 0;

    virtual int32_t PortChangedEvent(const PortInfo &info) = 0;

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
} // namespace Usb
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_USB_V2_0_IUSBD_SUBSCRIBER_H
