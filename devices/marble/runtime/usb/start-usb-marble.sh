#!/busybox sh
# SPDX-License-Identifier: MIT
#
# marble (SM7475/ukee) USB gadget bring-up for HarmonyOS NEXT. Kernel
# (work/marble-baseline/kernel-config.gz): CONFIG_USB_CONFIGFS/_F_FS/
# _MASS_STORAGE = y, no f_mtp / no Huawei f_generic -> the OHOS usbfn_mtp HAL
# cannot bind; evidence chain in work/marble-usb-report.md.
# NAMESPACE: HOST namespace, never chroot; only configfs plus the FunctionFS node
# at the exact path hdcd reads, existing init identity (hdcd, uid shell,
# u:r:hdcd:s0). No chown/chmod/su, no devmode flip, no TCP, no root shell.
# ORDER: configfs before /config; g1 creates its default groups, so probe
# FunctionFS after it; the bind gate is ep1+ep2 in THIS instance's functionfs
# mount (never a param echo); exclude existing bindings first; UDC bind last.
# MODE=hdc|mtp|storage|hdc,storage  (default hdc)
# Exit codes:
#   0  ok / nothing to do (already-bound reported, not rewritten)
#   41 no UDC (controller not registered)
#   42 no function selected / unknown MODE part
#   43 UDC bind failed (readback mismatch)
#   44 missing mass-storage image
#   45 MODE=mtp without USB_MTP_RESPONDER=1
#   46 not in the host namespace
#   47 UDC already held; USB kept
#   48 ep0 missing or wrong uid/gid
#   49 wrong link/mount, missing endpoints, or stale daemon
#   50 hdcd precondition missing (developer mode)
#   51 configfs mount failed; USB kept
#   52 FunctionFS instance cannot be created
#   53 virtual UDC (dummy_hcd); no physical host can see it
set -eu

B=${USB_BUSYBOX:-/busybox}
R=${USB_OHOS_ROOT:-/hmos-root}
SYSFS=${USB_SYSFS:-/sys}
MOUNT_TABLE=${USB_MOUNT_TABLE:-/proc/mounts}
M="$R/vendor_dlkm/lib/modules"

CFG=${USB_CONFIGFS:-/config}
GCFG="$CFG/usb_gadget/g1"
DCONF="$GCFG/configs/b.1"
FFS_ROOT="$R/dev/usb-ffs"

MODE=${MODE:-hdc}
USB_VID=${USB_VID:-0x2717}
USB_PID=${USB_PID:-0xff48}
USB_MAXPOWER=${USB_MAXPOWER:-900}
USB_UDC=${USB_UDC:-}
USB_STORAGE_IMAGE=${USB_STORAGE_IMAGE:-}
USB_MTP_RESPONDER=${USB_MTP_RESPONDER:-0}
# Donor identity (hdcd 2000/2000, marble mtp 1024/1024).
USB_FFS_UID=${USB_FFS_UID:-2000}
USB_FFS_GID=${USB_FFS_GID:-2000}
USB_FFS_MTP_UID=${USB_FFS_MTP_UID:-1024}
USB_FFS_MTP_GID=${USB_FFS_MTP_GID:-1024}
USB_FFS_RMODE=${USB_FFS_RMODE:-0770}
USB_FFS_FMODE=${USB_FFS_FMODE:-0660}
USB_WAIT_FFS_SECONDS=${USB_WAIT_FFS_SECONDS:-10}
USB_WAIT_UDC_SECONDS=${USB_WAIT_UDC_SECONDS:-10}
USB_UDC_RETRY=${USB_UDC_RETRY:-5}

stage=preflight
 trap 'c=$?; echo "USB_START_EXIT=$c STAGE=$stage"' EXIT

say() { echo "USB_STAGE=$1 $2"; }

# busybox sleep may not accept fractions.
nap() { $B sleep "$1" 2>/dev/null || $B sleep 1; }

# param get pads values; strip the padding before comparing.
oget() {
 v=$($B chroot "$R" /system/bin/param get "$1" 2>/dev/null) || return 0
 printf '%s\n' "$v" | $B sed 's/[[:space:]]*$//'
}
setp() { $B chroot "$R" /system/bin/param set "$1" "$2" 2>/dev/null || true; }

