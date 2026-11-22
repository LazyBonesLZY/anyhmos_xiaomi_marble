#!/bin/bash
set -euo pipefail

repo=$(realpath "${1:?source checkout required}")
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/marble-kernel-build.XXXXXX")

kernel_repo=${MARBLE_KERNEL_REPO:-https://github.com/Pzqqt/android_kernel_xiaomi_marble.git}
kernel_commit=${MARBLE_KERNEL_COMMIT:-987b4d173c77aa702f7fe848090e6d42d9f021a5}
llvm_url=${MARBLE_LLVM_URL:-https://mirrors.edge.kernel.org/pub/tools/llvm/files/llvm-23.1.6-x86_64.tar.xz}
defconfig=${MARBLE_DEFCONFIG:-marble_defconfig}
build_mode=${MARBLE_BUILD_MODE:-full}
localversion=${MARBLE_LOCALVERSION:--anyhmos}
jobs=${MARBLE_JOBS:-$(nproc --all)}

kernel_dir=$work/kernel
output=$work/kernel-out
ipc_dir=$work/ipc-module
dmabuf_dir=$work/dmabuf-sync
uapi_dir=$work/uapi-headers
uapi_android_dir=$work/uapi-android
artifacts=${MARBLE_ARTIFACT_DIR:-$work/artifacts}
mkdir -p "$kernel_dir" "$output" "$artifacts/modules" "$artifacts/tests" "$artifacts/audit"
artifacts=$(realpath "$artifacts")

stage=init
finish() {
	local status=$?
	printf '{"exit_code":%d,"stage":"%s","build_mode":"%s"}\n' \
		"$status" "$stage" "$build_mode" > "$artifacts/build-result.json"
	exit "$status"
}
trap finish EXIT
exec > >(tee "$artifacts/build.log") 2>&1

stage=toolchain
if [[ -n "${MARBLE_CLANG_BIN:-}" && -x "${MARBLE_CLANG_BIN}/clang" ]]; then
	clang_bin=$(realpath "$MARBLE_CLANG_BIN")
else
	mkdir -p "$work/toolchain"
	curl --fail --location --retry 3 "$llvm_url" -o "$work/toolchain/llvm.tar.xz"
	tar -xJf "$work/toolchain/llvm.tar.xz" -C "$work/toolchain"
	clang_bin=$(dirname "$(find "$work/toolchain" -maxdepth 3 -type f -name clang | head -n 1)")
fi
export PATH="$clang_bin:$PATH"
clang --version | tee "$artifacts/audit/clang-version.txt"

stage=source-fetch
git -C "$kernel_dir" init -q
git -C "$kernel_dir" remote add origin "$kernel_repo"
git -C "$kernel_dir" fetch --depth=1 origin "$kernel_commit"
git -C "$kernel_dir" checkout -q --detach FETCH_HEAD
test "$(git -C "$kernel_dir" rev-parse HEAD)" = "$kernel_commit"
git -C "$kernel_dir" rev-parse HEAD > "$artifacts/audit/kernel-revision.txt"
git -C "$repo" rev-parse HEAD > "$artifacts/audit/source-revision.txt"
export PATH="$clang_bin:$kernel_dir/build-tools:$PATH"

stage=apply-exports
cp "$repo/devices/marble/kernel/gki-exports/drivers/android/sheng_hmos_exports.c" \
	"$kernel_dir/drivers/android/"
cp "$repo/devices/marble/kernel/gki-exports/drivers/android/sheng_hmos_api_v1.h" \
	"$kernel_dir/drivers/android/"
git -C "$kernel_dir" apply "$repo/devices/marble/kernel/gki-exports.patch"

stage=kernel-config
export KBUILD_BUILD_USER=anyhmos
export KBUILD_BUILD_HOST=marble
export LOCALVERSION="$localversion"

make_flags=(ARCH=arm64 LLVM=1 LLVM_IAS=1 O="$output")
if ! command -v ccache >/dev/null 2>&1; then
	make_flags+=(CCACHE=)
fi
make_kcflags="-D__ANDROID_COMMON_KERNEL__ -O3"
make_kbuild_ldflags="-O3 --lto-O3"

make -C "$kernel_dir" "${make_flags[@]}" KCFLAGS="$make_kcflags" \
	KBUILD_LDFLAGS="$make_kbuild_ldflags" "$defconfig"
# Disable Android-only uname spoofing so HarmonyOS and IPC contract tests see exact kernel release.
"$kernel_dir/scripts/config" --file "$output/.config" -d UNAME_OVERRIDE
make -C "$kernel_dir" "${make_flags[@]}" KCFLAGS="$make_kcflags" \
	KBUILD_LDFLAGS="$make_kbuild_ldflags" olddefconfig
cp "$output/.config" "$artifacts/audit/kernel.config"

stage=kernel-build
case "$build_mode" in
	full)
		make -C "$kernel_dir" "${make_flags[@]}" KCFLAGS="$make_kcflags" \
			KBUILD_LDFLAGS="$make_kbuild_ldflags" -j"$jobs"
		;;
	ipc-only)
		make -C "$kernel_dir" "${make_flags[@]}" KCFLAGS="$make_kcflags" \
			KBUILD_LDFLAGS="$make_kbuild_ldflags" -j"$jobs" Image modules_prepare
		if [[ ! -s "$output/vmlinux.symvers" ]]; then
			make -C "$kernel_dir" "${make_flags[@]}" KCFLAGS="$make_kcflags" \
				KBUILD_LDFLAGS="$make_kbuild_ldflags" -j"$jobs" vmlinux.symvers
		fi
		if [[ ! -s "$output/Module.symvers" && -s "$output/vmlinux.symvers" ]]; then
			cp "$output/vmlinux.symvers" "$output/Module.symvers"
		fi
		;;
	*)
		echo "Unsupported MARBLE_BUILD_MODE: $build_mode (expected 'full' or 'ipc-only')" >&2
		exit 2
		;;
