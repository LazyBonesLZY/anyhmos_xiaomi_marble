#!/busybox sh
# SPDX-License-Identifier: MIT
#
# Xiaomi marble USB gadget rollback.  Restores the exact state the device is in
# before start-usb-marble.sh runs: no UDC bound, no f-links, no gadget tree.
#
# Safe by construction:
#   * unplugging the cable is always safe - a bound gadget without a host is
#     idle, it is not part of the boot path and holds no storage reference;
#   * the serial console (COM9 / UART) is a host-side USB-UART bridge, it does
#     not depend on the SoC gadget (dwc3-msm.ko is not loaded on a normal boot),
#     so unbinding the UDC cannot take the console away;
#   * this script never touches the boot chain, vendor_boot, or /data; the worst
#     case is a gadget that stays unbound (current state).
#
# Steps: unbind UDC -> drop f-links -> drop os_desc link -> remove gadget dirs.
# Unmounting functionfs and unloading dwc3-msm are opt-in because a live hdcd
# holds ep0 open and a module unload can leave a stale /sys/class/udc entry.
set -eu

B=/busybox
R=/hmos-root
CFG=${USB_CONFIGFS:-/config}
GCFG="$CFG/usb_gadget/g1"
DCONF="$GCFG/configs/b.1"
USB_STOP_UNMOUNT=${USB_STOP_UNMOUNT:-0}
USB_STOP_UNLOAD=${USB_STOP_UNLOAD:-0}
USB_STOP_KEEP_GADGET=${USB_STOP_KEEP_GADGET:-0}

stage=preflight
trap 'c=$?; echo "USB_STOP_EXIT=$c STAGE=$stage"' EXIT

say() {
 echo "USB_STOP_STAGE=$1 $2"
}

read_attr() {
 $B cat "$1" 2>/dev/null || true
}

write_attr() {
 if [ ! -e "$1" ]; then
  echo "USB_STOP_ATTR_SKIP_MISSING $1"
  return 0
 fi
 if [ -n "${2:-}" ]; then
  printf '%s' "$2" > "$1"
 else
  # kernfs drops 0-byte writes; "\n" is the documented UDC detach value.
  printf '\n' > "$1"
 fi
}

say preflight "configfs=$CFG"
if ! $B grep -q " $CFG configfs " /proc/mounts && ! $B awk -v t="$CFG" '$2 == t && $3 == "configfs" { f = 1 } END { exit(f ? 0 : 1) }' /proc/mounts; then
 say no-configfs "nothing to roll back"
 exit 0
fi

# 1. Unbind the UDC.  Writing an empty string is the documented dwc3 detach;
#    this is the single command that stops all USB device activity.
stage=unbind-udc
if [ -e "$GCFG/UDC" ]; then
 cur=$(read_attr "$GCFG/UDC")
 if [ -n "$cur" ]; then
  write_attr "$GCFG/UDC" ""
  echo "USB_STOP_UDC_UNBOUND prev=$cur"
 else
  echo "USB_STOP_UDC_ALREADY_UNBOUND"
 fi
else
 echo "USB_STOP_NO_GADGET"
fi

# 2. Drop the function links (configfs requires an empty config before rmdir).
stage=unlink
if [ -d "$DCONF" ]; then
 for i in 1 2 3 4 5 6 7 8; do
  if [ -L "$DCONF/f$i" ]; then
   $B rm -f "$DCONF/f$i"
   echo "USB_STOP_UNLINKED f$i"
  fi
 done
fi
if [ -L "$GCFG/os_desc/b.1" ]; then
 $B rm -f "$GCFG/os_desc/b.1"
 echo "USB_STOP_UNLINKED os_desc/b.1"
fi

if [ "$USB_STOP_KEEP_GADGET" = 1 ]; then
 say keep-gadget "gadget tree kept, UDC unbound"
 stage=complete
 exit 0
fi

# 3. Remove the gadget tree bottom-up.
stage=teardown
for d in \
 "$DCONF/strings/0x409" "$DCONF/strings" "$DCONF" \
 "$GCFG/configs/strings/0x409" "$GCFG/configs/strings" "$GCFG/configs" \
 "$GCFG/functions/ffs.hdc" "$GCFG/functions/ffs.mtp" "$GCFG/functions/mass_storage.0" \
 "$GCFG/functions" \
 "$GCFG/strings/0x409" "$GCFG/strings" "$GCFG"; do
 if [ -d "$d" ]; then
  if $B rmdir "$d" 2>/dev/null; then
   echo "USB_STOP_RMDIR $d"
  else
   echo "USB_STOP_RMDIR_BUSY $d"
  fi
 fi
done

if [ "$USB_STOP_UNMOUNT" = 1 ]; then
 stage=unmount-ffs
 for inst in hdc mtp; do
  dir="$R/dev/usb-ffs/$inst"
  if $B awk -v t="$dir" '$2 == t && $3 == "functionfs" { f = 1 } END { exit(f ? 0 : 1) }' /proc/mounts; then
   if $B umount "$dir" 2>/dev/null; then
    echo "USB_STOP_FFS_UNMOUNTED $dir"
   else
    echo "USB_STOP_FFS_BUSY $dir (hdcd still holds ep0; stop hdcd first)"
   fi
  fi
 done
fi

if [ "$USB_STOP_UNLOAD" = 1 ]; then
 stage=unload
 for ko in dwc3-msm usb_bam phy-msm-snps-eusb2 phy-msm-ssusb-qmp; do
  if $B grep -q "^$ko " /proc/modules; then
   if $B rmmod "$ko" 2>/dev/null; then
    echo "USB_STOP_RMMOD $ko"
   else
    echo "USB_STOP_RMMOD_BUSY $ko"
   fi
  fi
 done
fi

stage=report
echo "USB_STOP_UDC_STATE=$($B ls /sys/class/udc 2>/dev/null | $B tr '\n' ' ')"
echo "USB_STOP_CONFIGFS_GADGETS=$($B ls "$CFG/usb_gadget" 2>/dev/null | $B tr '\n' ' ')"
stage=complete