# keep_usb <reason> <code>: leave the current USB state untouched.
keep_usb() {
 echo "USB_KEEP_ORIGINAL reason=$1"
 echo "USB_KEEP_ORIGINAL_UDC=$($B cat "$GCFG/UDC" 2>/dev/null || echo unknown)"
 exit "$2"
}

# mk <dir>: configfs fails on an existing entry, so check first.
mk() {
 if [ -d "$1" ]; then
  return 0
 fi
 $B mkdir "$1"
}

# Optional writers; missing nodes only log.
write_attr() {
 if [ ! -e "$1" ]; then
  echo "USB_ATTR_SKIP_MISSING $1"
  return 0
 fi
 if [ -n "${2:-}" ]; then
  printf '%s' "$2" > "$1"
 else
  # kernfs drops 0-byte writes; "\n" is the documented detach value.
  printf '\n' > "$1"
 fi
}

# Mandatory attribute: never bind a half-configured gadget.
write_attr_req() {
 if [ ! -e "$1" ]; then
  echo "USB_ATTR_MISSING_REQUIRED $1"
  return 1
 fi
 if [ -n "${2:-}" ]; then
  printf '%s' "$2" > "$1"
 else
  printf '\n' > "$1"
 fi
}

read_attr() { $B cat "$1" 2>/dev/null || true; }

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
  echo "USB_MOD_ABSENT $ko"
  return 0
 fi
 if $B insmod "$kpath" 2>&1; then
  echo "USB_MOD_LOADED $ko"
 else
  echo "USB_MOD_WARN $ko rc=$?"
 fi
}

is_mounted_as() {
 $B awk -v t="$1" -v f="$2" -v s="${3:-}" '$2==t{n++; ok=($3==f && (s=="" || $1==s))} END{exit !(n==1 && ok)}' "$MOUNT_TABLE"
}

is_mounted() { $B awk -v t="$1" '$2==t{ok=1} END{exit !ok}' "$MOUNT_TABLE"; }

# Idempotent mount; a failed mount is the evidence configfs is absent.
mount_configfs_at() {
 mnt="$1"
 [ ! -L "$mnt" ] || return 1
 [ -d "$mnt" ] || $B mkdir -p "$mnt" || return 1
 if is_mounted_as "$mnt" configfs; then
  echo "USB_CONFIGFS_ALREADY_MOUNTED $mnt"
  return 0
 fi
 if is_mounted "$mnt" || ! $B mount -t configfs configfs "$mnt" 2>&1; then
  echo "USB_CONFIGFS_MOUNT_FAILED $mnt"
  return 1
 fi
 echo "USB_CONFIGFS_MOUNTED $mnt"
}

# mount_ffs <inst> <uid> <gid> [opts]; FFS_MOUNTED_NOW=1 means a fresh ep0.
mount_ffs() {
 inst="$1"
 fuid="$2"
 fgid="$3"
 extra="${4:-}"
 dir="$FFS_ROOT/$inst"
 opts="uid=$fuid,gid=$fgid,rmode=$USB_FFS_RMODE,fmode=$USB_FFS_FMODE"
 if [ -n "$extra" ]; then
  opts="$opts,$extra"
 fi
 FFS_MOUNTED_NOW=0
 [ ! -L "$dir" ] || return 1
 [ -d "$dir" ] || $B mkdir -p "$dir" || return 1
 if is_mounted_as "$dir" functionfs "$inst"; then
  echo "USB_FFS_ALREADY_MOUNTED $dir"
  return 0
 fi
 if is_mounted "$dir" || ! $B mount -t functionfs -o "$opts" "$inst" "$dir" 2>&1; then
  echo "USB_FFS_MOUNT_FAILED inst=$inst dir=$dir opts=$opts"
  return 1
 fi
 FFS_MOUNTED_NOW=1
 echo "USB_FFS_MOUNTED inst=$inst dir=$dir opts=$opts"
}

