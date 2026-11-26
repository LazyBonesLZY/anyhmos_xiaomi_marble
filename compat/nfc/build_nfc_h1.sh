#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch H1 SM7475 Hardware NFC artifacts for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-h1-build/marble_nfc_hw_probe
#   - work/marble-h1-build/libmarble_nfc_interface_service_1.1.z.so
#   - work/marble-h1-build/native_nfc_hdi_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${H1_BUILD_OUT:-$ROOT/work/marble-h1-build}"
mkdir -p "$OUT"

SDK="$ROOT/work/ohos-sdk/installed/native"
CLANG="$SDK/llvm/bin/clang"
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
  -I"$ROOT/compat/nfc/include"
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

# 1. Standalone C11 NXP SN110T NCI 2.0 hardware probe
"$CLANG" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -O2 -std=gnu11 -Wall -Wextra -Werror \
  "$ROOT/compat/nfc/marble_nfc_hw_probe.c" -o "$OUT/marble_nfc_hw_probe"

# 2. Clean-room OHOS NFC HDI v1.1 implementation (SONAME libnfc_interface_service_1.1.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/nfc/marble_nfc_hdi_impl.cpp" -o "$OUT/marble_nfc_hdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libnfc_interface_service_1.1.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_nfc_hdi_impl.o" \
  -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lutils.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_nfc_interface_service_1.1.z.so"

# 3. End-to-end OHOS Binder/HDI NFC probe linked against donor libnfc_proxy_1.1.z.so
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/nfc/native_nfc_hdi_probe.cpp" \
  -L"$SYS_LIB" -L"$PLAT_LIB" -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lnfc_proxy_1.1.z -lnfc_proxy_1.0.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_nfc_hdi_probe"

echo "=== NFC HDI ELF & VTABLE AUDIT ==="
"$READELF" -d "$OUT/libmarble_nfc_interface_service_1.1.z.so" | grep -E 'NEEDED|SONAME'
"$NM" -D "$OUT/libmarble_nfc_interface_service_1.1.z.so" | grep NfcInterfaceImplGetInstance
"$READELF" -sW "$OUT/libmarble_nfc_interface_service_1.1.z.so" | grep -E '_ZTV|_ZTC' | grep NfcImpl
echo "=== NFC PROBE ELF AUDIT ==="
"$READELF" -d "$OUT/native_nfc_hdi_probe" | grep NEEDED
"$READELF" -d "$OUT/marble_nfc_hw_probe" | grep NEEDED
ls -la "$OUT/libmarble_nfc_interface_service_1.1.z.so" "$OUT/native_nfc_hdi_probe" "$OUT/marble_nfc_hw_probe"
