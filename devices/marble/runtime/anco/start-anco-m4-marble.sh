#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch M4: Prepare DroiTong / ANCO local runtime environment (cgroups,
# binderfs, isula bind-mount, updater_sa/broker/virt_service directories) and
# trigger on-demand ANCO + updater_sa (SA 3006) readiness on Xiaomi marble.
# Strictly opt-in / post-boot gated (marble.anco.ready=true); zero boot-block risk.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
PROC_MOUNTS=${MARBLE_PROC_MOUNTS:-/proc/mounts}
PROC_FILESYSTEMS=${MARBLE_PROC_FILESYSTEMS:-/proc/filesystems}
DRY_RUN=${MARBLE_ANCO_DRY_RUN:-0}

say() { echo "$*"; }

SKIP=$("$B" cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | "$B" tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,anco,*|*,rgm,*|*,droitong,*)
    say "ANCO_M4_SKIP=requested"
    exit 0
    ;;
esac

# Exact numeric UID:GID pairs verified against donor /system/etc/{passwd,group}:
#   root=0, system=1000, radio=1001, media=1013, media_rw=1023, audio=1041,
#   virt_device=3075, rgm_manager=5002, audio_broker_uid=5532,
#   anco_service_broker=5557, update=6666, input=6696, hmos_fusion_manager=7558
mk_dir() {
  dpath="$1"; dmode="$2"; downer="$3"
  "$B" mkdir -p "$dpath" 2>/dev/null || true
  "$B" chown "$downer" "$dpath" 2>/dev/null || true
  "$B" chmod "$dmode" "$dpath" 2>/dev/null || true
}

is_mounted() {
  target="$1"
  "$B" grep -Fq " $target " "$PROC_MOUNTS" 2>/dev/null
}

# 1. Verify kernel container prerequisites and staged ANCO/isula binaries.
has_binder_fs=0
has_overlay_fs=0
has_cgroup_fs=0
has_cgroup2_fs=0
if [ -r "$PROC_FILESYSTEMS" ]; then
  "$B" grep -qw 'binder' "$PROC_FILESYSTEMS" 2>/dev/null && has_binder_fs=1 || true
  "$B" grep -qw 'overlay' "$PROC_FILESYSTEMS" 2>/dev/null && has_overlay_fs=1 || true
  "$B" grep -qw 'cgroup' "$PROC_FILESYSTEMS" 2>/dev/null && has_cgroup_fs=1 || true
  "$B" grep -qw 'cgroup2' "$PROC_FILESYSTEMS" 2>/dev/null && has_cgroup2_fs=1 || true
fi
isula_bins=0
for ibin in isulad lxc-start lxc-attach rgm_engine.sh; do
  if [ -e "$R/system/bin/virt_service/rgm_engine/isula/bin/$ibin" ]; then
    isula_bins=$((isula_bins + 1))
  fi
done
broker_bin=missing
audio_bin=missing
[ -e "$R/system/bin/anco_service_broker" ] && broker_bin=present
[ -e "$R/system/bin/anco_audio" ] && audio_bin=present
say "ANCO_M4_PRECHECK binder_fs=$has_binder_fs overlay_fs=$has_overlay_fs cgroup_fs=$has_cgroup_fs cgroup2_fs=$has_cgroup2_fs isula_bins=$isula_bins/4 broker_bin=$broker_bin audio_bin=$audio_bin"

# 2. Prepare directories with exact donor UID/GID permissions.
# Create child directories BEFORE tightening parent permissions (e.g. rgm_engine 0505).
mk_dir "$R/data/service/el1/public/rgm_manager" 0770 5002:5002
mk_dir "$R/data/service/el1/public/rgm_manager/data" 0770 5002:5002
mk_dir "$R/data/service/el1/public/rgm_manager/bridge_network" 0700 5002:5002

"$B" mkdir -p "$R/data/service/el1/public/rgm_engine/isula/var" 2>/dev/null || true
"$B" chown 0:0 "$R/data/service/el1/public/rgm_engine" \
  "$R/data/service/el1/public/rgm_engine/isula" \
  "$R/data/service/el1/public/rgm_engine/isula/var" 2>/dev/null || true
