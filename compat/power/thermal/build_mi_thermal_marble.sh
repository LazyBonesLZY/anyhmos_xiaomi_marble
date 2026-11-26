#!/bin/sh
# SPDX-License-Identifier: MIT
# Build Batch M4 (Part B) Xiaomi marble mi_thermald charging-thermal runtime package:
#   - work/marble-m4-build/thermal/native_thermal_launcher_marble  (aarch64-linux-ohos)
#   - work/marble-m4-build/thermal/marble-mi-thermal.tar.gz        (< 1.5 MB minimal chroot overlay)
# Runs ELF audits, symbol resolution verification, and strict offline host unit tests.
set -eu

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
OUT="${M4_THERMAL_BUILD_OUT:-$ROOT/work/marble-m4-build/thermal}"
mkdir -p "$OUT"

SDK="$ROOT/work/ohos-sdk/installed/native"
CLANG="$SDK/llvm/bin/clang"
READELF="$SDK/llvm/bin/llvm-readelf"
NM="$SDK/llvm/bin/llvm-nm"
SYSROOT="$SDK/sysroot"

VENDOR_TAR="$ROOT/work/marble-stock-vendor/marble-stock-vendor.tar.gz"
BIONIC_LIB64="$ROOT/work/marble-bionic/lib64"
AUDIO_RT_LIB64="$ROOT/compat/audio/runtime/lib64"

SRC_DIR="$ROOT/compat/power/thermal"
LAUNCHER_SRC="$SRC_DIR/native_thermal_launcher_marble.c"
TEST_SRC="$SRC_DIR/test_native_thermal_launcher_marble.c"
LAUNCHER_BIN="$OUT/native_thermal_launcher_marble"
THERMAL_TAR="$OUT/marble-mi-thermal.tar.gz"

# 1. Cross-compile clean-room native_thermal_launcher_marble for aarch64-linux-ohos.
"$CLANG" \
  --target=aarch64-linux-ohos \
  --sysroot="$SYSROOT" \
  -O2 -std=c11 \
  -Wall -Wextra -Werror \
  -fPIE -pie \
  "$LAUNCHER_SRC" \
  -o "$LAUNCHER_BIN"
chmod 0755 "$LAUNCHER_BIN"

echo "=== ELF AUDIT: native_thermal_launcher_marble ==="
"$READELF" -h -l -d "$LAUNCHER_BIN" | grep -E 'Class:|Machine:|Type:|interpreter|NEEDED|SONAME'
"$NM" -D --defined-only "$LAUNCHER_BIN" | head -n 20 || true

# 2. Build deterministic compact marble-mi-thermal.tar.gz (< 1.5 MB).
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

CONF_LIST_FILE="$STAGE/thermal_confs.list"
tar -tzf "$VENDOR_TAR" | grep -E '^vendor/etc/thermal.*\.conf$' | sort > "$CONF_LIST_FILE"
CONF_COUNT="$(wc -l < "$CONF_LIST_FILE" | tr -d ' \t\r\n')"

if [ "$CONF_COUNT" -eq 0 ]; then
  echo "ERROR: no vendor/etc/thermal*.conf entries found in $VENDOR_TAR" >&2
  exit 1
fi

printf 'vendor/bin/mi_thermald\n' | cat - "$CONF_LIST_FILE" > "$STAGE/extract.list"
tar -xzf "$VENDOR_TAR" -C "$STAGE" -T "$STAGE/extract.list"
rm -f "$CONF_LIST_FILE" "$STAGE/extract.list"
mkdir -p "$STAGE/vendor/lib64" "$STAGE/bin"

for lib in libcrypto.so libssl.so; do
  if [ ! -f "$BIONIC_LIB64/$lib" ]; then
    echo "ERROR: missing marble bionic lib: $BIONIC_LIB64/$lib" >&2
    exit 1
  fi
  cp "$BIONIC_LIB64/$lib" "$STAGE/vendor/lib64/$lib"
  chmod 0755 "$STAGE/vendor/lib64/$lib"
done
chmod 0755 "$STAGE/vendor/bin/mi_thermald"
ln -sf ../vendor/bin/mi_thermald "$STAGE/bin/mi_thermald"

