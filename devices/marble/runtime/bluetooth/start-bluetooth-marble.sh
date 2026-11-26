#!/busybox sh
# marble (SM7475 / ukee) Bluetooth bring-up: host-side H4 bridge + OHOS HDI.
#
# Runs on the device only. It never flashes, never uses adb/fastboot/serial,
# and only reads bluetooth_a (vfat, read-only). Every stage either prints an
# explicit success marker or exits with a distinct code plus the reason.
set -eu
B=/busybox
R=/hmos-root
D=$R/data/local/tmp

# --- tunables (override with env, never inline literals below) ---------------
BT_PART=${BT_PART:-/dev/block/by-name/bluetooth_a}
BT_MOUNT=${BT_MOUNT:-/compat/original-bluetooth}
BT_FW_ROOT=${BT_FW_ROOT:-/bt_firmware}
BT_FW_DIR=${BT_FW_DIR:-$BT_FW_ROOT/image}
BT_TLV=${BT_TLV:-hpbtfw21.tlv}
BT_NVM=${BT_NVM:-hpnv21g.bin}
BT_TTY=${BT_TTY:-/dev/ttyHS0}
BT_POWER=${BT_POWER:-/dev/btpower}
BT_SOCKET=${BT_SOCKET:-/dev/unix/socket/hmos.bt.h4}
# BT_BRIDGE is the path passed to exec inside the OHOS root.
BT_BRIDGE=${BT_BRIDGE:-/compat/bin/hmos_bt_bridge_marble}
BT_HDI=${BT_HDI:-$R/compat/bin/hmos_bt_hdi_publisher_marble}
BT_BRIDGE_LOG=${BT_BRIDGE_LOG:-$D/marble-bluetooth-bridge.txt}
BT_HDI_LOG=${BT_HDI_LOG:-$D/hmos-bluetooth-hdi.txt}
BT_BLUETOOTH_UID=${BT_BLUETOOTH_UID:-1002}
BT_BLUE_HOST_GID=${BT_BLUE_HOST_GID:-3021}
BT_FCM_USER=${BT_FCM_USER:-fusion_connectivity_manager_ext}
BT_WAIT_TICKS=${BT_WAIT_TICKS:-40}
BT_WAIT_STEP=${BT_WAIT_STEP:-0.2}

stage=precheck
# shellcheck disable=SC2154  # c is assigned inside the trap
 trap 'c=$?; echo "BT_START_EXIT=$c STAGE=$stage"' EXIT

fail()
{
 echo "BT_START_FAILED stage=$stage reason=$*"
 exit 1
}

note()
{
 echo "BT_STAGE=$stage $*"
}

