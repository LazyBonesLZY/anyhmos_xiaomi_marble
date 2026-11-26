#!/bin/sh
# SPDX-License-Identifier: MIT
# Stage Xiaomi marble Phase 4 Display + Adreno 725 GPU + Goodix Touch + Desktop overlays in RAM.
set -eu
B=/busybox
R=/hmos-root

# Idempotent bind mount.  The stage script is also re-run by hand while
# debugging; without this guard every re-run stacks another bind layer on the
# same target, which makes later "why is the old file back?" hunts pointless.
# Selective stage skip: `echo audio,bluetooth > /data/local/tmp/marble-skip-stages`
# lets a single subsystem be disabled for one boot without rebuilding or
# reflashing, which is the fastest way to bisect a boot regression on hardware.
# Strip spaces so `audio, bluetooth` works like `audio,bluetooth`.
SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d " ")
skipped() {
 [ -n "$SKIP" ] || return 1
 case ",$SKIP," in
  *",$1,"*) return 0 ;;
 esac
 return 1
}
echo "MARBLE_SKIP_STAGES=${SKIP:-none}"

bind_once() {
 $B grep -q " $2 " /proc/mounts 2>/dev/null && return 0
 if $B mount --bind "$1" "$2" 2>/dev/null; then
  return 0
 fi
 echo "MARBLE_BIND_MOUNT_FAIL dst=$2"
 return 1
}

echo MARBLE_PHASE4_STAGE_BEGIN

$B mkdir -p \
 "$R/compat/bin" "$R/compat/lib64" "$R/compat/etc/vulkan/icd.d" \
 "$R/compat/etc/sandbox" "$R/compat/etc/cust" "$R/compat/firmware" \
 "$R/system/etc/shng" "$R/system/etc/hdfconfig" "$R/chip_prod/etc/hdfconfig" \
 "$R/data/local/tmp/hmos-graphics" "$R/data/local/tmp/hmos-capture"

# 1. Stage Adreno 725 GPU + Goodix 9916r touch firmware BEFORE loading kernel drivers.
if $B test -d /stock-gpu-firmware; then
 for f in /stock-gpu-firmware/*; do
  [ -f "$f" ] || continue
  base=${f##*/}
  $B cp "$f" "$R/compat/firmware/$base"
  $B chmod 0644 "$R/compat/firmware/$base"
 done
 # Kernel firmware_class runs in root mount namespace; expose /compat/firmware at root too.
 $B mkdir -p /compat/firmware
 for f in "$R/compat/firmware"/*; do
  [ -f "$f" ] || continue
  base=${f##*/}
  $B cp "$f" "/compat/firmware/$base"
  $B chmod 0644 "/compat/firmware/$base"
 done
 echo "MARBLE_FIRMWARE_MIRRORED n=$($B ls /compat/firmware 2>/dev/null | $B wc -l) src=$($B ls "$R/compat/firmware" 2>/dev/null | $B wc -l)"
 if $B test -w /sys/module/firmware_class/parameters/path; then
  printf '/compat/firmware' > /sys/module/firmware_class/parameters/path
 fi
 echo MARBLE_PHASE4_FIRMWARE_STAGED
fi

