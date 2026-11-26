#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch M4: Prepare Wi-Fi Hotspot / SoftAP runtime on Xiaomi marble (QCA6490 / SM7475):
#   1. Enable net.ipv4.ip_forward=1 for tethering NAT/routing.
#   2. Prepare /data/service/el1/public/wifi/{wpa_supplicant,sockets/wpa} (0770 wifi:wifi_group)
#      and seed hostapd.conf only if absent (never clobbering existing STA/AP configs).
#   3. Trigger marble.softap.ready=true to launch HDF host-16 (hostapd_host).
# Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}
# Donor /system/etc/{passwd,group}: wifi UID 1010, wifi_group GID 1011.
WIFI_OWNER=${MARBLE_WIFI_OWNER:-1010:1011}
IP_FORWARD_PATH=${MARBLE_IP_FORWARD_PATH:-/proc/sys/net/ipv4/ip_forward}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,softap,*|*,hotspot,*|*,hostapd,*)
    say "SOFTAP_M4_SKIP=requested"
    exit 0
    ;;
esac

# 1. Verify required vendor hostapd HDI + libwpa libraries are present.
for rel in \
  vendor/lib64/libhostapd_hdi_c_device.z.so \
  vendor/lib64/libhostapd_interface_service_1.0.z.so \
  vendor/lib64/libwpa.z.so; do
  if [ ! -f "$R/$rel" ]; then
    say "SOFTAP_M4_ERR=missing_${rel##*/}"
    exit 0
  fi
done

# 2. Enable IPv4 forwarding for tethering (netmanager / libnet_tether_manager.z.so).
if [ -w "$IP_FORWARD_PATH" ]; then
  if echo 1 > "$IP_FORWARD_PATH" 2>/dev/null; then
    fwd_val=$($B cat "$IP_FORWARD_PATH" 2>/dev/null | $B tr -d ' \t\r\n' || echo unknown)
    say "SOFTAP_M4_IP_FORWARD=$fwd_val"
  else
    say "SOFTAP_M4_WARN=ip_forward_write_failed"
  fi
else
  say "SOFTAP_M4_WARN=ip_forward_unwritable path=$IP_FORWARD_PATH"
fi

# 3. Prepare Wi-Fi config & socket directories with 0770 wifi:wifi_group.
WIFI_DIR="$R/data/service/el1/public/wifi"
CONF_DIR="$WIFI_DIR/wpa_supplicant"
SOCK_DIR="$WIFI_DIR/sockets"
WPA_SOCK_DIR="$SOCK_DIR/wpa"
for d in "$WIFI_DIR" "$CONF_DIR" "$SOCK_DIR" "$WPA_SOCK_DIR"; do
  if [ ! -d "$d" ]; then
    $B mkdir -p "$d" 2>/dev/null || say "SOFTAP_M4_WARN=mkdir_failed:$d"
  fi
  $B chown "$WIFI_OWNER" "$d" 2>/dev/null || true
  $B chmod 0770 "$d" 2>/dev/null || true
done

# Remove any stale global hostapd socket left over from an unclean shutdown
# only when hostapd_host is not running.
if ! $B pgrep -f 'hostapd_host.*-i 16|hdf_devhost.*hostapd_host' >/dev/null 2>&1; then
  $B rm -f "$WPA_SOCK_DIR/hostapd" 2>/dev/null || true
fi

# 4. Seed default nl80211 hostapd.conf only if absent.
HAPD_DST="$CONF_DIR/hostapd.conf"
HAPD_SRC=""
for cand in \
  "${MARBLE_HOSTAPD_CONF:-}" \
  "$R/compat/etc/hostapd-marble.conf" \
  /compat/etc/hostapd-marble.conf \
  /graphics/hostapd-marble.conf \
  "$R/system/etc/wifi/hostapd.conf"; do
  if [ -n "$cand" ] && [ -f "$cand" ]; then
    HAPD_SRC="$cand"
    break
  fi
done

if [ -f "$HAPD_DST" ]; then
  say "SOFTAP_M4_CONF=keep path=$HAPD_DST"
elif [ -n "$HAPD_SRC" ]; then
  if $B cp "$HAPD_SRC" "$HAPD_DST" 2>/dev/null; then
    $B chown "$WIFI_OWNER" "$HAPD_DST" 2>/dev/null || true
    $B chmod 0660 "$HAPD_DST" 2>/dev/null || true
    say "SOFTAP_M4_CONF=seed src=$HAPD_SRC dst=$HAPD_DST"
  else
    say "SOFTAP_M4_WARN=seed_copy_failed"
  fi
else
  say "SOFTAP_M4_WARN=no_hostapd_conf_template"
fi

# 5. Request hostapd_host via OHOS init condition parameter.
if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.softap.ready true >/dev/null 2>&1 || say "SOFTAP_M4_PARAM_RC=$?"
else
  say "SOFTAP_M4_ERR=missing_param_bin"
  exit 0
fi

host_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'hostapd_host.*-i 16|hdf_devhost.*hostapd_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "SOFTAP_M4_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "SOFTAP_M4_DONE=1"
exit 0
