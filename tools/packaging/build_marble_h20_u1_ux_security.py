#!/usr/bin/env python3
"""Package Batch U1 (Soft-HUKS SA 3510, HDR screenshot freeze fix, OEM/version/CPU/OUC, security/permission/cloud SAs, Themes, Live Window) onto verified F5 vendor_boot_a."""
import argparse
import copy
import importlib.util
import json
import subprocess
import tempfile
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / 'work'
U1_BUILD = WORK / 'marble-u1-build'
HUKS_ARM64 = WORK / 'marble-huks-build/arm64'
INIT_DIR = ROOT / 'devices/marble/runtime/init'

BASE_DEFAULT = Path('/mnt/e/marble-tmp/vendor_boot_h20_f5_cellular_gnss_34b842d0.img')
BASE_SHA = '34b842d0821cca2187f308db222bff5e07e5c3ba3002c0b58966cfec044ca6d9'
ORACLE = WORK / 'marble-m15-role-storage.1v2ynS/build_m15_role_storage.py'
ORACLE_SHA = '7d4f79857a3458ffdb7c8fb86b1cfd893fd9addb02b13e7e05d7b5fa7093cfaa'

INIT_DEBUG_MEMBER = 'hmos-compat-marble/hmos-init-debug.cfg'
INIT_B2_MEMBER = 'hmos-compat-marble/graphics/init-b2-apps-marble.cfg'
INIT_DESKTOP_MEMBER = 'hmos-compat-marble/graphics/init-desktop-marble.cfg'
START_PHASE4_MEMBER = 'hmos-compat-marble/graphics/start-phase4-marble.sh'
STAGE_MEMBER = 'hmos-compat-marble/graphics/stage-phase4-marble.sh'

# Core protected members that U1 must keep byte-for-byte identical to F5 base.
GUARD_MEMBERS = (
    'hmos-compat-marble/graphics/libhmos_audio_vdi.so',
    'hmos-compat-marble/graphics/audio-runtime/lib64/libhmos_pal_server.so',
    'hmos-compat-marble/graphics/init-bluetooth-marble.cfg',
    'hmos-compat-marble/graphics/init-d-sensors-marble.cfg',
    'hmos-compat-marble/graphics/hmos_tqftpserv',
    'hmos-compat-marble/graphics/marble_modem_probe',
    'hmos-compat-marble/graphics/native_gnss_hdi_publisher',
    'hmos-compat-marble/graphics/libril_vendor_qmi.z.so',
    'hmos-compat-marble/graphics/start-modem-f5-marble.sh',
    'hmos-compat-marble/graphics/hmos_rmtfs',
    'hmos-compat-marble/graphics/native_ril_hdi_loader',
    'hmos-compat-marble/graphics/hdf_default_phase4.hcb',
    'hmos-compat-marble/graphics/telephony-marble.json',
)

# New or replaced ramdisk members under hmos-compat-marble/.
STAGED_FILES = (
    ('graphics/libhmos_sceneboard_optional_sa.so', U1_BUILD / 'libhmos_sceneboard_optional_sa.so', 0o644, True),
    ('graphics/native_bundle_cli', U1_BUILD / 'native_bundle_cli', 0o755, True),
    ('graphics/libdbkeys.so', U1_BUILD / 'libdbkeys.so', 0o644, True),
    ('graphics/libhuks_hdi_service_1.1.z.so', HUKS_ARM64 / 'libhuks_hdi_service_1.1.z.so', 0o644, False),
    ('graphics/libhuks_engine_core_standard.z.so', HUKS_ARM64 / 'libhuks_engine_core_standard.z.so', 0o644, False),
    ('graphics/tee_backend_probe', HUKS_ARM64 / 'tee_backend_probe', 0o755, False),
    ('graphics/init-u1-services-marble.cfg', INIT_DIR / 'init-u1-services-marble.cfg', 0o644, False),
    ('graphics/u1-cust-version.tar.gz', U1_BUILD / 'u1-cust-version.tar.gz', 0o644, False),
)

OPTIONAL_SA_F5 = b'4005 4016 1903 1905 1907 3008 3203 4300 5204 6001 8250 302 3506 3526 8003 10120 65862 65866 65936 65952 65962 65974 65993 66006 66048 66106 66134 66208 66234 66250 66288 66358 66532 66588 70633'
# Remove running / newly enabled SAs from optional-SA skip list:
# 3203 (Live Window/ans), 3506 (security_component),
# 65974 (appgallery_service), 65993 (security_privacy_server), 66134 (video_processing_service).
# Keep 65866 (dcm_service) skipped (requires Huawei hardware TEE), while whiteout'ing its 1.2 HDI lib.
OPTIONAL_SA_U1 = b'4005 4016 1903 1905 1907 3006 3008 4300 5204 6001 8250 302 3526 8003 10120 65862 65866 65920 65930 65936 65952 65962 66006 66048 66106 66208 66234 66250 66288 66358 66406 66532 66588 70633 70634'

STAGE_ANCHOR = b'$B test -x "$R/compat/bin/init"\necho MARBLE_PHASE4_RAM_COMPAT_READY'