# 2. Extract exact bootloader msm_drm panel selector from /proc/cmdline.
dsi_display0=""
oled_wp=""
for tok in $($B cat /proc/cmdline); do
 case "$tok" in
  msm_drm.dsi_display0=*) dsi_display0=${tok#msm_drm.dsi_display0=} ;;
  msm_drm.oled_wp=*) oled_wp=${tok#msm_drm.oled_wp=} ;;
 esac
done
echo "MARBLE_PHASE4_PANEL_PARAM dsi_display0=$dsi_display0 oled_wp=$oled_wp"

load_ko() {
 ko="$1"; shift || true
 mod=$(printf '%s' "${ko%.ko}" | $B tr - _)
 if $B grep -q "^$mod " /proc/modules; then
  return 0
 fi
 if $B test -f "/graphics/$ko"; then
  kpath="/graphics/$ko"
 elif $B test -f "$R/vendor_dlkm/lib/modules/$ko"; then
  kpath="$R/vendor_dlkm/lib/modules/$ko"
 elif $B test -f "/lib/modules/$ko"; then
  kpath="/lib/modules/$ko"
 else
  return 0
 fi
 if $B insmod "$kpath" "$@" 2>&1; then
  echo "MARBLE_MOD_LOADED $ko"
 else
  echo "MARBLE_MOD_WARN $ko rc=$?"
 fi
}

# 3. Load regulators, SMMU, QFPROM (KGSL supplier), I2C/SPI (Goodix bus), Display, GPU, Touch.
for ko in \
 arm_smmu.ko rpmh-regulator.ko qti-fixed-regulator.ko qpnp-amoled-regulator.ko \
 qcom_pm8008-regulator.ko nvmem_qfprom.ko nvmem_qcom-spmi-sdam.ko qcom_hwspinlock.ko \
 regmap-spmi.ko qcom-i2c-pmic.ko i2c-msm-geni.ko spi-msm-geni.ko \
 msm-mmrm.ko panel_event_notifier.ko qseecom-mod.ko smcinvoke_mod.ko hdcp.ko \
 fsa4480-i2c.ko msm_ext_display.ko gh_irq_lend.ko gh_mem_notifier.ko; do
 load_ko "$ko"
done

# Load msm_drm.ko with exact bootloader panel parameter so m16t 1080x2400 OLED binds!
if [ -n "$dsi_display0" ]; then
 if [ -n "$oled_wp" ]; then
  load_ko msm_drm.ko "dsi_display0=$dsi_display0" "oled_wp=$oled_wp"
 else
  load_ko msm_drm.ko "dsi_display0=$dsi_display0"
 fi
else
 load_ko msm_drm.ko
fi

for ko in \
 gpucc-waipio.ko qcom_llcc_pmu.ko qcom-pmu-lib.ko qcom-cpufreq-hw.ko sched-walt.ko \
 msm_performance.ko dcvs_fp.ko qcom-dcvs.ko qcom_lpm.ko mdt_loader.ko msm_kgsl.ko \
 reboot-mode.ko qcom-pon.ko xiaomi_touch.ko goodix_core.ko; do
 load_ko "$ko"
done

# 3b. Mount stock modem_a RO, stage ADSP + speaker amp firmware + PD maps,
# load all ADSP/Battery/Audio/Haptics/BT transport modules BEFORE booting ADSP
# so SoundWire/PDR/PMIC-GLINK/ukee-mtp-snd-card all enumerate cleanly at boot.
$B mkdir -p /compat/original-firmware /compat/pd-maps /compat/firmware "$R/compat/firmware"
if ! $B grep -q ' /compat/original-firmware ' /proc/mounts; then
 if $B mount -t vfat -o ro /dev/block/by-name/modem_a /compat/original-firmware >/dev/null 2>&1; then
  echo "MARBLE_MODEM_FIRMWARE_MOUNTED"
 else
  echo "MARBLE_MODEM_FIRMWARE_MOUNT_FAIL"
 fi
fi
if $B test -f /graphics/aw882xx_acf.bin; then
 $B cp /graphics/aw882xx_acf.bin /compat/firmware/aw882xx_acf.bin
 $B cp /graphics/aw882xx_acf.bin "$R/compat/firmware/aw882xx_acf.bin"
 $B chmod 0644 /compat/firmware/aw882xx_acf.bin "$R/compat/firmware/aw882xx_acf.bin"
fi
if $B test -f /compat/original-firmware/image/adsp2.mdt; then
 for f in /compat/original-firmware/image/adsp2.* /compat/original-firmware/image/*.jsn; do
  [ -f "$f" ] || continue
  base=${f##*/}
  $B cp "$f" "/compat/firmware/$base"
  $B cp "$f" "$R/compat/firmware/$base"
  $B chmod 0644 "/compat/firmware/$base" "$R/compat/firmware/$base"
 done
 $B cp /compat/original-firmware/image/*.jsn /compat/pd-maps/ 2>/dev/null || true
 echo "MARBLE_PD_FIRMWARE_STAGED adsp=$($B ls "$R/compat/firmware" 2>/dev/null | $B grep -c '^adsp2\.') jsn=$($B ls "$R/compat/firmware" 2>/dev/null | $B grep -c '\.jsn$') maps=$($B ls /compat/pd-maps 2>/dev/null | $B wc -l)"
 for ko in \
  qmi_helpers.ko qcom_ramdump.ko qcom_smd.ko qcom_glink.ko qcom_glink_smem.ko \
  rproc_qcom_common.ko qcom_pil_info.ko qcom_sysmon.ko qcom_q6v5.ko smp2p.ko \
  qcom_q6v5_pas.ko pdr_interface.ko qrtr.ko qrtr-smd.ko \
  pmic_glink.ko charger-ulog-glink.ko qti_battery_charger_main.ko \
  qpnp-pbs.ko qcom-hv-haptics.ko \
  snd_event_dlkm.ko q6_pdr_dlkm.ko q6_notifier_dlkm.ko gpr_dlkm.ko spf_core_dlkm.ko \
  audpkt_ion_dlkm.ko audio_pkt_dlkm.ko audio_prm_dlkm.ko pinctrl_lpi_dlkm.ko \
  stub_dlkm.ko swr_dlkm.ko swr_ctrl_dlkm.ko wcd_core_dlkm.ko lpass_cdc_dlkm.ko \
  lpass_cdc_va_macro_dlkm.ko lpass_cdc_wsa_macro_dlkm.ko lpass_cdc_wsa2_macro_dlkm.ko \
  lpass_cdc_rx_macro_dlkm.ko lpass_cdc_tx_macro_dlkm.ko mbhc_dlkm.ko \
  wcd9xxx_dlkm.ko wcd938x_dlkm.ko wcd938x_slave_dlkm.ko wcd937x_dlkm.ko wcd937x_slave_dlkm.ko \
  wsa881x_dlkm.ko wsa883x_dlkm.ko swr_dmic_dlkm.ko swr_haptics_dlkm.ko hdmi_dlkm.ko \
  btpower.ko slimbus.ko slim-qcom-ngd-ctrl.ko bt_fm_slim.ko q6_dlkm.ko snd-usb-audio-qmi.ko \
  aw882xx_dlkm.ko machine_dlkm.ko adsp_loader_dlkm.ko; do
  load_ko "$ko"
 done
 if $B test -x /graphics/hmos-pd-mapper; then
  $B mkdir -p /compat/bin
  $B cp /graphics/hmos-pd-mapper /compat/bin/hmos-pd-mapper
  $B chmod 0755 /compat/bin/hmos-pd-mapper
  $B setsid /compat/bin/hmos-pd-mapper --maps-dir /compat/pd-maps > "$R/data/local/tmp/marble-pd-mapper.txt" 2>&1 </dev/null &
  i=0
  while [ "$i" -lt 25 ]; do
   $B grep -q '^PD_LOCATOR_PUBLISHED=64:257:0$' "$R/data/local/tmp/marble-pd-mapper.txt" 2>/dev/null && break
   i=$((i + 1))
   $B sleep 0.1
  done
 fi
 # The DSP reads its dynamic modules from the dsp_a partition.  Mount it before
 # the ADSP boots so the module set is visible when SPF builds its registry;
 # registering modules later (from userspace) is rejected with status 9.
 $B mkdir -p "$R/dsp"
 if ! $B grep -q ' /hmos-root/dsp ' /proc/mounts 2>/dev/null; then
  $B mount -o ro /dev/block/by-name/dsp_a "$R/dsp" 2>/dev/null && \
   echo "MARBLE_DSP_FS_MOUNTED mods=$($B ls "$R/dsp/adsp" 2>/dev/null | $B wc -l)" || \
   echo "MARBLE_DSP_FS_MOUNT_FAIL"
 fi

 i=0
 while [ "$i" -lt 30 ] && [ ! -w /sys/kernel/boot_adsp/boot ]; do
  i=$((i + 1))
  $B sleep 0.1
 done
 if $B test -w /sys/kernel/boot_adsp/boot; then
  echo 1 > /sys/kernel/boot_adsp/boot 2>&1 || true
  echo "MARBLE_ADSP_BOOT_REQUESTED"
 fi
fi

# Wait briefly for async display/GPU/backlight/touch probe completion.
i=0
while [ "$i" -lt 25 ]; do
 if $B test -e /sys/class/drm/card0/dev && $B test -e /sys/class/kgsl/kgsl-3d0/dev && $B test -e /sys/class/input/event1/dev; then
  break
 fi
 i=$((i + 1))
 $B sleep 0.2
done

# Log probed hardware state clearly.
if $B test -d /sys/class/drm/card0-DSI-1; then
 echo "MARBLE_DRM_DSI1 status=$($B cat /sys/class/drm/card0-DSI-1/status 2>/dev/null) modes=$($B cat /sys/class/drm/card0-DSI-1/modes 2>/dev/null | $B tr '\n' ',')"
fi
if $B test -r /sys/class/kgsl/kgsl-3d0/gpu_model; then
 echo "MARBLE_KGSL model=$($B cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null) dev=$($B cat /sys/class/kgsl/kgsl-3d0/dev 2>/dev/null)"
fi
if $B test -r /sys/class/backlight/panel0-backlight/max_brightness; then
 echo "MARBLE_BACKLIGHT max=$($B cat /sys/class/backlight/panel0-backlight/max_brightness 2>/dev/null) cur=$($B cat /sys/class/backlight/panel0-backlight/brightness 2>/dev/null)"
fi

# 4. Install Phase 4 HDF config (allocator_host + composer_host + hmos_userauth_host).
if $B test -f /graphics/hdf_default_phase4.hcb; then
 $B cp /graphics/hdf_default_phase4.hcb "$R/system/etc/hdfconfig/hdf_default.hcb"
 $B cp /graphics/hdf_default_phase4.hcb "$R/chip_prod/etc/hdfconfig/hdf_default.hcb"
 $B chmod 0644 "$R/system/etc/hdfconfig/hdf_default.hcb" "$R/chip_prod/etc/hdfconfig/hdf_default.hcb"
 echo MARBLE_PHASE4_HCB_STAGED
fi

# 5. Prepend /compat/lib64 and /compat/bin to musl loader search configs in RAM overlay.
path_cfg="$R/system/etc/ld-musl-aarch64.path"
if $B test -f "$path_cfg" && ! $B grep -q '^/compat/lib64:' "$path_cfg"; then
 cur=$($B cat "$path_cfg")
 printf '/compat/lib64:%s\n' "$cur" > "$path_cfg.new"
 $B chmod 0644 "$path_cfg.new"
 $B mv "$path_cfg.new" "$path_cfg"
fi
for ini in \
 "$R/system/etc/ld-musl-namespace-aarch64.ini" \
 "$R/system/etc/ld-musl-namespace-aarch64-New.ini" \
 "$R/system/etc/ld-musl-namespace-aarch64-flex.ini" \
 "$R/system/etc/ld-musl-namespace-aarch64-test.ini"; do
 if $B test -f "$ini"; then
  $B sed \
   -e '/systemscence[[:space:]]*=[[:space:]]*\/compat\/bin:/! s|^\([[:space:]]*systemscence[[:space:]]*=[[:space:]]*\)|\1/compat/bin:|' \
   -e '/\.lib\.paths[[:space:]]*=[[:space:]]*\/compat\/lib64:/! s|^\([[:space:]]*namespace\.[^=]*\.lib\.paths[[:space:]]*=[[:space:]]*\)|\1/compat/lib64:|' \
   "$ini" > "$ini.new"
  $B chmod 0644 "$ini.new"
  $B mv "$ini.new" "$ini"
 fi
done
echo MARBLE_PHASE4_MUSL_PATHS_STAGED

# 6. Extract Mesa Turnip (Adreno 725 KGSL Vulkan) + Gallium-EGL/Zink into /compat/lib64.
$B gzip -dc /graphics/libvulkan_freedreno.so.gz > "$R/compat/lib64/libvulkan_freedreno.so"
$B chmod 0644 "$R/compat/lib64/libvulkan_freedreno.so"
$B gzip -dc /graphics/zink-runtime-marble.tar.gz | $B tar -xf - -C "$R/compat/lib64"
$B chmod 0644 "$R/compat/lib64/libEGL_impl.so" "$R/compat/lib64/libGLESv1_CM_impl.so.1" \
 "$R/compat/lib64/libGLESv2_impl.so.2" "$R/compat/lib64/libgallium-25.0.1.so"
$B cp /graphics/hmos-turnip.json "$R/compat/etc/vulkan/icd.d/hmos-turnip.json"
$B cp /graphics/hmos-turnip.json "$R/data/local/tmp/hmos-turnip.json"
$B chmod 0644 "$R/compat/etc/vulkan/icd.d/hmos-turnip.json" "$R/data/local/tmp/hmos-turnip.json"
echo MARBLE_PHASE4_MESA_TURNIP_ZINK_STAGED

# 7. Stage all Phase 4 compiled shared libraries and binaries.
for lib in \
 libdisplay_composer_vdi_impl.z.so \
 libdisplay_buffer_vdi_impl.z.so \
 libhmos_account_hmac.so \
 libdbkeys.so \
 libhmos_ram_no_tee.so \
 libhmos_el5_unavailable.so \
 libhmos_sceneboard_optional_sa.so \
 libhmos_wifi_category_v134.so \
 libhmos_userauth_4_0.so \
 libuser_auth_interface_service_4.1.z.so \
 libhmos_user_auth_driver.so \
 libhmos_surface_diagnostic.so \
 libhmos_boot_vulkan.so; do
 if $B test -f "/graphics/$lib"; then
  $B cp "/graphics/$lib" "$R/compat/lib64/$lib"
  $B chmod 0644 "$R/compat/lib64/$lib"
 fi
done
echo "MARBLE_COMPAT_LIB64_STAGED n=$($B ls "$R/compat/lib64" 2>/dev/null | $B wc -l)"

for bin in \
 storage-ram-launcher \
 native_render_launcher \
 native_display_wake \
 native_screenlock_request \
 native_brightness_launcher \
 native_bundle_cli \
 native_battery_hdi_publisher_v131 \
 native_sa_check \
 native_sa_dump \
 native_input_dispatch \
 bootanimation-launcher \
 hmos_display_scanout \
 hmos_kms_diagnostic \
 capture_scanout \
 hmos-pd-mapper \
 hmos_hisysevent_sink \
 hmos_brightness_persist \
 start-location-marble.sh \
 start-theme-marble.sh \
 start-bluetooth-marble.sh \
 start-usb-marble.sh \
 stop-usb-marble.sh \
 hdc-usb-permit.sh \
 start-codec-marble.sh \
 start-modem-marble.sh \
 marble_verify_all.sh \
 marble_identity.sh \
 qmi_probe \
 native_codec_probe \
 hmos_bt_bridge_marble \
 hmos_bt_hdi_publisher_marble \
 tee_backend_probe \
 start-wifi-marble.sh; do
 if $B test -f "/graphics/$bin"; then
  $B cp "/graphics/$bin" "$R/compat/bin/$bin"
  $B chmod 0755 "$R/compat/bin/$bin"
 fi
done

if $B test -x "$R/system/bin/bootanimation"; then
 $B cp "$R/system/bin/bootanimation" "$R/compat/bin/bootanimation-real"
 $B chmod 0755 "$R/compat/bin/bootanimation-real"
fi
if $B test -x "$R/system/bin/uinput"; then
 $B cp "$R/system/bin/uinput" "$R/compat/bin/uinput-original"
 $B chmod 0755 "$R/compat/bin/uinput-original"
fi
echo MARBLE_PHASE4_COMPAT_BINS_STAGED

# 8. Stage init imports, profiles, sandbox mounts, preload policy, distributed_file fallback.
for cfg in \
 init-desktop-marble.cfg \
 init-platform-marble.cfg \
 init-hardware-marble.cfg \
 init-wifi-marble.cfg \
 init-location-marble.cfg \
 init-theme-marble.cfg \
 init-bluetooth-marble.cfg \
 init-usb-marble.cfg \
 init-modem-marble.cfg \
 useriam-core.json \
 powermgr.json; do
 $B cp "/graphics/$cfg" "$R/compat/etc/$cfg"

# Subsystem libraries and the locationhub profile staged by the same payload.
for lib in libbt_vendor.z.so libhmos_bt_bridge_marble.so \
 libhuks_engine_core_standard.z.so libhuks_hdi_service_1.1.z.so; do
 if $B test -f "/graphics/$lib"; then
  $B cp "/graphics/$lib" "$R/compat/lib64/$lib"
  $B chmod 0644 "$R/compat/lib64/$lib"
 fi
done
if $B test -f /graphics/locationhub-marble.json; then
 $B cp /graphics/locationhub-marble.json "$R/compat/etc/locationhub-marble.json"
 $B chmod 0644 "$R/compat/etc/locationhub-marble.json"
fi
echo "MARBLE_SUBSYS_STAGED"
 $B chmod 0644 "$R/compat/etc/$cfg"
done

# Adapt display DPI (440) and centered punch-hole cutout for marble (1080x2400, 6.67").
if $B test -f /graphics/display_manager_config.xml && $B test -f "$R/sys_prod/etc/window/resources/display_manager_config.xml"; then
 $B cp /graphics/display_manager_config.xml "$R/compat/etc/display_manager_config.xml"
 $B chmod 0644 "$R/compat/etc/display_manager_config.xml"
 bind_once "$R/compat/etc/display_manager_config.xml" "$R/sys_prod/etc/window/resources/display_manager_config.xml"
 echo MARBLE_DISPLAY_MANAGER_CONFIG_STAGED
fi

# Extend default screen_off_timeout from 30s to 120s in RAM overlay.
ds_src="$R/system/variant/phone/base/etc/settings_data/default_settings.json"
if $B test -f "$ds_src"; then
 $B awk '
  /"settings\.display\.screen_off_timeout"/ { seen=1 }
  seen && /"value"[[:space:]]*:[[:space:]]*"30000"/ { sub(/"30000"/, "\"120000\""); seen=0 }
  { print }
 ' "$ds_src" > "$R/compat/etc/default_settings.json"
 $B chmod 0644 "$R/compat/etc/default_settings.json"
 bind_once "$R/compat/etc/default_settings.json" "$ds_src"
# Auto-brightness keeps re-deriving the same level every boot and discards the
# manual slider position, so the saved value never wins. Force manual mode and
# let hmos_brightness_persist own the level.
$B sed -i 's/"settings\.display\.auto_screen_brightness", '\''value'\'': '\''1'\''/"settings.display.auto_screen_brightness", '\''value'\'': '\''0'\''/' "$R/compat/etc/default_settings.json" 2>/dev/null || true
echo "MARBLE_AUTO_BRIGHTNESS_DISABLED=$($B grep -c 'auto_screen_brightness.*0' "$R/compat/etc/default_settings.json" 2>/dev/null)"
 echo MARBLE_DEFAULT_SETTINGS_STAGED
fi

# Allowlist of absent optional SAs so SceneBoard and apps do not stall their UI thread
# in synchronous 1.2s-5.0s retry/load timeouts (THREAD_BLOCK_3S/6S).
# Real Wi-Fi (1120/1124/1126), Netstack (1151/1158), and Audio/Media (3001/3002/3009/3010/3011) are enabled.
printf '%s\n' '4005 1903 1905 1907 2802 3008 3203 3601 1130 65862 65926 65962 65974 66048 66106 66134 66250 66288 70633' > "$R/compat/etc/sceneboard-optional-sa.list"
$B chmod 0644 "$R/compat/etc/sceneboard-optional-sa.list"

# Stage donor chip_prod audio policy configs for audio_server (SA 3001/3009).
$B mkdir -p "$R/chip_prod/etc/audio"
for acfg in audio_policy_configuration.xml audio_cust_config.xml; do
 if $B test -f "/graphics/$acfg"; then
  $B cp "/graphics/$acfg" "$R/chip_prod/etc/audio/$acfg"
  $B chmod 0644 "$R/chip_prod/etc/audio/$acfg"
 fi
done

# Offline OOBE page policy (skips wlan, activation_lock, account_login during offline bring-up).
if $B test -f /graphics/oobe_page_offline.json && $B test -d "$R/sys_prod/etc/oobe"; then
 $B mkdir -p "$R/compat/oobe-offline-v1"
 if $B test -f "$R/sys_prod/etc/oobe/hwupgrade_extend_pages.json"; then
  $B cp "$R/sys_prod/etc/oobe/hwupgrade_extend_pages.json" "$R/compat/oobe-offline-v1/hwupgrade_extend_pages.json"
 fi
 $B cp /graphics/oobe_page_offline.json "$R/compat/oobe-offline-v1/oobe_page.json"
 $B chmod 0755 "$R/compat/oobe-offline-v1"
 $B chmod 0644 "$R/compat/oobe-offline-v1"/*.json
 bind_once "$R/compat/oobe-offline-v1" "$R/sys_prod/etc/oobe"
fi

$B cp /graphics/foundation-app-core.json "$R/system/profile/foundation.json"
$B cp /graphics/installs-debug-start.json "$R/system/profile/installs.json"
$B cp /graphics/msdp-input-only.json "$R/system/profile/msdp.json"
$B cp /graphics/pasteboard-service-eager.json "$R/system/profile/pasteboard_service.json"
for prof in i18n_service.json service_router.json download_server.json deviceinfoservice.json device_manager.json; do
 if $B test -f "/graphics/$prof"; then
  $B cp "/graphics/$prof" "$R/system/profile/$prof"
  $B chmod 0644 "$R/system/profile/$prof"
 fi
done
if $B test -f /graphics/wifi-manager-eager.json; then
 $B cp /graphics/wifi-manager-eager.json "$R/system/profile/wifi_manager_service.json"
 $B chmod 0644 "$R/system/profile/wifi_manager_service.json"
fi
$B chmod 0644 "$R/system/profile/foundation.json" "$R/system/profile/installs.json" \
 "$R/system/profile/msdp.json" "$R/system/profile/pasteboard_service.json"

# Bind-mount /compat-enabled appdata-sandbox configs over RO /system/etc/sandbox/.
if $B test -f /graphics/appdata-sandbox-marble.json; then
 $B cp /graphics/appdata-sandbox-marble.json "$R/compat/etc/sandbox/appdata-sandbox.json"
 $B chmod 0644 "$R/compat/etc/sandbox/appdata-sandbox.json"
 bind_once "$R/compat/etc/sandbox/appdata-sandbox.json" "$R/system/etc/sandbox/appdata-sandbox.json"
fi
if $B test -f /graphics/appdata-sandbox-isolated-marble.json; then
 $B cp /graphics/appdata-sandbox-isolated-marble.json "$R/compat/etc/sandbox/appdata-sandbox-isolated.json"
 $B chmod 0644 "$R/compat/etc/sandbox/appdata-sandbox-isolated.json"
 bind_once "$R/compat/etc/sandbox/appdata-sandbox-isolated.json" "$R/system/etc/sandbox/appdata-sandbox-isolated.json"
fi

# Native StorageDaemon fallback without HMDFS + /storage/media directory.
df_src="$R/system/etc/param/distributed_file.para"
if $B test -f "$df_src" && ! $B grep -qw hmdfs /proc/filesystems; then
 $B sed 's/^const\.distributed_file_property\.enabled=true$/const.distributed_file_property.enabled=false/' "$df_src" > "$R/compat/etc/distributed_file.para"
 $B chmod 0644 "$R/compat/etc/distributed_file.para"
 bind_once "$R/compat/etc/distributed_file.para" "$df_src"
fi
$B mkdir -p "$R/storage/media"
$B chown 0:0 "$R/storage/media"
$B chmod 0711 "$R/storage/media"

# Preload policy root for ADY-AL10 (/preload/ADY-AL10/all/cn).
bl_src="$R/system/etc/cust/baselayer.cfg"
if $B test -f "$bl_src" && $B test -d "$R/preload/ADY-AL10/all/cn"; then
 $B cp "$bl_src" "$R/compat/etc/cust/baselayer.cfg"
 printf '\n# marble compatibility: mounted ADY-AL10 preload policy\n/preload/ADY-AL10/all/cn\n' >> "$R/compat/etc/cust/baselayer.cfg"
 $B chmod 0644 "$R/compat/etc/cust/baselayer.cfg"
 bind_once "$R/compat/etc/cust/baselayer.cfg" "$bl_src"
fi

# Prepare .hmos-dbkeys/<uid> directories for libdbkeys.so.
$B mkdir -p "$R/data/service/el1/public/database/.hmos-dbkeys"
$B chown 0:0 "$R/data/service/el1/public/database/.hmos-dbkeys"
$B chmod 0711 "$R/data/service/el1/public/database/.hmos-dbkeys"
if $B test -f "$R/system/etc/passwd"; then
 while IFS=: read -r _name _pass uid gid _rest; do
  case "$uid" in ''|*[!0-9]*) continue;; esac
  [ "$uid" -lt 10000 ] || continue
  p="$R/data/service/el1/public/database/.hmos-dbkeys/$uid"
  $B mkdir -p "$p"
  $B chown "$uid:$gid" "$p"
  $B chmod 0700 "$p"
 done < "$R/system/etc/passwd"
fi

# Disable donor Kirin bbox/bootfail 180s reboot watchdog plugins in RAM overlay.
$B rm -f \
 "$R/system/lib64/init/libinit_bootDetector.z.so" \
 "$R/system/lib64/init/autorun/libcheckpoint.z.so" \
 "$R/system/etc/init/boot_detector.cfg"

# 9. Apply the selected SHA-256-guarded ADY-AL10 donor patches in RAM.
$B sh /graphics/apply-donor-patches-phase4.sh

# 10. Expose required hardware device nodes inside /hmos-root/dev/ with exact OHOS UID/GIDs.
mk_char_sys() {
 sys="$1"; target="$2"; uid="$3"; gid="$4"; mode="$5"
 [ -r "$sys/dev" ] || return 0
 nums=$($B cat "$sys/dev")
 parent=${target%/*}
 $B mkdir -p "$parent"
 # An existing node may carry a stale major:minor (misc devices get dynamic
 # minors), so compare against sysfs instead of trusting the path.
 want=$($B printf '%x:%x' "${nums%:*}" "${nums#*:}")
 if [ -e "$target" ]; then
  have=$($B stat -c '%t:%T' "$target" 2>/dev/null)
  if [ "$have" != "$want" ]; then
   $B rm -f "$target"
   $B mknod "$target" c "${nums%:*}" "${nums#*:}"
  fi
 else
  $B mknod "$target" c "${nums%:*}" "${nums#*:}"
 fi
 $B chown "$uid:$gid" "$target"
 $B chmod "$mode" "$target"
}

mk_char_sys /sys/class/drm/card0 "$R/dev/dri/card0" 0 1003 0660
mk_char_sys /sys/class/drm/renderD128 "$R/dev/dri/renderD128" 0 1003 0660
mk_char_sys /sys/class/dma_heap/system "$R/dev/dma_heap/system" 0 3070 0660
mk_char_sys /sys/class/kgsl/kgsl-3d0 "$R/dev/kgsl-3d0" 0 1003 0666
mk_char_sys /sys/class/misc/ashmem "$R/dev/ashmem" 0 0 0666
# storage_daemon opens /dev/fuse in the native root before mounting media.
# This bring-up has no devtmpfs there; only the root storage daemon needs it.
mk_char_sys /sys/class/misc/fuse "$R/dev/fuse" 0 0 0600
mk_char_sys /sys/class/misc/hmos_dmabuf_sync "$R/dev/hmos_dmabuf_sync" 0 1003 0666
mk_char_sys /sys/class/misc/rfkill "$R/dev/rfkill" 0 1010 0660
mk_char_sys /sys/class/misc/tun "$R/dev/net/tun" 0 3074 0660

$B mkdir -p "$R/dev/input"
$B chmod 0755 "$R/dev/input"
for ev in /sys/class/input/event*; do
 [ -r "$ev/dev" ] || continue
 base=${ev##*/}
 mk_char_sys "$ev" "$R/dev/input/$base" 6696 6696 0660
 ev_name=$($B cat "$ev/device/name" 2>/dev/null || echo unknown)
 echo "MARBLE_INPUT_NODE $base name=$ev_name"
