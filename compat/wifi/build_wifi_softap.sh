#!/bin/bash
# SPDX-License-Identifier: MIT
# Cross-build dual STA + SoftAP libwpa.z.so (wpa_supplicant + hostapd 2.9,
# OpenHarmony fork) for Xiaomi marble (QCA6490 / SM7475, aarch64-linux-ohos).
#
# Contract satisfied:
#   * SONAME                   libwpa.z.so
#   * Versioned exports (1.0)  wpa_main@@1.0, ap_main@@1.0, getHostapd@@1.0
#   * Control interface        CONFIG_CTRL_IFACE=unix (filesystem + @abstract:)
#   * Driver                   CONFIG_DRIVER_NL80211 (cfg80211 / qca_cld3_qca6490)
#   * AP + STA unified         CONFIG_AP=y + -DHOSTAPD + src/ap/* (no wpa_hapd_stubs)
#   * Minimal DT_NEEDED        libnl_share.z.so, libc.so
#
# Output directory defaults to work/marble-m4-build/wifi (never overwrites
# compat/wifi/runtime/lib64/libwpa.z.so).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SRC=${WIFI_WPA_SRC:-$ROOT/work/marble-wifi-src/wpa_supplicant_4.1/wpa_supplicant-2.9}
SDK=${HMOS_SDK:-${SHENG_SDK:-$ROOT/work/ohos-sdk/installed/native}}
OUT=${WIFI_SOFTAP_OUT:-$ROOT/work/marble-m4-build/wifi}
WORK=${WIFI_SOFTAP_WORK:-$OUT/build}
FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
        *) echo "SOFTAP_STAGE=args FAIL unknown argument: $arg" >&2; exit 2 ;;
    esac
done

say() { echo "SOFTAP_STAGE=$1${2:+ $2}"; }
die() { echo "SOFTAP_STAGE=$1 FAIL ${2:-}" >&2; exit "${3:-1}"; }

TARGET_TRIPLE=aarch64-linux-ohos
CLANG=$SDK/llvm/bin/clang
STRIP=$SDK/llvm/bin/llvm-strip
READELF=$SDK/llvm/bin/llvm-readelf
NM=$SDK/llvm/bin/llvm-nm
SYSROOT=$SDK/sysroot

say precheck "src=$SRC sdk=$SDK out=$OUT"
[ -d "$SRC/wpa_supplicant" ] || die precheck "missing source tree $SRC" 3
[ -d "$SRC/hostapd" ] || die precheck "missing hostapd tree $SRC/hostapd" 3
[ -x "$CLANG" ] || die precheck "missing clang $CLANG" 3
[ -d "$SYSROOT/usr/lib/$TARGET_TRIPLE" ] || die precheck "bad sysroot $SYSROOT" 3

LIB_OUT=$OUT/libwpa.z.so
WPA_BIN_OUT=$OUT/wpa_supplicant
HAPD_BIN_OUT=$OUT/hostapd

if [ "$FORCE" -eq 0 ] && [ -f "$LIB_OUT" ] && [ -f "$WPA_BIN_OUT" ] && [ -f "$HAPD_BIN_OUT" ]; then
    say skip "artifacts exist (use --force to rebuild): $LIB_OUT"
    sha256sum "$LIB_OUT" "$WPA_BIN_OUT" "$HAPD_BIN_OUT"
    exit 0
fi

# ---------------------------------------------------------------- build tree
say stage "prepare build tree"
BUILD_SRC=$WORK/src
mkdir -p "$WORK" "$OUT"
rm -rf "$BUILD_SRC"
cp -a "$SRC" "$BUILD_SRC"

# Source correction 1: OHOS 4.1 wpa_supplicant/Makefile references
# ../src/ap/rrm.o while hostapd/Makefile references ../src/ap/ap_rrm.o
# (the actual shipped file is src/ap/ap_rrm.c). Normalize the build copy of
# wpa_supplicant/Makefile so both link the single src/ap/ap_rrm.o unit.
if grep -q '\.\./src/ap/rrm\.o' "$BUILD_SRC/wpa_supplicant/Makefile"; then
    say stage "normalize wpa_supplicant/Makefile ../src/ap/rrm.o -> ../src/ap/ap_rrm.o"
    sed -i 's|\.\./src/ap/rrm\.o|../src/ap/ap_rrm.o|g' "$BUILD_SRC/wpa_supplicant/Makefile"
fi

