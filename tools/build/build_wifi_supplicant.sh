#!/bin/bash
# SPDX-License-Identifier: MIT
# Cross-build wpa_supplicant 2.9 (OpenHarmony fork) for marble / aarch64-linux-ohos.
#
# Contract this build must satisfy (see work/marble-wifi-supplicant-report.md):
#   * SONAME                          libwpa.z.so
#   * versioned exported symbol       wpa_main@@1.0   (verdef name "1.0")
#   * control interface               CONFIG_CTRL_IFACE=unix incl. "@abstract:" names
#   * driver                          CONFIG_DRIVER_NL80211 (cfg80211, QCA6490 qca_cld3)
#   * runtime argv used by wpa_host   -c<conf> -g@abstract:/data/service/el1/public/wifi/sockets/wpa/wlan0
#
# The script is idempotent: an existing, verified artifact set is kept unless
# --force is passed.  Every stage prints a WIFI_STAGE= line so a serial log can
# be triaged without guessing.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${WIFI_WPA_SRC:-$ROOT/work/marble-wifi-src/wpa_supplicant_4.1/wpa_supplicant-2.9}
SDK=${HMOS_SDK:-${SHENG_SDK:-$ROOT/work/ohos-sdk/installed/native}}
WORK=${WIFI_BUILD_DIR:-$ROOT/work/marble-wifi-build}
OUT=${WIFI_OUT_DIR:-$ROOT/compat/wifi/runtime}
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "WIFI_STAGE=args FAIL unknown argument: $arg" >&2; exit 2 ;;
    esac
done

say() { echo "WIFI_STAGE=$1${2:+ $2}"; }
die() { echo "WIFI_STAGE=$1 FAIL ${2:-}" >&2; exit "${3:-1}"; }

TARGET_TRIPLE=aarch64-linux-ohos
CLANG=$SDK/llvm/bin/clang
STRIP=$SDK/llvm/bin/llvm-strip
SYSROOT=$SDK/sysroot

say precheck "src=$SRC sdk=$SDK"
[ -d "$SRC/wpa_supplicant" ] || die precheck "missing source tree $SRC" 3
[ -x "$CLANG" ] || die precheck "missing clang $CLANG" 3
[ -d "$SYSROOT/usr/lib/$TARGET_TRIPLE" ] || die precheck "bad sysroot $SYSROOT" 3

