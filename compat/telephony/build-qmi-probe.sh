#!/bin/sh
# SPDX-License-Identifier: MIT
# Build compat/telephony/qmi_probe.c into a static aarch64 binary for marble.
#
# The probe deliberately has zero dependencies (no libqmi, no libqrtr, no vendor
# .so): it only needs an AF_QIPCRTR socket and the QMI TLV codec it carries.
# A static Bionic binary is used so it runs from the init ramdisk (/graphics) and
# from /compat/bin without any LD_LIBRARY_PATH or musl-namespace tricks.
#
# Usage:
#   compat/telephony/build-qmi-probe.sh [OUT_PATH]
# Default OUT_PATH: work/marble-telephony/bin/qmi_probe
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
SRC="$ROOT/compat/telephony/qmi_probe.c"
OUT=${1:-$ROOT/work/marble-telephony/bin/qmi_probe}
NDK=${ANDROID_NDK_HOME:-${NDK:-/home/lazybones/Android/Sdk/ndk/android-ndk-r27d}}
API=${ANDROID_API:-26}
TRIPLE=aarch64-linux-android

mkdir -p "$(dirname -- "$OUT")"

if [ ! -x "$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/$TRIPLE$API-clang" ]; then
  echo "QMI_PROBE_BUILD=no_ndk ndk=$NDK" >&2
  exit 2
fi

"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/$TRIPLE$API-clang" \
  -static -s -O2 -Wall -Wextra -Werror \
  -o "$OUT" "$SRC"

echo "QMI_PROBE_BUILD=ok out=$OUT"
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -h "$OUT" 2>/dev/null \
  | grep -E 'Class|Machine|Type' || true
ls -l "$OUT" | awk '{print "QMI_PROBE_SIZE=" $5}'