"$B" chmod 0700 "$R/data/service/el1/public/rgm_engine/isula/var" 2>/dev/null || true
"$B" chmod 0705 "$R/data/service/el1/public/rgm_engine/isula" 2>/dev/null || true
"$B" chmod 0505 "$R/data/service/el1/public/rgm_engine" 2>/dev/null || true

mk_dir "$R/data/service/el1/public/hmos_fusion_manager" 0711 7558:7558
mk_dir "$R/data/service/el1/public/bundle_framework_broker" 0711 5557:5557
mk_dir "$R/data/service/el1/public/perf_broker" 0700 5557:5557
mk_dir "$R/data/service/el1/public/audio_broker" 2770 5532:1023

# updater_sa (SA 3006) directories: update:update (6666:6666)
U="$R/data/service/el1/public/update"
mk_dir "$U" 0751 6666:6666
mk_dir "$U/cloudification" 0770 6666:6666
mk_dir "$U/dupdate_engine" 0711 6666:6666
mk_dir "$U/dupdate_engine/log" 0740 6666:6666
mk_dir "$U/dupdate_engine/results" 0711 6666:6666
mk_dir "$U/dupdate_engine/results/plugin" 0755 6666:6666
for usub in cccs_grs databases preferences httpdns files \
  files/accessory files/accessory/changelogs files/accessory/notify_custom \
  files/assist files/assist/changelogs \
  files/firmware files/firmware/changelog_resource files/firmware/changelogs \
  files/firmware/notify_custom files/firmware/downgrade_polices \
  files/firmware/remind_polices files/plugin files/plugin/changelogs; do
  mk_dir "$U/dupdate_engine/$usub" 0700 6666:6666
done
mk_dir "$U/param_service" 0751 6666:6666
mk_dir "$U/param_service/download" 0750 6666:6666
for psub in install install/system install/system/etc; do
  mk_dir "$U/param_service/$psub" 0775 6666:6666
done

mk_dir "$R/data/update" 0770 6666:6666
mk_dir "$R/data/update/upload" 0750 6666:6666
mk_dir "$R/data/update/sd_package" 0777 6666:6666
mk_dir "$R/data/update/ota_package" 0770 6666:6666
for osub in accessory accessory/versions assist assist/versions \
  firmware firmware/versions plugin plugin/versions cloud_rom \
  dh_ota dh_ota/versions large_model large_model/versions \
  dh_auto dh_auto/versions; do
  mk_dir "$R/data/update/ota_package/$osub" 0770 6666:6666
done

# virt_service + ANCO broker UDS directories
V="$R/data/virt_service"
mk_dir "$V" 0701 0:0
mk_dir "$V/anco_decoupling" 0755 7558:7558
mk_dir "$V/rgm_hmos" 0701 0:0
mk_dir "$V/rgm_hmos/anco_hmos_data" 0755 0:0
mk_dir "$V/rgm_hmos/anco_hmos_cache" 0755 0:0
mk_dir "$V/rgm_hmos/anco_hmos_log" 0775 0:1000
mk_dir "$V/rgm_hmos/anco_hmos_data/keystore" 0755 0:0
for csub in container container/clone container/clone_data container/backup \
  container/backup_data container/hisuite container/hisuite_data; do
  mk_dir "$V/rgm_hmos/anco_hmos_data/$csub" 0755 0:0
done
mk_dir "$V/rgm_hmos/anco_hmos_data/cota" 0775 1000:1000
mk_dir "$V/rgm_hmos/anco_hmos_data/cota/anco" 0755 7558:7558
mk_dir "$V/rgm_hmos/anco_hmos_data/cota/anco_sdk" 0755 7558:7558
for dsub in cota/anco_dms cota/anco_dms/logicCamera cota/anco_dms/logicCamera/lap \
  cota/anco_dms/logicCamera/grlb settings settings/users; do
  mk_dir "$V/rgm_hmos/anco_hmos_data/$dsub" 0755 1000:1000