# verify_ep0: read-only proof of the node and its owner.
verify_ep0() {
 inst="$1"
 ep0="$FFS_ROOT/$inst/ep0"
 want="$2:$3"
 if [ ! -f "$ep0" ] || [ -L "$ep0" ]; then
  echo "USB_FFS_EP0_MISSING $ep0"
  return 1
 fi
 owner=$($B ls -ln "$ep0" 2>/dev/null | $B awk 'NR == 1 { print $3":"$4 }' || true)
 echo "USB_FFS_EP0_OWNER=${owner:-unknown} want=$want"
 [ "${owner:-unknown}" = "$want" ] || return 1
 return 0
}

# Ready = ep1 AND ep2 in THIS instance's functionfs mount (f_fs.c creates them
# only after descriptors+strings; donor hdcd opens ep1=out, ep2=in). Sets FFS_ST.
ffs_check() {
 d="$FFS_ROOT/$1"
 e1=0
 e2=0
 FFS_ST="mount=0 ep1=0 ep2=0"
 if is_mounted_as "$d" functionfs "$1"; then
  e1=$([ -f "$d/ep1" ] && [ ! -L "$d/ep1" ] && echo 1 || echo 0)
  e2=$([ -f "$d/ep2" ] && [ ! -L "$d/ep2" ] && echo 1 || echo 0)
  FFS_ST="mount=1 ep1=$e1 ep2=$e2"
 fi
 case "$e1$e2" in
  11) return 0 ;;
 esac
 return 1
}

# Configfs stores its own relative link, not the original ln argument.
g1_has_ffs() {
 [ -d "$GCFG/functions/ffs.hdc" ] || return 1
 for l in "$DCONF"/*; do
  if [ -L "$l" ] && [ "$($B readlink -f "$l" 2>/dev/null || true)" = "$GCFG/functions/ffs.hdc" ]; then
   return 0
  fi
 done
 return 1
}

# ep0 holder via /proc (an open fd proves the holder, not descriptors).
ep0_holder() {
 want="$FFS_ROOT/$1/ep0"
 hit=""
 for c in /proc/[0-9]*/comm; do
  [ -f "$c" ] || continue
  IFS= read -r name < "$c" 2>/dev/null || name=""
  case "$name" in
   hdc*|mtp*) ;;
   *) continue ;;
  esac
  p=${c#/proc/}
  p=${p%/comm}
  for f in "/proc/$p"/fd/*; do
   if [ "$($B readlink "$f" 2>/dev/null || true)" = "$want" ]; then
    hit="$hit $name:$p"
   fi
  done
 done
 echo "USB_FFS_EP0_HOLDER inst=$1 held_by=${hit:-none}"
}

# Another gadget holding the controller is not fought over.
udc_owner_scan() {
 for g in "$CFG"/usb_gadget/*; do
  if [ ! -d "$g" ]; then
   continue
  fi
  if [ "$g" = "$GCFG" ]; then
   continue
  fi
  v=$(read_attr "$g/UDC")
  if [ -n "$v" ]; then
   echo "USB_UDC_BUSY_OTHER_GADGET name=${g##*/} udc=$v"
   return 1
  fi
 done
 return 0
}

say preflight "mode=$MODE vid=$USB_VID pid=$USB_PID configfs=$CFG root=$R"
$B cat /proc/sys/kernel/random/boot_id 2>/dev/null || true

stage=hostns
if [ ! -d "$R/system/bin" ] || [ ! -d "$R/dev" ]; then
 echo "USB_NOT_HOST_NAMESPACE root=$R"
 echo "USB_NOT_HOST_NAMESPACE_HINT run as: /busybox sh $R/compat/bin/start-usb-marble.sh (no chroot)"
 exit 46
fi
say hostns "ok"

stage=configfs
mount_configfs_at "$CFG" || keep_usb configfs-mount 51
stage=udc-scan
udc_owner_scan || keep_usb udc-held-by-other-gadget 47
own=$(read_attr "$GCFG/UDC")
for u in "$USB_UDC" "$own"; do
 case "$u" in dummy*) echo "USB_UDC_VIRTUAL name=$u"; keep_usb virtual-udc 53 ;; esac