done

if $B test -e /sys/class/backlight/panel0-backlight/brightness; then
 $B chown 0:3036 /sys/class/backlight/panel0-backlight/brightness
 $B chmod 0660 /sys/class/backlight/panel0-backlight/brightness
fi

# 11. Pre-create service diagnostic logs & runtime markers with exact service ownership.
init_log() {
 : > "$1"
 $B chown "$2" "$1"
 $B chmod "$3" "$1"
}
init_log "$R/data/local/tmp/hmos-render-stderr.txt" 1003:1003 0600
init_log "$R/data/local/tmp/hmos-mesa.txt" 1003:1003 0600
init_log "$R/data/local/tmp/hmos-graphics/bootanimation-adapter.log" 1003:1003 0600
init_log "$R/data/local/tmp/hmos-composer-metrics.txt" 3036:3036 0640
init_log "$R/data/local/tmp/hmos-brightness-stderr.txt" 5528:5528 0600
init_log "$R/data/local/tmp/hmos-battery-hdi.txt" 3025:3025 0600
init_log "$R/data/local/tmp/hmos-wifi-category-v134.txt" 3026:3026 0600
init_log "$R/data/local/tmp/hmos-input-last.log" 2000:2000 0600

# 11b. PAL/AGM runtime for the vendor audio stack (Bionic loader + vendor libs).
# Without it audio_server waits 5000ms for IAudioPolicy and volume keys stay dead.
ART_SRC=/graphics/audio-runtime
ART="$R/compat/audio-runtime"
if skipped audio; then echo "MARBLE_STAGE_SKIPPED=audio"; elif $B test -x "$ART_SRC/bin/linker64"; then
 $B mkdir -p "$ART/bin" "$ART/lib64"
 for f in "$ART_SRC"/bin/* "$ART_SRC"/lib64/*; do
  [ -f "$f" ] || continue
  rel=${f#"$ART_SRC"/}
  $B cp "$f" "$ART/$rel"
  $B chmod 0755 "$ART/$rel"
 done
 echo "MARBLE_AUDIO_RUNTIME_STAGED libs=$($B ls "$ART/lib64" | $B wc -l)"
fi

# 11c. /dev/snd nodes.  The card exposes 42 PCM devices (playback *and*
# capture: pcmC0D6c..D11c, D14c, ...); creating only the playback ones left the
# recorder with no capture device at all, so enumerate whatever the card has.
$B mkdir -p "$R/dev/snd"
mk_char_sys /sys/class/sound/controlC0 "$R/dev/snd/controlC0" 0 1041 0660
for snd in /sys/class/sound/pcmC0D*; do
 [ -r "$snd/dev" ] || continue
 mk_char_sys "$snd" "$R/dev/snd/${snd##*/}" 0 1041 0660
