/* SPDX-License-Identifier: MIT */
/*
 * Clean-room SM7475 Hardware USB Type-C Port HDI v2.0 implementation for Xiaomi marble.
 *
 * SONAME: libusb_port_interface_service_2.0.z.so
 * Loaded by unmodified donor /vendor/lib64/libusb_port_driver.z.so inside HDF host-13 (usb_host).
 *
 * Hardware data sources (read-only on live Linux Type-C & USB power_supply sysfs):
 *   - /sys/class/typec/portN/power_role  (e.g. "source [sink]")
 *   - /sys/class/typec/portN/data_role   (e.g. "host [device]")
 *   - /sys/class/typec/portN/port_type   (e.g. "dual", "sink", "source")
 *   - /sys/class/power_supply/usb/online (fallback when Type-C class absent)
 *
 * Safety invariant: NEVER writes to /sys/class/typec or /config/usb_gadget/g1/UDC so
 * active COM9/USB debugging sessions can never be torn down by a role switch request.
 */

#include "v2_0/iusb_port_interface.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

struct HdfDeviceObject;

namespace OHOS {
namespace HDI {
namespace Usb {
namespace V2_0 {

namespace {

constexpr const char *kTypecRoot = "/sys/class/typec";
constexpr const char *kUsbPsyOnline = "/sys/class/power_supply/usb/online";

std::mutex g_usbMu;
std::vector<sptr<IUsbdSubscriber>> g_subscribers;

bool ReadSysfsLine(const std::string &path, std::string &out)
{
    FILE *fp = std::fopen(path.c_str(), "re");
    if (fp == nullptr) {
        return false;
    }
    char buf[256] = {0};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, fp);
    std::fclose(fp);
    if (n == 0) {
        return false;
    }
    buf[n] = '\0';
    while (n > 0) {
        break;
    }
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
        out.pop_back();
    }
    return !out.empty();
}

/* Extract the active bracketed token "[foo]" from Linux Type-C role files, or whole trimmed string. */
std::string ActiveToken(const std::string &raw)
{
    const size_t lb = raw.find('[');
    if (lb != std::string::npos) {
        const size_t rb = raw.find(']', lb + 1);
        if (rb != std::string::npos && rb > lb + 1) {
            return raw.substr(lb + 1, rb - lb - 1);
        }
    }
    return raw;
}

int32_t ParsePowerRole(const std::string &raw)
{
    const std::string active = ActiveToken(raw);
    if (active == "source") {
        return MARBLE_POWER_ROLE_SOURCE;
    }
    if (active == "sink") {
        return MARBLE_POWER_ROLE_SINK;
    }
    return MARBLE_POWER_ROLE_NONE;
}

int32_t ParseDataRole(const std::string &raw)
{
    const std::string active = ActiveToken(raw);
    if (active == "host") {
        return MARBLE_DATA_ROLE_HOST;
    }
    if (active == "device") {
        return MARBLE_DATA_ROLE_DEVICE;
    }
    return MARBLE_DATA_ROLE_NONE;
}

int32_t ParseSupportedModes(const std::string &powerRaw, const std::string &dataRaw, const std::string &portTypeRaw)
{
    const bool canHost = (dataRaw.find("host") != std::string::npos) ||
                         (powerRaw.find("source") != std::string::npos) ||
                         (portTypeRaw.find("source") != std::string::npos) ||
                         (portTypeRaw.find("dual") != std::string::npos);
    const bool canDevice = (dataRaw.find("device") != std::string::npos) ||
                           (powerRaw.find("sink") != std::string::npos) ||
                           (portTypeRaw.find("sink") != std::string::npos) ||
                           (portTypeRaw.find("dual") != std::string::npos);
    if (canHost && canDevice) {
        return MARBLE_SUPPORTED_MODES_UFP_DFP;
    }
    if (canHost) {
        return MARBLE_SUPPORTED_MODES_DFP;
    }
    if (canDevice) {
        return MARBLE_SUPPORTED_MODES_UFP;
    }
    return MARBLE_SUPPORTED_MODES_NONE;
}

bool ReadTypecPort(int32_t kernelIdx, UsbPort &out)
{
    const std::string base = std::string(kTypecRoot) + "/port" + std::to_string(kernelIdx);
    std::string powerRaw;
    std::string dataRaw;
    std::string portTypeRaw;
    const bool hasPower = ReadSysfsLine(base + "/power_role", powerRaw);
    const bool hasData = ReadSysfsLine(base + "/data_role", dataRaw);
    (void)ReadSysfsLine(base + "/port_type", portTypeRaw);
    if (!hasPower && !hasData) {
        return false;
    }

    const int32_t powerRole = hasPower ? ParsePowerRole(powerRaw) : MARBLE_POWER_ROLE_SINK;
    const int32_t dataRole = hasData ? ParseDataRole(dataRaw) : MARBLE_DATA_ROLE_DEVICE;
    int32_t mode = MARBLE_PORT_MODE_NONE;
    if (dataRole == MARBLE_DATA_ROLE_HOST) {
        mode = MARBLE_PORT_MODE_HOST;
    } else if (dataRole == MARBLE_DATA_ROLE_DEVICE) {
        mode = MARBLE_PORT_MODE_DEVICE;
    } else if (powerRole == MARBLE_POWER_ROLE_SOURCE) {
        mode = MARBLE_PORT_MODE_HOST;
    } else if (powerRole == MARBLE_POWER_ROLE_SINK) {
        mode = MARBLE_PORT_MODE_DEVICE;
    }

    out.id = kernelIdx + 1; /* port0 -> DEFAULT_PORT_ID (1) */
    out.supportedModes = ParseSupportedModes(powerRaw, dataRaw, portTypeRaw);
    if (out.supportedModes == MARBLE_SUPPORTED_MODES_NONE) {
        out.supportedModes = MARBLE_SUPPORTED_MODES_UFP_DFP;
    }
    out.usbPortStatus.currentMode = mode;
    out.usbPortStatus.currentPowerRole = powerRole;
    out.usbPortStatus.currentDataRole = dataRole;
    out.usbPortStatus.reserved = 0;
    return true;
}

std::vector<UsbPort> ScanHardwarePorts()
{
    std::vector<int32_t> indices;
    DIR *dir = opendir(kTypecRoot);
    if (dir != nullptr) {
        struct dirent *ent = nullptr;
        while ((ent = readdir(dir)) != nullptr) {
            if (std::strncmp(ent->d_name, "port", 4) != 0) {
                continue;
            }
            const char *numStr = ent->d_name + 4;
            if (*numStr < '0' || *numStr > '9') {
                continue;
            }
            char *endp = nullptr;
            long val = std::strtol(numStr, &endp, 10);
            if (endp != nullptr && *endp == '\0' && val >= 0 && val < 16) {
                indices.push_back(static_cast<int32_t>(val));
            }
        }
        closedir(dir);
    }
    std::sort(indices.begin(), indices.end());

    std::vector<UsbPort> ports;
    for (int32_t idx : indices) {
        UsbPort p {};
        if (ReadTypecPort(idx, p)) {
            ports.push_back(p);
        }
    }

    if (ports.empty()) {
        std::string online;
        UsbPort fallback {};
        fallback.id = MARBLE_DEFAULT_PORT_ID;
        fallback.supportedModes = MARBLE_SUPPORTED_MODES_UFP_DFP;
        if (ReadSysfsLine(kUsbPsyOnline, online) && online == "1") {
            fallback.usbPortStatus.currentMode = MARBLE_PORT_MODE_DEVICE;
            fallback.usbPortStatus.currentPowerRole = MARBLE_POWER_ROLE_SINK;
            fallback.usbPortStatus.currentDataRole = MARBLE_DATA_ROLE_DEVICE;
        } else {
            fallback.usbPortStatus.currentMode = MARBLE_PORT_MODE_NONE;
            fallback.usbPortStatus.currentPowerRole = MARBLE_POWER_ROLE_NONE;
            fallback.usbPortStatus.currentDataRole = MARBLE_DATA_ROLE_NONE;
        }
        ports.push_back(fallback);
    }
    return ports;
}

PortInfo ToPortInfo(const UsbPort &p)
{
    PortInfo info {};
    info.portId = p.id;
    info.powerRole = p.usbPortStatus.currentPowerRole;
    info.dataRole = p.usbPortStatus.currentDataRole;
    info.mode = p.usbPortStatus.currentMode;
    info.supportedModes = p.supportedModes;
    info.reserved = 0;
    return info;
}

void DispatchPortInfoAsync(const sptr<IUsbdSubscriber> &sub, const PortInfo &info)
{
    if (sub == nullptr) {
        return;
    }
    std::thread([sub, info]() {
        usleep(25000);
        (void)sub->PortChangedEvent(info);
    }).detach();
}

} // namespace

/*
 * Exact donor layout & ABI:
 *   - Sole non-static data member `device_` at offset +8 (vptr at +0, virtual base HdiBase at +16)
 *   - Matches donor libusb_port_driver.z.so `str x21, [x8, #8]` and 240-byte vtable.
 *   - Exports `UsbPortImpl::UsbdEventHandle(const sptr<UsbPortImpl> &inst)` called by driver Bind.
 */
class UsbPortImpl : public IUsbPortInterface {
public:
    UsbPortImpl() = default;
    ~UsbPortImpl() override = default;