# Source correction 2: Wire clean-room marble HDI shim hooks into build copy
# of hostapd/ap_main.c (getHostapd lifecycle) and hostapd/ap_ctrl_iface.c
# (HostapdEventReport forwarding). Upstream clone under work/marble-wifi-src/
# remains untouched.
say stage "wire clean-room hostapd HDI shim hooks in build copy"
python3 - "$BUILD_SRC/hostapd/ap_main.c" "$BUILD_SRC/hostapd/ap_ctrl_iface.c" <<'PYEOF'
import sys
from pathlib import Path

ap_main = Path(sys.argv[1])
src = ap_main.read_text(encoding='utf-8')

inc_anchor = '#include "ap/ap_config.h"\n'
if inc_anchor not in src:
    raise SystemExit('ap_main.c include anchor not found')
src = src.replace(
    inc_anchor,
    inc_anchor + '#include "wpa_softap_hdi_shim.h"\n',
    1,
)

drv_anchor = (
    '\tif (hapd->driver == NULL || hapd->driver->hapd_init == NULL) {\n'
    '\t\twpa_printf(MSG_ERROR, "No hostapd driver wrapper available");\n'
    '\t\treturn -1;\n'
    '\t}\n'
)
if drv_anchor not in src:
    raise SystemExit('ap_main.c driver_init anchor not found')
src = src.replace(
    drv_anchor,
    drv_anchor
    + '\tif (hapd->conf != NULL &&\n'
    + '\t    (os_strncmp(hapd->conf->iface, "wlan", 4) == 0 ||\n'
    + '\t     os_strncmp(hapd->conf->iface, "ap", 2) == 0)) {\n'
    + '\t\tmarble_hostapd_set_active(hapd);\n'
    + '\t}\n',
    1,
)

out_anchor = ' out:\n\thostapd_global_ctrl_iface_deinit(&interfaces);\n'
if out_anchor not in src:
    raise SystemExit('ap_main.c teardown anchor not found')
src = src.replace(
    out_anchor,
    ' out:\n\tmarble_hostapd_set_active(NULL);\n\thostapd_global_ctrl_iface_deinit(&interfaces);\n',
    1,
)
ap_main.write_text(src, encoding='utf-8')

ap_ctrl = Path(sys.argv[2])
csrc = ap_ctrl.read_text(encoding='utf-8')
cinc_anchor = '#include "ap/ap_config.h"\n'
if cinc_anchor not in csrc:
    raise SystemExit('ap_ctrl_iface.c include anchor not found')
csrc = csrc.replace(
    cinc_anchor,
    cinc_anchor + '#include "wpa_softap_hdi_shim.h"\n',
    1,
)

cb_anchor = (
    'static void hostapd_ctrl_iface_msg_cb(void *ctx, int level,\n'
    '\t\t\t\t      enum wpa_msg_type type,\n'
    '\t\t\t\t      const char *txt, size_t len)\n'
    '{\n'
    '\tstruct hostapd_data *hapd = ctx;\n'
    '\tif (hapd == NULL)\n'
    '\t\treturn;\n'
    '\thostapd_ctrl_iface_send(hapd, level, type, txt, len);\n'
    '}\n'
)
if cb_anchor not in csrc:
    raise SystemExit('ap_ctrl_iface.c msg_cb anchor not found')
csrc = csrc.replace(
    cb_anchor,
    'static void hostapd_ctrl_iface_msg_cb(void *ctx, int level,\n'
    '\t\t\t\t      enum wpa_msg_type type,\n'
    '\t\t\t\t      const char *txt, size_t len)\n'
    '{\n'
    '\tstruct hostapd_data *hapd = ctx;\n'
    '\tif (hapd == NULL)\n'
    '\t\treturn;\n'
    '\thostapd_ctrl_iface_send(hapd, level, type, txt, len);\n'
    '\tif (hapd->conf != NULL)\n'
    '\t\tmarble_hostapd_notify_msg(hapd->conf->iface, txt, len);\n'
    '}\n',
    1,
)
ap_ctrl.write_text(csrc, encoding='utf-8')
PYEOF