STAGE_U1_BLOCK = (
    b'# === MARBLE_U1_UX_SECURITY_STAGE_BEGIN ===\n'
    b'if $B test -f /graphics/init-u1-services-marble.cfg; then\n'
    b' $B cp /graphics/init-u1-services-marble.cfg "$R/compat/etc/init-u1-services-marble.cfg"\n'
    b' $B chmod 0644 "$R/compat/etc/init-u1-services-marble.cfg"\n'
    b'fi\n'
    b'for u1_lib in libhuks_hdi_service_1.1.z.so libhuks_engine_core_standard.z.so libhmos_sceneboard_optional_sa.so; do\n'
    b' if $B test -f "/graphics/$u1_lib"; then\n'
    b'  $B cp "/graphics/$u1_lib" "$R/compat/lib64/$u1_lib"\n'
    b'  $B chmod 0644 "$R/compat/lib64/$u1_lib"\n'
    b' fi\n'
    b'done\n'
    b'if $B test -f /graphics/tee_backend_probe; then\n'
    b' $B cp /graphics/tee_backend_probe "$R/compat/bin/tee_backend_probe"\n'
    b' $B chmod 0755 "$R/compat/bin/tee_backend_probe"\n'
    b'fi\n'
    b'if $B test -f /graphics/native_bundle_cli; then\n'
    b' $B cp /graphics/native_bundle_cli "$R/compat/bin/native_bundle_cli"\n'
    b' $B chmod 0755 "$R/compat/bin/native_bundle_cli"\n'
    b'fi\n'
    b'pt_ovl=fail\n'
    b'if $B test -d "$R/vendor/lib64/passthrough" && ! $B grep -q " /hmos-root/vendor/lib64/passthrough " /proc/mounts 2>/dev/null; then\n'
    b' PT_OVL=/tmp/vendor-pt-ovl\n'
    b' $B mkdir -p "$PT_OVL"\n'
    b' if $B mount -t tmpfs tmpfs "$PT_OVL" 2>/dev/null &&\n'
    b'    $B mkdir -p "$PT_OVL/upper" "$PT_OVL/work" &&\n'
    b'    $B mount -t overlay overlay -o "lowerdir=$R/vendor/lib64/passthrough,upperdir=$PT_OVL/upper,workdir=$PT_OVL/work" "$R/vendor/lib64/passthrough" 2>/dev/null; then\n'
    b'  $B rm -f "$R/vendor/lib64/passthrough/libhuks_hdi_service_1.2.z.so" "$R/vendor/lib64/passthrough/libhuks_ext_hdi_service_1.1.z.so" "$R/vendor/lib64/passthrough/libdevice_certificate_manager_interfaces_service_1.2.z.so"\n'
    b'  $B cp /graphics/libhuks_hdi_service_1.1.z.so "$R/vendor/lib64/passthrough/libhuks_hdi_service_1.1.z.so"\n'
    b'  $B chmod 0644 "$R/vendor/lib64/passthrough/libhuks_hdi_service_1.1.z.so"\n'
    b'  pt_ovl=overlay\n'
    b' fi\n'
    b'fi\n'
    b'if [ "$pt_ovl" = fail ] && $B test -f "$R/vendor/lib64/passthrough/libhuks_hdi_service_1.2.z.so"; then\n'
    b' $B dd if=/dev/zero of="$R/compat/lib64/huks-1.2-disabled.bin" bs=4096 count=1 2>/dev/null || true\n'
    b' $B chmod 0644 "$R/compat/lib64/huks-1.2-disabled.bin" 2>/dev/null || true\n'
    b' for u1_dis in libhuks_hdi_service_1.2.z.so libhuks_ext_hdi_service_1.1.z.so libdevice_certificate_manager_interfaces_service_1.2.z.so; do\n'
    b'  if $B test -f "$R/vendor/lib64/passthrough/$u1_dis"; then\n'
    b'   $B mount --bind "$R/compat/lib64/huks-1.2-disabled.bin" "$R/vendor/lib64/passthrough/$u1_dis" 2>/dev/null || true\n'
    b'  fi\n'
    b' done\n'
    b' pt_ovl=bind_fallback\n'
    b'fi\n'
    b'$B mkdir -p "$R/chip_prod/lib64/passthrough"\n'
    b'$B cp /graphics/libhuks_hdi_service_1.1.z.so "$R/chip_prod/lib64/passthrough/libhuks_hdi_service_1.1.z.so"\n'
    b'$B chmod 0644 "$R/chip_prod/lib64/passthrough/libhuks_hdi_service_1.1.z.so"\n'
    b'$B mkdir -p "$R/data/service/el1/public/huks_soft"\n'
    b'$B chown 3510:3510 "$R/data/service/el1/public/huks_soft"\n'
    b'$B chmod 0700 "$R/data/service/el1/public/huks_soft"\n'
    b'for u1_hd in "$R/data/service/el1/public/huks_service" "$R/data/data/huks_service" "$R"/data/service/el2/*/huks_service "$R"/data/service/el4/*/huks_service; do\n'
    b' if $B test -d "$u1_hd"; then\n'
    b'  $B find "$u1_hd" -type f -size 20c 2>/dev/null | while IFS= read -r u1_kf; do\n'
    b'   if [ "$($B od -An -tx1 -N4 "$u1_kf" 2>/dev/null | $B tr -d " \\n")" = "53484b31" ]; then\n'
    b'    $B rm -f "$u1_kf"\n'
    b'   fi\n'
    b'  done\n'
    b' fi\n'
    b'done\n'
    b'u1_clk=ok\n'
    b'u1_now=$($B date +%s 2>/dev/null || echo 0)\n'
    b'case "$u1_now" in \'\'|*[!0-9]*) u1_now=0;; esac\n'
    b'if [ "$u1_now" -lt 1767225600 ]; then\n'
    b' u1_seed=1795478400\n'
    b' for u1_tf in "$R"/data/app/el1/0/database/com.ohos.settingsdata/entry/rdb/* "$R"/data/service/el1/public/access_token/*; do\n'
    b'  $B test -f "$u1_tf" || continue\n'
    b'  u1_mt=$($B stat -c \'%Y\' "$u1_tf" 2>/dev/null || echo 0)\n'
    b'  case "$u1_mt" in \'\'|*[!0-9]*) continue;; esac\n'
    b'  if [ "$u1_mt" -gt "$u1_seed" ] && [ "$u1_mt" -lt 2208988800 ]; then u1_seed="$u1_mt"; fi\n'
    b' done\n'
    b' if $B date -s "@$u1_seed" >/dev/null 2>&1 || $B date -u -s "2026-11-25 08:00:00" >/dev/null 2>&1; then\n'
    b'  u1_clk="seeded:$u1_seed"\n'
    b' else\n'
    b'  u1_clk="seed_err:$u1_seed"\n'
    b' fi\n'
    b'fi\n'
    b'u1_dbk_n=0\n'
    b'$B mkdir -p "$R/data/service/el1/public/database/.hmos-dbkeys"\n'
    b'$B chown 0:0 "$R/data/service/el1/public/database/.hmos-dbkeys"\n'
    b'$B chmod 0711 "$R/data/service/el1/public/database/.hmos-dbkeys"\n'
    b'$B mkdir -p "$R/data/service/el1/public/database/.hmos-dbkeys/7023"\n'
    b'$B chown 7023:7023 "$R/data/service/el1/public/database/.hmos-dbkeys/7023" 2>/dev/null || true\n'
    b'$B chmod 0700 "$R/data/service/el1/public/database/.hmos-dbkeys/7023" 2>/dev/null || true\n'
    b'for u1_d in "$R"/data/app/el1/0/base/* "$R"/data/app/el1/0/database/*; do\n'
    b' $B test -d "$u1_d" || continue\n'
    b' u1_ug=$($B stat -c \'%u:%g\' "$u1_d" 2>/dev/null || true)\n'
    b' u1_u="${u1_ug%%:*}"\n'
    b' case "$u1_u" in \'\'|*[!0-9]*) continue;; esac\n'
    b' if [ "$u1_u" -ge 10000 ] && [ "$u1_u" -lt 20000000 ]; then\n'
    b'  u1_kp="$R/data/service/el1/public/database/.hmos-dbkeys/$u1_u"\n'
    b'  if ! $B test -d "$u1_kp"; then u1_dbk_n=$((u1_dbk_n + 1)); fi\n'
    b'  $B mkdir -p "$u1_kp"\n'
    b'  $B chown "$u1_ug" "$u1_kp" 2>/dev/null || true\n'
    b'  $B chmod 0700 "$u1_kp" 2>/dev/null || true\n'
    b' fi\n'
    b'done\n'
    b'u1_hwid_rst=skip\n'
    b'u1_rst_flag="$R/data/service/el1/public/.u15_crypto_db_reset_v2"\n'
    b'if ! $B test -f "$u1_rst_flag"; then\n'
    b' u1_hwid_rst=done\n'
    b' for u1_rd in "$R/data/service" "$R/data/app" "$R/data/data"; do\n'
    b'  if $B test -d "$u1_rd"; then\n'
    b'   $B find "$u1_rd" -maxdepth 8 -type f \\( -name \'AccountDataShare.db*\' -o -name \'AccountDataShare.pub_key_v2\' -o -name \'AccountDataShare.key_lock\' -o -name \'AccountSecretData.db*\' -o -name \'AccountSecretData.pub_key_v2\' -o -name \'AccountSecretData.key_lock\' -o -name \'system_data_el2_100.db*\' \\) 2>/dev/null | while IFS= read -r u1_rf; do\n'
    b'    $B rm -f "$u1_rf" 2>/dev/null || true\n'
    b'   done\n'
    b'  fi\n'
    b' done\n'
    b' if $B test -d "$R/data/app/el1/0/database/com.huawei.hmos.location"; then\n'
    b'  $B rm -f "$R/data/app/el1/0/database/com.huawei.hmos.location"/key/*.key_lock 2>/dev/null || true\n'
    b'  $B rm -rf "$R/data/app/el1/0/database/com.huawei.hmos.location/kvdb"/* 2>/dev/null || true\n'
    b' fi\n'
    b' u1_meta=$($B find "$R/data/service/el1/public/database/distributeddata/meta" -maxdepth 4 -type f -name \'gen_natural_store.db\' 2>/dev/null | $B head -n 1 || true)\n'
    b' if [ -n "$u1_meta" ] && $B test -x "$R/system/bin/sqlite3"; then\n'
    b'  u1_meta_owner=$($B stat -c \'%u:%g\' "$u1_meta" 2>/dev/null || echo 3012:3012)\n'
    b'  u1_meta_rel="${u1_meta#$R}"\n'
    b'  if $B chroot "$R" /system/bin/sqlite3 "$u1_meta_rel" "DELETE FROM local_data WHERE CAST(key AS TEXT) LIKE \'SecretKey###%\' AND (CAST(key AS TEXT) LIKE \'%system_data_el2_100%\' OR CAST(key AS TEXT) LIKE \'%AccountDataShare%\' OR CAST(key AS TEXT) LIKE \'%AccountSecretData%\'); PRAGMA wal_checkpoint(TRUNCATE);" >/dev/null 2>&1; then\n'
    b'   $B rm -f "$u1_meta-dwr" 2>/dev/null || true\n'
    b'   $B chown "$u1_meta_owner" "$u1_meta"* 2>/dev/null || true\n'
    b'   $B chmod 0640 "$u1_meta" 2>/dev/null || true\n'
    b'   for u1_aux in "$u1_meta-wal" "$u1_meta-shm"; do\n'
    b'    $B test -f "$u1_aux" && $B chmod 0660 "$u1_aux" 2>/dev/null || true\n'
    b'   done\n'
    b'  else\n'
    b'   u1_hwid_rst=meta_err\n'
    b'  fi\n'
    b' fi\n'
    b' $B touch "$u1_rst_flag" 2>/dev/null || true\n'
    b'fi\n'
    b'u1_nt_rst=skip\n'
    b'u1_nt_flag="$R/data/service/el1/public/.u17_nativetoken_refresh_v3"\n'
    b'if ! $B test -f "$u1_nt_flag"; then\n'
    b' $B rm -f "$R/data/service/el0/access_token/nativetoken.json" 2>/dev/null || true\n'
    b' $B touch "$u1_nt_flag" 2>/dev/null || true\n'
    b' u1_nt_rst=done\n'
    b'fi\n'
    b'u1_loc_rst=skip\n'
    b'u1_loc_flag="$R/data/service/el1/public/.u16_location_kv_v1"\n'
    b'if ! $B test -f "$u1_loc_flag"; then\n'
    b' u1_loc_rst=done\n'
    b' if $B test -d "$R/data/app/el1/0/database/com.huawei.hmos.location"; then\n'
    b'  $B rm -f "$R/data/app/el1/0/database/com.huawei.hmos.location"/key/*.key_lock 2>/dev/null || true\n'
    b'  $B rm -rf "$R/data/app/el1/0/database/com.huawei.hmos.location/kvdb"/* 2>/dev/null || true\n'
    b' fi\n'
    b' for u1_u0app in com.huawei.hms.pushservice com.huawei.hms.weather com.huawei.hms.analyticsservice; do\n'
    b'  if $B test -d "$R/data/app/el1/0/database/$u1_u0app/key"; then\n'
    b'   $B rm -f "$R/data/app/el1/0/database/$u1_u0app"/key/*.key_lock 2>/dev/null || true\n'
    b'  fi\n'
    b' done\n'
    b' $B touch "$u1_loc_flag" 2>/dev/null || true\n'
    b'fi\n'
    b'u1_oobe=skip\n'
    b'u1_sd_rel="/data/app/el1/0/database/com.ohos.settingsdata/entry/rdb"\n'
    b'u1_sd_dir="$R$u1_sd_rel"\n'
    b'u1_ver="ADY-AL10 207.0.0.111(SP10C00E111R5P2)"\n'
    b'if $B test -f "$u1_sd_dir/settingsdata.db" && $B test -x "$R/system/bin/sqlite3"; then\n'
    b' u1_prov="$($B chroot "$R" /system/bin/sqlite3 -readonly "$u1_sd_rel/settingsdata.db" "SELECT VALUE FROM SETTINGSDATA WHERE KEYWORD=\'device_provisioned\' LIMIT 1;" 2>/dev/null || true)"\n'
    b' u1_setup="$($B chroot "$R" /system/bin/sqlite3 -readonly "$u1_sd_rel/settingsdata.db" "SELECT VALUE FROM USER_SETTINGSDATA_SECURE_100 WHERE KEYWORD=\'user_setup_complete\' LIMIT 1;" 2>/dev/null || true)"\n'
    b' u1_ota="$($B chroot "$R" /system/bin/sqlite3 -readonly "$u1_sd_rel/settingsdata.db" "SELECT VALUE FROM USER_SETTINGSDATA_SECURE_100 WHERE KEYWORD=\'is_ota_finished\' LIMIT 1;" 2>/dev/null || true)"\n'
    b' u1_bver="$($B chroot "$R" /system/bin/sqlite3 -readonly "$u1_sd_rel/settingsdata.db" "SELECT VALUE FROM USER_SETTINGSDATA_SECURE_100 WHERE KEYWORD=\'buildversionrelease\' LIMIT 1;" 2>/dev/null || true)"\n'
    b' u1_fp="$($B chroot "$R" /system/bin/sqlite3 -readonly "$u1_sd_rel/settingsdata.db" "SELECT VALUE FROM USER_SETTINGSDATA_SECURE_100 WHERE KEYWORD=\'hw_upgrade_pre_fingerprint\' LIMIT 1;" 2>/dev/null || true)"\n'
    b' u1_old="$($B chroot "$R" /system/bin/sqlite3 -readonly "$u1_sd_rel/settingsdata.db" "SELECT VALUE FROM SETTINGSDATA WHERE KEYWORD=\'settings.control.ota_old_version\' LIMIT 1;" 2>/dev/null || true)"\n'
    b' if [ "$u1_prov" = "1" ] && [ "$u1_setup" = "1" ]; then\n'
    b'  if [ "$u1_ota" = "0" ] || [ "$u1_bver" != "$u1_ver" ] || [ "$u1_fp" != "$u1_ver" ] || [ "$u1_old" != "$u1_ver" ]; then\n'
    b'   u1_sd_owner="$($B stat -c \'%u:%g\' "$u1_sd_dir/settingsdata.db" 2>/dev/null || echo 20004:20004)"\n'
    b'   u1_sql="UPDATE USER_SETTINGSDATA_SECURE_100 SET VALUE=\'1\' WHERE KEYWORD=\'is_ota_finished\' AND VALUE=\'0\'; UPDATE USER_SETTINGSDATA_SECURE_100 SET VALUE=\'$u1_ver\' WHERE KEYWORD IN (\'buildversionrelease\',\'hw_upgrade_pre_fingerprint\') AND VALUE!=\'$u1_ver\'; UPDATE SETTINGSDATA SET VALUE=\'1\' WHERE KEYWORD=\'is_ota_finished\' AND VALUE=\'0\'; UPDATE SETTINGSDATA SET VALUE=\'$u1_ver\' WHERE KEYWORD=\'settings.control.ota_old_version\' AND VALUE!=\'$u1_ver\'; PRAGMA wal_checkpoint(TRUNCATE);"\n'
    b'   u1_oobe=fixed\n'
    b'   for u1_dbn in settingsdata.db settingsdata_slave.db; do\n'
    b'    if $B test -f "$u1_sd_dir/$u1_dbn"; then\n'
    b'     if $B chroot "$R" /system/bin/sqlite3 "$u1_sd_rel/$u1_dbn" "$u1_sql" >/dev/null 2>&1; then\n'
    b'      $B rm -f "$u1_sd_dir/$u1_dbn-dwr" 2>/dev/null || true\n'
    b'      $B chown "$u1_sd_owner" "$u1_sd_dir/$u1_dbn"* 2>/dev/null || true\n'
    b'      $B chmod 0660 "$u1_sd_dir/$u1_dbn" 2>/dev/null || true\n'
    b'      for u1_aux in "$u1_sd_dir/$u1_dbn-wal" "$u1_sd_dir/$u1_dbn-shm"; do\n'
    b'       $B test -f "$u1_aux" && $B chmod 0670 "$u1_aux" 2>/dev/null || true\n'
    b'      done\n'
    b'     else\n'
    b'      u1_oobe=err\n'
    b'     fi\n'
    b'    fi\n'
    b'   done\n'
    b'  else\n'
    b'   u1_oobe=ok\n'
    b'  fi\n'
    b' fi\n'
    b'fi\n'
    b'if $B test -f "$R/system/etc/initHosts"; then\n'
    b' $B mkdir -p "$R/data/service/el1/public/hosts_user"\n'
    b' if ! $B test -f "$R/data/service/el1/public/hosts_user/hosts"; then\n'
    b'  $B cp "$R/system/etc/initHosts" "$R/data/service/el1/public/hosts_user/hosts" 2>/dev/null || true\n'
    b' fi\n'
    b' $B chmod 0655 "$R/data/service/el1/public/hosts_user/hosts" 2>/dev/null || true\n'
    b'fi\n'
    b'if $B test -f "$R/data/service/el1/public/hosts_user/hosts" && ! $B grep -q "MARBLE_U1_OTA_BLOCK_BEGIN" "$R/data/service/el1/public/hosts_user/hosts" 2>/dev/null; then\n'
    b' $B cat <<\'EOF\' >> "$R/data/service/el1/public/hosts_user/hosts"\n'
    b'\n# MARBLE_U1_OTA_BLOCK_BEGIN\n'
    b'0.0.0.0 query.hicloud.com\n'
    b'0.0.0.0 update.hicloud.com\n'
    b'0.0.0.0 update-drcn.platform.hicloud.com\n'
    b'0.0.0.0 update-dra.platform.hicloud.com\n'
    b'0.0.0.0 update-dre.platform.hicloud.com\n'
    b'0.0.0.0 update-drru.platform.dbankcloud.ru\n'
    b'0.0.0.0 update-drsx.platform.dbankcloud.cn\n'
    b'0.0.0.0 updatesaas.platform.dbankcloud.com\n'
    b'0.0.0.0 cloudrom-global.emui.dbankcloud.com\n'
    b'0.0.0.0 update.dbankcdn.com\n'
    b'# MARBLE_U1_OTA_BLOCK_END\n'
    b'EOF\n'
    b' $B chmod 0655 "$R/data/service/el1/public/hosts_user/hosts" 2>/dev/null || true\n'
    b'fi\n'
    b'u1_ota_blk=skip\n'
    b'u1_ota_flag="$R/data/service/el1/public/.u17_ota_block_v1"\n'
    b'if ! $B test -f "$u1_ota_flag"; then\n'
    b' u1_ota_blk=done\n'
    b' $B rm -rf "$R/data/service/el1/public/update/dupdate_engine/httpdns"/* "$R/data/service/el1/public/update/dupdate_engine/files"/* "$R/data/service/el1/public/update/dupdate_engine/results"/* "$R/data/update/ota_package"/* 2>/dev/null || true\n'
    b' if $B test -f "$u1_sd_dir/settingsdata.db" && $B test -x "$R/system/bin/sqlite3"; then\n'
    b'  u1_sd_owner="$($B stat -c \'%u:%g\' "$u1_sd_dir/settingsdata.db" 2>/dev/null || echo 20004:20004)"\n'
    b'  u1_ota_sql="UPDATE SETTINGSDATA SET VALUE=\'1\' WHERE KEYWORD=\'ota_disable_automatic_update\'; UPDATE SETTINGSDATA SET VALUE=\'0\' WHERE KEYWORD IN (\'hw_new_system_update\',\'hw_new_system_update_status\'); UPDATE USER_SETTINGSDATA_SECURE_100 SET VALUE=\'0\' WHERE KEYWORD IN (\'hw_new_system_update\',\'hw_new_system_update_status\'); PRAGMA wal_checkpoint(TRUNCATE);"\n'
    b'  for u1_dbn in settingsdata.db settingsdata_slave.db; do\n'
    b'   if $B test -f "$u1_sd_dir/$u1_dbn"; then\n'
    b'    if $B chroot "$R" /system/bin/sqlite3 "$u1_sd_rel/$u1_dbn" "$u1_ota_sql" >/dev/null 2>&1; then\n'
    b'     $B rm -f "$u1_sd_dir/$u1_dbn-dwr" 2>/dev/null || true\n'
    b'     $B chown "$u1_sd_owner" "$u1_sd_dir/$u1_dbn"* 2>/dev/null || true\n'
    b'     $B chmod 0660 "$u1_sd_dir/$u1_dbn" 2>/dev/null || true\n'
    b'     for u1_aux in "$u1_sd_dir/$u1_dbn-wal" "$u1_sd_dir/$u1_dbn-shm"; do\n'
    b'      $B test -f "$u1_aux" && $B chmod 0670 "$u1_aux" 2>/dev/null || true\n'
    b'     done\n'
    b'    fi\n'
    b'   fi\n'
    b'  done\n'
    b' fi\n'
    b' $B touch "$u1_ota_flag" 2>/dev/null || true\n'
    b'fi\n'
    b'if $B test -d "$R/data/service/el1/public/cert_manager_service"; then\n'
    b' $B chown -R 3515:3515 "$R/data/service/el1/public/cert_manager_service" 2>/dev/null || true\n'
    b'fi\n'
    b'if $B test -f /graphics/u1-cust-version.tar.gz; then\n'
    b' $B tar -xzf /graphics/u1-cust-version.tar.gz -C "$R"\n'
    b' $B chown -R 0:0 "$R/version" "$R/cust" 2>/dev/null || true\n'
    b' $B chmod -R a+rX "$R/version" "$R/cust" 2>/dev/null || true\n'
    b'fi\n'
    b'u1_mem_kb=$($B awk \'/^MemTotal:/{print $2; exit}\' /proc/meminfo 2>/dev/null || echo 0)\n'
    b'case "$u1_mem_kb" in \'\'|*[!0-9]*) u1_mem_kb=0;; esac\n'
    b'if [ "$u1_mem_kb" -gt 12582912 ]; then\n'
    b' u1_ddr=16\n'
    b'elif [ "$u1_mem_kb" -gt 8388608 ]; then\n'
    b' u1_ddr=12\n'
    b'else\n'
    b' u1_ddr=8\n'
    b'fi\n'
    b'$B cat <<EOF > "$R/system/etc/param/marble_oem.para"\n'
    b'const.cust.devmodel=ADY-AL10\n'
    b'const.cust.vendor=all\n'
    b'const.cust.region=cn\n'
    b'const.cust.home_region=cn\n'
    b'const.settings.processor_info_show=true\n'
    b'const.settings.processor_info_display=Qualcomm Snapdragon 7+ Gen 2\n'
    b'ohos.boot.vendor.ddrsize=$u1_ddr\n'
    b'const.distributed_collaboration.productId=P1610\n'
    b'EOF\n'
    b'$B chmod 0644 "$R/system/etc/param/marble_oem.para"\n'
    b'$B mkdir -p "$R/chip_prod/variant/special_cust/ADY-AL10/all/cn/etc/param"\n'
    b'$B cat <<EOF > "$R/chip_prod/variant/special_cust/ADY-AL10/all/cn/etc/param/hw_defaults.para"\n'
    b'const.display.aihdr_picture_default_enable=false\n'
    b'const.display.hdr_picture_support=false\n'
    b'const.display.support_aihdr_picture=false\n'
    b'const.settings.processor_info_show=true\n'
    b'const.settings.processor_info_display=Qualcomm Snapdragon 7+ Gen 2\n'
    b'const.product.hardwareversion=SM7475\n'
    b'ohos.boot.vendor.ddrsize=$u1_ddr\n'
    b'EOF\n'
    b'$B chmod 0644 "$R/chip_prod/variant/special_cust/ADY-AL10/all/cn/etc/param/hw_defaults.para"\n'
    b'if $B test -f "$R/compat/etc/cust/baselayer.cfg"; then\n'
    b' $B cat <<\'EOF\' >> "$R/compat/etc/cust/baselayer.cfg"\n'
    b'\n# marble U1.1: explicit literal ADY-AL10 OEM/version/chip_prod cust layers\n'
    b'/sys_prod/variant/hw_oem/ADY-AL10\n'
    b'/version/common_cust/all/cn\n'
    b'/version/special_cust/ADY-AL10/all/cn\n'
    b'/chip_prod/variant/special_cust/ADY-AL10/all/cn\n'
    b'EOF\n'
    b'fi\n'
    b'u1_patch_para() {\n'
    b' u1_src="$1"; u1_dst="$2"; shift 2\n'
    b' if ! $B test -f "$u1_src"; then return 0; fi\n'
    b' $B mkdir -p "$R/compat/etc"\n'
    b' $B cp "$u1_src" "$u1_dst"\n'
    b' for u1_kv in "$@"; do\n'
    b'  u1_k="${u1_kv%%=*}"\n'
    b'  u1_v="${u1_kv#*=}"\n'
    b'  $B awk -v k="$u1_k" -v v="$u1_v" \'BEGIN{seen=0; rk=k; gsub(/[.]/, "[.]", rk); pat="^[ \\t]*" rk "[ \\t]*="} $0 ~ pat{print k "=" v; seen=1; next} {print} END{if(!seen) print k "=" v}\' "$u1_dst" > "$u1_dst.tmp"\n'
    b'  $B mv "$u1_dst.tmp" "$u1_dst"\n'
    b' done\n'
    b' $B chmod 0644 "$u1_dst"\n'
    b' if ! $B grep -q " $u1_src " /proc/mounts 2>/dev/null; then\n'
    b'  $B mount --bind "$u1_dst" "$u1_src" || true\n'
    b' fi\n'
    b'}\n'
    b'u1_patch_para "$R/sys_prod/etc/param/hw_defaults.para" "$R/compat/etc/u1-sysprod-hw-defaults.para" \\\n'
    b' "const.display.aihdr_picture_default_enable=false" \\\n'
    b' "const.display.hdr_picture_support=false" \\\n'
    b' "const.display.support_aihdr_picture=false"\n'
    b'$B cat <<\'EOF\' > "$R/system/profile/msdp.json"\n'
    b'{\n'
    b'  "process": "msdp",\n'
    b'  "systemability": [\n'
    b'    {\n'
    b'      "name": 2902,\n'
    b'      "libpath": "libdevicestatus_service.z.so",\n'
    b'      "run-on-create": true,\n'
    b'      "distributed": false,\n'
    b'      "dump_level": 1,\n'
    b'      "min_hdi_proxy_version": []\n'
    b'    },\n'
    b'    {\n'
    b'      "name": 2907,\n'
    b'      "libpath": "libuser_status_service.z.so",\n'
    b'      "run-on-create": true,\n'
    b'      "distributed": false,\n'
    b'      "dump_level": 1,\n'
    b'      "min_hdi_proxy_version": []\n'
    b'    }\n'
    b'  ]\n'
    b'}\n'
    b'EOF\n'
    b'$B chmod 0644 "$R/system/profile/msdp.json"\n'
    b'echo "MARBLE_U1_STAGE_READY pt_ovl=$pt_ovl ddr=$u1_ddr oobe=$u1_oobe hwid_reset=$u1_hwid_rst clk=$u1_clk dbk=$u1_dbk_n nt=$u1_nt_rst loc_rst=$u1_loc_rst ota_blk=$u1_ota_blk"\n'
    b'# === MARBLE_U1_UX_SECURITY_STAGE_END ===\n'
)