done
for adev in aud_pasthru_adsp aud_pasthru_modem msm_audio_ion gpr_channel; do
 mk_char_sys "/sys/class/$adev/$adev" "$R/dev/$adev" 0 1041 0660
done
# libar-gpr opens /dev/gpr_channel; report whether this kernel even exposes it.
echo "MARBLE_GPR_NODE=$([ -e "$R/dev/gpr_channel" ] && echo ok || echo missing)"
echo "MARBLE_AUDIO_DEVS total=$($B ls "$R/dev/snd" 2>/dev/null | $B wc -l) capture=$($B ls "$R/dev/snd" 2>/dev/null | $B grep -c 'c$')"

# 11d. PAL/AGM read /vendor/etc/{audio,acdbdata,kvh2xml.xml,...}.  /vendor is a
# read-only erofs mount, so a tmpfs upper is mounted over /vendor/etc and the
# stock files are copied in (overlayfs rejects an upper dir that is itself on an
# overlay, hence the tmpfs).
VND_SRC=/graphics/audio-marble
if skipped audio; then echo "MARBLE_STAGE_SKIPPED=audio-overlay"; elif $B test -d "$VND_SRC/etc" && ! $B grep -q " /hmos-root/vendor/etc " /proc/mounts 2>/dev/null; then
 OVL=/tmp/vendor-etc-ovl
 $B mkdir -p "$OVL"
 if $B mount -t tmpfs tmpfs "$OVL" 2>/dev/null &&
    $B mkdir -p "$OVL/upper" "$OVL/work" &&
    $B mount -t overlay overlay -o "lowerdir=$R/vendor/etc,upperdir=$OVL/upper,workdir=$OVL/work" "$R/vendor/etc" 2>/dev/null; then
  echo "MARBLE_VENDOR_ETC_MOUNTED"
  $B cp -a "$VND_SRC/etc/." "$R/vendor/etc/" 2>/dev/null || true
  # libagm's graph_init() hardcodes the donor board name and opens
  # /vendor/etc/acdbdata/Forte, returning EINVAL when it is missing ("session_obj_init Error:22").
  $B mkdir -p "$R/vendor/etc/acdbdata/Forte" "$R/data/vendor/audio/acdbdata/delta"
  # Two candidate ACDB sets: marble's own (ukee_mtp) or the vendor's Forte set.
  # `touch /data/local/tmp/marble-acdb-forte` selects the second for one boot.
  if [ -f "$R/data/local/tmp/marble-acdb-forte" ] && [ -f /graphics/Forte_acdb_cal.acdb ]; then
   $B cp /graphics/Forte_acdb_cal.acdb "$R/vendor/etc/acdbdata/Forte/Forte_acdb_cal.acdb" 2>/dev/null || true
   $B cp /graphics/Forte_workspaceFileXml.qwsp "$R/vendor/etc/acdbdata/Forte/" 2>/dev/null || true
   echo "MARBLE_ACDB_SET=forte"
  else
   $B cp -a "$R/vendor/etc/acdbdata/ukee_mtp/." "$R/vendor/etc/acdbdata/Forte/" 2>/dev/null || true
   echo "MARBLE_ACDB_SET=ukee_mtp"
  fi
  $B cp -a "$R/vendor/etc/acdbdata/ukee_mtp/." "$R/data/vendor/audio/acdbdata/" 2>/dev/null || true
  echo "MARBLE_VENDOR_ETC_OVERLAY xml=$($B ls "$R/vendor/etc/audio/sku_ukee" 2>/dev/null | $B wc -l) forte=$($B ls "$R/vendor/etc/acdbdata/Forte" 2>/dev/null | $B wc -l) kv=$($B ls "$R/vendor/etc" 2>/dev/null | $B grep -c kvh2xml)"
 else
  echo "MARBLE_VENDOR_ETC_OVERLAY_FAIL"
 fi
