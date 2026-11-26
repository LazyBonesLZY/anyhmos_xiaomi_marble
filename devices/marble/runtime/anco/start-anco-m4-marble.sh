#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch M4 (Part A): Prepare DroiTong / ANCO container runtime environment and
# activate rgm_manager (SA 65601), rgm_engine_plugin (SA 65604),
# virt_device (SA 65610), hmos_fusion_manager (SA 66854), and anco_audio
# on Xiaomi marble (SM7475).
# Strictly opt-in / post-boot gated (marble.anco.ready=true); zero boot-block risk.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
PROC_MOUNTS=${MARBLE_PROC_MOUNTS:-/proc/mounts}
PROC_FILESYSTEMS=${MARBLE_PROC_FILESYSTEMS:-/proc/filesystems}
DRY_RUN=${MARBLE_ANCO_DRY_RUN:-0}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,anco,*|*,rgm,*|*,droitong,*)
    say "ANCO_M4_SKIP=requested"
    exit 0
    ;;
esac

# Exact numeric UID:GID pairs verified against donor /system/etc/{passwd,group}:
#   root=0, system=1000, radio=1001, media=1013, media_rw=1023, audio=1041,
#   virt_device=3075, rgm_manager=5002, anco_service_broker=5557,
#   input=6696, hmos_fusion_manager=7558
mk_dir() {
  dpath="$1"; dmode="$2"; downer="$3"
  $B mkdir -p "$dpath" 2>/dev/null || true
  $B chown "$downer" "$dpath" 2>/dev/null || true
  $B chmod "$dmode" "$dpath" 2>/dev/null || true
}

is_mounted() {
  target="$1"
  $B grep -q " $target " "$PROC_MOUNTS" 2>/dev/null
}

# 1. Verify kernel container prerequisites and staged isula binaries.
has_binder_fs=0
has_overlay_fs=0
if [ -r "$PROC_FILESYSTEMS" ]; then
  $B grep -qw 'binder' "$PROC_FILESYSTEMS" 2>/dev/null && has_binder_fs=1 || true
  $B grep -qw 'overlay' "$PROC_FILESYSTEMS" 2>/dev/null && has_overlay_fs=1 || true
fi
isula_bins=0
for ibin in isulad lxc-start lxc-attach rgm_engine.sh; do
  if [ -e "$R/system/bin/virt_service/rgm_engine/isula/bin/$ibin" ]; then
    isula_bins=$((isula_bins + 1))
  fi
done
say "ANCO_M4_PRECHECK binder_fs=$has_binder_fs overlay_fs=$has_overlay_fs isula_bins=$isula_bins/4"

# 2. Prepare directories with exact donor UID/GID permissions.
# Create child directories BEFORE tightening parent permissions (e.g. rgm_engine 0505).
mk_dir "$R/data/service/el1/public/rgm_manager" 0770 5002:5002
mk_dir "$R/data/service/el1/public/rgm_manager/data" 0770 5002:5002
mk_dir "$R/data/service/el1/public/rgm_manager/bridge_network" 0700 5002:5002

$B mkdir -p "$R/data/service/el1/public/rgm_engine/isula/var" 2>/dev/null || true
$B chown 0:0 "$R/data/service/el1/public/rgm_engine" \
  "$R/data/service/el1/public/rgm_engine/isula" \
  "$R/data/service/el1/public/rgm_engine/isula/var" 2>/dev/null || true
$B chmod 0700 "$R/data/service/el1/public/rgm_engine/isula/var" 2>/dev/null || true
$B chmod 0705 "$R/data/service/el1/public/rgm_engine/isula" 2>/dev/null || true
$B chmod 0505 "$R/data/service/el1/public/rgm_engine" 2>/dev/null || true

mk_dir "$R/data/service/el1/public/hmos_fusion_manager" 0711 7558:7558
mk_dir "$R/data/service/el1/public/bundle_framework_broker" 0711 5557:5557

mk_dir "$R/data/virt_service" 0701 0:0
mk_dir "$R/data/virt_service/anco_decoupling" 0755 7558:7558
mk_dir "$R/data/virt_service/rgm_hmos" 0701 0:0
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data" 0755 0:0
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_cache" 0755 0:0
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_log" 0775 0:1000
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/keystore" 0755 0:0
for csub in container container/clone container/clone_data container/backup \
  container/backup_data container/hisuite container/hisuite_data; do
  mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/$csub" 0755 0:0