START_U1_EARLY_BLOCK = (
    b'# === MARBLE_U1_HUKS_EARLY_BEGIN ===\n'
    b'stage=u1_huks\n'
    b'pset marble.debug.start.u1_dirs true\n'
    b'pset marble.debug.start.huks true\n'
    b'u1_w=0\n'
    b'while [ "$u1_w" -lt 15 ]; do\n'
    b' [ "$(pget bootevent.huksService.ready)" = true ] && break\n'
    b' u1_w=$((u1_w + 1))\n'
    b' $B sleep 0.2\n'
    b'done\n'
    b'log "MARBLE_U1_HUKS_READY=$(pget bootevent.huksService.ready) wait=${u1_w}"\n'
    b'pset marble.debug.start.hwid true\n'
    b'# === MARBLE_U1_HUKS_EARLY_END ===\n'
)


def require(cond, msg):
    if not cond:
        raise ValueError(msg)


def splice_once(data: bytes, pairs: tuple[tuple[bytes, bytes], ...], label: str) -> bytes:
    out = data
    for old, new in pairs:
        require(out.count(old) == 1, f'{label}: insertion anchor not unique (count={out.count(old)}): {old[:80]!r}')
        out = out.replace(old, new, 1)
    reverted = out
    for old, new in reversed(pairs):
        require(reverted.count(new) == 1, f'{label}: delta not separable')
        reverted = reverted.replace(new, old, 1)
    require(reverted == data, f'{label}: non-target bytes changed')
    return out