else
 echo "MARBLE_AUDIO_VENDOR_SRC_MISSING"
fi

# 11c2. The subsystem scripts use "#!/busybox sh"; inside the chroot that path
# must exist, otherwise init/exec returns 127 and nothing runs.
if [ ! -x "$R/busybox" ]; then
 $B cp /busybox "$R/busybox" 2>/dev/null && $B chmod 0755 "$R/busybox"
fi
echo "MARBLE_CHROOT_BUSYBOX=$([ -x "$R/busybox" ] && echo ok || echo missing)"

# 11d2. Bluetooth: the H4 bridge reads the rampatch/NVM from bluetooth_a and
# needs the tty/power char nodes that PID 1's ueventd never created.
BT_PART=/dev/block/by-name/bluetooth_a
if skipped bluetooth; then echo "MARBLE_STAGE_SKIPPED=bluetooth"; elif ! $B grep -q ' /hmos-root/bt_firmware ' /proc/mounts 2>/dev/null; then
 $B mkdir -p "$R/bt_firmware"
 $B mount -o ro "$BT_PART" "$R/bt_firmware" 2>/dev/null && \
  echo "MARBLE_BT_FIRMWARE_MOUNTED files=$($B ls "$R/bt_firmware/image" 2>/dev/null | $B grep -c hpbtfw)" || \
  echo "MARBLE_BT_FIRMWARE_MOUNT_FAIL"