write_provenance() {
    local wpa_branch wpa_commit wifi_branch wifi_commit
    wpa_branch=$(git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
    wpa_commit=$(git -C "$SRC" log -1 --format=%H 2>/dev/null || echo unknown)
    local wifi_src=$ROOT/work/marble-wifi-src/communication_wifi_4.1
    wifi_branch=$(git -C "$wifi_src" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
    wifi_commit=$(git -C "$wifi_src" log -1 --format=%H 2>/dev/null || echo unknown)
    python3 - "$OUT" "$SRC" "$wpa_branch" "$wpa_commit" "$wifi_src" "$wifi_branch" "$wifi_commit" \
        "$WORK/libwpa.map" "$WORK/src/wpa_supplicant/.config" "$WORK/libnl.tar.gz" <<'PYEOF'
import hashlib, json, sys
from pathlib import Path
out = Path(sys.argv[1])
wpa_src, wpa_branch, wpa_commit = sys.argv[2], sys.argv[3], sys.argv[4]
wifi_src, wifi_branch, wifi_commit = sys.argv[5], sys.argv[6], sys.argv[7]
extras = sys.argv[8:]
def sha(p):
    p = Path(p)
    if not p.is_file():
        return None
    h = hashlib.sha256()
    with p.open('rb') as fh:
        for c in iter(lambda: fh.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()
data = {
    'generated_by': 'tools/build/build_wifi_supplicant.sh',
    'toolchain': 'OHOS SDK clang (aarch64-linux-ohos, musl sysroot)',
    'wpa_supplicant': {
        'repo': 'https://gitee.com/openharmony/third_party_wpa_supplicant.git',
        'branch': wpa_branch, 'commit': wpa_commit, 'path': wpa_src,
    },
    'communication_wifi': {
        'repo': 'https://gitee.com/openharmony/communication_wifi.git',
        'branch': wifi_branch, 'commit': wifi_commit, 'path': wifi_src,
    },
    'libnl': {
        'headers': 'OpenHarmony third_party/libnl 3.7.0 (include/ only)',
        'runtime_needed': '/system/lib64/chipset-sdk/libnl_share.z.so',
    },
    'sources': {Path(e).name: sha(e) for e in extras if sha(e)},
    'artifacts': {
        rel: sha(out / rel) for rel in ('lib64/libwpa.z.so', 'bin/wpa_supplicant')
        if (out / rel).is_file()
    },
    'contract': {
        'soname': 'libwpa.z.so',
        'symbol': 'wpa_main@@1.0',
        'ctrl_iface': 'unix + @abstract:',
        'driver': 'nl80211',
        'compiled_out': ['CONFIG_SAE', 'CONFIG_OWE', 'CONFIG_FILS', 'CONFIG_AP', 'CONFIG_IEEE8021X_EAPOL'],
    },
}
(out / 'PROVENANCE.json').write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
print('WIFI_PROVENANCE=' + str(out / 'PROVENANCE.json'))
PYEOF
}

LIB_OUT=$OUT/lib64/libwpa.z.so
BIN_OUT=$OUT/bin/wpa_supplicant

if [ "$FORCE" -eq 0 ] && [ -f "$LIB_OUT" ] && [ -f "$BIN_OUT" ]; then
    say skip "artifacts exist (use --force to rebuild): $LIB_OUT"
    write_provenance
    sha256sum "$LIB_OUT" "$BIN_OUT"
    exit 0
fi

# ---------------------------------------------------------------- build tree
say stage "prepare build tree"
BUILD_SRC=$WORK/src
mkdir -p "$WORK" "$OUT/lib64" "$OUT/bin" "$OUT/etc"
rm -rf "$BUILD_SRC"
cp -a "$SRC" "$BUILD_SRC"

# ---------------------------------------------------------- source correction
# The OpenHarmony-4.1-Release snapshot references ../src/ap/rrm.o from
# wpa_supplicant/Makefile (CONFIG_AP) but ships the file as src/ap/ap_rrm.c.
# Provide the expected translation unit without touching the clone.
if [ ! -e "$BUILD_SRC/src/ap/rrm.c" ]; then
    say stage "apply source correction src/ap/rrm.c -> ap_rrm.c"
    printf '/* marble build: Makefile links ../src/ap/rrm.o, source is ap_rrm.c */\n#include "ap_rrm.c"\n' > "$BUILD_SRC/src/ap/rrm.c"
fi

# .config drives the upstream makefile.  Values mirror the donor image
# requirements (nl80211 driver + unix/@abstract control interface) but are not
# the OHOS default (which selects CONFIG_DRIVER_HDF and CONFIG_CTRL_IFACE=udp;
# both are wrong for a cfg80211/QCA6490 target reached through nl80211).
cat > "$BUILD_SRC/wpa_supplicant/.config" <<'EOF'
# marble QCA6490 / cfg80211 (cnss2 + qca_cld3) - generated by build_wifi_supplicant.sh
CONFIG_BACKEND=file
CONFIG_OS=unix
CONFIG_CTRL_IFACE=unix
CONFIG_DRIVER_NL80211=y
CONFIG_L2_PACKET=linux
CONFIG_ELOOP=eloop
CONFIG_TLS=internal
CONFIG_CRYPTO_INTERNAL=y
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_IEEE80211N=y
CONFIG_IEEE80211AC=y
CONFIG_IEEE80211AX=y
CONFIG_IEEE80211W=y
CONFIG_WNM=y
CONFIG_SME=y
# SAE/OWE/FILS need a bignum backend (crypto_openssl only in this snapshot).
# The internal crypto backend has no crypto_bignum_* implementation, so WPA3
# stays off until libcrypto_openssl.z.so is linked (see report: follow-up).
CONFIG_P2P=y
CONFIG_PKCS12=y
CONFIG_SHA256=y
CONFIG_GETRANDOM=y
CONFIG_NO_RANDOM_POOL=y
CONFIG_NO_CONFIG_BLOBS=y
CONFIG_DEBUG_FILE=y
CONFIG_BGSCAN_SIMPLE=y
# libnl is provided by the image itself: /system/lib64/chipset-sdk/libnl_share.z.so
# (OHOS third_party/libnl 3.7.0, same NEEDED name the donor libwpa.z.so uses).
CONFIG_LIBNL32=y
EOF

# --------------------------------------------------------------- libnl deps
# The device ships one combined libnl (libnl_share.z.so) instead of libnl-3 +
# libnl-genl-3, and no headers.  Headers come from OHOS third_party/libnl 3.7.0;
# the link stub is the donor lib itself so DT_NEEDED stays "libnl_share.z.so"
# and the runtime namespace of wpa_host resolves it exactly like the donor lib.
say stage "prepare libnl headers + link stub"
LIB_DONOR=${WIFI_LIBNL_DONOR:-$ROOT/work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/lib64/chipset-sdk/libnl_share.z.so}
LIBNL_INC=${WIFI_LIBNL_INCLUDE:-$WORK/deps/include}
[ -f "$LIBNL_INC/netlink/genl/genl.h" ] || die libnl "missing libnl headers under $LIBNL_INC" 7
[ -f "$LIB_DONOR" ] || die libnl "missing $LIB_DONOR" 7
mkdir -p "$WORK/deps/lib"
cp -f "$LIB_DONOR" "$WORK/deps/lib/libnl_share.z.so"
ln -sf libnl_share.z.so "$WORK/deps/lib/libnl-3.so"
ln -sf libnl_share.z.so "$WORK/deps/lib/libnl-genl-3.so"

# ------------------------------------------------------------------- compile
say stage "compile libwpa.z.so (kbuild-free make, clang $TARGET_TRIPLE)"
VERSION_MAP=$WORK/libwpa.map
cat > "$VERSION_MAP" <<'EOF'
1.0 {
    global:
        wpa_main;
    local:
        *;
};
EOF

# Object list is taken from make itself (the OHOS makefile expands CONFIG_*
# into ~165 objects); the shared library is then linked here instead of via
# build/Makefile so the SONAME and the "1.0" version script are guaranteed.
DUMP_MK=$WORK/dump_objs.mk
cat > "$DUMP_MK" <<'EOF'
.PHONY: __dump_objs
__dump_objs:
	@echo $(OBJS)
EOF

MAKE_VARS=(LIB_TYPE=1 KERNEL_TYPE=linux COMPILER_TYPE=clang COMPILER_DIR="$SDK/llvm"
           SYSROOT_PATH="$SYSROOT" ARCH_CFLAGS="--target=$TARGET_TRIPLE"
           EXTRA_CFLAGS="-D__MUSL__ -fno-lto -I$LIBNL_INC"
           DEPDIR="$WORK/deps/lib" LIBS="-lpthread -lm")

make -C "$BUILD_SRC/wpa_supplicant" clean >/dev/null 2>&1 || true
OBJS=$(make -s -C "$BUILD_SRC/wpa_supplicant" -f Makefile -f "$DUMP_MK" "${MAKE_VARS[@]}" __dump_objs) \
    || die compile "cannot expand OBJS" 4
OBJCOUNT=$(echo "$OBJS" | wc -w)
[ "$OBJCOUNT" -gt 100 ] || die compile "unexpected object count $OBJCOUNT" 4
say stage "compile $OBJCOUNT objects"
make -C "$BUILD_SRC/wpa_supplicant" -j"$(nproc)" "${MAKE_VARS[@]}" $OBJS \
    > "$WORK/build.log" 2>&1 || {
        tail -40 "$WORK/build.log" >&2
        die compile "make failed, see $WORK/build.log" 4
    }

# Station-only build: the two hostapd event hooks that events.c references are
# provided by compat/wifi/wpa_hapd_stubs.c (see that file for why linking
# src/ap/** compiled without -DHOSTAPD would only add empty objects).
say stage "compile compat stubs"
STUB_OBJ=$WORK/wpa_hapd_stubs.o
"$CLANG" --target="$TARGET_TRIPLE" --sysroot="$SYSROOT" -O2 -fPIC -fno-lto \
    -I"$BUILD_SRC/src" -I"$BUILD_SRC/src/utils" -I"$BUILD_SRC/src/common" \
    -I"$BUILD_SRC/wpa_supplicant" \
    -c "$ROOT/compat/wifi/wpa_hapd_stubs.c" -o "$STUB_OBJ" \
    >>"$WORK/build.log" 2>&1 || {
        tail -20 "$WORK/build.log" >&2
        die compile "compat stubs failed" 4
    }

say stage "link libwpa.z.so"
# Both object sets overlap on ../src/ap/*.o, so de-duplicate by real path and
# keep the first spelling that still resolves from the build root.
ALLOBJS=$( { for o in $OBJS; do echo "wpa_supplicant/$o"; done; echo "$STUB_OBJ"; } \
    | ( cd "$BUILD_SRC" && while read -r o; do case "$o" in /*) echo "$o";; *) printf '%s\n' "$(realpath -- "$o")";; esac; done ) \
    | awk '!seen[$0]++' )
( cd "$BUILD_SRC" && \
  "$CLANG" --target="$TARGET_TRIPLE" --sysroot="$SYSROOT" \
    -shared -fPIC -Wl,-z,defs \
    -Wl,--version-script="$VERSION_MAP" -Wl,-soname,libwpa.z.so \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
    -o wpa_supplicant/libwpa.z.so $ALLOBJS \
    -L"$WORK/deps/lib" -lnl-3 -lnl-genl-3 -lpthread -lm ) >>"$WORK/build.log" 2>&1 || {
        tail -40 "$WORK/build.log" >&2
        die link "libwpa.z.so link failed, see $WORK/build.log" 4
    }

LIB=$BUILD_SRC/wpa_supplicant/libwpa.z.so
[ -f "$LIB" ] || die compile "libwpa.z.so not produced" 4
cp "$LIB" "$LIB_OUT"
$STRIP --strip-unneeded "$LIB_OUT" 2>/dev/null || true

# ------------------------------------------------------------- launcher bin
# The donor ships the same 5.5 KB shape: a pthread wrapper around wpa_main
# (source: wpa_test/wpa_sample.c).  Keep the ABI identical so the binary can be
# used from a serial shell with the same argv the HDI service uses.
say stage "link bin/wpa_supplicant"
cat > "$WORK/wpa_launcher.c" <<'EOF'
/* marble launcher: run wpa_supplicant in a thread, like the donor binary */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "libwpa.h"

extern int wpa_main(int argc, char *argv[]);

static int g_argc;
static char **g_argv;

static void *thread_main(void *arg)
{
    (void)arg;
    return (void *)(long)wpa_main(g_argc, g_argv);
}

int main(int argc, char *argv[])
{
    pthread_t tid;
    int ret;

    g_argc = argc;
    g_argv = argv;
    ret = pthread_create(&tid, NULL, thread_main, NULL);
    if (ret != 0) {
        fprintf(stderr, "wpa_supplicant: pthread_create: %s\n", strerror(ret));
        return 1;
    }
    pthread_join(tid, NULL);
    return 0;
}
EOF

"$CLANG" --target="$TARGET_TRIPLE" --sysroot="$SYSROOT" \
    -O2 -fPIE -pie -I"$BUILD_SRC/build/include" \
    "$WORK/wpa_launcher.c" \
    -L"$OUT/lib64" -l:libwpa.z.so -Wl,-rpath,/vendor/lib64:/system/lib64 \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
    -o "$BIN_OUT" >>"$WORK/build.log" 2>&1 || {
        tail -20 "$WORK/build.log" >&2
        die link "launcher link failed" 5
    }
$STRIP --strip-unneeded "$BIN_OUT" 2>/dev/null || true

# ------------------------------------------------------------------- verify
say stage "verify artifacts"
file "$LIB_OUT" "$BIN_OUT" | sed 's/^/WIFI_ARTIFACT= /'
LC_ALL=C readelf -d "$LIB_OUT" | grep -q 'libwpa.z.so' \
    || die verify "SONAME libwpa.z.so missing" 6
LC_ALL=C readelf -V "$LIB_OUT" | grep -q '1\.0' \
    || die verify "version definition 1.0 missing" 6
LC_ALL=C readelf --dyn-syms -W "$LIB_OUT" | grep -qE 'wpa_main@@1\.0|wpa_main$' \
    || die verify "wpa_main not exported" 6
LC_ALL=C readelf -d "$LIB_OUT" | grep -q 'libc.so' \
    || die verify "libc dependency missing" 6

write_provenance
sha256sum "$LIB_OUT" "$BIN_OUT" | sed 's/^/WIFI_SHA256= /'
say complete "lib64/libwpa.z.so + bin/wpa_supplicant"