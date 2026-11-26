#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch M4 (Part B): Load SM7475 Xiaomi thermal interface kernel module
# (mi_thermal_interface.ko + ADC dependencies), stage minimal Bionic chroot
# from marble-mi-thermal.tar.gz using existing /compat/audio-runtime, and
# trigger OHOS init service marble_mi_thermald via native_thermal_launcher_marble.
# Strictly opt-in / post-boot gated (marble.mi_thermal.ready=true).
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
PROC_MODULES=${MARBLE_PROC_MODULES:-/proc/modules}
PROC_MOUNTS=${MARBLE_PROC_MOUNTS:-/proc/mounts}
THERMAL_ROOT=${MARBLE_THERMAL_ROOT:-$R/data/local/tmp/hmos-mi-thermal/root}
DRY_RUN=${MARBLE_THERMAL_DRY_RUN:-0}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,mi_thermal,*|*,mi_thermald,*|*,thermal_daemon,*)
    say "MI_THERMAL_M4_SKIP=requested"
    exit 0
    ;;
esac

find_ko() {
  ko="$1"
  for dir in \
    "${MARBLE_KMOD_DIR:-}" \
    "/graphics" \
    "/hmos/vendor_dlkm/lib/modules" \
    "$R/vendor_dlkm/lib/modules" \
    "/vendor_dlkm/lib/modules" \
    "/lib/modules" \
    "$R/lib/modules"; do
    [ -n "$dir" ] || continue
    if [ -f "$dir/$ko" ]; then
      echo "$dir/$ko"
      return 0
    fi
  done
  return 1
}

load_ko() {
  ko="$1"
  mod=$(printf '%s' "${ko%.ko}" | $B tr '-' '_')
  if [ -d "/sys/module/$mod" ] || $B grep -q "^$mod " "$PROC_MODULES" 2>/dev/null; then
    echo "loaded"
    return 0
  fi
  kpath=$(find_ko "$ko" || true)
  if [ -z "$kpath" ]; then
    echo "missing"
    return 1
  fi
  if [ "$DRY_RUN" = "1" ]; then
    echo "dry_run:$kpath"
    return 0
  fi
  if $B insmod "$kpath" >/dev/null 2>&1; then
    echo "insmod_ok"
    return 0
  fi
  echo "insmod_fail"
  return 1
}

# 1. Ensure SM7475 ADC thermistor drivers, panel_event_notifier, and mi_thermal_interface are loaded.
st_panel=$(load_ko panel_event_notifier.ko || true)
st_vadc=$(load_ko qcom-vadc-common.ko || true)
st_adc5=$(load_ko qcom-spmi-adc5.ko || true)
st_adctm=$(load_ko qti-adc-tm.ko || true)
st_mi=$(load_ko mi_thermal_interface.ko || true)
has_msg=0
[ -d /sys/class/thermal/thermal_message ] && has_msg=1
say "MI_THERMAL_M4_KMOD panel=$st_panel vadc=$st_vadc adc5=$st_adc5 adctm=$st_adctm mi_thermal=$st_mi sysfs_msg=$has_msg"

# 2. Locate marble-mi-thermal.tar.gz payload.
TAR=${MARBLE_THERMAL_TAR:-}
if [ -z "$TAR" ] || [ ! -f "$TAR" ]; then
  for cand in \
    "/graphics/marble-mi-thermal.tar.gz" \
    "$R/compat/etc/marble-mi-thermal.tar.gz" \
    "$R/data/local/tmp/marble-mi-thermal.tar.gz"; do
    if [ -f "$cand" ]; then
      TAR="$cand"
      break
    fi
  done
fi
if [ -z "$TAR" ] || [ ! -f "$TAR" ]; then
  say "MI_THERMAL_M4_ERR=missing_thermal_tar"
  exit 0
fi

# 3. Extract minimal marble thermal chroot and bind-mount /sys, /proc, /dev, /compat/audio-runtime.
$B mkdir -p "$THERMAL_ROOT" 2>/dev/null || {
  say "MI_THERMAL_M4_ERR=mkdir_root_failed"
  exit 0
}
if ! $B gzip -dc "$TAR" | $B tar -xf - -C "$THERMAL_ROOT" 2>/dev/null; then
  say "MI_THERMAL_M4_ERR=extract_failed"
  exit 0
fi

