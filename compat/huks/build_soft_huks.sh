#!/usr/bin/env bash
# Build the marble software HUKS backend.
#
#   compat/huks/build_soft_huks.sh --all        # host + aarch64 + probe
#   compat/huks/build_soft_huks.sh --host       # x86_64 self-test build only
#   compat/huks/build_soft_huks.sh --cross      # aarch64 device artifacts
#   compat/huks/build_soft_huks.sh --probe-only # only tee_backend_probe
#
# Outputs (default work/marble-huks-build/):
#   arm64/libhuks_engine_core_standard.z.so   engine seam (HuksCreateHdiDevicePtr)
#   arm64/libhuks_hdi_service_1.1.z.so        HDI impl seam (HuksImplGetInstance)
#   arm64/tee_backend_probe                   Bionic one-shot self-check
#   host/libsoft_huks_engine.so               same sources, system OpenSSL
#   host/libsoft_huks_hdi.so
#   host/soft_huks_selftest                   host behavioural test binary
#   host/tee_backend_probe
#
# The two aarch64 libraries contain the same code; the engine build exposes the
# engine vtable, the HDI build additionally exposes struct IHuks.  Deploying one
# file at /vendor/lib64/passthrough/libhuks_hdi_service_1.1.z.so is enough.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT_DEFAULT="$REPO/work/marble-huks-build"
OUT="$OUT_DEFAULT"

SDK_DEFAULT="$REPO/work/ohos-sdk/installed/native"
SDK="${HMOS_SDK:-$SDK_DEFAULT}"
OPENSSL_INC="${HUKS_SOFT_OPENSSL_INC:-$REPO/work/openssl-inc}"
CRYPTO_LIB_DIR="${HUKS_SOFT_CRYPTO_DIR:-$REPO/work/donor-hdi-link}"

DO_HOST=0
DO_CROSS=0
DO_PROBE=0
CLEAN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --host) DO_HOST=1 ;;
        --cross) DO_CROSS=1 ;;
        --all) DO_HOST=1; DO_CROSS=1 ;;
        --probe-only) DO_PROBE=1 ;;
        --clean) CLEAN=1 ;;
        --out) shift; OUT="$1" ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$DO_HOST" -eq 0 ] && [ "$DO_CROSS" -eq 0 ] && [ "$DO_PROBE" -eq 0 ]; then
    DO_HOST=1
    DO_CROSS=1
fi

[ "$CLEAN" -eq 1 ] && rm -rf "$OUT"
mkdir -p "$OUT/host" "$OUT/arm64" "$OUT/obj"

WARN="-Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations"
HOST_WARN="$WARN -Wno-format-truncation"
DEFS="-D_GNU_SOURCE -DSOFT_HUKS_HOST_BUILD=0"
SRCS_COMMON="$HERE/soft_huks_backend.c $HERE/soft_huks_store.c $HERE/soft_huks_engine.c"
SRCS_HDI="$SRCS_COMMON $HERE/soft_huks_hdi.c"

