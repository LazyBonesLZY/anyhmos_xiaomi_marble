#!/bin/sh
# 只读采集小米 marble (Redmi Note 12 Turbo / POCO F5) Phase 0 移植基线信息。
# 用法：在已 Root 的原厂系统或 TWRP/OrangeFox Shell 中执行：
#   sh collect-marble-baseline.sh [输出目录]
# 本脚本不修改任何分区或系统文件，不采集账号/密码/IMEI 等敏感数据。

set -eu

OUT="${1:-/data/local/tmp/marble-baseline}"
mkdir -p "$OUT"

capture() {
    name="$1"
    shift
    {
        echo "### CMD: $*"
        "$@" 2>&1 || echo "### EXIT=$?"
    } > "$OUT/$name"
}

echo "[marble-baseline] Writing read-only snapshot to $OUT"

capture uname.txt uname -a
capture cmdline.txt cat /proc/cmdline
capture bootconfig.txt cat /proc/bootconfig
capture partitions.txt cat /proc/partitions
capture mounts.txt cat /proc/mounts
capture filesystems.txt cat /proc/filesystems
capture misc.txt cat /proc/misc
capture lsmod.txt lsmod
capture input-devices.txt cat /proc/bus/input/devices
capture sound-cards.txt cat /proc/asound/cards

if [ -r /proc/config.gz ]; then
    cp /proc/config.gz "$OUT/kernel-config.gz" || true
fi

{
    echo "### /dev/block/by-name"
    ls -la /dev/block/by-name 2>&1 || true
    echo ""
    echo "### Key partition sizes (bytes)"
    for part in super userdata boot boot_a boot_b init_boot init_boot_a vendor_boot vendor_boot_a vendor_boot_b recovery recovery_a vendor_dlkm bluetooth bluetooth_a persist metadata; do
        dev="/dev/block/by-name/$part"
        if [ -e "$dev" ]; then
            real=$(readlink -f "$dev" 2>/dev/null || echo "$dev")
            size=$(blockdev --getsize64 "$dev" 2>/dev/null || echo "unknown")
            printf "%-18s -> %-24s bytes=%s\n" "$part" "$real" "$size"
        fi
    done
} > "$OUT/block-layout.txt"

{
    for dir in /lib/modules /vendor/lib/modules /vendor_dlkm/lib/modules; do
        if [ -d "$dir" ]; then
            echo "### $dir"
            ls -la "$dir" 2>&1 || true
            for meta in modules.load modules.dep modules.softdep modules.alias; do
                if [ -r "$dir/$meta" ]; then
                    echo "--- $dir/$meta ---"
                    cat "$dir/$meta" 2>&1 || true
                fi
            done
        fi
    done
} > "$OUT/modules-inventory.txt"

{
    echo "### DRM"
    ls -la /sys/class/drm 2>&1 || true
    for p in /sys/class/drm/card*-DSI-*/status /sys/class/drm/card*-DSI-*/modes; do
        [ -r "$p" ] && { echo "--- $p ---"; cat "$p"; }
    done
    echo "### Backlight"
    ls -la /sys/class/backlight 2>&1 || true
    for d in /sys/class/backlight/*; do
        [ -d "$d" ] || continue
        echo "--- $d ---"
        for f in brightness max_brightness actual_brightness bl_power; do
            [ -r "$d/$f" ] && printf "%s=%s\n" "$f" "$(cat "$d/$f" 2>/dev/null)"
        done
    done
    echo "### GPU (KGSL)"
    for f in gpu_model gpuclk devfreq/available_frequencies; do
        p="/sys/class/kgsl/kgsl-3d0/$f"
        [ -r "$p" ] && { echo "--- $p ---"; cat "$p"; }
    done
} > "$OUT/display-gpu.txt"

{
    echo "### USB / UDC / Role"
    ls -la /sys/class/udc /sys/class/usb_role 2>&1 || true
    ls -ld /sys/bus/platform/devices/*ssusb* /sys/bus/platform/devices/*dwc3* 2>&1 || true
    for p in /sys/bus/platform/devices/*ssusb*/mode /sys/class/usb_role/*/role; do
        [ -r "$p" ] && printf "%s=%s\n" "$p" "$(cat "$p" 2>/dev/null)"
    done
} > "$OUT/usb.txt"

{
    echo "### Remoteproc"
    for d in /sys/class/remoteproc/remoteproc*; do
        [ -d "$d" ] || continue
        printf "%s name=%s state=%s\n" "$d" "$(cat "$d/name" 2>/dev/null || echo ?)" "$(cat "$d/state" 2>/dev/null || echo ?)"
    done
    echo "### Firmware listings"
    for dir in /vendor/firmware_mnt/image /vendor/firmware /bt_firmware/image /odm/firmware; do
        if [ -d "$dir" ]; then
            echo "--- $dir ---"
            ls -la "$dir" 2>&1 || true
        fi
    done
} > "$OUT/remoteproc-firmware.txt"

{
    echo "### Power supply & qcom-battery"
    ls -la /sys/class/power_supply 2>&1 || true
    for d in /sys/class/power_supply/battery /sys/class/power_supply/usb /sys/class/qcom-battery; do
        [ -d "$d" ] || continue
        echo "--- $d ---"
        ls -la "$d" 2>&1 || true
        for f in status capacity online real_type quick_charge_type pd_verifed authentic power_max; do
            [ -r "$d/$f" ] && printf "%s=%s\n" "$f" "$(cat "$d/$f" 2>/dev/null)"
        done
    done
} > "$OUT/power-battery.txt"

{
    echo "### Relevant dmesg (panel/drm/kgsl/touch/usb/wlan/bt/adsp)"
    dmesg 2>&1 | grep -iE 'dsi|drm|panel|kgsl|adreno|goodix|focaltech|xiaomi_touch|ssusb|dwc3|cnss|wlan|btpower|adsp|remoteproc|ukee' | head -n 400 || true
} > "$OUT/dmesg-filtered.txt"

echo "[marble-baseline] Done. Pull $OUT to host and share non-private outputs."
