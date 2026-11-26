#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch U2 Stage V1: Qualcomm SM7475 Venus VPU firmware + videocc-waipio + msm_video (/dev/video*).
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
KDIR=/hmos/vendor_dlkm/lib/modules
[ -d "$KDIR" ] || KDIR="$R/vendor_dlkm/lib/modules"
[ -d "$KDIR" ] || KDIR=/lib/modules

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,venus,*)
    say "VENUS_SKIP=requested"
    exit 0
    ;;
esac

# 1. Verify vpu20_4v firmware in /compat/firmware and set firmware_class search path.
$B mkdir -p /compat/firmware "$R/compat/firmware"
if [ ! -f /compat/firmware/vpu20_4v.mbn ] && [ -f "$R/compat/firmware/vpu20_4v.mbn" ]; then
  for fw in "$R"/compat/firmware/vpu20_4v.*; do
    [ -f "$fw" ] || continue
    $B ln -sf "$fw" "/compat/firmware/${fw##*/}" 2>/dev/null || $B cp "$fw" "/compat/firmware/${fw##*/}" 2>/dev/null || true
  done
fi
fw_cnt=$($B ls -1 /compat/firmware/vpu20_4v.* 2>/dev/null | $B wc -l | $B tr -d ' \t\r\n' || echo 0)
say "VENUS_FW_COUNT=${fw_cnt:-0}"
if [ "${fw_cnt:-0}" -lt 14 ] || [ ! -f /compat/firmware/vpu20_4v.mbn ]; then
  say "VENUS_ERR=missing_vpu20_4v_fw"
  exit 0
fi
if $B test -w /sys/module/firmware_class/parameters/path; then
  $B printf '/compat/firmware' > /sys/module/firmware_class/parameters/path 2>/dev/null || true
fi

# 2. Load videocc-waipio.ko (qcom,cape-videocc DT) then msm_video.ko (qcom,vidc@aa00000).
load_ko() {
  ko="$1"
  mod=$(printf '%s' "${ko%.ko}" | $B tr - _)
  if $B grep -q "^$mod " /proc/modules 2>/dev/null; then
    say "VENUS_KO $ko=present"
    return 0
  fi
  if [ -f "$KDIR/$ko" ] && $B insmod "$KDIR/$ko" 2>/dev/null; then
    say "VENUS_KO $ko=loaded"
    return 0
  fi
  say "VENUS_KO $ko=failed"
  return 1
}

load_ko videocc-waipio.ko || true
load_ko msm_video.ko || {
  $B dmesg | $B grep -iE 'vidc|venus|videocc|vpu' | $B tail -n 15 || true
  exit 0
}

# 3. Wait up to 4s for /sys/class/video4linux/video* and create device nodes in /dev and $R/dev.
i=0
v_cnt=0
while [ "$i" -lt 20 ]; do
  v_cnt=0
  for d in /sys/class/video4linux/video*; do
    [ -d "$d" ] && v_cnt=$((v_cnt + 1))
  done
  [ "$v_cnt" -ge 2 ] && break
  i=$((i + 1))
  $B sleep 0.2
done

created=0
for d in /sys/class/video4linux/video*; do
  [ -d "$d" ] || continue
  node=${d##*/}
  mm=$($B cat "$d/dev" 2>/dev/null || true)
  maj=${mm%:*}
  min=${mm#*:}
  [ -n "$maj" ] && [ -n "$min" ] || continue
  vname=$($B cat "$d/name" 2>/dev/null | $B tr '\n' ' ' || echo unknown)
  for root in "" "$R"; do
    devp="$root/dev/$node"
    [ -c "$devp" ] || $B mknod "$devp" c "$maj" "$min" 2>/dev/null || true
    $B chmod 0666 "$devp" 2>/dev/null || true
  done
  created=$((created + 1))
  say "VENUS_V4L2 node=$node dev=$maj:$min name=$vname"
done
say "VENUS_NODES_READY count=$created wait_steps=$i"
exit 0
