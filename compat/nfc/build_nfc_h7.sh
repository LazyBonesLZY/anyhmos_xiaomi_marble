#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch H7 SM7475 HarmonyOS NFC Framework SA 1140 artifacts for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-h7-build/libmarble_nci_native_vendor.z.so (SONAME libnci_native_vendor.z.so)
#   - work/marble-h7-build/native_nfc_fwk_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${H7_BUILD_OUT:-$ROOT/work/marble-h7-build}"
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
  -I"$UP/communication_ipc/interfaces/innerkits/ipc_core/include"
  -I"$UP/commonlibrary_c_utils/base/include"
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

# 1. Clean-room libnci_native_vendor.z.so (maps missing /sys/nfc/nfc_chip_type -> sn110u -> libnci_native_t001.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC \
  -c "$ROOT/compat/nfc/marble_nci_native_vendor.cpp" -o "$OUT/marble_nci_native_vendor.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libnci_native_vendor.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_nci_native_vendor.o" \
  -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_nci_native_vendor.z.so"

# 2. End-to-end HarmonyOS NFC Framework SA 1140 probe linked against donor libnfc_inner_kits_controller.z.so
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/nfc/native_nfc_fwk_probe.cpp" \
  -L"$SYS_LIB" -L"$PLAT_LIB" -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lnfc_inner_kits_controller.z -lnfc_inner_kits_common.z -lutils.z -lipc_single.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_nfc_fwk_probe"

echo "=== H7 VENDOR ADAPTER ELF & VTABLE AUDIT ==="
"$READELF" -d "$OUT/libmarble_nci_native_vendor.z.so" | grep -E 'NEEDED|SONAME'
"$NM" -D --defined-only "$OUT/libmarble_nci_native_vendor.z.so" | grep -E 'NewInterface|DeleteInterface|NciNativeAdapter|NciNativeFactory|NciLibsLoader|NciNativepProxyImpl'
"$READELF" -sW "$OUT/libmarble_nci_native_vendor.z.so" | grep -E '_ZTVN4OHOS3NFC3NCI19NciNativepProxyImplE'
echo "=== H7 FWK PROBE ELF AUDIT ==="
"$READELF" -d "$OUT/native_nfc_fwk_probe" | grep NEEDED
ls -la "$OUT/libmarble_nci_native_vendor.z.so" "$OUT/native_nfc_fwk_probe"