# Honor an older host-prefixed override, but execute the inner path in chroot.
case "$BT_BRIDGE" in
 "$R"/*) BT_BRIDGE=${BT_BRIDGE#"$R"} ;;
 /*) : ;;
 *) fail "BT_BRIDGE must be an absolute path" ;;
esac

# 0. Install the marble Bluetooth artifacts staged under /graphics by the
#    packaging step (the stage-phase4 hook only has to stage and copy this
#    script itself). Nothing is fetched at runtime.
stage=install
$B mkdir -p "$R/compat/bin" "$R/compat/lib64"
for f in hmos_bt_bridge_marble hmos_bt_hdi_publisher_marble; do
 [ -f "/graphics/$f" ] || fail "missing /graphics/$f (packaging did not stage it)"
 $B cp "/graphics/$f" "$R/compat/bin/$f" || fail "cp /graphics/$f failed"
 $B chmod 0755 "$R/compat/bin/$f" || fail "chmod 0755 $R/compat/bin/$f failed"
 echo "BT_INSTALLED_BIN $f"
done
for f in libbt_vendor.z.so libhmos_bt_bridge_marble.so; do
 [ -f "/graphics/$f" ] || fail "missing /graphics/$f (packaging did not stage it)"
 $B cp "/graphics/$f" "$R/compat/lib64/$f" || fail "cp /graphics/$f failed"
 $B chmod 0644 "$R/compat/lib64/$f" || fail "chmod 0644 $R/compat/lib64/$f failed"
 echo "BT_INSTALLED_LIB $f"
done
note "artifacts_installed=4"

# 1. Kernel side must already be up (btpower.ko + msm_geni_serial.ko load in
#    stage-phase4-marble.sh). Missing nodes mean the UART driver never probed.
[ -r /sys/class/bt-dev/btpower/dev ] || fail "missing /sys/class/bt-dev/btpower/dev (btpower.ko not probed)"
[ -r /sys/class/tty/ttyHS0/dev ] || fail "missing /sys/class/tty/ttyHS0/dev (msm_geni_serial.ko not probed)"
[ -x "$R$BT_BRIDGE" ] || fail "bridge binary $R$BT_BRIDGE is not executable"
note "kernel_nodes_present=2"

# 2. Character nodes in both namespaces: the host root for diagnostics and
#    /hmos-root/dev for the OHOS services (same dev_t as /sys).
stage=nodes
mk_bt_node()
{
 name=$1
 if [ ! -r "/sys/class/bt-dev/$name/dev" ] && [ ! -r "/sys/class/tty/$name/dev" ]; then
  fail "no sysfs dev attribute for $name"
 fi
 value=$($B cat "/sys/class/bt-dev/$name/dev" 2>/dev/null || $B cat "/sys/class/tty/$name/dev")
 [ -n "$value" ] || fail "empty dev attribute for $name"
 major=${value%:*}
 minor=${value#*:}
 [ -n "$major" ] && [ -n "$minor" ] || fail "unparsable dev attribute '$value' for $name"
 for root in /dev "$R/dev"; do
  $B mkdir -p "$root"
  if [ ! -e "$root/$name" ]; then
   $B mknod -m 0600 "$root/$name" c "$major" "$minor" || fail "mknod $root/$name c $major $minor failed"
  fi
  $B chown 0:0 "$root/$name" || fail "chown $root/$name failed"
  $B chmod 0600 "$root/$name" || fail "chmod 0600 $root/$name failed"
 done
 echo "BT_NODE $name dev=$value"
}
mk_bt_node btpower
mk_bt_node ttyHS0
if [ -r /sys/class/misc/tun/dev ]; then
 tun_dev=$($B cat /sys/class/misc/tun/dev 2>/dev/null || true)
 tun_ma=${tun_dev%:*}; tun_mi=${tun_dev#*:}
 if [ -n "$tun_ma" ] && [ -n "$tun_mi" ]; then
  [ -e "$R/dev/tun" ] || $B mknod -m 0660 "$R/dev/tun" c "$tun_ma" "$tun_mi" || true
  $B chown "$BT_BLUETOOTH_UID:$BT_BLUETOOTH_UID" "$R/dev/tun" 2>/dev/null || true
  $B chmod 0660 "$R/dev/tun" 2>/dev/null || true
 fi
fi
note "nodes_ready=2"

# 3. Firmware: mount bluetooth_a read-only and expose it as /bt_firmware
#    inside the OHOS root (the path the vendor HAL and this bridge expect).
stage=firmware
$B mkdir -p "$BT_MOUNT" "$R$BT_FW_ROOT"
if ! $B grep -q " $BT_MOUNT " /proc/mounts; then
 $B mount -t vfat -o ro "$BT_PART" "$BT_MOUNT" || fail "mount -t vfat -o ro $BT_PART $BT_MOUNT failed"
fi
$B grep -q "^$BT_PART $BT_MOUNT vfat ro," /proc/mounts || \
 $B grep -q " $BT_MOUNT vfat ro," /proc/mounts || fail "$BT_MOUNT is not a read-only vfat mount"
[ -f "$BT_MOUNT/image/$BT_TLV" ] || fail "firmware $BT_MOUNT/image/$BT_TLV missing"
[ -f "$BT_MOUNT/image/$BT_NVM" ] || echo "BT_NVM_ABSENT $BT_MOUNT/image/$BT_NVM (bridge may proceed without it)"
if ! $B grep -q " $R$BT_FW_ROOT " /proc/mounts; then
 $B mount --bind "$BT_MOUNT" "$R$BT_FW_ROOT" || fail "mount --bind $BT_MOUNT $R$BT_FW_ROOT failed"
fi
$B chroot "$R" $B test -f "$BT_FW_DIR/$BT_TLV" || fail "$BT_FW_DIR/$BT_TLV not visible inside the OHOS root"
$B chroot "$R" $B test -f "$BT_FW_DIR/$BT_NVM" || echo "BT_NVM_NOT_VISIBLE_IN_ROOT $BT_FW_DIR/$BT_NVM"
note "firmware=$BT_TLV nvm=$BT_NVM mount=$BT_MOUNT"

# 4. Bluetooth MAC: prefer the NV blob shipped on bluetooth_a. When nothing is
#    available the service derives its own value; say so loudly instead of
#    pretending the address is known.
stage=mac
$B mkdir -p "$R/data/vendor/bluetooth"
mac_written=0
for candidate in "$BT_MOUNT/image/bt_nv.bin" "$BT_MOUNT/bt_nv.bin"; do
 [ -f "$candidate" ] || continue
 size=$($B stat -c %s "$candidate")
 [ "$size" -eq 6 ] || { echo "BT_MAC_SKIP $candidate size=$size (expected 6)"; continue; }
 # shellcheck disable=SC2046  # one token per byte; splitting is intended
 set -- $($B od -An -tu1 "$candidate")
 [ "$#" -eq 6 ] || fail "od returned $# bytes for $candidate"
 printf '%02X:%02X:%02X:%02X:%02X:%02X' "$1" "$2" "$3" "$4" "$5" "$6" > "$R/data/vendor/bluetooth/btmac.txt.new"
 $B chown $BT_BLUE_HOST_GID:$BT_BLUE_HOST_GID "$R/data/vendor/bluetooth/btmac.txt.new"
 $B chmod 0640 "$R/data/vendor/bluetooth/btmac.txt.new"
 $B mv "$R/data/vendor/bluetooth/btmac.txt.new" "$R/data/vendor/bluetooth/btmac.txt"
 echo "BT_MAC_SOURCE=$candidate"
 mac_written=1
 break
done
if [ "$mac_written" -eq 0 ]; then
 echo "BT_MAC_UNAVAILABLE reason=no_bt_nv_on_bluetooth_a (service will not have a persisted address)"
fi
note "mac_written=$mac_written"

# 5. Writable service directories with the donor UIDs/GIDs.
stage=dirs
$B chown $BT_BLUE_HOST_GID:$BT_BLUE_HOST_GID "$R/data/vendor/bluetooth"
$B chmod 0770 "$R/data/vendor/bluetooth"
$B mkdir -p "$R/data/log/bt" "$R/data/service/el1/public/bluetooth"
$B chown $BT_BLUETOOTH_UID:$BT_BLUETOOTH_UID "$R/data/log/bt" "$R/data/service/el1/public/bluetooth"
$B chmod 0711 "$R/data/service/el1/public/bluetooth"
fcm_id=$($B awk -F: -v u="$BT_FCM_USER" '$1==u {print $3}' "$R/system/etc/passwd")
if [ -n "$fcm_id" ]; then
 $B mkdir -p "$R/data/service/el1/public/fcm"
 $B chown "$fcm_id:$fcm_id" "$R/data/service/el1/public/fcm"
 $B chmod 0700 "$R/data/service/el1/public/fcm"
 echo "BT_FCM_DIR user=$BT_FCM_USER uid=$fcm_id"
else
 echo "BT_FCM_DIR_SKIPPED user=$BT_FCM_USER not in passwd"
fi
$B mkdir -p "$D"
: > "$BT_HDI_LOG"
$B chown "$BT_BLUE_HOST_GID:$BT_BLUE_HOST_GID" "$BT_HDI_LOG"
$B chmod 0664 "$BT_HDI_LOG"
: > "$D/hmos-bluetooth-check.txt"
$B chown 2000:2000 "$D/hmos-bluetooth-check.txt"
$B chmod 0664 "$D/hmos-bluetooth-check.txt"
note "dirs_ready=1"

# 6. Preflight inside the OHOS root: validates tty/power/socket paths without
#    powering the controller.
stage=preflight
$B chroot "$R" "$BT_BRIDGE" --check --tty "$BT_TTY" --power "$BT_POWER" \
 --firmware-dir "$BT_FW_DIR" --socket "$BT_SOCKET" || fail "bridge --check failed inside $R"
note "preflight_ok=1"

# 7. Start the H4 bridge (owns /dev/ttyHS0 and /dev/btpower inside the root).
stage=bridge
[ ! -S "$R$BT_SOCKET" ] || fail "stale socket $R$BT_SOCKET present before start"
: > "$BT_BRIDGE_LOG"
$B setsid $B chroot "$R" "$BT_BRIDGE" \
 --tty "$BT_TTY" --power "$BT_POWER" --firmware-dir "$BT_FW_DIR" \
 --rampatch "$BT_TLV" --nvm "$BT_NVM" --socket "$BT_SOCKET" --max-sessions 0 \
 > "$BT_BRIDGE_LOG" 2>&1 </dev/null &
bridge_pid=$!
echo "$bridge_pid" > "$D/marble-bluetooth-bridge.pid"
i=0
while ! $B grep -q '^HMOS_BT BRIDGE_LISTENING=1' "$BT_BRIDGE_LOG" 2>/dev/null; do
 if $B grep -q '^HMOS_BT BRINGUP_FAILED' "$BT_BRIDGE_LOG" 2>/dev/null; then
  $B tail -n 20 "$BT_BRIDGE_LOG"
  fail "bridge bring-up failed (see $BT_BRIDGE_LOG)"
 fi
 [ -d "/proc/$bridge_pid" ] || { $B tail -n 20 "$BT_BRIDGE_LOG"; fail "bridge process $bridge_pid exited early"; }
 [ "$i" -lt "$BT_WAIT_TICKS" ] || { $B tail -n 20 "$BT_BRIDGE_LOG"; fail "bridge did not report BRIDGE_LISTENING within $BT_WAIT_TICKS ticks"; }
 i=$((i + 1))
 $B sleep "$BT_WAIT_STEP"
done
note "bridge_pid=$bridge_pid listening=1"

# 8. Port contract: HDI hands this socket fd to the H4 protocol, so ownership
#    and permissions have to match what the bluetooth domain expects.
stage=port_contract
[ -c /dev/btpower ] || fail "/dev/btpower is not a character device"
[ -c /dev/ttyHS0 ] || fail "/dev/ttyHS0 is not a character device"
[ -c "$R$BT_POWER" ] || fail "$R$BT_POWER is not a character device"
[ -c "$R$BT_TTY" ] || fail "$R$BT_TTY is not a character device"
[ -S "$R$BT_SOCKET" ] || fail "$R$BT_SOCKET is not a socket"
socket_stat=$($B stat -c '%a:%u:%g' "$R$BT_SOCKET")
[ "$socket_stat" = "660:0:$BT_BLUE_HOST_GID" ] || fail "socket $R$BT_SOCKET has $socket_stat, expected 660:0:$BT_BLUE_HOST_GID"
note "socket=$socket_stat"

# 9. Ask init to start the OHOS side in deterministic order:
#    (a) stop any premature pre-HDI bluetooth_service + start hmos_bluetooth_hdi;
#    (b) wait for hci_interface_service PUBLISH_RC=0 and stale PID exit;
#    (c) start bluetooth_service (SA 1130) + fusion_connectivity_manager_ext (SA 1191).
stage=native_services
$B chroot "$R" /system/bin/param set vendor.setup_mpxx_dir_mode success || true
$B chroot "$R" /system/bin/param set marble.debug.start.bluetooth.hdi true || fail "param set marble.debug.start.bluetooth.hdi failed"
i=0
while [ "$i" -lt "$BT_WAIT_TICKS" ]; do
 if [ -f "$BT_HDI_LOG" ] && $B grep -q 'PUBLISH_RC=0' "$BT_HDI_LOG" && ! $B pidof bluetooth_service >/dev/null 2>&1; then
  echo "BT_HDI_SERVICE_PUBLISHED=1"
  break
 fi
 i=$((i + 1))
 $B sleep "$BT_WAIT_STEP"
done
if $B grep -q 'PUBLISH_RC=' "$BT_HDI_LOG" 2>/dev/null && ! $B grep -q 'PUBLISH_RC=0' "$BT_HDI_LOG"; then
 $B tail -n 20 "$BT_HDI_LOG"
 fail "hci_interface_service publish failed (see $BT_HDI_LOG)"
fi
[ "$i" -lt "$BT_WAIT_TICKS" ] || echo "BT_HDI_PUBLISH_UNCONFIRMED within $BT_WAIT_TICKS ticks (see $BT_HDI_LOG)"
$B chroot "$R" /system/bin/param set marble.debug.start.bluetooth true || fail "param set marble.debug.start.bluetooth failed"
stage=complete
echo "BT_NATIVE_SERVICE_REQUESTED=1"
echo "BT_CHECK_SA1130=chroot $R /system/bin/sa_check 1130"
echo "BT_LOGS=$BT_BRIDGE_LOG $BT_HDI_LOG"