fail() { echo "BUILD FAILED: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# host build (x86_64, system OpenSSL) - the build the unit tests exercise
# ---------------------------------------------------------------------------
if [ "$DO_HOST" -eq 1 ]; then
    CC=${HOST_CC:-cc}
    INC="-I$HERE"
    echo "== host build with $CC =="
    $CC -O2 -g -fPIC $HOST_WARN $DEFS $INC -shared -Wl,-soname,libsoft_huks_engine.so \
        $SRCS_COMMON -lcrypto -lpthread -ldl -o "$OUT/host/libsoft_huks_engine.so" \
        || fail "host engine"
    $CC -O2 -g -fPIC $HOST_WARN $DEFS $INC -shared -Wl,-soname,libsoft_huks_hdi.so \
        $SRCS_HDI -lcrypto -lpthread -ldl -o "$OUT/host/libsoft_huks_hdi.so" \
        || fail "host hdi"
    if [ -f "$HERE/tests/soft_huks_selftest.c" ]; then
        $CC -O1 -g $HOST_WARN $INC "$HERE/tests/soft_huks_selftest.c" -ldl -lpthread \
            -o "$OUT/host/soft_huks_selftest" || fail "host selftest"
    fi
    $CC -O2 $HOST_WARN "$HERE/tee_backend_probe.c" -ldl -o "$OUT/host/tee_backend_probe" \
        || fail "host probe"
    echo "   -> $OUT/host"
fi

# ---------------------------------------------------------------------------
# probe only (no OpenSSL dependency, Bionic friendly)
# ---------------------------------------------------------------------------
if [ "$DO_PROBE" -eq 1 ] && [ "$DO_CROSS" -eq 0 ]; then
    CROSS_CC="$SDK/llvm/bin/aarch64-unknown-linux-ohos-clang"
    [ -x "$CROSS_CC" ] || fail "OHOS clang not found: $CROSS_CC"
    "$CROSS_CC" --sysroot="$SDK/sysroot" -O2 $WARN "$HERE/tee_backend_probe.c" -ldl \
        -o "$OUT/arm64/tee_backend_probe" || fail "cross probe"
    echo "   -> $OUT/arm64/tee_backend_probe"
fi

# ---------------------------------------------------------------------------
# aarch64 build with the OHOS SDK clang, linked against the device libcrypto
# ---------------------------------------------------------------------------
if [ "$DO_CROSS" -eq 1 ]; then
    CROSS_CC="$SDK/llvm/bin/aarch64-unknown-linux-ohos-clang"
    SYSROOT="$SDK/sysroot"
    [ -x "$CROSS_CC" ] || fail "OHOS clang not found: $CROSS_CC (set HMOS_SDK)"
    [ -d "$OPENSSL_INC/openssl" ] || fail "OpenSSL headers not found: $OPENSSL_INC"
    if [ ! -f "$CRYPTO_LIB_DIR/libcrypto_openssl.z.so" ]; then
        fail "libcrypto_openssl.z.so not found in $CRYPTO_LIB_DIR"
    fi

    CROSS_FLAGS="--sysroot=$SYSROOT -target aarch64-linux-ohos -O2 -fPIC $WARN $DEFS -I$HERE -I$OPENSSL_INC"
    echo "== aarch64 build with $CROSS_CC =="

    OBJ="$OUT/obj/arm64"
    mkdir -p "$OBJ"
    OBJS_COMMON=""
    for src in $SRCS_COMMON; do
        obj="$OBJ/$(basename "${src%.c}").o"
        $CROSS_CC $CROSS_FLAGS -c "$src" -o "$obj" || fail "cross compile $(basename "$src")"
        OBJS_COMMON="$OBJS_COMMON $obj"
    done
    $CROSS_CC $CROSS_FLAGS -c "$HERE/soft_huks_hdi.c" -o "$OBJ/soft_huks_hdi.o" || fail "cross compile hdi"

    LINK_FLAGS="-shared -L$CRYPTO_LIB_DIR -l:libcrypto_openssl.z.so -lpthread -ldl"
    $CROSS_CC $CROSS_FLAGS $LINK_FLAGS -Wl,-soname,libhuks_engine_core_standard.z.so \
        $OBJS_COMMON -o "$OUT/arm64/libhuks_engine_core_standard.z.so" || fail "cross engine link"
    $CROSS_CC $CROSS_FLAGS $LINK_FLAGS -Wl,-soname,libhuks_hdi_service_1.1.z.so \
        $OBJS_COMMON "$OBJ/soft_huks_hdi.o" -o "$OUT/arm64/libhuks_hdi_service_1.1.z.so" \
        || fail "cross hdi link"

    $CROSS_CC --sysroot="$SYSROOT" -target aarch64-linux-ohos -O2 $WARN "$HERE/tee_backend_probe.c" \
        -ldl -o "$OUT/arm64/tee_backend_probe" || fail "cross probe"

    echo "   -> $OUT/arm64"
    NM="$SDK/llvm/bin/llvm-nm"
    if [ -x "$NM" ]; then
        echo "   exported symbols (engine):"
        "$NM" -D --defined-only "$OUT/arm64/libhuks_engine_core_standard.z.so" | \
            grep -E "Huks(Create|Destory)HdiDevicePtr" | sed 's/^/     /'
        echo "   exported symbols (hdi):"
        "$NM" -D --defined-only "$OUT/arm64/libhuks_hdi_service_1.1.z.so" | \
            grep -E "HuksImpl(GetInstance|Release)|Huks(Create|Destory)HdiDevicePtr" | sed 's/^/     /'
    fi
fi

echo "done: $OUT"