def patch_init_debug(raw: bytes) -> bytes:
    require(b'init-u1-services-marble.cfg' not in raw, 'init-debug already imports init-u1-services-marble.cfg')
    old_imp = b'    "/compat/etc/init-modem-f2-marble.cfg"\n  ],'
    new_imp = b'    "/compat/etc/init-modem-f2-marble.cfg",\n    "/compat/etc/init-u1-services-marble.cfg"\n  ],'
    old_priv = (
        b'      "name": "privacy_service",\n'
        b'      "path": [\n'
        b'        "/system/bin/sa_main",\n'
        b'        "/system/profile/privacy_service.json"\n'
        b'      ],\n'
        b'      "uid": "access_token",\n'
        b'      "gid": [\n'
        b'        "access_token",\n'
        b'        "shell"\n'
        b'      ],\n'
        b'      "permission": [\n'
        b'        "ohos.permission.GET_BUNDLE_INFO_PRIVILEGED",\n'
        b'        "ohos.permission.GET_RUNNING_INFO",\n'
        b'        "ohos.permission.RUNNING_STATE_OBSERVER",\n'
        b'        "ohos.permission.MANAGE_LOCAL_ACCOUNTS",\n'
        b'        "ohos.permission.INTERACT_ACROSS_LOCAL_ACCOUNTS",\n'
        b'        "ohos.permission.DISTRIBUTED_DATASYNC",\n'
        b'        "ohos.permission.ACCESS_SERVICE_DM"\n'
        b'      ],\n'
        b'      "secon": "u:r:privacy_service:s0",'
    )
    new_priv = (
        b'      "name": "privacy_service",\n'
        b'      "path": [\n'
        b'        "/system/bin/sa_main",\n'
        b'        "/system/profile/privacy_service.json"\n'
        b'      ],\n'
        b'      "uid": "access_token",\n'
        b'      "gid": [\n'
        b'        "access_token",\n'
        b'        "shell"\n'
        b'      ],\n'
        b'      "permission": [\n'
        b'        "ohos.permission.GET_RUNNING_INFO",\n'
        b'        "ohos.permission.GET_BUNDLE_INFO_PRIVILEGED",\n'
        b'        "ohos.permission.GET_SENSITIVE_PERMISSIONS",\n'
        b'        "ohos.permission.MANAGE_DISPOSED_APP_STATUS",\n'
        b'        "ohos.permission.MICROPHONE",\n'
        b'        "ohos.permission.RUNNING_STATE_OBSERVER",\n'
        b'        "ohos.permission.PUBLISH_SYSTEM_COMMON_EVENT",\n'
        b'        "ohos.permission.CAMERA_CONTROL",\n'
        b'        "ohos.permission.MICROPHONE_CONTROL",\n'
        b'        "ohos.permission.MANAGE_LOCAL_ACCOUNTS",\n'
        b'        "ohos.permission.SET_SUPER_PRIVACY",\n'
        b'        "ohos.permission.PERMISSION_RECORD_TOGGLE",\n'
        b'        "ohos.permission.USE_SECURITY_PRIVACY_MESSAGER",\n'
        b'        "ohos.permission.MANAGE_SECURE_SETTINGS",\n'
        b'        "ohos.permission.PERMISSION_USED_STATS",\n'
        b'        "ohos.permission.ACCESS_SECURITY_PRIVACY_CENTER",\n'
        b'        "ohos.permission.START_SYSTEM_DIALOG",\n'
        b'        "ohos.permission.START_ABILITIES_FROM_BACKGROUND",\n'
        b'        "ohos.permission.INTERACT_ACROSS_LOCAL_ACCOUNTS",\n'
        b'        "ohos.permission.DISTRIBUTED_DATASYNC",\n'
        b'        "ohos.permission.ACCESS_SERVICE_DM"\n'
        b'      ],\n'
        b'      "permission_acls": [\n'
        b'        "ohos.permission.MANAGE_DISPOSED_APP_STATUS",\n'
        b'        "ohos.permission.PERMISSION_RECORD_TOGGLE",\n'
        b'        "ohos.permission.USE_SECURITY_PRIVACY_MESSAGER",\n'
        b'        "ohos.permission.CAMERA_CONTROL",\n'
        b'        "ohos.permission.MICROPHONE_CONTROL",\n'
        b'        "ohos.permission.SET_SUPER_PRIVACY"\n'
        b'      ],\n'
        b'      "secon": "u:r:privacy_service:s0",'
    )
    out = splice_once(raw, ((old_imp, new_imp), (old_priv, new_priv)), INIT_DEBUG_MEMBER)
    json.loads(out.decode('utf-8'))
    return out


