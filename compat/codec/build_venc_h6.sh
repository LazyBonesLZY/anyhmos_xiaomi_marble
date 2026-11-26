#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build Batch H6 SM7475 Hardware Video Encoder EOS Fix + native_venc_probe artifacts:
#   - Host self-test: omx_venus_host_test (incl. Test #4 DYNAMIC_HANDLE exact EOS)
#   - work/marble-h6-build/libOMX_Core.z.so (aarch64-linux-ohos, SONAME libOMX_Core.z.so)
#   - work/marble-h6-build/native_venc_probe (aarch64-linux-ohos, public AVCodecKit NDK)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${HMOS_SDK:-$ROOT/work/ohos-sdk/installed/native}"
OUT="${H6_BUILD_OUT:-$ROOT/work/marble-h6-build}"
CLANG="$SDK/llvm/bin/clang"
READELF="$SDK/llvm/bin/llvm-readelf"
SYSROOT="$SDK/sysroot"
mkdir -p "$OUT"

# 1. Host unit test suite (including DYNAMIC_HANDLE encoder EOS parity test)
cc -std=c11 -DOMX_VENUS_HOST_TEST -O2 -Wall -Wextra -Werror -pthread \
  "$ROOT/compat/codec/omx_core_venus.c" "$ROOT/compat/codec/venus_v4l2.c" \
  -o "$OUT/omx_venus_host_test"
"$OUT/omx_venus_host_test"

# 2. Cross-compile libOMX_Core.z.so (pure C11, NEEDED=[libc.so])
"$CLANG" --target=aarch64-linux-ohos --sysroot="$SYSROOT" \
  -std=c11 -O2 -fPIC -Wall -Wextra -Werror -shared \
  -Wl,-soname,libOMX_Core.z.so \
  "$ROOT/compat/codec/omx_core_venus.c" "$ROOT/compat/codec/venus_v4l2.c" \
  -o "$OUT/libOMX_Core.z.so"

# 3. Cross-compile native_venc_probe (OH_VideoEncoder + OH_VideoDecoder loopback)
"$CLANG" --target=aarch64-linux-ohos --sysroot="$SYSROOT" \
  -O2 -Wall -Wextra -Werror \
  -I"$SYSROOT/usr/include" \
  "$ROOT/compat/codec/native_venc_probe.c" \
  -o "$OUT/native_venc_probe" \
  -L"$SYSROOT/usr/lib/aarch64-linux-ohos" \
  -lnative_media_codecbase -lnative_media_venc -lnative_media_vdec -lnative_media_core

echo "=== libOMX_Core.z.so SONAME & NEEDED ==="
"$READELF" -d "$OUT/libOMX_Core.z.so" | grep -E 'SONAME|NEEDED'
echo "=== native_venc_probe NEEDED ==="
"$READELF" -d "$OUT/native_venc_probe" | grep NEEDED
echo "H6_VENC_BUILD_OK out=$OUT"
