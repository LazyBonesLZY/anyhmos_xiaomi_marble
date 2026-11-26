/* SPDX-License-Identifier: MIT */
#ifndef OHOS_HDI_THERMAL_V1_1_THERMALTYPES_H
#define OHOS_HDI_THERMAL_V1_1_THERMALTYPES_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace OHOS {
namespace HDI {
namespace Thermal {
namespace V1_1 {

struct ThermalZoneInfo {
    std::string type;
    int32_t temp;
};

struct HdfThermalCallbackInfo {
    std::vector<ThermalZoneInfo> info;
};

static_assert(sizeof(ThermalZoneInfo) == 32, "ThermalZoneInfo ABI size mismatch vs donor libthermal_stub_1.1.z.so");
static_assert(offsetof(ThermalZoneInfo, type) == 0, "ThermalZoneInfo::type offset mismatch");
static_assert(offsetof(ThermalZoneInfo, temp) == 24, "ThermalZoneInfo::temp offset mismatch");
static_assert(sizeof(HdfThermalCallbackInfo) == 24, "HdfThermalCallbackInfo ABI size mismatch");

} // namespace V1_1
} // namespace Thermal
} // namespace HDI
} // namespace OHOS

#endif // OHOS_HDI_THERMAL_V1_1_THERMALTYPES_H