tar -C "$STAGE" \
  --sort=name \
  --mtime='UTC 1970-01-01' \
  --owner=0 --group=0 --numeric-owner \
  -cf - bin vendor | gzip -9n > "$THERMAL_TAR"

TAR_BYTES="$(stat -c '%s' "$THERMAL_TAR")"
MAX_BYTES=$((1500 * 1000))
echo "=== TARBALL AUDIT: marble-mi-thermal.tar.gz ==="
echo "size=$TAR_BYTES bytes (limit=$MAX_BYTES bytes), conf_count=$CONF_COUNT"
if [ "$TAR_BYTES" -ge "$MAX_BYTES" ]; then
  echo "ERROR: $THERMAL_TAR exceeds 1.5 MB compact budget ($TAR_BYTES >= $MAX_BYTES)" >&2
  exit 1
fi

# 3. Verify 100% ELF dynamic symbol resolution for mi_thermald + libcrypto.so + libssl.so
#    against staged /compat/audio-runtime/lib64.
python3 - "$NM" "$READELF" "$STAGE/vendor/bin/mi_thermald" \
  "$STAGE/vendor/lib64/libcrypto.so" "$STAGE/vendor/lib64/libssl.so" "$AUDIO_RT_LIB64" <<'PY'
import os, subprocess, sys

nm, readelf, mi_bin, libcrypto, libssl, audio_rt = sys.argv[1:7]

def get_needed(path):
    out = subprocess.check_output([readelf, '-d', path], text=True)
    res = []
    for line in out.splitlines():
        if '(NEEDED)' in line and '[' in line and ']' in line:
            res.append(line.split('[', 1)[1].split(']', 1)[0])
    return res

def get_undef(path):
    out = subprocess.check_output([nm, '-D', '--undefined-only', path], text=True)
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-2] == 'U':
            syms.add(parts[-1].split('@')[0])
    return syms

def get_def(path):
    out = subprocess.check_output([nm, '-D', '--defined-only', path], text=True)
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            syms.add(parts[-1].split('@')[0])
    return syms

available = {
    'libcrypto.so': libcrypto,
    'libssl.so': libssl,
}
for name in os.listdir(audio_rt):
    p = os.path.join(audio_rt, name)
    if os.path.isfile(p):
        available[name] = p

targets = [mi_bin, libcrypto, libssl]
used_libs = set([libcrypto, libssl])
for t in targets:
    needed = get_needed(t)
    missing_libs = [n for n in needed if n not in available]
    if missing_libs:
        raise SystemExit(f"Missing NEEDED libs for {os.path.basename(t)}: {missing_libs}")
    for n in needed:
        used_libs.add(available[n])
    print(f"NEEDED_OK {os.path.basename(t)}: {needed}")

defined = set()
for lib in sorted(used_libs):
    defined |= get_def(lib)

for t in targets:
    undef = get_undef(t)
    missing = sorted(undef - defined)
    if missing:
        raise SystemExit(f"Unresolved symbols in {os.path.basename(t)}: {missing[:20]}")
    print(f"SYMS_OK   {os.path.basename(t)}: undef={len(undef)} resolved={len(undef)} missing=0")
PY

# 4. Compile and run offline host unit test for native_thermal_launcher_marble.c.
HOST_CC="${HOST_CC:-cc}"
HOST_TEST_BIN="$STAGE/test_native_thermal_launcher_marble"
"$HOST_CC" -O2 -std=c11 -Wall -Wextra -Werror "$TEST_SRC" -o "$HOST_TEST_BIN"
"$HOST_TEST_BIN"

# 5. Also verify host --check mode against the freshly extracted tarball + stub linker64.
HOST_LAUNCHER_BIN="$STAGE/native_thermal_launcher_marble_host"
"$HOST_CC" -O2 -std=c11 -Wall -Wextra -Werror "$LAUNCHER_SRC" -o "$HOST_LAUNCHER_BIN"
mkdir -p "$STAGE/compat/audio-runtime/bin"
printf '#!/bin/sh\n' > "$STAGE/compat/audio-runtime/bin/linker64"
chmod 0755 "$STAGE/compat/audio-runtime/bin/linker64"
"$HOST_LAUNCHER_BIN" --check "$STAGE"

echo "=== SHA256SUMS ==="
sha256sum "$LAUNCHER_BIN" "$THERMAL_TAR"