# ------------------------------------------------------------------ configs
# Both wpa_supplicant and hostapd use identical driver/crypto/IEEE80211/ctrl
# settings so shared src/ap/*.o and src/drivers/*.o have 100% consistent ABI.
cat > "$BUILD_SRC/wpa_supplicant/.config" <<'EOF'
# marble QCA6490 / cfg80211 dual STA+AP - generated by build_wifi_softap.sh
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
CONFIG_AP=y
CONFIG_P2P=y
CONFIG_ACS=y
CONFIG_PKCS12=y
CONFIG_SHA256=y
CONFIG_GETRANDOM=y
CONFIG_NO_RANDOM_POOL=y
CONFIG_NO_CONFIG_BLOBS=y
CONFIG_NO_VLAN=y
CONFIG_NO_RADIUS=y
CONFIG_NO_ACCOUNTING=y
CONFIG_NO_DUMP_STATE=y
CONFIG_DEBUG_FILE=y
CONFIG_BGSCAN_SIMPLE=y
CONFIG_LIBNL32=y
EOF

cat > "$BUILD_SRC/hostapd/.config" <<'EOF'
# marble QCA6490 / cfg80211 SoftAP hostapd - generated by build_wifi_softap.sh
CONFIG_OS=unix
CONFIG_ELOOP=eloop
CONFIG_L2_PACKET=linux
CONFIG_CTRL_IFACE=unix
CONFIG_DRIVER_NL80211=y
CONFIG_LIBNL32=y
CONFIG_TLS=internal
CONFIG_CRYPTO_INTERNAL=y
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_IEEE80211N=y
CONFIG_IEEE80211AC=y
CONFIG_IEEE80211AX=y
CONFIG_IEEE80211W=y
CONFIG_ACS=y
CONFIG_SHA256=y
CONFIG_GETRANDOM=y
CONFIG_NO_RANDOM_POOL=y
CONFIG_NO_VLAN=y
CONFIG_NO_RADIUS=y
CONFIG_NO_ACCOUNTING=y
CONFIG_NO_DUMP_STATE=y
CONFIG_DEBUG_FILE=y
NEED_AP_MLME=y
EOF

# --------------------------------------------------------------- libnl deps
say stage "prepare libnl headers + link stub"
LIB_DONOR=${WIFI_LIBNL_DONOR:-$ROOT/work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs/system/system/lib64/chipset-sdk/libnl_share.z.so}
LIBNL_INC=${WIFI_LIBNL_INCLUDE:-$ROOT/work/marble-wifi-build/deps/include}
[ -f "$LIBNL_INC/netlink/genl/genl.h" ] || die libnl "missing libnl headers under $LIBNL_INC" 7
[ -f "$LIB_DONOR" ] || die libnl "missing $LIB_DONOR" 7
mkdir -p "$WORK/deps/lib"
cp -f "$LIB_DONOR" "$WORK/deps/lib/libnl_share.z.so"
ln -sf libnl_share.z.so "$WORK/deps/lib/libnl-3.so"
ln -sf libnl_share.z.so "$WORK/deps/lib/libnl-genl-3.so"

# ------------------------------------------------------------------- compile
say stage "compile wpa_supplicant + hostapd objects (clang $TARGET_TRIPLE)"
VERSION_MAP=$WORK/libwpa.map
cat > "$VERSION_MAP" <<'EOF'
1.0 {
    global:
        wpa_main;
        ap_main;
        getHostapd;
    local:
        *;
};
EOF

DUMP_MK=$WORK/dump_objs.mk
cat > "$DUMP_MK" <<'EOF'
.PHONY: __dump_objs
__dump_objs:
	@echo $(OBJS)
EOF

# Uniform flags across both wpa_supplicant and hostapd so src/ap/* (including
# drv_callbacks.c -> wpa_supplicant_event_hapd / wpa_supplicant_event_global_hapd)
# always compile with -DHOSTAPD and identical struct layouts.
MAKE_VARS=(LIB_TYPE=1 KERNEL_TYPE=linux COMPILER_TYPE=clang COMPILER_DIR="$SDK/llvm"
           SYSROOT_PATH="$SYSROOT" ARCH_CFLAGS="--target=$TARGET_TRIPLE"
           EXTRA_CFLAGS="-D__MUSL__ -DHOSTAPD -DNEED_AP_MLME -fno-lto -I$LIBNL_INC -I$ROOT/compat/wifi"
           DEPDIR="$WORK/deps/lib" LIBS="-lpthread -lm")

: > "$WORK/build.log"

make -C "$BUILD_SRC/wpa_supplicant" clean >/dev/null 2>&1 || true
WPA_OBJS=$(make -s -C "$BUILD_SRC/wpa_supplicant" -f Makefile -f "$DUMP_MK" "${MAKE_VARS[@]}" __dump_objs) \
    || die compile "cannot expand wpa_supplicant OBJS" 4
