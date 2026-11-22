#!/bin/bash
set -euo pipefail

# Read-only backup & structure inspection of Xiaomi marble boot partitions via rooted ADB.
# Never writes to any partition on the device. Backups are stored under work/marble-backup/ (gitignored).

repo=$(realpath "$(dirname "$0")/../../..")
out_dir=${1:-"$repo/work/marble-backup/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$out_dir"

echo "[1/4] Checking ADB device & root access..."
adb wait-for-device
device_name=$(adb shell "getprop ro.product.device" | tr -d '\r\n')
soc_name=$(adb shell "getprop ro.vendor.qti.soc_name" | tr -d '\r\n')
slot=$(adb shell "getprop ro.boot.slot_suffix" | tr -d '\r\n')
if [[ "$device_name" != "marble" && "$soc_name" != "ukee" ]]; then
	echo "ERROR: Connected device is not marble/ukee (device='$device_name', soc='$soc_name')" >&2
	exit 1
fi
if [[ "$slot" != "_a" && "$slot" != "_b" ]]; then
	echo "ERROR: Unexpected active slot_suffix='$slot'" >&2
	exit 1
fi
echo "Device verified: device=$device_name soc=$soc_name active_slot=$slot"
printf 'device=%s\nsoc=%s\nslot=%s\ntimestamp=%s\n' \
	"$device_name" "$soc_name" "$slot" "$(date -Is)" > "$out_dir/metadata.txt"

parts=("boot${slot}" "vendor_boot${slot}" "dtbo${slot}" "vbmeta${slot}" "vbmeta_system${slot}")

echo "[2/4] Computing on-device SHA-256 checksums (read-only)..."
: > "$out_dir/device-sha256.txt"
for part in "${parts[@]}"; do
	dev_sha=$(adb shell "su -c 'sha256sum /dev/block/by-name/${part}'" | tr -d '\r' | awk '{print $1}')
	if [[ ! "$dev_sha" =~ ^[0-9a-f]{64}$ ]]; then
		echo "ERROR: Failed to read SHA-256 of /dev/block/by-name/${part}: '$dev_sha'" >&2
		exit 1
	fi
	printf '%s  %s.img\n' "$dev_sha" "$part" >> "$out_dir/device-sha256.txt"
	echo "  device $part: $dev_sha"
done

echo "[3/4] Streaming raw partition images to $out_dir..."
for part in "${parts[@]}"; do
	echo "  dumping /dev/block/by-name/${part} -> ${part}.img ..."
	adb exec-out "su -c 'dd if=/dev/block/by-name/${part} bs=1048576 2>/dev/null'" > "$out_dir/${part}.img"
done

echo "[4/4] Verifying local backup SHA-256 matches live device bit-for-bit..."
(cd "$out_dir" && sha256sum -c device-sha256.txt)
cp "$out_dir/device-sha256.txt" "$out_dir/SHA256SUMS.txt"

if command -v unpack_bootimg >/dev/null 2>&1; then
	echo "[extra] Unpacking boot & vendor_boot headers for structure inspection..."
	mkdir -p "$out_dir/unpacked-boot" "$out_dir/unpacked-vendor_boot"
	unpack_bootimg --boot_img "$out_dir/boot${slot}.img" --out "$out_dir/unpacked-boot" \
		> "$out_dir/boot-info.txt"
	unpack_bootimg --boot_img "$out_dir/vendor_boot${slot}.img" --out "$out_dir/unpacked-vendor_boot" \
		> "$out_dir/vendor_boot-info.txt"
	cat "$out_dir/boot-info.txt"
	echo "---"
	cat "$out_dir/vendor_boot-info.txt"
fi

echo "BACKUP COMPLETE & VERIFIED: $out_dir"