$B mkdir -p \
  "$THERMAL_ROOT/sys" \
  "$THERMAL_ROOT/proc" \
  "$THERMAL_ROOT/dev" \
  "$THERMAL_ROOT/system/bin" \
  "$THERMAL_ROOT/compat/audio-runtime" \
  "$THERMAL_ROOT/data/vendor/thermal/config" 2>/dev/null || true
$B chmod 0755 "$THERMAL_ROOT" "$THERMAL_ROOT/data" "$THERMAL_ROOT/data/vendor" 2>/dev/null || true
$B chmod 0775 "$THERMAL_ROOT/data/vendor/thermal" "$THERMAL_ROOT/data/vendor/thermal/config" 2>/dev/null || true

# Expose linker64 at /system/bin/linker64 inside the chroot as a convenience symlink.
if [ ! -e "$THERMAL_ROOT/system/bin/linker64" ]; then
  $B ln -sf ../../compat/audio-runtime/bin/linker64 "$THERMAL_ROOT/system/bin/linker64" 2>/dev/null || true
fi

ART_SRC="$R/compat/audio-runtime"
if [ ! -d "$ART_SRC" ] && [ -d "/compat/audio-runtime" ]; then
  ART_SRC="/compat/audio-runtime"
fi
if [ ! -x "$ART_SRC/bin/linker64" ]; then
  say "MI_THERMAL_M4_ERR=missing_audio_runtime_linker64 src=$ART_SRC"
  exit 0
fi

bind_chroot() {
  src="$1"; dst="$2"
  if $B grep -q " $dst " "$PROC_MOUNTS" 2>/dev/null; then
    return 0
  fi
  if [ "$DRY_RUN" = "1" ]; then
    return 0
  fi
  $B mount --bind "$src" "$dst" 2>/dev/null
}

bind_fail=0
bind_chroot /sys "$THERMAL_ROOT/sys" || bind_fail=$((bind_fail + 1))
bind_chroot /proc "$THERMAL_ROOT/proc" || bind_fail=$((bind_fail + 1))
bind_chroot /dev "$THERMAL_ROOT/dev" || bind_fail=$((bind_fail + 1))
bind_chroot "$ART_SRC" "$THERMAL_ROOT/compat/audio-runtime" || bind_fail=$((bind_fail + 1))

conf_cnt=$($B ls "$THERMAL_ROOT/vendor/etc"/thermal*.conf 2>/dev/null | $B wc -l | $B tr -d ' \t\r\n' || echo 0)
say "MI_THERMAL_M4_CHROOT root=$THERMAL_ROOT tar=$TAR confs=$conf_cnt art_src=$ART_SRC bind_fail=$bind_fail"
if [ "$bind_fail" -gt 0 ]; then
  say "MI_THERMAL_M4_ERR=chroot_bind_failed count=$bind_fail"
  exit 0
fi

# 4. Stage native_thermal_launcher_marble if shipped in /graphics and not yet staged.
LAUNCHER_DST="$R/compat/bin/native_thermal_launcher_marble"
if [ ! -x "$LAUNCHER_DST" ] && [ -f "/graphics/native_thermal_launcher_marble" ]; then
  $B mkdir -p "$R/compat/bin" 2>/dev/null || true
  $B cp "/graphics/native_thermal_launcher_marble" "$LAUNCHER_DST" 2>/dev/null || true
  $B chmod 0755 "$LAUNCHER_DST" 2>/dev/null || true
fi
if [ ! -x "$LAUNCHER_DST" ]; then
  say "MI_THERMAL_M4_ERR=missing_launcher path=$LAUNCHER_DST"
  exit 0
fi

# 5. Trigger OHOS init service marble_mi_thermald.
if [ "$DRY_RUN" = "1" ]; then
  say "MI_THERMAL_M4_PARAM state=dry_run key=marble.mi_thermal.ready val=true"
  say "MI_THERMAL_M4_PROCS mi_thermald_pid=dry_run wait_steps=0"
  say "MI_THERMAL_M4_DONE=1"
  exit 0
fi

if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.mi_thermal.ready true >/dev/null 2>&1 || say "MI_THERMAL_M4_PARAM_RC=$?"
else
  say "MI_THERMAL_M4_ERR=missing_param_bin"
  exit 0
fi

# 6. Wait up to 6s for mi_thermald process.
td_pid=""
i=0
while [ "$i" -lt 30 ]; do
  td_pid=$($B pgrep -f 'mi_thermald' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$td_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "MI_THERMAL_M4_PROCS mi_thermald_pid=${td_pid:-none} wait_steps=$i"
say "MI_THERMAL_M4_DONE=1"
exit 0