def patch_init_b2(raw: bytes) -> bytes:
    old1 = (
        b'      "path": [\n'
        b'        "/system/bin/sa_main",\n'
        b'        "/system/profile/huawei_id_svc.json"\n'
        b'      ],\n'
        b'      "ondemand": true,\n'
        b'      "uid": "huawei_id",'
    )
    new1 = (
        b'      "path": [\n'
        b'        "/system/bin/sa_main",\n'
        b'        "/system/profile/huawei_id_svc.json"\n'
        b'      ],\n'
        b'      "uid": "huawei_id",'
    )
    old2 = (
        b'      "secon": "u:r:huaweiid_service:s0",\n'
        b'      "disabled": 1,\n'
        b'      "once": 1,\n'
        b'      "sandbox": 0,'
    )
    new2 = (
        b'      "secon": "u:r:huaweiid_service:s0",\n'
        b'      "sandbox": 0,'
    )
    out = splice_once(raw, ((old1, new1), (old2, new2)), INIT_B2_MEMBER)
    json.loads(out.decode('utf-8'))
    return out


def patch_init_desktop(raw: bytes) -> bytes:
    old1 = (
        b'      "secon": "u:r:theme_manager_service:s0",\n'
        b'      "disabled": 1,\n'
        b'      "once": 1,\n'
        b'      "saspawn": false,'
    )
    new1 = (
        b'      "secon": "u:r:theme_manager_service:s0",\n'
        b'      "ondemand": true,\n'
        b'      "saspawn": false,'
    )
    old2 = (
        b'      "apl": "system_core",\n'
        b'      "permission": [\n'
        b'        "ohos.permission.INJECT_INPUT_EVENT"\n'
        b'      ],'
    )
    new2 = (
        b'      "apl": "system_core",\n'
        b'      "permission": [\n'
        b'        "ohos.permission.INJECT_INPUT_EVENT",\n'
        b'        "ohos.permission.START_ABILITIES_FROM_BACKGROUND"\n'
        b'      ],'
    )
    out = splice_once(raw, ((old1, new1), (old2, new2)), INIT_DESKTOP_MEMBER)
    json.loads(out.decode('utf-8'))
    return out