done

for vdir in rgm_linux virt_comm virt_comm/rgm_linux virt_comm/rgm_hmos \
  virt_comm/rgm_hmos/uds rgm_hmos/cross rgm_hmos/cross/uds; do
  mk_dir "$V/$vdir" 0701 0:0
done
mk_dir "$V/virt_comm/rgm_linux/uds" 0711 0:0
mk_dir "$V/virt_comm/rgm_hmos/uds/virt_ril" 0710 1001:1001
mk_dir "$V/virt_comm/rgm_hmos/uds/cameradaemon_broker" 0711 1013:1013
for ub in virt_comm/rgm_hmos/uds rgm_hmos/cross/uds; do
  mk_dir "$V/$ub/anco_audio" 0701 1041:1041
  mk_dir "$V/$ub/input" 0701 6696:6696
  mk_dir "$V/$ub/virt_graphic_broker" 0701 5557:5557
done

for uds in virt_device_broker virt_factory_broker virt_sensors_broker \
  virt_vibrator_broker virt_input_broker virt_gnss_broker \
  virt_perfgenius_broker virt_wifi_broker virt_keymaster_broker \
  virt_consumerir_broker; do
  mk_dir "$V/rgm_hmos/cross/uds/$uds" 0711 3075:3075
  mk_dir "$V/virt_comm/rgm_hmos/uds/$uds" 0711 3075:3075
done

# 3. Idempotently mount /sys/fs/cgroup/{freezer,systemd,devices} (non-fatal).
# Preserve an existing /sys/fs/cgroup mount (e.g. cgroup2 from netsysnative).
CGROUP_ROOT="$R/sys/fs/cgroup"
FRZ_DIR="$R/dev/frz"
mk_dir "$CGROUP_ROOT" 0755 0:0
cg_root_state=skipped
if is_mounted "$CGROUP_ROOT"; then
  cg_root_state=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  cg_root_state=dry_run
else
  if "$B" mount -t tmpfs tmpfs "$CGROUP_ROOT" 2>/dev/null; then
    cg_root_state=mounted_tmpfs
  else
    cg_root_state=mount_failed
  fi
fi

for cg in freezer systemd devices; do
  mk_dir "$CGROUP_ROOT/$cg" 0755 0:0
done

cg_freezer=skipped
if is_mounted "$CGROUP_ROOT/freezer"; then
  cg_freezer=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  cg_freezer=dry_run
elif [ "$has_cgroup_fs" = "1" ]; then
  if ! is_mounted "$FRZ_DIR"; then
    mk_dir "$FRZ_DIR" 0755 0:0
    "$B" mount -t cgroup -o freezer none "$FRZ_DIR" 2>/dev/null || true
  fi
  if is_mounted "$FRZ_DIR"; then
    if "$B" mount --bind "$FRZ_DIR" "$CGROUP_ROOT/freezer" 2>/dev/null; then
      cg_freezer=mounted_bind_frz
    else
      cg_freezer=bind_failed
    fi
  elif "$B" mount -t cgroup -o freezer none "$CGROUP_ROOT/freezer" 2>/dev/null; then
    cg_freezer=mounted
  else
    cg_freezer=unavailable
  fi
else
  cg_freezer=no_kernel_cgroup
fi

cg_systemd=skipped
if is_mounted "$CGROUP_ROOT/systemd"; then
  cg_systemd=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  cg_systemd=dry_run
elif [ "$has_cgroup_fs" = "1" ]; then
  if "$B" mount -t cgroup -o none,name=systemd cgroup "$CGROUP_ROOT/systemd" 2>/dev/null; then
    cg_systemd=mounted
  else
    cg_systemd=unavailable
  fi
else
  cg_systemd=no_kernel_cgroup
fi

cg_devices=skipped
if is_mounted "$CGROUP_ROOT/devices"; then
  cg_devices=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  cg_devices=dry_run
