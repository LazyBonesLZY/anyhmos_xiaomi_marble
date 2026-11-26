#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch M1 SM7475 Hardware Vibrator / Haptics HDI v2.0 artifacts for Xiaomi marble:
#   - work/marble-m1-build/vibrator/libmarble_vibrator_interface_service_2.0.z.so
#   - work/marble-m1-build/vibrator/native_vibrator_hdi_probe
#   - work/marble-m1-build/vibrator/test_vibrator_host (offline host unit test)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${M1_VIBRATOR_BUILD_OUT:-$ROOT/work/marble-m1-build/vibrator}"
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
  -I"$ROOT/compat/vibrator/include"
  -I"$ROOT/compat/vibrator"
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

# 1. Clean-room OHOS Vibrator HDI v2.0 implementation (SONAME libvibrator_interface_service_2.0.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/vibrator/marble_vibrator_hdi_impl.cpp" \
  -o "$OUT/marble_vibrator_hdi_impl.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libvibrator_interface_service_2.0.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_vibrator_hdi_impl.o" \
  -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lutils.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_vibrator_interface_service_2.0.z.so"

# 2. End-to-end OHOS Binder/HDI Vibrator v2.0 probe linked against donor libvibrator_proxy_2.0.z.so
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/vibrator/native_vibrator_hdi_probe.cpp" \
  -L"$CHIP_SDK" -L"$SYS_LIB" -L"$PLAT_LIB" -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lvibrator_proxy_2.0.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_vibrator_hdi_probe"

# 3. Offline host unit test
HOST_CXX="${HOST_CXX:-g++}"
"$HOST_CXX" -O2 -std=c++17 -pthread -Wall -Wextra -Werror \
  -I"$ROOT/compat/vibrator/include" \
  -I"$ROOT/compat/vibrator" \
  "$ROOT/compat/vibrator/test_vibrator_host.cpp" \
  -o "$OUT/test_vibrator_host"

echo "=== RUNNING HOST UNIT TEST ==="
"$OUT/test_vibrator_host"

SO="$OUT/libmarble_vibrator_interface_service_2.0.z.so"
PROBE="$OUT/native_vibrator_hdi_probe"

echo "=== VIBRATOR HDI ELF & VTABLE AUDIT ==="
"$READELF" -d "$SO" | grep -E 'NEEDED|SONAME'
"$NM" -D "$SO" | grep VibratorInterfaceImplGetInstance
VTABLE_LINE="$("$READELF" -sW "$SO" | grep -E '_ZTVN4OHOS3HDI8Vibrator4V2_017VibratorIfServiceE' | head -n 1)"
echo "$VTABLE_LINE"

# Verify strict NEEDED set == [libc++.so, libc.so, libutils.z.so]
NEEDED_SORTED="$("$READELF" -d "$SO" | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ',' | sed 's/,$//')"
if [ "$NEEDED_SORTED" != "libc++.so,libc.so,libutils.z.so" ]; then
  echo "ERROR: Unexpected NEEDED set: $NEEDED_SORTED" >&2
  exit 1
fi

# Verify exact SONAME
SONAME_VAL="$("$READELF" -d "$SO" | sed -n 's/.*(SONAME).*Library soname: \[\(.*\)\]/\1/p')"
if [ "$SONAME_VAL" != "libvibrator_interface_service_2.0.z.so" ]; then
  echo "ERROR: Unexpected SONAME: $SONAME_VAL" >&2
  exit 1
fi

# Verify exact 360-byte (45-slot) vtable matching donor
VTABLE_SIZE="$(echo "$VTABLE_LINE" | awk '{print $3}')"
if [ "$VTABLE_SIZE" != "360" ]; then
  echo "ERROR: VibratorIfService vtable size is $VTABLE_SIZE (expected 360)" >&2
  exit 1
fi

echo "=== VIBRATOR PROBE ELF AUDIT ==="
"$READELF" -d "$PROBE" | grep NEEDED

echo "=== ARTIFACT SIZES & SHA256 ==="
ls -la "$SO" "$PROBE"
sha256sum "$SO" "$PROBE"