WPA_COUNT=$(echo "$WPA_OBJS" | wc -w)
[ "$WPA_COUNT" -gt 100 ] || die compile "unexpected wpa_supplicant object count $WPA_COUNT" 4
say stage "compile $WPA_COUNT wpa_supplicant objects"
make -C "$BUILD_SRC/wpa_supplicant" -j"$(nproc)" "${MAKE_VARS[@]}" $WPA_OBJS \
    >> "$WORK/build.log" 2>&1 || {
        tail -40 "$WORK/build.log" >&2
        die compile "wpa_supplicant make failed, see $WORK/build.log" 4
    }

# Do NOT clean shared ../src/* objects; expand hostapd OBJS and build any
# hostapd-specific / src/ap/* objects that were not built by wpa_supplicant,
# forcing src/ap/drv_callbacks.o + hostapd/*.o to build under hostapd's flags.
HAPD_OBJS=$(make -s -C "$BUILD_SRC/hostapd" -f Makefile -f "$DUMP_MK" "${MAKE_VARS[@]}" __dump_objs) \
    || die compile "cannot expand hostapd OBJS" 4
HAPD_COUNT=$(echo "$HAPD_OBJS" | wc -w)
[ "$HAPD_COUNT" -gt 40 ] || die compile "unexpected hostapd object count $HAPD_COUNT" 4
rm -f "$BUILD_SRC/src/ap/drv_callbacks.o" "$BUILD_SRC/hostapd/"*.o
say stage "compile $HAPD_COUNT hostapd objects"
make -C "$BUILD_SRC/hostapd" -j"$(nproc)" "${MAKE_VARS[@]}" $HAPD_OBJS \
    >> "$WORK/build.log" 2>&1 || {
        tail -40 "$WORK/build.log" >&2
        die compile "hostapd make failed, see $WORK/build.log" 4
    }

# Compile clean-room HDI shim strictly with -Wall -Wextra -Werror.
say stage "compile clean-room hostapd HDI shim"
SHIM_OBJ=$WORK/wpa_softap_hdi_shim.o
"$CLANG" --target="$TARGET_TRIPLE" --sysroot="$SYSROOT" \
    -O2 -fPIC -fno-lto -fvisibility=hidden -Wall -Wextra -Werror \
    -I"$ROOT/compat/wifi" \
    -c "$ROOT/compat/wifi/wpa_softap_hdi_shim.c" -o "$SHIM_OBJ" \
    >>"$WORK/build.log" 2>&1 || {
        tail -20 "$WORK/build.log" >&2
        die compile "wpa_softap_hdi_shim.c failed" 4
    }