elif [ "$has_cgroup_fs" = "1" ]; then
  if "$B" mount -t cgroup -o devices none "$CGROUP_ROOT/devices" 2>/dev/null; then
    cg_devices=mounted
  else
    cg_devices=unavailable
  fi
else
  cg_devices=no_kernel_cgroup
fi
say "ANCO_M4_CGROUP root=$cg_root_state freezer=$cg_freezer systemd=$cg_systemd devices=$cg_devices path=$CGROUP_ROOT"

# 4. Mount binderfs onto $R/dev/binderfs (idempotent).
BINDERFS_DIR="$R/dev/binderfs"
mk_dir "$BINDERFS_DIR" 0755 0:0
binderfs_state=skipped
if is_mounted "$BINDERFS_DIR"; then
  binderfs_state=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  binderfs_state=dry_run
elif [ "$has_binder_fs" = "1" ]; then
  if "$B" mount -t binder binder "$BINDERFS_DIR" 2>/dev/null; then
    binderfs_state=mounted
  else
    binderfs_state=mount_failed
  fi
else
  binderfs_state=no_kernel_binderfs
fi
binder_ctrl=missing
[ -e "$BINDERFS_DIR/binder-control" ] && binder_ctrl=present
say "ANCO_M4_BINDERFS state=$binderfs_state control=$binder_ctrl path=$BINDERFS_DIR"

# 5. Create /mnt/isula and bind-mount /data/service/el1/public/rgm_engine/isula/var -> /mnt/isula/var.
ISULA_SRC="$R/data/service/el1/public/rgm_engine/isula/var"
ISULA_MNT="$R/mnt/isula"
ISULA_VAR="$ISULA_MNT/var"
mk_dir "$ISULA_MNT" 0755 0:0
if [ -L "$ISULA_VAR" ]; then
  "$B" rm -f "$ISULA_VAR" 2>/dev/null || true
fi
mk_dir "$ISULA_VAR" 0700 0:0
isula_bind=skipped
if is_mounted "$ISULA_VAR"; then
  isula_bind=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  isula_bind=dry_run
else
  if "$B" mount --bind "$ISULA_SRC" "$ISULA_VAR" 2>/dev/null; then
    isula_bind=mounted
  else
    isula_bind=bind_failed
  fi
fi
say "ANCO_M4_ISULA_VAR state=$isula_bind src=$ISULA_SRC dst=$ISULA_VAR"

# 6. Check & log presence of agi.img / anco_hmos.img container images.
IMG_AGI="$R/data/service/el1/public/rgm_manager/data/rgm_hmos/image/agi.img"
IMG_FUSION="$R/data/service/el1/public/hmos_fusion_manager/anco_hmos.img"
IMG_SYSPROD="$R/sys_prod/opt/virt_service/rgm_hmos/anco_hmos.img"
agi_status=missing
fusion_img_status=missing
sysprod_img_status=missing
[ -f "$IMG_AGI" ] && agi_status=present
[ -f "$IMG_FUSION" ] && fusion_img_status=present
[ -f "$IMG_SYSPROD" ] && sysprod_img_status=present
data_entries=$("$B" ls -A "$R/data/virt_service/rgm_hmos/anco_hmos_data" 2>/dev/null | "$B" wc -l | "$B" tr -d ' \t\r\n' || echo 0)
say "ANCO_M4_IMG agi=$agi_status fusion_anco_img=$fusion_img_status sysprod_anco_img=$sysprod_img_status anco_data_entries=$data_entries"

# 7. Set donor hmos_fusion_manager params and trigger marble.anco.ready=true.
broker_ctl="unset"
perf_ver="unset"
if [ "$DRY_RUN" = "1" ]; then
  broker_ctl=dry_run
  perf_ver=dry_run
  say "ANCO_M4_PARAMS state=dry_run support_hmos=true rgm_enable=true marble_anco_ready=true broker_ctl=$broker_ctl perf_ver=$perf_ver"
