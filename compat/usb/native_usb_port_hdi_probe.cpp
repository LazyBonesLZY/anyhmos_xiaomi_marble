/* SPDX-License-Identifier: MIT */
/*
 * End-to-end OHOS Binder probe for SM7475 Hardware USB Port HDI v2.0 (Batch H4).
 *
 * Links against unmodified donor chipset-sdk-sp/libusb_proxy_2.0.z.so and exercises
 * real Binder IPC to HDF usb_host (host-13, service usb_port_interface_service):
 *   1. IUsbPortInterface::Get(false) + GetVersion == 2.0
 *   2. QueryPort (live SM7475 /sys/class/typec/port0 power_role, data_role, mode)
 *   3. QueryPorts (std::vector<UsbPort>, verifying 24-byte donor ABI round-trip)
 *   4. BindUsbdPortSubscriber -> async Binder PortChangedEvent callback -> Unbind
 *   5. SetPortRole no-op on current live role (rc=0) + invalid port/role rejection
 */

#include "v2_0/iusb_port_interface.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

using OHOS::sptr;
using OHOS::HDI::Usb::V2_0::IUsbdSubscriber;
using OHOS::HDI::Usb::V2_0::IUsbPortInterface;
using OHOS::HDI::Usb::V2_0::PortInfo;
using OHOS::HDI::Usb::V2_0::USBDeviceInfo;
using OHOS::HDI::Usb::V2_0::UsbPort;

namespace {

class ProbeUsbSubscriber : public IUsbdSubscriber {
public:
    int32_t DeviceEvent(const USBDeviceInfo &info) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        devEvents_++;
        lastDev_ = info;
        cv_.notify_all();
        return 0;
    }

    int32_t PortChangedEvent(const PortInfo &info) override
    {
        std::lock_guard<std::mutex> lock(mu_);
        portEvents_++;
        lastPort_ = info;
        cv_.notify_all();
        return 0;
    }

    bool WaitPortEvent(int timeoutMs, PortInfo &out, int &count)
    {
        std::unique_lock<std::mutex> lock(mu_);
        const bool ok = cv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                     [this]() { return portEvents_ > 0; });
        out = lastPort_;
        count = portEvents_;
        return ok;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    int devEvents_ = 0;
    int portEvents_ = 0;
    USBDeviceInfo lastDev_ {};
    PortInfo lastPort_ {};
};

} // namespace

int main()
{
    sptr<IUsbPortInterface> svc = IUsbPortInterface::Get(false);
    if (svc == nullptr) {
        std::printf("USB_PORT_HDI_SERVICE=unavailable\n");
        std::printf("MARBLE_USB_PORT_HDI_PROBE_SUMMARY hdi_ver=0.0 port_id=0 power_role=0 data_role=0"
                    " mode=0 ports=0 supported_modes=0 cb_ok=0 set_ok=0 all_ok=0\n");
        return 2;
    }
    std::printf("USB_PORT_HDI_SERVICE=connected\n");

    uint32_t major = 0;
    uint32_t minor = 0;
    const int32_t verRc = svc->GetVersion(major, minor);
    std::printf("USB_PORT_HDI_VERSION rc=%d ver=%u.%u\n", verRc, major, minor);

    int32_t portId = -1;
    int32_t powerRole = -1;
    int32_t dataRole = -1;
    int32_t mode = -1;
    const int32_t qRc = svc->QueryPort(portId, powerRole, dataRole, mode);
    std::printf("USB_PORT_QUERY rc=%d port_id=%d power_role=%d data_role=%d mode=%d\n",
                qRc, portId, powerRole, dataRole, mode);

    std::vector<UsbPort> ports;
    const int32_t qsRc = svc->QueryPorts(ports);
    int32_t supportedModes = 0;
    bool portsMatch = false;
    if (qsRc == 0 && !ports.empty()) {
        const UsbPort &p0 = ports[0];
        supportedModes = p0.supportedModes;
        portsMatch = (p0.id == portId &&
                      p0.usbPortStatus.currentMode == mode &&
                      p0.usbPortStatus.currentPowerRole == powerRole &&
                      p0.usbPortStatus.currentDataRole == dataRole &&
                      p0.supportedModes > 0);
        std::printf("USB_PORT_QUERY_PORTS rc=%d count=%zu p0_id=%d sup=%d mode=%d power=%d data=%d match=%d\n",
                    qsRc, ports.size(), p0.id, p0.supportedModes,
                    p0.usbPortStatus.currentMode, p0.usbPortStatus.currentPowerRole,
                    p0.usbPortStatus.currentDataRole, portsMatch ? 1 : 0);
    } else {
        std::printf("USB_PORT_QUERY_PORTS rc=%d count=%zu match=0\n", qsRc, ports.size());
    }

    sptr<ProbeUsbSubscriber> cb(new ProbeUsbSubscriber());
    const int32_t bindRc = svc->BindUsbdPortSubscriber(cb);
    PortInfo evInfo {};
    int evCount = 0;
    const bool gotCb = (bindRc == 0) && cb->WaitPortEvent(2000, evInfo, evCount);
    const int32_t unbindRc = svc->UnbindUsbdPortSubscriber(cb);
    const bool cbOk = (bindRc == 0 && unbindRc == 0 && gotCb &&
                       evInfo.portId == portId && evInfo.powerRole == powerRole &&
                       evInfo.dataRole == dataRole && evInfo.mode == mode &&
                       evInfo.supportedModes == supportedModes);
    std::printf("USB_PORT_CALLBACK bind=%d unbind=%d events=%d ev_port=%d ev_power=%d ev_data=%d ev_mode=%d ev_sup=%d ok=%d\n",
                bindRc, unbindRc, evCount, evInfo.portId, evInfo.powerRole,
                evInfo.dataRole, evInfo.mode, evInfo.supportedModes, cbOk ? 1 : 0);

    const int32_t setSameRc = svc->SetPortRole(portId, powerRole, dataRole);
    const int32_t setBadPortRc = svc->SetPortRole(99, powerRole, dataRole);
    const int32_t setBadRoleRc = svc->SetPortRole(portId, 9, 9);
    const bool setOk = (setSameRc == 0 && setBadPortRc != 0 && setBadRoleRc != 0);
    std::printf("USB_PORT_SET_ROLE same=%d bad_port=%d bad_role=%d ok=%d\n",
                setSameRc, setBadPortRc, setBadRoleRc, setOk ? 1 : 0);

    const bool allOk = (verRc == 0 && major == 2 && minor == 0 &&
                        qRc == 0 && portId == 1 && powerRole > 0 && dataRole > 0 && mode > 0 &&
                        portsMatch && cbOk && setOk);
    std::printf("MARBLE_USB_PORT_HDI_PROBE_SUMMARY hdi_ver=%u.%u port_id=%d power_role=%d data_role=%d"
                " mode=%d ports=%zu supported_modes=%d cb_ok=%d set_ok=%d all_ok=%d\n",
                major, minor, portId, powerRole, dataRole, mode, ports.size(), supportedModes,
                cbOk ? 1 : 0, setOk ? 1 : 0, allOk ? 1 : 0);
    return allOk ? 0 : 2;
}