done
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/cota" 0775 1000:1000
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/cota/anco" 0755 7558:7558
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/cota/anco_sdk" 0755 7558:7558
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/cota/anco_dms" 0755 1000:1000
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/cota/anco_dms/logicCamera" 0755 1000:1000
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/cota/anco_dms/logicCamera/lap" 0755 1000:1000
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/cota/anco_dms/logicCamera/grlb" 0755 1000:1000
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/settings" 0755 1000:1000
mk_dir "$R/data/virt_service/rgm_hmos/anco_hmos_data/settings/users" 0755 1000:1000

mk_dir "$R/data/virt_service/rgm_linux" 0701 0:0
mk_dir "$R/data/virt_service/virt_comm" 0701 0:0
mk_dir "$R/data/virt_service/virt_comm/rgm_linux" 0701 0:0
mk_dir "$R/data/virt_service/virt_comm/rgm_linux/uds" 0711 0:0
mk_dir "$R/data/virt_service/virt_comm/rgm_hmos" 0701 0:0
mk_dir "$R/data/virt_service/virt_comm/rgm_hmos/uds" 0701 0:0
mk_dir "$R/data/virt_service/virt_comm/rgm_hmos/uds/virt_ril" 0710 1001:1001
mk_dir "$R/data/virt_service/virt_comm/rgm_hmos/uds/anco_audio" 0701 1041:1041
mk_dir "$R/data/virt_service/virt_comm/rgm_hmos/uds/cameradaemon_broker" 0711 1013:1013
mk_dir "$R/data/virt_service/virt_comm/rgm_hmos/uds/input" 0701 6696:6696

mk_dir "$R/data/virt_service/rgm_hmos/cross" 0701 0:0
mk_dir "$R/data/virt_service/rgm_hmos/cross/uds" 0701 0:0
mk_dir "$R/data/virt_service/rgm_hmos/cross/uds/anco_audio" 0701 1041:1041
mk_dir "$R/data/virt_service/rgm_hmos/cross/uds/input" 0701 6696:6696

for uds in virt_device_broker virt_factory_broker virt_sensors_broker \
  virt_vibrator_broker virt_input_broker virt_gnss_broker \
  virt_perfgenius_broker virt_wifi_broker virt_keymaster_broker \
  virt_consumerir_broker; do
  mk_dir "$R/data/virt_service/rgm_hmos/cross/uds/$uds" 0711 3075:3075
  mk_dir "$R/data/virt_service/virt_comm/rgm_hmos/uds/$uds" 0711 3075:3075
done

# 3. Mount binderfs onto $R/dev/binderfs (idempotent).
BINDERFS_DIR="$R/dev/binderfs"
mk_dir "$BINDERFS_DIR" 0755 0:0
binderfs_state=skipped
if is_mounted "$BINDERFS_DIR"; then
  binderfs_state=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  binderfs_state=dry_run
elif [ "$has_binder_fs" = "1" ]; then
  if $B mount -t binder binder "$BINDERFS_DIR" 2>/dev/null; then
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

# 4. Create /mnt/isula and bind-mount /data/service/el1/public/rgm_engine/isula/var -> /mnt/isula/var.
ISULA_SRC="$R/data/service/el1/public/rgm_engine/isula/var"
ISULA_MNT="$R/mnt/isula"
ISULA_VAR="$ISULA_MNT/var"
mk_dir "$ISULA_MNT" 0755 0:0
if [ -L "$ISULA_VAR" ]; then
  $B rm -f "$ISULA_VAR" 2>/dev/null || true
fi
mk_dir "$ISULA_VAR" 0700 0:0
isula_bind=skipped
if is_mounted "$ISULA_VAR"; then
  isula_bind=already_mounted
elif [ "$DRY_RUN" = "1" ]; then
  isula_bind=dry_run
else
  if $B mount --bind "$ISULA_SRC" "$ISULA_VAR" 2>/dev/null; then
    isula_bind=mounted
  else
    isula_bind=bind_failed
  fi
fi
say "ANCO_M4_ISULA_VAR state=$isula_bind src=$ISULA_SRC dst=$ISULA_VAR"

