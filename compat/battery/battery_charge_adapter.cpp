#include "battery_charge_adapter.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace Hmos::Charging {
namespace {
constexpr const char* USB_ROOT = "/sys/class/power_supply/usb/";
constexpr const char* QCOM_ROOT = "/sys/class/qcom-battery/";

#ifdef HMOS_BATTERY_HOST_TEST
std::string g_testSysfsRoot;
#endif

std::string SysfsPath(const char* base, const char* leaf)
{
#ifdef HMOS_BATTERY_HOST_TEST
    if (!g_testSysfsRoot.empty()) {
        return g_testSysfsRoot + base + leaf;
    }
#endif
    return std::string(base) + leaf;
}

bool ReadText(const std::string& path, std::string& value)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    char buffer[129];
    size_t used = 0;
    bool ok = true;
    while (used < sizeof(buffer)) {
        const ssize_t count = read(fd, buffer + used, sizeof(buffer) - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            ok = false;
            break;
        }
        if (count == 0) {
            break;
        }
        used += static_cast<size_t>(count);
    }
    close(fd);
    if (!ok || used == 0 || used == sizeof(buffer)) {
        return false;
    }
    value.assign(buffer, used);
    const size_t last = value.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) {
        return false;
    }
    value.erase(last + 1);
    const size_t first = value.find_first_not_of(" \t\r\n");
    value.erase(0, first);
    return value.find('\0') == std::string::npos;
}

bool ReadNumber(const std::string& path, int32_t& value)
{
    std::string text;
    if (!ReadText(path, text) || text.find_first_not_of("0123456789") != std::string::npos) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long result = strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' || result < 0 || result > INT32_MAX) {
        return false;
    }
    value = static_cast<int32_t>(result);
    return true;
}

bool ReadSignedMagnitude(const std::string& path, int32_t& value)
{
    std::string text;
    if (!ReadText(path, text) || text.empty()) {
        return false;
    }
    size_t start = 0;
    if (text[0] == '-' || text[0] == '+') {
        start = 1;
    }
    if (start >= text.size() || text.find_first_not_of("0123456789", start) != std::string::npos) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long result = strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    const long long mag = result < 0 ? -result : result;
    if (mag > INT32_MAX) {
        return false;
    }
    value = static_cast<int32_t>(mag);
    return true;
}

void ReadOptionalFlag(const std::string& path, int32_t& out)
{
    int32_t tmp = 0;
    if (ReadNumber(path, tmp) && tmp <= 1) {
        out = tmp;
    }
}

void ReadOptionalNumber(const std::string& path, int32_t& out)
{
    int32_t tmp = 0;
    if (ReadNumber(path, tmp)) {
        out = tmp;
    }
}

void ReadOptionalSignedMagnitude(const std::string& path, int32_t& out)
{
    int32_t tmp = 0;
    if (ReadSignedMagnitude(path, tmp)) {
        out = tmp;
    }
}

void ReadOptionalText(const std::string& path, std::string& out)
{
    std::string tmp;
    if (ReadText(path, tmp)) {
        out = tmp;
    }
}

int32_t NormalizeMicroToMilli(int32_t raw)
{
    if (raw <= 0) {
        return 0;
    }
    // Linux power_supply sysfs reports uV / uA; normalize to mV / mA.
    return raw >= 100000 ? raw / 1000 : raw;
}

int32_t WattsToMw(int32_t watts)
{
    if (watts <= 0) {
        return 0;
    }
    if (watts > INT32_MAX / 1000) {
        return INT32_MAX;
    }
    return watts * 1000;
}

struct Facts {
    int32_t online = 0;
    int32_t authentic = 0;
    int32_t verified = 0;
    int32_t quickType = 0;
    int32_t powerW = 0;
    int32_t apdoMaxW = 0;
    int32_t fastchgMode = 0;
    std::string realType;
    std::string usbRealType;
    int32_t usbVoltageMax = 0;
    int32_t usbCurrentMax = 0;
    int32_t usbVoltageNow = 0;
    int32_t usbCurrentNow = 0;
};

