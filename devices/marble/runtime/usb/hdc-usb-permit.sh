#!/busybox sh
# SPDX-License-Identifier: MIT
#
# marble HDC-over-USB minimal-permission profile.
#
# Goal: file transfer + a read-only command channel, NOT a root shell.
#   * persist.hdc.root=0                 -> hdcd never elevates the peer
#   * persist.hdc.control.shell=false    -> `hdc shell` is rejected
#   * persist.hdc.control.fport=false    -> no port forwarding (no tunnel pivots)
#   * persist.hdc.control.file=true      -> `hdc file send/recv` stays usable
#   * persist.hdc.mode.tcp=disable       -> no 0.0.0.0:8710 listener
#   * persist.hdc.mode.uart=disable      -> never steals the serial console
#
# Parameter defaults and DAC rules come from the donor ROM:
#   system/etc/param/hdc.para        const.hdc.version = "Ver: 3.2.0f"
#   system/etc/param/hdc.para.dac    persist.hdc.control = usb_host:edm:0664
#   system/etc/init/hdcd.cfg         "param:persist.hdc.control=false -> ..."
# hdcd reads these once at start, so the service is restarted afterwards when it
# is already running.
#
# Host side prerequisites for `hdc list targets`:
#   1. hdc client >= 3.2 (device reports const.hdc.version = "Ver: 3.2.0f").
#   2. /dev/usb-ffs/hdc/ep0 exists and hdcd has published sys.usb.ffs.ready=1.
#   3. The gadget has ffs.hdc linked in configs/b.1/f1 and the UDC is bound
#      (start-usb-marble.sh MODE=hdc).
#   4. First connect needs the on-device authorization (hdcd_user_permit /
#      persist.hdc.daemon.auth_result), the host key is remembered in
#      /data/service/el1/public/hdc/hdc_keys.
#   5. hdc tcp mode is disabled here on purpose; for tcp use
#      `hdc tconn HOST:8710` only after `setparam persist.hdc.mode.tcp enable`.
#
# This script never writes a file, never changes SELinux mode, and never enables
# root unless the operator explicitly asks for it.
set -eu

B=/busybox
R=/hmos-root
MARBLE_HDC_ALLOW_ROOT=${MARBLE_HDC_ALLOW_ROOT:-0}

stage=preflight
trap 'c=$?; echo "USB_HDC_PERMIT_EXIT=$c STAGE=$stage"' EXIT

say() {
 echo "USB_HDC_STAGE=$1 $2"
}

oget() {
 $B chroot "$R" /system/bin/param get "$1" 2>/dev/null || true
}

setp() {
 if [ -z "$1" ] || [ -z "${2:-}" ]; then
  echo "USB_HDC_PARAM_SKIP name=$1"
  return 0
 fi
 if $B chroot "$R" /system/bin/param set "$1" "$2" 2>/dev/null; then
  echo "USB_HDC_PARAM_SET $1=$2"
 else
  echo "USB_HDC_PARAM_FAIL $1=$2 (read-only or unknown parameter in this image)"
 fi
}

say preflight "hdc_version=$(oget const.hdc.version) root_optin=$MARBLE_HDC_ALLOW_ROOT"
echo "USB_HDC_KEYS_DIR=$([ -d "$R/data/service/el1/public/hdc" ] && echo present || echo absent)"
echo "USB_HDC_FFS_EP0=$([ -e "$R/dev/usb-ffs/hdc/ep0" ] && echo present || echo absent)"

# 1. Transport: USB only.  Keeps the UART console (COM9 path) untouched.
stage=transport
setp persist.hdc.mode.usb enable
setp persist.hdc.mode.tcp disable
setp persist.hdc.mode.uart disable

# 2. Capabilities: file transfer yes, interactive shell and port forwarding no.
stage=capability
setp persist.hdc.control.file true
setp persist.hdc.control.shell false
setp persist.hdc.control.fport false

# 3. Privilege: read-only by default.
stage=privilege
if [ "$MARBLE_HDC_ALLOW_ROOT" = 1 ]; then
 echo "USB_HDC_ROOT_ENABLED explicitly requested by MARBLE_HDC_ALLOW_ROOT=1"
 setp persist.hdc.root 1
else
 setp persist.hdc.root 0
 echo "USB_HDC_ROOT_DISABLED (set MARBLE_HDC_ALLOW_ROOT=1 and re-run to opt in)"
fi

# 4. hdcd caches the parameters, restart it so the profile takes effect.
stage=restart
if [ "$(oget init.svc.hdcd)" = running ]; then
 if $B chroot "$R" /system/bin/param set ctl.stop hdcd 2>/dev/null; then
  nap_hdc=0
  while [ "$(oget init.svc.hdcd)" = running ] && [ "$nap_hdc" -lt 5 ]; do
   $B sleep 1
   nap_hdc=$((nap_hdc + 1))
  done
 fi
 if $B chroot "$R" /system/bin/param set ctl.start hdcd 2>/dev/null; then
  echo "USB_HDC_RESTARTED state=$(oget init.svc.hdcd)"
 else
  echo "USB_HDC_RESTART_UNSUPPORTED (stop/start hdcd from the init console if it stays stale)"
 fi
else
 echo "USB_HDC_NOT_RUNNING state=$(oget init.svc.hdcd) (start with: param set marble.debug.start.usb true)"
fi

stage=report
for p in persist.hdc.mode.usb persist.hdc.mode.tcp persist.hdc.mode.uart \
 persist.hdc.control.file persist.hdc.control.shell persist.hdc.control.fport \
 persist.hdc.root const.hdc.version; do
 echo "USB_HDC_PARAM $p=$(oget "$p")"
done
stage=complete