esac

test -s "$output/arch/arm64/boot/Image"
test -s "$output/Module.symvers"
grep -Eq '[[:space:]]sheng_hmos_ipc_api_v1[[:space:]]' "$output/Module.symvers"
cp "$output/arch/arm64/boot/Image" "$artifacts/Image"
cp "$output/Module.symvers" "$artifacts/audit/Module.symvers"
if [[ -s "$output/vmlinux.symvers" ]]; then
	cp "$output/vmlinux.symvers" "$artifacts/audit/vmlinux.symvers"
fi
release=$(cat "$output/include/config/kernel.release")
printf '%s\n' "$release" > "$artifacts/audit/kernel-release.txt"
echo "Built marble kernel release: $release"

stage=hmos-modules-build
cp -a "$repo/compat/ipc" "$ipc_dir"
make -C "$kernel_dir" "${make_flags[@]}" KCFLAGS="$make_kcflags" \
	KBUILD_LDFLAGS="$make_kbuild_ldflags" M="$ipc_dir" -j"$jobs" modules
test -s "$ipc_dir/sheng_hmos_ipc.ko"

cp -a "$repo/compat/graphics/dmabuf_sync" "$dmabuf_dir"
make -C "$kernel_dir" "${make_flags[@]}" KCFLAGS="$make_kcflags" \
	KBUILD_LDFLAGS="$make_kbuild_ldflags" M="$dmabuf_dir/kernel" -j"$jobs" modules
test -s "$dmabuf_dir/kernel/sheng_dmabuf_sync.ko"