    int32_t SetPortRole(int32_t portId, int32_t powerRole, int32_t dataRole) override;
    int32_t QueryPort(int32_t &portId, int32_t &powerRole, int32_t &dataRole, int32_t &mode) override;
    int32_t QueryPorts(std::vector<UsbPort> &portList) override;
    int32_t BindUsbdPortSubscriber(const sptr<IUsbdSubscriber> &subscriber) override;
    int32_t UnbindUsbdPortSubscriber(const sptr<IUsbdSubscriber> &subscriber) override;

    static int32_t UsbdEventHandle(const sptr<UsbPortImpl> &inst);

    HdfDeviceObject *device_ = nullptr;
};

int32_t UsbPortImpl::UsbdEventHandle(const sptr<UsbPortImpl> &inst)
{
    if (inst == nullptr) {
        return HDF_ERR_INVALID_PARAM;
    }
    return HDF_SUCCESS;
}

int32_t UsbPortImpl::QueryPort(int32_t &portId, int32_t &powerRole, int32_t &dataRole, int32_t &mode)
{
    const std::vector<UsbPort> ports = ScanHardwarePorts();
    if (ports.empty()) {
        return HDF_FAILURE;
    }
    const UsbPort &p = ports[0];
    portId = p.id;
    powerRole = p.usbPortStatus.currentPowerRole;
    dataRole = p.usbPortStatus.currentDataRole;
    mode = p.usbPortStatus.currentMode;
    return HDF_SUCCESS;
}

int32_t UsbPortImpl::QueryPorts(std::vector<UsbPort> &portList)
{
    portList = ScanHardwarePorts();
    return portList.empty() ? HDF_FAILURE : HDF_SUCCESS;
}

int32_t UsbPortImpl::SetPortRole(int32_t portId, int32_t powerRole, int32_t dataRole)
{
    if (powerRole != MARBLE_POWER_ROLE_SOURCE && powerRole != MARBLE_POWER_ROLE_SINK) {
        return HDF_ERR_INVALID_PARAM;
    }
    if (dataRole != MARBLE_DATA_ROLE_HOST && dataRole != MARBLE_DATA_ROLE_DEVICE) {
        return HDF_ERR_INVALID_PARAM;
    }

    const std::vector<UsbPort> ports = ScanHardwarePorts();
    const UsbPort *target = nullptr;
    for (const auto &p : ports) {
        if (p.id == portId) {
            target = &p;
            break;
        }
    }
    if (target == nullptr) {
        return HDF_ERR_INVALID_PARAM;
    }

    /*
     * Safe non-disruptive guard:
     * If requested role already matches live SM7475 Type-C hardware state, succeed immediately.
     * Never mutate /sys/class/typec or UDC to avoid dropping active USB/COM9 debug connections.
     */
    if (target->usbPortStatus.currentPowerRole == powerRole &&
        target->usbPortStatus.currentDataRole == dataRole) {
        return HDF_SUCCESS;
    }
    return HDF_ERR_NOT_SUPPORT;
}

int32_t UsbPortImpl::BindUsbdPortSubscriber(const sptr<IUsbdSubscriber> &subscriber)
{
    if (subscriber == nullptr) {
        return HDF_ERR_INVALID_PARAM;
    }

    {
        std::lock_guard<std::mutex> lock(g_usbMu);
        bool exists = false;
        for (const auto &s : g_subscribers) {
            if (s.GetRefPtr() == subscriber.GetRefPtr()) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            if (g_subscribers.size() >= 8) {
                g_subscribers.erase(g_subscribers.begin());
            }
            g_subscribers.push_back(subscriber);
        }
    }

    const std::vector<UsbPort> ports = ScanHardwarePorts();
    if (!ports.empty()) {
        DispatchPortInfoAsync(subscriber, ToPortInfo(ports[0]));
    }
    return HDF_SUCCESS;
}

int32_t UsbPortImpl::UnbindUsbdPortSubscriber(const sptr<IUsbdSubscriber> &subscriber)
{
    if (subscriber == nullptr) {
        return HDF_ERR_INVALID_PARAM;
    }
    std::lock_guard<std::mutex> lock(g_usbMu);
    g_subscribers.erase(
        std::remove_if(g_subscribers.begin(), g_subscribers.end(),
                       [&subscriber](const sptr<IUsbdSubscriber> &s) {
                           return s.GetRefPtr() == subscriber.GetRefPtr();
                       }),
        g_subscribers.end());
    return HDF_SUCCESS;
}

extern "C" IUsbPortInterface *UsbPortInterfaceImplGetInstance(void)
{
    return new (std::nothrow) UsbPortImpl();
}

} // namespace V2_0
} // namespace Usb
} // namespace HDI
} // namespace OHOS