elif [ -x "$R/system/bin/param" ]; then
  "$B" chroot "$R" /system/bin/param set persist.hmos_fusion_mgr.ctl.support_hmos true >/dev/null 2>&1 || say "ANCO_M4_PARAM_WARN key=persist.hmos_fusion_mgr.ctl.support_hmos rc=$?"
  "$B" chroot "$R" /system/bin/param set persist.hmos_fusion_mgr.ctl.rgm.enable true >/dev/null 2>&1 || say "ANCO_M4_PARAM_WARN key=persist.hmos_fusion_mgr.ctl.rgm.enable rc=$?"
  "$B" chroot "$R" /system/bin/param set marble.anco.ready true >/dev/null 2>&1 || say "ANCO_M4_PARAM_WARN key=marble.anco.ready rc=$?"
  # Bounded <= 2s settle for param:marble-anco-ready to publish broker/perf params.
  w=0
  while [ "$w" -lt 4 ]; do
    broker_ctl=$("$B" chroot "$R" /system/bin/param get startup.service.ctl.broker 2>/dev/null | "$B" tr -d ' \t\r\n' || true)
    perf_ver=$("$B" chroot "$R" /system/bin/param get hmos_fusion_mgr.oh.perf.version 2>/dev/null | "$B" tr -d ' \t\r\n' || true)
    [ "$broker_ctl" = "2" ] && break
    w=$((w + 1))
    "$B" sleep 0.5
  done
  say "ANCO_M4_PARAMS state=set support_hmos=true rgm_enable=true marble_anco_ready=true broker_ctl=${broker_ctl:-unset} perf_ver=${perf_ver:-unset}"
else
  say "ANCO_M4_ERR=missing_param_bin"
  exit 0
fi

# 8. Read-only probe of updater_sa (SA 3006) + on-demand ANCO SAs (65601, 65604,
#    65610, 66818, 66854) and process table. On-demand SAs remain idle until
#    activated by samgr/hmos_fusion_manager, so offline here is non-fatal.
check_sa() {
  sa_id="$1"
  if [ "$DRY_RUN" = "1" ]; then
    echo "dry_run"
    return 0
  fi
  if [ -x "$R/compat/bin/native_sa_check" ]; then
    if "$B" timeout 2 "$B" chroot "$R" /compat/bin/native_sa_check "$sa_id" >/dev/null 2>&1; then
      echo "online"
      return 0
    fi
  elif [ -x "$R/system/bin/hidumper" ]; then
    if "$B" timeout 2 "$B" chroot "$R" /system/bin/hidumper -ls 2>/dev/null | "$B" grep -qw "$sa_id"; then
      echo "online"
      return 0
    fi
  fi
  echo "ondemand_idle"
  return 0
}

pid_of() {
  pat="$1"
  if [ "$DRY_RUN" = "1" ]; then
    echo "dry_run"
    return 0
  fi
  p=$("$B" pgrep -f "$pat" 2>/dev/null | "$B" head -n 1 || true)
  echo "${p:-none}"
}

sa3006=$(check_sa 3006)
sa65601=$(check_sa 65601)
sa65604=$(check_sa 65604)
sa65610=$(check_sa 65610)
sa66816=$(check_sa 66816)
sa66818=$(check_sa 66818)
sa66854=$(check_sa 66854)

say "ANCO_M4_SA sa3006=$sa3006 sa65601=$sa65601 sa65604=$sa65604 sa65610=$sa65610 sa66816=$sa66816 sa66818=$sa66818 sa66854=$sa66854"
say "ANCO_M4_PROCS updater_sa=$(pid_of 'updater_sa\.json') bms_broker=$(pid_of 'bms_broker\.json') broker=$(pid_of '/system/bin/anco_service_broker') rgm_manager=$(pid_of 'rgm_manager\.json') rgm_engine_plugin=$(pid_of 'rgm_engine_plugin\.json') virt_device=$(pid_of 'virt_device\.json') hmos_fusion_manager=$(pid_of 'hmos_fusion_manager\.json') start_anco_audio=$(pid_of '/system/bin/anco_audio') rgm_engine=$(pid_of 'isulad')"
say "ANCO_M4_DONE=1"
exit 0