bool ReadFacts(Facts& facts)
{
    facts = {};
    if (!ReadNumber(SysfsPath(USB_ROOT, "online"), facts.online) || facts.online > 1) {
        return false;
    }
    if (facts.online == 0) {
        return true;
    }

    // Missing or unreadable auth/protocol nodes default to 0 instead of failing the query.
    ReadOptionalFlag(SysfsPath(QCOM_ROOT, "authentic"), facts.authentic);
    int32_t pdVerified = 0;
    if (ReadNumber(SysfsPath(QCOM_ROOT, "pd_verifed"), pdVerified) && pdVerified <= 1) {
        facts.verified = pdVerified;
    } else {
        ReadOptionalFlag(SysfsPath(QCOM_ROOT, "pd_verified"), facts.verified);
    }
    ReadOptionalNumber(SysfsPath(QCOM_ROOT, "quick_charge_type"), facts.quickType);
    ReadOptionalNumber(SysfsPath(QCOM_ROOT, "power_max"), facts.powerW);
    ReadOptionalNumber(SysfsPath(QCOM_ROOT, "apdo_max"), facts.apdoMaxW);
    ReadOptionalNumber(SysfsPath(QCOM_ROOT, "fastchg_mode"), facts.fastchgMode);
    ReadOptionalText(SysfsPath(QCOM_ROOT, "real_type"), facts.realType);
    ReadOptionalText(SysfsPath(QCOM_ROOT, "usb_real_type"), facts.usbRealType);
    if (facts.usbRealType.empty()) {
        ReadOptionalText(SysfsPath(USB_ROOT, "real_type"), facts.usbRealType);
    }

    ReadOptionalNumber(SysfsPath(USB_ROOT, "voltage_max"), facts.usbVoltageMax);
    ReadOptionalNumber(SysfsPath(USB_ROOT, "current_max"), facts.usbCurrentMax);
    ReadOptionalNumber(SysfsPath(USB_ROOT, "voltage_now"), facts.usbVoltageNow);
    ReadOptionalSignedMagnitude(SysfsPath(USB_ROOT, "current_now"), facts.usbCurrentNow);
    return true;
}

bool MatchesType(const std::string& type, const char* expected)
{
    return type == expected;
}

bool MatchesAnyRealType(const Facts& facts, const char* expected)
{
    return MatchesType(facts.realType, expected) || MatchesType(facts.usbRealType, expected);
}

bool IsQuickRealTypeName(const std::string& type)
{
    return type == "USB_PD" || type == "PD_PPS" || type == "USB_HVDCP" ||
        type == "USB_HVDCP_3" || type == "USB_HVDCP_3P5";
}

bool IsQuickRealType(const Facts& facts)
{
    return IsQuickRealTypeName(facts.realType) || IsQuickRealTypeName(facts.usbRealType);
}

ChargeSnapshot BuildSnapshotFromFacts(const Facts& facts, bool stillOnline)
{
    ChargeSnapshot snapshot;
    snapshot.valid = true;
    snapshot.online = facts.online != 0 && stillOnline;
    if (!snapshot.online) {
        return snapshot;
    }

    const int32_t peakPowerW = std::max(facts.powerW, facts.apdoMaxW);
    const int32_t usbVMaxMv = NormalizeMicroToMilli(facts.usbVoltageMax);
    const int32_t usbIMaxMa = NormalizeMicroToMilli(facts.usbCurrentMax);
    const int32_t usbMaxPowerMw = (usbVMaxMv > 0 && usbIMaxMa > 0)
        ? static_cast<int32_t>(std::min<int64_t>(
            (static_cast<int64_t>(usbVMaxMv) * usbIMaxMa) / 1000, INT32_MAX))
        : 0;

    if (facts.quickType == 3 || facts.quickType == 4 ||
        (MatchesAnyRealType(facts, "PD_PPS") && peakPowerW >= 30) ||
        facts.fastchgMode == 1) {
        snapshot.type = Battery::CHARGE_TYPE_WIRED_SUPER_QUICK;
        snapshot.maxPowerMw = peakPowerW > 0 ? WattsToMw(peakPowerW) : usbMaxPowerMw;
        return snapshot;
    }

    if (facts.quickType == 1 || facts.quickType == 2 ||
        IsQuickRealType(facts) || peakPowerW >= 15) {
        snapshot.type = Battery::CHARGE_TYPE_WIRED_QUICK;
        snapshot.maxPowerMw = WattsToMw(std::max(peakPowerW, int32_t{18}));
        return snapshot;
    }

    snapshot.type = Battery::CHARGE_TYPE_WIRED_NORMAL;
    return snapshot;
}