fi
mk_char_sys /sys/class/tty/ttyHS0 "$R/dev/ttyHS0" 0 1002 0660
mk_char_sys /sys/class/bt-dev/btpower "$R/dev/btpower" 0 1002 0660
echo "MARBLE_BT_NODES tty=$($B test -e "$R/dev/ttyHS0" && echo ok || echo missing) power=$($B test -e "$R/dev/btpower" && echo ok || echo missing)"

# 11e. hisysevent sink.  Clients (samgr and every app) write to the socket
# inside the chroot, so the sink must run *in* the chroot: a host-path launch
# cannot resolve the musl interpreter and dies with rc=127.
if [ -x "$R/compat/bin/hmos_hisysevent_sink" ]; then
 $B mkdir -p "$R/dev/unix/socket" 2>/dev/null || true
 $B setsid $B chroot "$R" /compat/bin/hmos_hisysevent_sink -q \
  --socket-dir /dev/unix/socket --stats /data/local/tmp/hisysevent-sink.log \
  > "$R/data/local/tmp/hisysevent-sink.out" 2>&1 </dev/null &
 $B sleep 1
 echo "MARBLE_HISYSEVENT_SINK_STARTED pid=$($B pidof hmos_hisysevent_sink 2>/dev/null || echo none) sockets=$($B ls "$R/dev/unix/socket" 2>/dev/null | $B grep -c hisysevent)"