stage=hmos-modules-audit
audit_module() {
	local src=$1
	local name=$2
	local stripped="$artifacts/modules/${name}.ko"

	llvm-strip -S "$src" -o "$stripped"
	llvm-nm --defined-only "$stripped" > "$artifacts/audit/${name}.symbols.txt"
	grep -Eq '[[:space:]]__cfi_check$' "$artifacts/audit/${name}.symbols.txt"
	modinfo "$stripped" > "$artifacts/audit/${name}.modinfo.txt"
	local vermagic
	read -r vermagic < <(modinfo -F vermagic "$stripped")
	if [[ "$vermagic" != "$release "* || "$vermagic" != *"modversions"* || "$vermagic" != *"aarch64"* ]]; then
		echo "Unexpected vermagic for $name: $vermagic (expected prefix '$release ')" >&2
		exit 1
	fi
	modprobe --dump-modversions "$stripped" > "$artifacts/audit/${name}.modversions.txt"
	python3 - "$output/Module.symvers" "$artifacts/audit/${name}.modversions.txt" "$name" <<'PY'
import pathlib
import sys

symvers_path, modver_path, mod_name = sys.argv[1:4]
expected = {}
for line in pathlib.Path(symvers_path).read_text().splitlines():
    fields = line.split()
    if len(fields) >= 2:
        expected[fields[1]] = int(fields[0], 16)

for line in pathlib.Path(modver_path).read_text().splitlines():
    fields = line.split()
    if len(fields) < 2:
        continue
    crc, sym = int(fields[0], 16), fields[1]
    if expected.get(sym) != crc:
        raise SystemExit(
            f"{mod_name}: symbol CRC mismatch for {sym}: got {crc:#x}, expected {expected.get(sym)}"
        )
PY
}

audit_module "$ipc_dir/sheng_hmos_ipc.ko" "sheng_hmos_ipc"
audit_module "$dmabuf_dir/kernel/sheng_dmabuf_sync.ko" "sheng_dmabuf_sync"

stage=ipc-tests-build
make -C "$kernel_dir" ARCH=arm64 O="$output" INSTALL_HDR_PATH="$uapi_dir" headers_install
mkdir -p "$uapi_android_dir/linux/android"
cp "$uapi_dir/include/linux/android/"*.h "$uapi_android_dir/linux/android/"

cc_arm64=${MARBLE_ARM64_CC:-aarch64-linux-gnu-gcc}
"$cc_arm64" -static -O2 -Wall -pthread \
	-I"$uapi_android_dir" -I"$repo/compat/ipc" \
	"$repo/tests/ipc/token_contract.c" -o "$artifacts/tests/token_contract"
"$cc_arm64" -static -O2 -Wall -pthread \
	-I"$uapi_android_dir" -I"$repo/compat/ipc" \
	"$repo/tests/ipc/binder_nested.c" -o "$artifacts/tests/binder_nested"
"$cc_arm64" -static -O2 -Wall -pthread \
	-I"$uapi_android_dir" -I"$repo/compat/ipc" \
	"$repo/tests/ipc/binder_oneway_pid.c" -o "$artifacts/tests/binder_oneway_pid"
file "$artifacts/tests/"* | tee "$artifacts/audit/ipc-tests-file.txt"

if [[ "$build_mode" == "full" ]]; then
	stage=vendor-modules-package
	vb_dir=$work/vendor_boot_modules
	vd_dir=$work/vendor_dlkm_modules
	mkdir -p "$vb_dir" "$vd_dir"

	python3 - "$kernel_dir/build_bouquet.sh" "$output" "$vb_dir" "$vd_dir" "$artifacts/audit/vendor-modules-summary.json" <<'PY'
import json
import pathlib
import re
import shutil
import subprocess
import sys

bouquet_sh = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
kout = pathlib.Path(sys.argv[2])
vb_dir = pathlib.Path(sys.argv[3])
vd_dir = pathlib.Path(sys.argv[4])
summary_path = pathlib.Path(sys.argv[5])

def extract_list(var_name):
    m = re.search(rf"{var_name}='(.*?)'", bouquet_sh, re.S)
    if not m:
        raise SystemExit(f"Could not locate {var_name} in build_bouquet.sh")
    return [line.strip() for line in m.group(1).splitlines() if line.strip()]