def patch_start_phase4(raw: bytes) -> bytes:
    require(b'MARBLE_U1_HUKS_EARLY_BEGIN' not in raw, 'start-phase4 already patched for U1')
    anchor = b'stage=services\nfor gate in storage kvdata useriam account appspawn theme wallpaper ui diagnostics deviceproviders b1; do'
    out = splice_once(raw, ((anchor, START_U1_EARLY_BLOCK + anchor),), START_PHASE4_MEMBER)
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def patch_stage(raw: bytes) -> bytes:
    for marker in (b'MARBLE_U1_UX_SECURITY_STAGE_BEGIN', b'marble_oem.para', b'u1-cust-version.tar.gz'):
        require(marker not in raw, f'stage script already contains {marker!r}')
    out = splice_once(
        raw,
        (
            (OPTIONAL_SA_F5, OPTIONAL_SA_U1),
            (STAGE_ANCHOR, STAGE_U1_BLOCK + STAGE_ANCHOR),
        ),
        STAGE_MEMBER,
    )
    subprocess.run(['sh', '-n'], input=out, check=True)
    return out


def validate_inputs() -> None:
    for rel, src, _mode, _replaces in STAGED_FILES:
        require(src.is_file(), f'Missing staged U1 artifact: {src}')
        raw = src.read_bytes()
        if rel.endswith('.cfg'):
            json.loads(raw.decode('utf-8'))
        elif rel.endswith('.tar.gz'):
            require(raw[:2] == b'\x1f\x8b', f'Expected gzip tarball for {rel}')
        else:
            require(raw[:4] == b'\x7fELF', f'Expected ELF binary for {rel}')