# 5. Check & log presence of agi.img / anco_hmos.img container images.
IMG_AGI="$R/data/service/el1/public/rgm_manager/data/rgm_hmos/image/agi.img"
IMG_FUSION="$R/data/service/el1/public/hmos_fusion_manager/anco_hmos.img"
IMG_SYSPROD="$R/sys_prod/opt/virt_service/rgm_hmos/anco_hmos.img"
agi_status=missing
fusion_img_status=missing
sysprod_img_status=missing
[ -f "$IMG_AGI" ] && agi_status=present
[ -f "$IMG_FUSION" ] && fusion_img_status=present
[ -f "$IMG_SYSPROD" ] && sysprod_img_status=present
data_entries=$($B ls -A "$R/data/virt_service/rgm_hmos/anco_hmos_data" 2>/dev/null | $B wc -l | $B tr -d ' \t\r\n' || echo 0)
say "ANCO_M4_IMG agi=$agi_status fusion_anco_img=$fusion_img_status sysprod_anco_img=$sysprod_img_status anco_data_entries=$data_entries"

# 6. Set donor hmos_fusion_manager params and trigger marble.anco.ready=true.
if [ "$DRY_RUN" = "1" ]; then
  say "ANCO_M4_PARAMS state=dry_run support_hmos=true rgm_enable=true marble_anco_ready=true"
elif [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set persist.hmos_fusion_mgr.ctl.support_hmos true >/dev/null 2>&1 || say "ANCO_M4_PARAM_WARN key=persist.hmos_fusion_mgr.ctl.support_hmos rc=$?"
  $B chroot "$R" /system/bin/param set persist.hmos_fusion_mgr.ctl.rgm.enable true >/dev/null 2>&1 || say "ANCO_M4_PARAM_WARN key=persist.hmos_fusion_mgr.ctl.rgm.enable rc=$?"
  $B chroot "$R" /system/bin/param set marble.anco.ready true >/dev/null 2>&1 || say "ANCO_M4_PARAM_WARN key=marble.anco.ready rc=$?"
  say "ANCO_M4_PARAMS state=set support_hmos=true rgm_enable=true marble_anco_ready=true"
else
  say "ANCO_M4_ERR=missing_param_bin"
  exit 0
fi

# 7. Probe SAs 65601 (rgm_manager), 65604 (rgm_engine_plugin), 65610 (virt_device),
#    66854 (hmos_fusion_manager) and process status (bounded <= 8s).
check_sa() {
  sa_id="$1"
  if [ "$DRY_RUN" = "1" ]; then
    echo "dry_run"
    return 0
  fi
  if [ -x "$R/compat/bin/native_sa_check" ]; then
    if $B timeout 2 $B chroot "$R" /compat/bin/native_sa_check "$sa_id" >/dev/null 2>&1; then
      echo "online"
      return 0
    fi
  elif [ -x "$R/system/bin/hidumper" ]; then
    if $B timeout 2 $B chroot "$R" /system/bin/hidumper -ls 2>/dev/null | $B grep -qw "$sa_id"; then
      echo "online"
      return 0
    fi
  fi
  echo "offline"
  return 1
}

sa65601=offline
sa65604=offline
sa65610=offline
sa66854=offline
wait_steps=0
if [ "$DRY_RUN" = "1" ]; then
  sa65601=dry_run
  sa65604=dry_run
  sa65610=dry_run
  sa66854=dry_run
else
  i=0
  while [ "$i" -lt 16 ]; do
    sa65601=$(check_sa 65601 || true)
    sa65604=$(check_sa 65604 || true)
    sa65610=$(check_sa 65610 || true)
    sa66854=$(check_sa 66854 || true)
    wait_steps=$((i + 1))
    if [ "$sa65601" = "online" ] && [ "$sa65604" = "online" ] && \
       [ "$sa65610" = "online" ] && [ "$sa66854" = "online" ]; then
      break
    fi
    i=$((i + 1))
    $B sleep 0.5
  done
fi

pid_of() {
  pat="$1"
  if [ "$DRY_RUN" = "1" ]; then
    echo "dry_run"
    return 0
  fi
  p=$($B pgrep -f "$pat" 2>/dev/null | $B head -n 1 || true)
  echo "${p:-none}"
}

say "ANCO_M4_SA sa65601=$sa65601 sa65604=$sa65604 sa65610=$sa65610 sa66854=$sa66854 wait_steps=$wait_steps"
say "ANCO_M4_PROCS rgm_manager=$(pid_of 'rgm_manager\.json') rgm_engine_plugin=$(pid_of 'rgm_engine_plugin\.json') virt_device=$(pid_of 'virt_device\.json') hmos_fusion_manager=$(pid_of 'hmos_fusion_manager\.json') anco_audio=$(pid_of '/system/bin/anco_audio')"
say "ANCO_M4_DONE=1"
exit 0
