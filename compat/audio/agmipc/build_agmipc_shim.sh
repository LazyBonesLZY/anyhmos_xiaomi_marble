#!/usr/bin/env bash
# Build libhmos_agmipc_shim.so: the in-process AGM passthrough for marble PAL.
#
# Mirrors the `pal-server` target in tools/build/build_current.py:
#   * OHOS NDK clang, --target=aarch64-linux-android33
#   * -nostdlib, linking only against the marble Bionic stubs in
#     work/marble-bionic/lib64 (the result must run inside the chroot's
#     Android linker, not against OHOS libc).
#
# Outputs (default work/marble-agmipc-shim):
#   libhmos_agmipc_shim.so
#   gen/agmipc_shim_syms.{h,S}, gen/agmipc_shim_names.h
#   PROVENANCE.md
#
# Override with:
#   HMOS_SDK       native SDK root           (default work/ohos-sdk/installed/native)
#   HMOS_BIONIC_LINK  bionic link stubs dir  (default work/marble-bionic/lib64)
#   HMOS_AGMIPC_OUT   output dir             (default work/marble-agmipc-shim)
#   HMOS_AGMIPCLIENT  reference marshaller   (default work/marble-audio-dump/lib64/libagmclient.so)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." && pwd)
SRC=$ROOT/compat/audio/agmipc
SDK=${HMOS_SDK:-$ROOT/work/ohos-sdk/installed/native}
BIONIC=${HMOS_BIONIC_LINK:-$ROOT/work/marble-bionic/lib64}
OUT=${HMOS_AGMIPC_OUT:-$ROOT/work/marble-agmipc-shim}
AGMIPCLIENT=${HMOS_AGMIPCLIENT:-$ROOT/work/marble-audio-dump/lib64/libagmclient.so}
CC=${HMOS_CLANG:-$SDK/llvm/bin/clang}
NM=${HMOS_NM:-$SDK/llvm/bin/llvm-nm}
READELF=${HMOS_READELF:-$SDK/llvm/bin/llvm-readelf}

PRODUCT=$OUT/libhmos_agmipc_shim.so
GEN=$OUT/gen

die() { echo "build_agmipc_shim: $*" >&2; exit 1; }

[[ -x $CC ]] || die "clang not found: $CC"
[[ -d $BIONIC ]] || die "bionic link dir not found: $BIONIC"
[[ -f $SRC/agm_symbols.txt ]] || die "symbol manifest missing: $SRC/agm_symbols.txt"

mkdir -p "$OUT" "$GEN"

# 1. Validate the checked-in manifest against the marshaller it was harvested
#    from, when that reference dump is available.
if [[ -f $AGMIPCLIENT ]]; then
    "$READELF" -sW --dyn-syms "$AGMIPCLIENT" \
        | awk '$4=="FUNC" && $5=="GLOBAL" && $7!="UND" {print $8}' \
        | grep -E '^agm_' | LC_ALL=C sort -u > "$GEN/agm_symbols.reference.txt"
    if ! diff -u "$SRC/agm_symbols.txt" "$GEN/agm_symbols.reference.txt" > "$GEN/agm_symbols.diff"; then
        die "agm_symbols.txt does not match $AGMIPCLIENT (see $GEN/agm_symbols.diff)"
    fi
    echo "manifest matches $AGMIPCLIENT ($(wc -l < "$SRC/agm_symbols.txt") symbols)"
else
    echo "warning: $AGMIPCLIENT absent, manifest not re-verified" >&2
fi

# 2. Generate trampolines + slot metadata from the single symbol list.
python3 "$SRC/gen_shim_sources.py" --symbols "$SRC/agm_symbols.txt" --out-dir "$GEN"

