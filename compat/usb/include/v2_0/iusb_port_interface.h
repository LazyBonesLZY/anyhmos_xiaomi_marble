/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_USB_V2_0_IUSB_PORT_INTERFACE_H
#define OHOS_HDI_USB_V2_0_IUSB_PORT_INTERFACE_H

#include <cstdint>
#include <string>
#include <vector>
#include <hdf_base.h>
#include <hdi_base.h>
#include "v2_0/iusbd_subscriber.h"
#include "v2_0/usb_types.h"

namespace OHOS {
namespace HDI {
namespace Usb {
namespace V2_0 {

class IUsbPortInterface : public HdiBase {
public:
    DECLARE_HDI_DESCRIPTOR(u"ohos.hdi.usb.v2_0.IUsbPortInterface");

    virtual ~IUsbPortInterface() = default;

    static sptr<IUsbPortInterface> Get(bool isStub = false);
    static sptr<IUsbPortInterface> Get(const std::string &serviceName, bool isStub = false);

    virtual int32_t SetPortRole(int32_t portId, int32_t powerRole, int32_t dataRole) = 0;

    virtual int32_t QueryPort(int32_t &portId, int32_t &powerRole, int32_t &dataRole, int32_t &mode) = 0;

    virtual int32_t QueryPorts(std::vector<UsbPort> &portList) = 0;

    virtual int32_t BindUsbdPortSubscriber(const sptr<IUsbdSubscriber> &subscriber) = 0;

    virtual int32_t UnbindUsbdPortSubscriber(const sptr<IUsbdSubscriber> &subscriber) = 0;

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

#endif // OHOS_HDI_USB_V2_0_IUSB_PORT_INTERFACE_H