bool ReadLiveChargeContext(ChargeSnapshot& snapshot, Facts& facts)
{
    int32_t stillOnline = 0;
    if (!ReadFacts(facts) || !ReadNumber(SysfsPath(USB_ROOT, "online"), stillOnline) ||
        stillOnline > 1) {
        snapshot = {};
        return false;
    }
    snapshot = BuildSnapshotFromFacts(facts, stillOnline != 0);
    return true;
}

void PopulatePluggedMaxInfo(Battery::BatteryInfo& value, const ChargeSnapshot& snapshot,
    const Facts* facts)
{
    if (!snapshot.valid || !snapshot.online) {
        return;
    }

    const int32_t usbVMaxMv = facts ? NormalizeMicroToMilli(facts->usbVoltageMax) : 0;
    const int32_t usbVNowMv = facts ? NormalizeMicroToMilli(facts->usbVoltageNow) : 0;
    const int32_t usbIMaxMa = facts ? NormalizeMicroToMilli(facts->usbCurrentMax) : 0;
    const int32_t usbINowMa = facts ? NormalizeMicroToMilli(facts->usbCurrentNow) : 0;

    int32_t targetV = std::max(usbVMaxMv, usbVNowMv);
    int32_t targetI = std::max(usbIMaxMa, usbINowMa);

    const bool isFast = (snapshot.type == Battery::CHARGE_TYPE_WIRED_QUICK ||
        snapshot.type == Battery::CHARGE_TYPE_WIRED_SUPER_QUICK);

    if (isFast && snapshot.maxPowerMw > 0) {
        const int64_t sysPowerMw = (targetV > 0 && targetI > 0)
            ? (static_cast<int64_t>(targetV) * targetI) / 1000
            : 0;
        if (targetV <= 5000 || targetI <= 0 || sysPowerMw < snapshot.maxPowerMw) {
            if (targetV <= 5000) {
                if (snapshot.type == Battery::CHARGE_TYPE_WIRED_SUPER_QUICK) {
                    targetV = snapshot.maxPowerMw >= 90000 ? 20000 : 11000;
                } else {
                    targetV = 9000;
                }
            }
            const int64_t derivedI =
                (static_cast<int64_t>(snapshot.maxPowerMw) * 1000 + targetV / 2) / targetV;
            if (derivedI > targetI && derivedI <= INT32_MAX) {
                targetI = static_cast<int32_t>(derivedI);
            }
        }
    } else if (!isFast) {
        if (targetV <= 0) {
            targetV = 5000;
        }
        if (targetI <= 0) {
            if (facts != nullptr && facts->powerW > 0 && facts->powerW <= INT32_MAX / 1000000) {
                targetI = static_cast<int32_t>(
                    (static_cast<int64_t>(facts->powerW) * 1000000 + targetV / 2) / targetV);
            } else {
                targetI = 500;
            }
        }
    }

    const bool donorMissingV = value.pluggedMaxVoltage <= 0;
    const bool donorMissingI = value.pluggedMaxCurrent <= 0;
    const int64_t donorPowerMw = (!donorMissingV && !donorMissingI)
        ? (static_cast<int64_t>(value.pluggedMaxVoltage) * value.pluggedMaxCurrent) / 1000
        : 0;
    const bool donorLowFastDefault = isFast &&
        (value.pluggedMaxVoltage <= 5000 ||
         (snapshot.maxPowerMw > 0 && donorPowerMw < snapshot.maxPowerMw));

    if ((donorMissingV || donorLowFastDefault) && targetV > 0) {
        value.pluggedMaxVoltage = targetV;
    }
    if ((donorMissingI || donorLowFastDefault) && targetI > 0) {
        value.pluggedMaxCurrent = targetI;
    }
}

class ChargeAdapter final : public Battery::IBatteryInterface {
public:
    ChargeAdapter(const OHOS::sptr<Battery::IBatteryInterface>& original, SnapshotReader reader)
        : original_(original), reader_(reader)
    {
    }

    int32_t Register(const OHOS::sptr<Battery::IBatteryCallback>& event) override
    {
        // Preserve the original callback object so donor proxy layout stays intact.
        return original_->Register(event);
    }