both = extract_list("both_need_modules")
vb_only = extract_list("vendor_boot_need_modules")
vd_only = extract_list("vendor_dlkm_need_modules")

required_critical = {
    "msm_drm.ko",
    "msm_kgsl.ko",
    "gpucc-waipio.ko",
    "dwc3-msm.ko",
    "goodix_core.ko",
    "xiaomi_touch.ko",
    "qca_cld3_qca6490.ko",
}

missing = []
copied = {"vendor_boot": [], "vendor_dlkm": []}

def strip_into(rel, dest_dir, bucket):
    src = kout / rel
    name = src.name
    if name == "qca6490.ko":
        name = "qca_cld3_qca6490.ko"
    if not src.is_file():
        missing.append(rel)
        return
    dst = dest_dir / name
    subprocess.run(["llvm-strip", "-S", str(src), "-o", str(dst)], check=True)
    copied[bucket].append(name)

for rel in both:
    strip_into(rel, vb_dir, "vendor_boot")
for f in sorted(vb_dir.glob("*.ko")):
    shutil.copy2(f, vd_dir / f.name)
    copied["vendor_dlkm"].append(f.name)
for rel in vb_only:
    strip_into(rel, vb_dir, "vendor_boot")
for rel in vd_only:
    strip_into(rel, vd_dir, "vendor_dlkm")

present = set(copied["vendor_boot"]) | set(copied["vendor_dlkm"])
missing_critical = sorted(required_critical - present)
summary = {
    "vendor_boot_count": len(copied["vendor_boot"]),
    "vendor_dlkm_count": len(copied["vendor_dlkm"]),
    "missing_optional": missing,
    "missing_critical": missing_critical,
}
summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
print(json.dumps(summary, indent=2))
if missing_critical:
    raise SystemExit(f"Missing critical marble modules: {missing_critical}")
PY

	tar -czf "$artifacts/vendor_boot_modules.tar.gz" -C "$vb_dir" .
	tar -czf "$artifacts/vendor_dlkm_modules.tar.gz" -C "$vd_dir" .

	stage=devicetree-package
	dt_src="$output/arch/arm64/boot/dts/vendor/qcom"
	if [[ -f "$dt_src/ukee.dtb" && -f "$dt_src/marble-sm7475-pm8008-overlay.dtbo" ]]; then
		dt_base=$work/dt_base
		dt_tp=$work/dt_techpack
		mkdir -p "$dt_base" "$dt_tp" "$artifacts/devicetree"
		cp "$dt_src/ukee.dtb" "$dt_base/"
		cp "$dt_src/marble-sm7475-pm8008-overlay.dtbo" "$dt_base/"
		for d in audio camera cvp display/display eva mmrm video; do
			if [[ -d "$dt_src/$d" ]]; then
				base=$(basename "$d")
				mkdir -p "$dt_tp/$base"
				cp "$dt_src/$d/"*.dtbo "$dt_tp/$base/" 2>/dev/null || true
			fi
		done
		merge_dtbs.py -b "$dt_base" -t "$dt_tp" -o "$artifacts/devicetree"
		mkdtboimg.py create "$artifacts/devicetree/dtbo.img" \
			"$artifacts/devicetree/marble-sm7475-pm8008-overlay.dtbo"
		avbtool add_hash_footer --partition_name dtbo \
			--partition_size $((24 * 1024 * 1024)) \
			--image "$artifacts/devicetree/dtbo.img"
	fi
fi

stage=checksums
(
	cd "$artifacts"
	find . -maxdepth 3 -type f ! -name SHA256SUMS.txt ! -name build.log ! -name build-result.json \
		| sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS.txt
)
cat "$artifacts/SHA256SUMS.txt"

stage=complete
printf 'ARTIFACT_DIRECTORY=%s\n' "$artifacts"