def build(base_path: Path, out: Path) -> Path:
    require(sha256(ORACLE.read_bytes()).hexdigest() == ORACLE_SHA, 'Archive verifier identity changed')
    spec = importlib.util.spec_from_file_location('h20_u1_archive_verifier', ORACLE)
    archive = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(archive)
    r = archive.load_tools()

    base = r.read_original(base_path)
    require(r.sha(base) == BASE_SHA, f'Base SHA mismatch: got {r.sha(base)}, want {BASE_SHA}')
    base_id = r.identity(base_path)
    validate_inputs()

    before_layout = r.layout(base)
    before_avb = r.verify_existing_avb(base, before_layout['payload_end'])
    out.mkdir(parents=True, exist_ok=True)
    U1_BUILD.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pack_u1.', dir=out) as tmp:
        scratch = Path(tmp)
        old, old_raw, kind, old_rd, args = r.extract_entries(base_path, base, before_layout, scratch / 'before')
        for gm in GUARD_MEMBERS:
            require(gm in old, f'Guard member missing in base image: {gm}')
        manifest = archive.parse(old[r.MANIFEST][1])
        r.manifest_holds(old, manifest)

        patched_members = {
            INIT_DEBUG_MEMBER: ('hmos-init-debug.cfg', patch_init_debug(old[INIT_DEBUG_MEMBER][1])),
            INIT_B2_MEMBER: ('graphics/init-b2-apps-marble.cfg', patch_init_b2(old[INIT_B2_MEMBER][1])),
            INIT_DESKTOP_MEMBER: ('graphics/init-desktop-marble.cfg', patch_init_desktop(old[INIT_DESKTOP_MEMBER][1])),
            START_PHASE4_MEMBER: ('graphics/start-phase4-marble.sh', patch_start_phase4(old[START_PHASE4_MEMBER][1])),
            STAGE_MEMBER: ('graphics/stage-phase4-marble.sh', patch_stage(old[STAGE_MEMBER][1])),
        }

        # Verify no duplicate service names across all ramdisk CFGs when adding init-u1-services-marble.cfg
        u1_cfg_obj = json.loads((INIT_DIR / 'init-u1-services-marble.cfg').read_text(encoding='utf-8'))
        u1_service_names = {s['name'] for s in u1_cfg_obj['services']}
        for member, (_, raw_member) in old.items():
            if member.endswith('.cfg'):
                m_bytes = patched_members[member][1] if member in patched_members else raw_member
                obj = json.loads(m_bytes.decode('utf-8'))
                existing_names = {s['name'] for s in obj.get('services', [])}
                overlap = existing_names & u1_service_names
                require(not overlap, f'Duplicate service name(s) in {member}: {sorted(overlap)}')

        expected = dict(old)
        next_inode = [max(fields[0] for fields, _ in old.values()) + 1]
        new_manifest = copy.deepcopy(manifest)

        modified_members = {r.MANIFEST}
        for full_member, (m_key, new_bytes) in patched_members.items():
            r.set_entry(expected, full_member, new_bytes)
            modified_members.add(full_member)
            if m_key in new_manifest:
                new_manifest[m_key].update(bytes=len(new_bytes), sha256=r.sha(new_bytes))

        staged_info = {}
        for rel, src, mode, replaces in STAGED_FILES:
            full = f'{r.image_tools.COMPAT_DIR}/{rel}'
            blob = src.read_bytes()
            if replaces:
                require(full in expected, f'Expected existing member to replace: {full}')
                r.set_entry(expected, full, blob)
                modified_members.add(full)
                new_manifest[rel].update(bytes=len(blob), mode=oct(mode), sha256=r.sha(blob))
            else:
                require(full not in expected, f'Duplicate ramdisk member: {full}')
                r.image_tools.put_file(expected, full, blob, mode, next_inode)
                expected[full][0][11] = len(full.encode('utf-8')) + 1
                new_manifest[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob)}
            staged_info[rel] = {'bytes': len(blob), 'mode': oct(mode), 'sha256': r.sha(blob), 'replaced': replaces}

        r.set_entry(expected, r.MANIFEST, r.dump(new_manifest))
        r.manifest_holds(expected, new_manifest)

        cpio, rd, pack = (scratch / n for n in ('new.cpio', 'new.lz4', 'payload.img'))
        raw = r.image_tools.pack_cpio_newc(expected)
        cpio.write_bytes(raw)
        r.image_tools.compress_ramdisk(cpio, rd, kind)
        require(args.count(str(old_rd)) == 1, 'Ambiguous ramdisk argument')
        flags = [str(rd) if arg == str(old_rd) else arg for arg in args]
        subprocess.run(['mkbootimg', *flags, '--vendor_boot', str(pack)], check=True, capture_output=True)
        payload = pack.read_bytes()
        after_layout = r.layout(payload)
        require(after_layout['payload_end'] == len(payload) <= before_layout['payload_end'] + 4 * 1024 * 1024,
                f'Unexpected payload growth: {before_layout["payload_end"]} -> {after_layout["payload_end"]}')
        r.check_container(base, before_layout, payload, after_layout)
        image, updated = r.image_tools.preserve_and_update_avb_footer(payload, base)
        require(updated, 'Existing AVB footer metadata not updated')
        after_avb = r.verify_existing_avb(image, len(payload))
        require(all(after_avb[k] == before_avb[k] for k in ('vbmeta_masked', 'footer_masked')),
                'Unexpected AVB metadata change')

        verify = scratch / 'roundtrip.img'
        verify.write_bytes(image)
        got, got_raw, got_kind, _, _ = r.extract_entries(verify, image, after_layout, scratch / 'after')
        require(got == expected and got_raw == raw and got_kind == kind, 'Exact unpack round-trip failed')

        for gm in GUARD_MEMBERS:
            require(got[gm][1] == old[gm][1], f'Protected member altered: {gm}')

        old_spans, old_trailer = archive.record_spans(old_raw)
        got_spans, got_trailer = archive.record_spans(got_raw)
        require(got_trailer == old_trailer, 'CPIO trailer changed')
        unchanged = set(old) - modified_members
        require(all(got_raw[slice(*got_spans[n])] == old_raw[slice(*old_spans[n])] for n in unchanged),
                'Unrelated CPIO record changed')

    cand_sha = r.sha(image)
    out_img = out / f'vendor_boot_h20_u1_ux_security_{cand_sha[:8]}.img'
    out_img.write_bytes(image)
    report = {
        'base': base_id,
        'candidate': r.identity(out_img),
        'entries_before': len(old),
        'entries_after': len(expected),
        'guard_members_verified': list(GUARD_MEMBERS),
        'staged': staged_info,
    }
    report_path = U1_BUILD / 'u1-build-report.json'
    report_path.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'CANDIDATE_IMAGE={out_img}')
    print(f'CANDIDATE_SHA256={cand_sha}')
    print(f'CANDIDATE_SHA8={cand_sha[:8]}')
    print(f'BUILD_REPORT={report_path}')
    return out_img


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--base', type=Path, default=BASE_DEFAULT)
    p.add_argument('--out', type=Path, default=Path('/mnt/e/marble-tmp'))
    args = p.parse_args()
    build(args.base, args.out)


if __name__ == '__main__':
    main()
