#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch H2 SM7475 Hardware Thermal HDI v1.1 artifacts for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-h2-build/libmarble_thermal_interface_service_1.1.z.so
#   - work/marble-h2-build/native_thermal_hdi_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${H2_BUILD_OUT:-$ROOT/work/marble-h2-build}"
mkdir -p "$OUT"

SDK="$ROOT/work/ohos-sdk/installed/native"
CLANGXX="$SDK/llvm/bin/clang++"
READELF="$SDK/llvm/bin/llvm-readelf"
NM="$SDK/llvm/bin/llvm-nm"
SYSROOT="$SDK/sysroot"
UP="$ROOT/work/upstream"
DONOR_FS="$ROOT/work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs"
LINK_DIR="$ROOT/work/donor-hdi-link"

SYS_LIB="$DONOR_FS/system/system/lib64"
CHIP_SP="$DONOR_FS/system/system/lib64/chipset-sdk-sp"
PLAT_LIB="$DONOR_FS/system/system/lib64/platformsdk"

INC_COMMON=(
  -I"$ROOT/compat/thermal/include"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/hdi"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/utils"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/core"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/osal/uhdf"
  -I"$UP/drivers_hdf_core/framework/include/utils"
  -I"$UP/drivers_hdf_core/framework/include/osal"
  -I"$UP/drivers_hdf_core/framework/include/core"
  -I"$UP/communication_ipc/interfaces/innerkits/ipc_core/include"
  -I"$UP/commonlibrary_c_utils/base/include"
  -I"$UP/foundation_systemabilitymgr_samgr/interfaces/innerkits/samgr_proxy/include"
  -I"$SYSROOT/usr/include"
)

CXX_ABI=(
  --target=aarch64-linux-ohos
  --sysroot="$SYSROOT"
  -O2 -std=c++17
  -DOHOS_PLATFORM
  -include "$ROOT/compat/platform/platform_cpp_config.h"
  -fno-rtti -fno-exceptions
  -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations
)

# 1. Clean-room OHOS Thermal HDI v1.1 implementation (SONAME libthermal_interface_service_1.1.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/thermal/marble_thermal_hdi_impl.cpp" -o "$OUT/marble_thermal_hdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libthermal_interface_service_1.1.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_thermal_hdi_impl.o" \
  -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lutils.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_thermal_interface_service_1.1.z.so"

# 2. End-to-end OHOS Binder/HDI Thermal probe linked against donor libthermal_proxy_1.1.z.so
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/thermal/native_thermal_hdi_probe.cpp" \
  -L"$SYS_LIB" -L"$PLAT_LIB" -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lthermal_proxy_1.1.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_thermal_hdi_probe"

echo "=== THERMAL HDI ELF & VTABLE AUDIT ==="
"$READELF" -d "$OUT/libmarble_thermal_interface_service_1.1.z.so" | grep -E 'NEEDED|SONAME'
"$NM" -D "$OUT/libmarble_thermal_interface_service_1.1.z.so" | grep -E 'ThermalInterfaceImplGetInstance|marble_thermal_scan_summary'
"$READELF" -sW "$OUT/libmarble_thermal_interface_service_1.1.z.so" | grep -E '_ZTV.*ThermalInterfaceImpl'
echo "=== THERMAL PROBE ELF AUDIT ==="
"$READELF" -d "$OUT/native_thermal_hdi_probe" | grep NEEDED
ls -la "$OUT/libmarble_thermal_interface_service_1.1.z.so" "$OUT/native_thermal_hdi_probe"
