#!/busybox sh
# SPDX-License-Identifier: MIT
# Xiaomi marble QCA6490 Wi-Fi bring-up (CNSS2 + MHI + qca_cld3_qca6490 -> wlan0 -> OHOS Wi-Fi hosts).
set -eu
B=/busybox
R=/hmos-root
S=/compat/original-firmware
M="$R/vendor_dlkm/lib/modules"
stage=firmware
# shellcheck disable=SC2154  # c is assigned inside the trap
 trap 'c=$?; echo "WIFI_START_EXIT=$c STAGE=$stage"' EXIT

$B cat /proc/sys/kernel/random/boot_id /proc/uptime

# 1. Mount stock modem_a read-only and stage QCA6490 firmware + WCNSS_qcom_cfg.ini
# into BOTH /compat/firmware and /hmos-root/compat/firmware (PID1/kworker root visibility).
$B mkdir -p "$S" /compat/firmware/qca6490 "$R/compat/firmware/qca6490" \
 /compat/firmware/wlan/qca_cld/qca6490 "$R/compat/firmware/wlan/qca_cld/qca6490"

if ! $B grep -q ' /compat/original-firmware ' /proc/mounts; then
 $B mount -t vfat -o ro /dev/block/by-name/modem_a "$S"
fi
[ -f "$S/image/qca6490/amss20.bin" ] || exit 31

for src in "$S"/image/qca6490/*; do
 [ -f "$src" ] || continue
 base=${src##*/}
 for dst_root in /compat/firmware "$R/compat/firmware"; do
  $B cp "$src" "$dst_root/qca6490/$base"
  $B chmod 0644 "$dst_root/qca6490/$base"
 done
done

if [ -f /graphics/WCNSS_qcom_cfg.ini ]; then
 for dst_root in /compat/firmware "$R/compat/firmware"; do
  $B cp /graphics/WCNSS_qcom_cfg.ini "$dst_root/wlan/qca_cld/qca6490/WCNSS_qcom_cfg.ini"
  $B cp /graphics/WCNSS_qcom_cfg.ini "$dst_root/wlan/qca_cld/WCNSS_qcom_cfg.ini"
  $B chmod 0644 "$dst_root/wlan/qca_cld/qca6490/WCNSS_qcom_cfg.ini" "$dst_root/wlan/qca_cld/WCNSS_qcom_cfg.ini"
 done
fi

# CRITICAL: no trailing newline in firmware_class path!
printf '/compat/firmware' > /sys/module/firmware_class/parameters/path
echo WIFI_RAM_FIRMWARE_STAGED=1

load_ko() {
 ko="$1"
 mod=$(printf '%s' "${ko%.ko}" | $B tr - _)
 if $B grep -q "^$mod " /proc/modules; then
  return 0
 fi
 if [ -f "$M/$ko" ]; then
  kpath="$M/$ko"
 elif [ -f "/lib/modules/$ko" ]; then
  kpath="/lib/modules/$ko"
 else
  return 0
 fi
 $B timeout 15 $B insmod "$kpath" || {
  rc=$?
  echo "WIFI_MOD_WARN $ko rc=$rc"
  return "$rc"
 }
 echo "WIFI_MOD_LOADED $ko"
}

# 2. Load CNSS2 platform + PCIe + MHI + QRTR-MHI transport.
stage=platform
for ko in \
 qmi_helpers.ko wlan_firmware_service.ko cnss_plat_ipc_qmi_svc.ko cnss_utils.ko \
 qcom_ramdump.ko qcom_smd.ko qcom_glink.ko qcom_glink_smem.ko rproc_qcom_common.ko \
 hwid.ko memory_dump_v2.ko pci-msm-drv.ko mhi.ko cnss2.ko qrtr.ko qrtr-mhi.ko; do
 load_ko "$ko" || true
done

# 3. Enable CNSS recovery before fs_ready so any firmware mismatch cannot panic the kernel.
stage=calibration
if [ -w /sys/kernel/cnss/recovery ]; then
 echo 1 > /sys/kernel/cnss/recovery
fi
if [ -w /sys/kernel/cnss/fs_ready ]; then
 echo 1 > /sys/kernel/cnss/fs_ready
fi

# 4. Load QCA6490 WLAN driver and trigger /dev/wlan ON so wlan0 is created.
stage=driver
for ko in cnss_prealloc.ko cnss_nl.ko cfg80211.ko; do
 load_ko "$ko" || true
done
load_ko qca_cld3_qca6490.ko

# Wait briefly for CNSS cold-boot calibration to settle, then trigger /dev/wlan ON.
$B sleep 2
wlan_major=$($B awk '$2=="wlan" {print $1; exit}' /proc/devices 2>/dev/null || true)
if [ -z "$wlan_major" ]; then
 wlan_major=$($B dmesg | $B sed -n 's/.*wlan_hdd_state wlan major(\([0-9]*\)).*/\1/p' | $B tail -n 1 || true)
fi
wlan_major=${wlan_major:-499}
if [ ! -c /dev/wlan ]; then
 $B rm -f /dev/wlan
 $B mknod /dev/wlan c "$wlan_major" 0
 $B chmod 0660 /dev/wlan
fi
echo "WIFI_TRIGGER_DEV_WLAN major=$wlan_major"
echo ON > /dev/wlan 2>&1 || true

# 5. Wait for wlan0 netdev to appear.
waited=0
while [ ! -e /sys/class/net/wlan0 ]; do
 [ "$waited" -lt 25 ] || exit 40
 if [ "$waited" -eq 8 ]; then
  echo ON > /dev/wlan 2>&1 || true
 fi
 $B sleep 1
 waited=$((waited + 1))
done
echo "WIFI_NETDEV_WAIT_SECONDS=$waited"
echo WIFI_KERNEL_NETDEV_READY=1

# 6. Seed missing configs before the explicit init gate. Preserve the legacy
# launch path if the readiness/ownership guard declines to touch existing data.
stage=supplicant-config
WPA_STAGE=/hmos-compat-marble/graphics/wifi-supplicant
if [ ! -f "$WPA_STAGE/marble-wifi-supplicant-stage.sh" ]; then
 echo WIFI_SUPPLICANT_STAGE_SKIPPED=not-bundled
elif $B pidof wpa_host wifi_manager_service >/dev/null 2>&1; then
 echo WIFI_SUPPLICANT_STAGE_SKIPPED=consumer-running
elif MARBLE_ROOT="$R" WIFI_PAYLOAD="$WPA_STAGE" "$B" sh "$WPA_STAGE/marble-wifi-supplicant-stage.sh"; then
 echo WIFI_SUPPLICANT_STAGE_OK=config-only
else
 echo "WIFI_SUPPLICANT_STAGE_WARN rc=$? legacy_launch=1"
fi

# 7. Request OHOS wifi_host, wpa_host, and wifi_manager_service.
stage=native-services
$B chroot "$R" /system/bin/param set marble.debug.start.wifi true
echo WIFI_NATIVE_SERVICES_REQUESTED=1
stage=complete