done
if [ -n "$own" ]; then
 echo "USB_UDC_ALREADY_BOUND udc=$own"
 case ",$MODE," in
  *,hdc,*)
   if ! g1_has_ffs; then
    echo "USB_HDC_LINK_MISSING gadget=$GCFG config=$DCONF"
    keep_usb bound-without-ffs-hdc 49
   fi
   if ! ffs_check hdc; then
    echo "USB_FFS_NOT_READY $FFS_ST inst=hdc reason=bound-ffs-not-active"
     keep_usb bound-without-active-endpoints 49
   fi
   echo "USB_FFS_READY=1 $FFS_ST inst=hdc already_bound=1"
   ep0_holder hdc
   ;;
 esac
 echo "USB_NOTHING_TO_DO already-bound"
 stage=report
 exit 0
fi

stage=gadget-skeleton
[ ! -L "$FFS_ROOT" ] || keep_usb ffs-parent-link 48
$B mkdir -p "$FFS_ROOT"
mount_configfs_at "$R/config" || keep_usb configfs-second-view 51
mk "$GCFG"
say gadget-skeleton "created=$GCFG"

stage=ffs-probe
ffs_probe="$GCFG/functions/ffs.hmos_probe"
if ! $B mkdir "$ffs_probe" 2>/dev/null; then
 echo "USB_NO_FUNCTIONFS_SUPPORT probe=$ffs_probe reason=mkdir-failed"
 echo "USB_FUNCTIONFS_EVIDENCE /proc/filesystems=$($B grep -c functionfs /proc/filesystems 2>/dev/null || echo 0) module=$([ -d "$SYSFS/module/usb_f_fs" ] && echo present || echo absent)"
 keep_usb functionfs-unavailable 52
fi
$B rmdir "$ffs_probe" 2>/dev/null || echo "USB_FUNCTIONFS_PROBE_RMDIR_BUSY $ffs_probe"
echo "USB_FUNCTIONFS_OK probe=$ffs_probe"

stage=modules
for ko in phy-msm-ssusb-qmp.ko phy-msm-snps-eusb2.ko phy-msm-snps-hs.ko usb_bam.ko dwc3-msm.ko; do
 load_ko "$ko" || true
done

stage=udc-detect
waited=0
while :; do
 udc=${USB_UDC:-$($B ls "$SYSFS/class/udc" 2>/dev/null | $B grep -v '^dummy' | $B head -n 1 || true)}
 if [ -n "$udc" ]; then
  break
 fi
 waited=$((waited + 1))
 if [ "$waited" -gt "$USB_WAIT_UDC_SECONDS" ]; then
  echo "USB_NO_UDC controller_not_registered"
  exit 41
 fi
 nap 1
done
[ -d "$SYSFS/class/udc/$udc" ] || keep_usb udc-not-present 41
echo "USB_UDC_NAME=$udc"

stage=gadget-attrs
write_attr_req "$GCFG/idVendor" "$USB_VID"
write_attr_req "$GCFG/idProduct" "$USB_PID"
write_attr "$GCFG/bcdUSB" "0x0200"
write_attr "$GCFG/bcdDevice" "0x0200"
write_attr "$GCFG/os_desc/use" "1"
write_attr "$GCFG/os_desc/b_vendor_code" "0x1"
write_attr "$GCFG/os_desc/qw_sign" "MSFT100"

stage=strings
serial=""
for prop in ohos.boot.sn const.product.serialnumber ro.serialno; do
 if [ -z "$serial" ]; then
  serial=$(oget "$prop")
 fi
done
if [ -z "$serial" ]; then
 serial=$($B sed -n 's/.*androidboot.serialno=\([^ ]*\).*/\1/p' /proc/cmdline 2>/dev/null | $B head -n 1 || true)
fi
serial=${serial:-MARBLEUSB0001}
mk "$GCFG/strings/0x409"
write_attr "$GCFG/strings/0x409/serialnumber" "$serial"
write_attr "$GCFG/strings/0x409/manufacturer" "Xiaomi"
write_attr "$GCFG/strings/0x409/product" "marble HarmonyOS USB"
echo "USB_STRINGS sid=$serial"

mk "$DCONF"
mk "$DCONF/strings/0x409"
write_attr "$DCONF/MaxPower" "$USB_MAXPOWER"
write_attr "$DCONF/strings/0x409/configuration" "harmonyos"

