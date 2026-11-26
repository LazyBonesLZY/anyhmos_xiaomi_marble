#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch H3 SM7475 Hardware Power HDI v1.3 artifacts for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-h3-build/libmarble_power_interface_service_1.3.z.so
#   - work/marble-h3-build/native_power_hdi_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${H3_BUILD_OUT:-$ROOT/work/marble-h3-build}"
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
CHIP_SDK="$DONOR_FS/system/system/lib64/chipset-sdk"
CHIP_SP="$DONOR_FS/system/system/lib64/chipset-sdk-sp"
PLAT_LIB="$DONOR_FS/system/system/lib64/platformsdk"

INC_COMMON=(
  -I"$ROOT/compat/power/include"
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
  -Wall -Wextra -Werror -Wno-unused-parameter -Wno-deprecated-declarations
)

# 1. Clean-room OHOS Power HDI v1.3 implementation (SONAME libpower_interface_service_1.3.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/power/marble_power_hdi_impl.cpp" -o "$OUT/marble_power_hdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libpower_interface_service_1.3.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_power_hdi_impl.o" \
  -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lutils.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_power_interface_service_1.3.z.so"

# 2. End-to-end OHOS Binder/HDI Power v1.3 probe linked against donor libpower_proxy_1.3.z.so
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/power/native_power_hdi_probe.cpp" \
  -L"$CHIP_SDK" -L"$SYS_LIB" -L"$PLAT_LIB" -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lpower_proxy_1.3.z -lpower_proxy_1.2.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_power_hdi_probe"

echo "=== POWER HDI ELF & VTABLE AUDIT ==="
"$READELF" -d "$OUT/libmarble_power_interface_service_1.3.z.so" | grep -E 'NEEDED|SONAME'
"$NM" -D "$OUT/libmarble_power_interface_service_1.3.z.so" | grep PowerInterfaceImplGetInstance
"$READELF" -sW "$OUT/libmarble_power_interface_service_1.3.z.so" | grep -E '_ZTV.*PowerInterfaceImpl'
echo "=== POWER PROBE ELF AUDIT ==="
"$READELF" -d "$OUT/native_power_hdi_probe" | grep NEEDED
ls -la "$OUT/libmarble_power_interface_service_1.3.z.so" "$OUT/native_power_hdi_probe"