    int32_t UnRegister() override { return original_->UnRegister(); }
    int32_t ChangePath(const std::string& path) override { return original_->ChangePath(path); }
    int32_t GetCapacity(int32_t& value) override { return original_->GetCapacity(value); }
    int32_t GetVoltage(int32_t& value) override { return original_->GetVoltage(value); }
    int32_t GetTemperature(int32_t& value) override { return original_->GetTemperature(value); }
    int32_t GetHealthState(Battery::BatteryHealthState& value) override { return original_->GetHealthState(value); }
    int32_t GetPluggedType(Battery::BatteryPluggedType& value) override { return original_->GetPluggedType(value); }
    int32_t GetChargeState(Battery::BatteryChargeState& value) override { return original_->GetChargeState(value); }
    int32_t GetPresent(bool& value) override { return original_->GetPresent(value); }
    int32_t GetTechnology(std::string& value) override { return original_->GetTechnology(value); }
    int32_t GetTotalEnergy(int32_t& value) override { return original_->GetTotalEnergy(value); }
    int32_t GetCurrentAverage(int32_t& value) override { return original_->GetCurrentAverage(value); }
    int32_t GetCurrentNow(int32_t& value) override { return original_->GetCurrentNow(value); }
    int32_t GetRemainEnergy(int32_t& value) override { return original_->GetRemainEnergy(value); }

    int32_t GetBatteryInfo(Battery::BatteryInfo& value) override
    {
        const int32_t rc = original_->GetBatteryInfo(value);
        if (rc != HDF_SUCCESS) {
            return rc;
        }
        if (reader_ == ReadRealChargeSnapshot) {
            ChargeSnapshot snapshot;
            Facts facts;
            if (ReadLiveChargeContext(snapshot, facts)) {
                PopulatePluggedMaxInfo(value, snapshot, &facts);
            }
        } else {
            const ChargeSnapshot snapshot = reader_();
            PopulatePluggedMaxInfo(value, snapshot, nullptr);
        }
        return HDF_SUCCESS;
    }

    int32_t SetChargingLimit(const std::vector<Battery::ChargingLimit>& value) override
    {
        return original_->SetChargingLimit(value);
    }

    int32_t GetChargeType(Battery::ChargeType& type) override
    {
        const ChargeSnapshot snapshot = reader_();
        type = snapshot.valid ? snapshot.type : Battery::CHARGE_TYPE_NONE;
        return HDF_SUCCESS;
    }

    int32_t SetBatteryConfig(const std::string& scene, const std::string& value) override
    {
        return original_->SetBatteryConfig(scene, value);
    }

    int32_t GetBatteryConfig(const std::string& scene, std::string& value) override
    {
        if (scene != "charger_type" && scene != "max_power" && scene != "display_max_power") {
            return original_->GetBatteryConfig(scene, value);
        }
        const ChargeSnapshot snapshot = reader_();
        value = std::to_string(!snapshot.valid ? 0 : scene == "charger_type" ?
            static_cast<int32_t>(snapshot.type) : snapshot.maxPowerMw);
        return HDF_SUCCESS;
    }

    int32_t IsBatteryConfigSupported(const std::string& scene, bool& supported) override
    {
        if (scene == "charger_type" || scene == "max_power" || scene == "display_max_power") {
            supported = true;
            return HDF_SUCCESS;
        }
        return original_->IsBatteryConfigSupported(scene, supported);
    }

    int32_t GetVersion(uint32_t& major, uint32_t& minor) override
    {
        return original_->GetVersion(major, minor);
    }

private:
    OHOS::sptr<Battery::IBatteryInterface> original_;
    SnapshotReader reader_;
};
} // namespace

#ifdef HMOS_BATTERY_HOST_TEST
void SetTestSysfsRoot(const char* root)
{
    g_testSysfsRoot = root ? root : "";
}
#endif

ChargeSnapshot ReadRealChargeSnapshot()
{
    ChargeSnapshot snapshot;
    Facts facts;
    if (!ReadLiveChargeContext(snapshot, facts)) {
        return {};
    }
    return snapshot;
}

OHOS::sptr<Battery::IBatteryInterface> MakeBatteryChargeAdapter(
    const OHOS::sptr<Battery::IBatteryInterface>& original, SnapshotReader reader)
{
    if (original == nullptr || reader == nullptr) {
        return nullptr;
    }
    return new ChargeAdapter(original, reader);
}
} // namespace Hmos::Charging