stage=functions
funcs=""
add_func() { funcs="${funcs:+$funcs }$1"; }

want_hdc=0
want_mtp=0
want_storage=0
old_ifs=$IFS
IFS=,
for part in $MODE; do
 case "$part" in
  hdc) want_hdc=1 ;;
  mtp) want_mtp=1 ;;
  storage) want_storage=1 ;;
  none|"") : ;;
  *) echo "USB_UNKNOWN_MODE_PART $part"; IFS=$old_ifs; exit 42 ;;
 esac
done
IFS=$old_ifs

if [ "$want_hdc" = 1 ]; then
 mk "$GCFG/functions/ffs.hdc" || keep_usb ffs-hdc-instance 52
 mount_ffs hdc "$USB_FFS_UID" "$USB_FFS_GID" || keep_usb ffs-hdc-mount 48
 HDC_FFS_MOUNTED_NOW=$FFS_MOUNTED_NOW
 verify_ep0 hdc "$USB_FFS_UID" "$USB_FFS_GID" || keep_usb ffs-hdc-ep0 48
 add_func ffs.hdc
fi

if [ "$want_mtp" = 1 ]; then
 mk "$GCFG/functions/ffs.mtp" || keep_usb ffs-mtp-instance 52
 mount_ffs mtp "$USB_FFS_MTP_UID" "$USB_FFS_MTP_GID" no_disconnect=1 || keep_usb ffs-mtp-mount 48
 verify_ep0 mtp "$USB_FFS_MTP_UID" "$USB_FFS_MTP_GID" || keep_usb ffs-mtp-ep0 48
 if [ "$USB_MTP_RESPONDER" != 1 ]; then
  echo "USB_MTP_RESPONDER_ABSENT mtp will not be linked or bound"
 else
  add_func ffs.mtp
 fi
fi

if [ "$want_storage" = 1 ]; then
 if [ -z "$USB_STORAGE_IMAGE" ] || [ ! -f "$USB_STORAGE_IMAGE" ]; then
  echo "USB_STORAGE_IMAGE_MISSING path=${USB_STORAGE_IMAGE:-unset}"
  exit 44
 fi
 mk "$GCFG/functions/mass_storage.0"
 write_attr "$GCFG/functions/mass_storage.0/lun.0/cdrom" "0"
 write_attr "$GCFG/functions/mass_storage.0/lun.0/removable" "1"
 write_attr "$GCFG/functions/mass_storage.0/lun.0/ro" "1"
 write_attr "$GCFG/functions/mass_storage.0/lun.0/file" "$USB_STORAGE_IMAGE"
 add_func mass_storage.0
fi

if [ -z "$funcs" ]; then
 if [ "$want_mtp" = 1 ]; then
  echo "USB_MTP_RESPONDER_ABSENT_ONLY mode=mtp"
  exit 45
 fi
 echo "USB_NO_FUNCTIONS_SELECTED mode=$MODE"
 exit 42
fi
echo "USB_FUNCTIONS=$funcs"

stage=link
idx=1
for fn in $funcs; do
 link="$DCONF/f$idx"
 src="$GCFG/functions/$fn"
 if [ -L "$link" ]; then
  target=$($B readlink -f "$link" 2>/dev/null || true)
  if [ "$target" = "$src" ]; then
   echo "USB_LINK_ALREADY f$idx=$fn"
   idx=$((idx + 1))
   continue
  fi
  keep_usb foreign-config-link 49
 fi
 $B ln -s "$src" "$link"
 echo "USB_LINKED f$idx=$fn"
 idx=$((idx + 1))
done
if [ -d "$GCFG/os_desc" ] && [ ! -e "$GCFG/os_desc/b.1" ]; then
 $B ln -s "$DCONF" "$GCFG/os_desc/b.1"
 echo "USB_OS_DESC_LINKED b.1"
fi