else
 echo "MARBLE_HISYSEVENT_SINK_MISSING"
fi

# 11f. Brightness watcher: records every slider change so it survives reboot.
if [ -x "$R/compat/bin/hmos_brightness_persist" ]; then
 $B setsid $B chroot "$R" /compat/bin/hmos_brightness_persist watch \
  > "$R/data/local/tmp/brightness-persist.log" 2>&1 </dev/null &
 echo "MARBLE_BRIGHTNESS_WATCH_STARTED pid=$($B pidof hmos_brightness_persist 2>/dev/null || echo none)"
fi

init_log "$R/data/local/tmp/marble-audio.log" 0:0 0600
$B chown 2000:2000 "$R/data/local/tmp/hmos-capture"
$B chmod 0755 "$R/data/local/tmp/hmos-capture"
# snapshot_display accepts only canonical /data/local/tmp paths. Bind a
# fresh tmpfs directory there so earlier snapshots on userdata stay untouched.
shot_ram="$R/dev/hmos-phase4-capture"
shot_path="$R/data/local/tmp/hmos-phase4-capture"
if [ ! -L "$shot_path" ] && $B mkdir -p "$shot_ram" "$shot_path" &&
   $B chown 2000:2000 "$shot_ram" && $B chmod 0750 "$shot_ram" &&
   $B mount --bind "$shot_ram" "$shot_path"; then
 echo MARBLE_PHASE4_CAPTURE_RAM_READY
else
 echo MARBLE_PHASE4_CAPTURE_RAM_UNAVAILABLE >&2
fi
init_log "$R/data/local/tmp/hmos-capture/unlock-request.txt" 2000:2000 0644
$B touch "$R/data/local/tmp/hmos-gpu-owner-cleanup"
$B chmod 0644 "$R/data/local/tmp/hmos-gpu-owner-cleanup"
$B mkdir -p "$R/data/service/el0/stats" "$R/data/service/el0/thermal/config"
$B chown -R 5528:5528 "$R/data/service/el0/stats" "$R/data/service/el0/thermal"

# The bootstrap installs /hmos-init-debug.cfg as /system/etc/init.cfg after
# this script returns. Do not stage a second competing init configuration.
$B test -x "$R/compat/bin/init"
echo MARBLE_PHASE4_RAM_COMPAT_READY
