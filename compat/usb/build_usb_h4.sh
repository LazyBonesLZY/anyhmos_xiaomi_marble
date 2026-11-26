#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch H4 SM7475 Hardware USB Port HDI v2.0 artifacts for Xiaomi marble (aarch64-linux-ohos):
#   - work/marble-h4-build/libmarble_usb_port_interface_service_2.0.z.so
#   - work/marble-h4-build/native_usb_port_hdi_probe
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${H4_BUILD_OUT:-$ROOT/work/marble-h4-build}"
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
  -I"$ROOT/compat/usb/include"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/hdi"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/utils"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/core"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/osal/uhdf"
  -I"$UP/drivers_hdf_core/framework/include/utils"
  -I"$UP/drivers_hdf_core/framework/include/osal"
  -I"$UP/drivers_hdf_core/framework/include/core"
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

# 1. Clean-room OHOS USB Port HDI v2.0 implementation (SONAME libusb_port_interface_service_2.0.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/usb/marble_usb_port_hdi_impl.cpp" -o "$OUT/marble_usb_port_hdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libusb_port_interface_service_2.0.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_usb_port_hdi_impl.o" \
  -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lutils.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_usb_port_interface_service_2.0.z.so"

# 2. End-to-end Binder probe linked against unmodified donor chipset-sdk-sp/libusb_proxy_2.0.z.so
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" \
  -c "$ROOT/compat/usb/native_usb_port_hdi_probe.cpp" -o "$OUT/native_usb_port_hdi_probe.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -pie \
  -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/native_usb_port_hdi_probe.o" \
  -L"$CHIP_SP" -L"$PLAT_LIB" -L"$SYS_LIB" -L"$LINK_DIR" \
  -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lusb_proxy_2.0.z -lipc_single.z -lutils.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_usb_port_hdi_probe"

echo "=== libmarble_usb_port_interface_service_2.0.z.so NEEDED ==="
"$READELF" -d "$OUT/libmarble_usb_port_interface_service_2.0.z.so" | grep -E 'SONAME|NEEDED'
echo "=== libmarble_usb_port_interface_service_2.0.z.so VTABLE & EXPORTS ==="
"$READELF" -sW "$OUT/libmarble_usb_port_interface_service_2.0.z.so" | grep -E '_ZTVN4OHOS3HDI3Usb4V2_011UsbPortImplE|UsbPortInterfaceImplGetInstance|UsbdEventHandle'
echo "=== native_usb_port_hdi_probe NEEDED ==="
"$READELF" -d "$OUT/native_usb_port_hdi_probe" | grep NEEDED
echo "H4_USB_PORT_BUILD_OK out=$OUT"
