#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch M4 SM7475 Clean-Room OHOS Motion HDI v1.1 + Raise-to-Wake artifacts for Xiaomi marble:
#   - work/marble-m4-build/motion/libmarble_motion_interface_service_1.1.z.so
#   - work/marble-m4-build/motion/native_motion_hdi_probe
#   - work/marble-m4-build/motion/host_pickup_detector_test
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${M4_MOTION_BUILD_OUT:-$ROOT/work/marble-m4-build/motion}"
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
CHIP_SDK="$DONOR_FS/system/system/lib64/chipset-sdk"
CHIP_SP="$DONOR_FS/system/system/lib64/chipset-sdk-sp"
PLAT_LIB="$DONOR_FS/system/system/lib64/platformsdk"

INC_COMMON=(
  -I"$ROOT/compat/motion/include"
  -I"$ROOT/compat/motion"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/hdi"
  -I"$UP/drivers_hdf_core/interfaces/inner_api/ipc"
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

C_ABI=(
  --target=aarch64-linux-ohos
  --sysroot="$SYSROOT"
  -O2 -std=c11 -D_GNU_SOURCE -fPIC -pthread
  -Wall -Wextra -Werror
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

# 1. Compile read-only QRTR & SSC wire C helpers (zero edits to compat/sensors/ or compat/audio/)
"$CLANG" "${C_ABI[@]}" -I"$ROOT/compat/audio/pd-mapper" \
  -c "$ROOT/compat/audio/pd-mapper/qrtr.c" -o "$OUT/qrtr.o"

"$CLANG" "${C_ABI[@]}" -I"$ROOT/compat/audio/pd-mapper" \
  -c "$ROOT/compat/audio/pd-mapper/logging.c" -o "$OUT/logging.o"

"$CLANG" "${C_ABI[@]}" -Wno-unused-function \
  -I"$ROOT/compat/motion" \
  -I"$ROOT/compat/audio/pd-mapper" \
  -I"$ROOT/compat/sensors" \
  -c "$ROOT/compat/motion/ssc_motion_wire.c" -o "$OUT/ssc_motion_wire.o"

# 2. Clean-room OHOS Motion HDI v1.1 implementation (SONAME libmotion_interface_service_1.1.z.so)
"$CLANGXX" "${CXX_ABI[@]}" -fPIC "${INC_COMMON[@]}" \
  -c "$ROOT/compat/motion/marble_motion_interface_service_1.1.cpp" \
  -o "$OUT/marble_motion_interface_service_1.1.o"

"$CLANGXX" --target=aarch64-linux-ohos --sysroot="$SYSROOT" -shared \
  -Wl,-soname,libmotion_interface_service_1.1.z.so \
  -Wl,--no-undefined -Wl,--allow-shlib-undefined -nostdlib++ \
  "$OUT/marble_motion_interface_service_1.1.o" \
  "$OUT/ssc_motion_wire.o" "$OUT/qrtr.o" "$OUT/logging.o" \
  -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lutils.z "$LINK_DIR/libc++.so" \
  -o "$OUT/libmarble_motion_interface_service_1.1.z.so"

# 3. End-to-end OHOS Binder/HDI Motion v1.1 probe linked against donor libmotion_proxy_1.1.z.so + 1.0
"$CLANGXX" "${CXX_ABI[@]}" "${INC_COMMON[@]}" -nostdlib++ \
  "$ROOT/compat/motion/native_motion_hdi_probe.cpp" \
  -L"$CHIP_SDK" -L"$SYS_LIB" -L"$PLAT_LIB" -L"$CHIP_SP" -L"$LINK_DIR" -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lmotion_proxy_1.1.z -lmotion_proxy_1.0.z -lutils.z -lipc_single.z -lhdi.z "$LINK_DIR/libc++.so" \
  -o "$OUT/native_motion_hdi_probe"

# 4. Offline host unit test for PickupDetector
HOST_CXX="${HOST_CXX:-g++}"
"$HOST_CXX" -O2 -std=c++17 -Wall -Wextra -Werror \
  -I"$ROOT/compat/motion" \
  "$ROOT/compat/motion/test_pickup_detector_host.cpp" \
  -lm \
  -o "$OUT/host_pickup_detector_test"

echo "=== RUNNING HOST UNIT TEST ==="
"$OUT/host_pickup_detector_test"

SO="$OUT/libmarble_motion_interface_service_1.1.z.so"
PROBE="$OUT/native_motion_hdi_probe"
TEST_BIN="$OUT/host_pickup_detector_test"

echo "=== MOTION HDI ELF & VTABLE AUDIT ==="
"$READELF" -d "$SO" | grep -E 'NEEDED|SONAME'
"$NM" -D "$SO" | grep MotionInterfaceImplGetInstance
VTABLE_LINE="$("$READELF" -sW "$SO" | grep -E '_ZTVN4OHOS3HDI6Motion4V1_115MotionIfServiceE' | head -n 1)"
echo "$VTABLE_LINE"

# Verify strict NEEDED set == [libc++.so, libc.so, libutils.z.so]
NEEDED_SORTED="$("$READELF" -d "$SO" | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ',' | sed 's/,$//')"
if [ "$NEEDED_SORTED" != "libc++.so,libc.so,libutils.z.so" ]; then
  echo "ERROR: Unexpected NEEDED set: $NEEDED_SORTED" >&2
  exit 1
fi

# Verify exact SONAME
SONAME_VAL="$("$READELF" -d "$SO" | sed -n 's/.*(SONAME).*Library soname: \[\(.*\)\]/\1/p')"
if [ "$SONAME_VAL" != "libmotion_interface_service_1.1.z.so" ]; then
  echo "ERROR: Unexpected SONAME: $SONAME_VAL" >&2
  exit 1
fi

# Verify exact 240-byte (30-slot) vtable matching donor libmotion_interface_service_1.1.z.so
VTABLE_SIZE="$(echo "$VTABLE_LINE" | awk '{print $3}')"
if [ "$VTABLE_SIZE" != "240" ]; then
  echo "ERROR: MotionIfService vtable size is $VTABLE_SIZE (expected 240)" >&2
  exit 1
fi

echo "=== MOTION PROBE ELF AUDIT ==="
"$READELF" -d "$PROBE" | grep NEEDED

echo "=== ARTIFACT SIZES & SHA256 ==="
ls -la "$SO" "$PROBE" "$TEST_BIN"
sha256sum "$SO" "$PROBE" "$TEST_BIN"