# this round's ep0: the flag is a reset-and-ignore echo; an older daemon is reported.
if [ "$want_hdc" = 1 ]; then
 stage=ffs-ready
 devmode=$(oget const.security.developermode.state)
 if [ "${devmode:-}" != true ]; then
  echo "USB_HDC_DEVMODE_BLOCKED state=${devmode:-unset}"
  keep_usb hdcd-developer-mode 50
 fi

 hdcd_state=$(oget init.svc.hdcd)
 ffs_flag=$(oget sys.usb.ffs.ready)
 if [ "${HDC_FFS_MOUNTED_NOW:-0}" = 1 ] && [ "$hdcd_state" = running ]; then
  echo "USB_FFS_EP0_DAEMON_STALE hdcd was already running before this round's ep0 mount"
  echo "USB_FFS_EP0_DAEMON_STALE_HINT restart hdcd from init; original USB kept"
  keep_usb stale-hdcd-on-new-ep0 49
 fi
 if [ "${HDC_FFS_MOUNTED_NOW:-0}" = 1 ] && [ "${ffs_flag:-0}" = 1 ]; then
  echo "USB_FFS_FLAG_STALE_RESET prev=$ffs_flag reason=fresh-ep0-mount"
  setp sys.usb.ffs.ready 0
  ffs_flag=$(oget sys.usb.ffs.ready)
  if [ "${ffs_flag:-0}" = 1 ]; then
   echo "USB_FFS_FLAG_RESET_FAILED prev=1 reason=param-write-not-effective"
   echo "USB_FFS_FLAG_UNTRUSTED gate=endpoint-files"
  fi
 fi

 if ! ffs_check hdc; then
  echo "USB_HDC_START_REQUEST init job marble.debug.start.usb (u:r:hdcd:s0)"
  setp marble.debug.start.usb true
 fi

 waited=0
 while :; do
  if ffs_check hdc; then
   break
  fi
  hdcd_state=$(oget init.svc.hdcd)
  ffs_flag=$(oget sys.usb.ffs.ready)
  waited=$((waited + 1))
  if [ "$waited" -gt "$USB_WAIT_FFS_SECONDS" ]; then
   echo "USB_FFS_NOT_READY $FFS_ST flag=${ffs_flag:-0} hdcd=${hdcd_state:-unknown} waited=${waited}s"
   keep_usb ffs-endpoint-not-ready 49
  fi
  nap 1
 done
 echo "USB_FFS_READY=1 $FFS_ST inst=hdc hdcd=${hdcd_state:-unknown} flag=${ffs_flag:-0} waited=${waited}s"
 ep0_holder hdc
fi

# UDC bind, last mutation: g1 takes a UDC only while unbound (configfs -EBUSY).
stage=udc-bind
udc_owner_scan || keep_usb udc-held-by-other-gadget 47
attempt=0
bound=0
while [ "$attempt" -lt "$USB_UDC_RETRY" ]; do
 attempt=$((attempt + 1))
 if ! write_attr_req "$GCFG/UDC" "$udc"; then
  echo "USB_UDC_WRITE_REFUSED attempt=$attempt"
 fi
 cur=$(read_attr "$GCFG/UDC")
 if [ "$cur" = "$udc" ]; then
  bound=1
  break
 fi
 echo "USB_UDC_BIND_RETRY attempt=$attempt readback=${cur:-none}"
 nap 1
done
if [ "$bound" != 1 ]; then
 echo "USB_UDC_BIND_FAILED udc=$udc attempts=$attempt"
 exit 43
fi
echo "USB_UDC_BOUND $udc attempt=$attempt"

stage=report
echo "USB_UDC_STATE=$(read_attr "$SYSFS/class/udc/$udc/state")"
echo "USB_UDC_SOFT_CONNECT=$(read_attr "$SYSFS/class/udc/$udc/soft_connect")"
echo "USB_UDC_CURRENT_SPEED=$(read_attr "$SYSFS/class/udc/$udc/current_speed")"
echo "USB_CONFIG_LINKS=$($B ls "$DCONF" 2>/dev/null | $B tr '\n' ' ')"
echo "USB_FFS_DIRS=$($B ls "$FFS_ROOT" 2>/dev/null | $B tr '\n' ' ')"
echo "USB_HDC_STATE=$(oget init.svc.hdcd)"
echo "USB_HDC_FFS_FLAG=$(oget sys.usb.ffs.ready)"
echo "USB_HDC_KEYS_DIR=$([ -d "$R/data/service/el1/public/hdc" ] && echo present || echo absent)"
stage=complete