# 3. Compile.  Same flag set as the pal-server target, plus .S support.
CFLAGS=(
    --target=aarch64-linux-android33
    --sysroot="$SDK/sysroot"
    -isystem "$SDK/sysroot/usr/include/aarch64-linux-ohos"
    -fuse-ld=lld -fPIC -shared -nostdlib -D_GNU_SOURCE -mno-outline-atomics
    -O2 -Wall -Wextra -Werror
    -Wl,-z,defs -Wl,-z,relro -Wl,-z,noexecstack
    -Wl,-soname,libhmos_agmipc_shim.so
    -L"$BIONIC"
    -I"$GEN"
)
"$CC" "${CFLAGS[@]}" \
    "$SRC/libagm_passthrough.c" \
    "$SRC/agmipc_fail_stub.S" \
    "$GEN/agmipc_shim_syms.S" \
    -lc -ldl \
    -o "$PRODUCT"

# 4. Self-describing provenance.
SHA=$(sha256sum "$PRODUCT" | awk '{print $1}')
{
    echo "# libhmos_agmipc_shim.so - provenance"
    echo
    echo "In-process AGM passthrough for the marble (SM7475) HarmonyOS NEXT port."
    echo "It replaces the AGM C API that \`libagmclient.so\` exports (the HIDL"
    echo "marshaller) with direct calls into \`libagm.so\` (the real GSL/DSP/ACDB"
    echo "implementation), because this device has no \`/dev/hwbinder\` for"
    echo "\`vendor.qti.hardware.AGMIPC@1.0-service\`."
    echo
    echo "## Inputs"
    echo
    echo '| Input | Path | SHA-256 |'
    echo '| --- | --- | --- |'
    for f in "$SRC/libagm_passthrough.c" "$SRC/agmipc_fail_stub.S" \
             "$SRC/agm_symbols.txt" "$SRC/gen_shim_sources.py"; do
        printf '| `%s` | `%s` | `%s` |\n' "$(basename "$f")" \
            "${f#"$ROOT"/}" "$(sha256sum "$f" | awk '{print $1}')"
    done
    for f in "$GEN/agmipc_shim_syms.S" "$GEN/agmipc_shim_syms.h" "$GEN/agmipc_shim_names.h"; do
        printf '| `%s` (generated) | `%s` | `%s` |\n' "$(basename "$f")" \
            "${f#"$ROOT"/}" "$(sha256sum "$f" | awk '{print $1}')"
    done
    echo
    echo "## Product"
    echo
    echo '```'
    echo "path:   ${PRODUCT#"$ROOT"/}"
    echo "sha256: $SHA"
    echo "size:   $(stat -c %s "$PRODUCT") bytes"
    echo '```'
    echo
    echo "## Build command"
    echo
    echo '```sh'
    echo "HMOS_AGMIPC_OUT=$OUT \\"
    echo "  bash compat/audio/agmipc/build_agmipc_shim.sh"
    echo '```'
    echo
    echo "Effective compiler invocation:"
    echo
    echo '```'
    printf '%s\n' "${CC} ${CFLAGS[*]} compat/audio/agmipc/libagm_passthrough.c" \
        "compat/audio/agmipc/agmipc_fail_stub.S work/marble-agmipc-shim/gen/agmipc_shim_syms.S -lc -ldl -o libhmos_agmipc_shim.so"
    echo '```'
    echo
    echo "## Symbol manifest"
    echo
    echo "\`compat/audio/agmipc/agm_symbols.txt\` lists the $(wc -l < "$SRC/agm_symbols.txt")"
    echo "symbols exported by \`libagmclient.so\`. The build re-derives the list from"
    echo "the reference dump and fails if the two differ."
    echo
    echo "## Not verified here"
    echo
    echo "The shim has never executed on the device. Only ELF-level facts are"
    echo "checked: exported symbol set, slot table, and the absence of undefined"
    echo "symbols outside libc/libdl. See \`work/marble-agmipc-report.md\`."
} > "$OUT/PROVENANCE.md"

echo "built $PRODUCT"
echo "sha256 $SHA"
"$NM" -D --defined-only "$PRODUCT" | awk '{print $3}' | grep -E '^agm_' | wc -l \
    | xargs -I{} echo "exported agm_* symbols: {}"