say stage "link libwpa.z.so (dual STA + SoftAP)"
ALLOBJS=$( {
    for o in $WPA_OBJS; do echo "wpa_supplicant/$o"; done
    for o in $HAPD_OBJS; do echo "hostapd/$o"; done
    echo "$SHIM_OBJ"
} | ( cd "$BUILD_SRC" && while read -r o; do
    case "$o" in
        /*) echo "$o" ;;
        *) printf '%s\n' "$(realpath -- "$o")" ;;
    esac
done ) | awk '!seen[$0]++' )

TOTAL_OBJCOUNT=$(echo "$ALLOBJS" | wc -w)
say stage "deduplicated link set: $TOTAL_OBJCOUNT objects"

( cd "$BUILD_SRC" && \
  "$CLANG" --target="$TARGET_TRIPLE" --sysroot="$SYSROOT" \
    -shared -fPIC -Wl,-z,defs \
    -Wl,--version-script="$VERSION_MAP" -Wl,-soname,libwpa.z.so \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
    -o "$WORK/libwpa.z.so" $ALLOBJS \
    -L"$WORK/deps/lib" -lnl-3 -lnl-genl-3 -lpthread -lm ) >>"$WORK/build.log" 2>&1 || {
        tail -40 "$WORK/build.log" >&2
        die link "libwpa.z.so link failed, see $WORK/build.log" 4
    }

cp -f "$WORK/libwpa.z.so" "$LIB_OUT"
"$STRIP" --strip-unneeded "$LIB_OUT" 2>/dev/null || true

# ------------------------------------------------------------- launcher bins
say stage "link standalone wpa_supplicant and hostapd debug launchers"
cat > "$WORK/wpa_launcher.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
#include <string.h>
extern int wpa_main(int argc, char *argv[]);
static int g_argc;
static char **g_argv;
static void *thread_main(void *arg) { (void)arg; return (void *)(long)wpa_main(g_argc, g_argv); }
int main(int argc, char *argv[]) {
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

cat > "$WORK/hostapd_launcher.c" <<'EOF'
#include <pthread.h>
#include <stdio.h>
#include <string.h>
extern int ap_main(int argc, char *argv[]);
static int g_argc;
static char **g_argv;
static void *thread_main(void *arg) { (void)arg; return (void *)(long)ap_main(g_argc, g_argv); }
int main(int argc, char *argv[]) {
    pthread_t tid;
    int ret;
    g_argc = argc;
    g_argv = argv;
    ret = pthread_create(&tid, NULL, thread_main, NULL);
    if (ret != 0) {
        fprintf(stderr, "hostapd: pthread_create: %s\n", strerror(ret));
        return 1;
    }
    pthread_join(tid, NULL);
    return 0;
}
EOF

"$CLANG" --target="$TARGET_TRIPLE" --sysroot="$SYSROOT" \
    -O2 -fPIE -pie -Wall -Wextra -Werror \
    "$WORK/wpa_launcher.c" \
    -L"$OUT" -l:libwpa.z.so -Wl,-rpath,/vendor/lib64:/system/lib64 \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
    -o "$WPA_BIN_OUT" >>"$WORK/build.log" 2>&1 || die link "wpa_supplicant launcher failed" 4

"$CLANG" --target="$TARGET_TRIPLE" --sysroot="$SYSROOT" \
    -O2 -fPIE -pie -Wall -Wextra -Werror \
    "$WORK/hostapd_launcher.c" \
    -L"$OUT" -l:libwpa.z.so -Wl,-rpath,/vendor/lib64:/system/lib64 \
    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack \
    -o "$HAPD_BIN_OUT" >>"$WORK/build.log" 2>&1 || die link "hostapd launcher failed" 4

"$STRIP" --strip-unneeded "$WPA_BIN_OUT" "$HAPD_BIN_OUT" 2>/dev/null || true

# -------------------------------------------------------------------- verify
say stage "verify ELF SONAME, NEEDED, versioned exports, and HDI UND satisfaction"
"$READELF" -d "$LIB_OUT" | grep -q 'Library soname: \[libwpa.z.so\]' || die verify "bad SONAME" 5
"$READELF" -d "$LIB_OUT" | grep -q 'Shared library: \[libnl_share.z.so\]' || die verify "missing libnl_share.z.so" 5
for sym in 'wpa_main@@1.0' 'ap_main@@1.0' 'getHostapd@@1.0'; do
    "$NM" -D --defined-only "$LIB_OUT" | grep -q " $sym\$" || die verify "missing export $sym" 5
done

# Verify donor libhostapd_interface_service_1.0.z.so & libwpa_interface_service_2.0.z.so
DONOR_FS=$ROOT/work/donor-rom/ADY-AL10-207.0.0.111-SP10/fs
if [ -f "$DONOR_FS/vendor/lib64/libhostapd_interface_service_1.0.z.so" ]; then
    python3 - "$NM" "$LIB_OUT" "$DONOR_FS" <<'PYEOF'
import subprocess, sys
from pathlib import Path

nm, libwpa, donor_fs = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])

def dyn_syms(path: Path, flag: str) -> set[str]:
    out = subprocess.check_output([nm, '-D', flag, str(path)], text=True)
    res = set()
    for line in out.splitlines():
        parts = line.split()
        if parts:
            res.add(parts[-1])
    return res

our_defs = {s.replace('@@', '@') for s in dyn_syms(libwpa, '--defined-only')}
donor_wpa_defs = {s.replace('@@', '@') for s in dyn_syms(donor_fs / 'vendor/lib64/libwpa.z.so', '--defined-only')}

for consumer in (
    donor_fs / 'vendor/lib64/libhostapd_interface_service_1.0.z.so',
    donor_fs / 'vendor/lib64/libwpa_interface_service_2.0.z.so',
):
    und = dyn_syms(consumer, '-u')
    needed_from_wpa = sorted(und & donor_wpa_defs)
    missing = [s for s in needed_from_wpa if s not in our_defs]
    print(f'SOFTAP_VERIFY consumer={consumer.name} from_libwpa={needed_from_wpa} missing={missing}')
    if missing:
        raise SystemExit(f'Unsatisfied symbols for {consumer.name}: {missing}')
PYEOF
fi

say done "lib=$LIB_OUT"
sha256sum "$LIB_OUT" "$WPA_BIN_OUT" "$HAPD_BIN_OUT"
