#!/bin/sh
# SPDX-License-Identifier: MIT
# Bounded Phase 3 core-service trigger and diagnostics collector for Xiaomi marble.
set -u
B=/busybox
R=/hmos-root
LOG="$R/data/local/tmp/phase3-status.txt"

klog() {
 printf '<6>[MARBLE-PHASE3] %s\n' "$*" > /dev/kmsg 2>/dev/null || true
 printf '[MARBLE-PHASE3] %s\n' "$*"
}

ohos_param() {
 $B chroot "$R" /system/bin/param "$@" 2>&1 || true
}

wait_param() {
 key="$1"
 want="$2"
 limit="$3"
 i=0
 while [ "$i" -lt "$limit" ]; do
  val=$(ohos_param get "$key" | $B tr -d ' \t\r\n')
  if [ "$val" = "$want" ]; then
   klog "PARAM_READY $key=$val after=${i}s"
   return 0
  fi
  $B sleep 1
  i=$((i + 1))
 done
 klog "PARAM_TIMEOUT $key want=$want got=${val:-empty} after=${limit}s"
 return 1
}

klog "START waiting for native init + samgr + hdf_devmgr"
wait_param bootevent.samgr.ready true 35 || true
wait_param bootevent.hdf_devmgr.ready true 20 || true

klog "TRIGGER marble.debug.start.coredeps=true"
ohos_param set marble.debug.start.coredeps true >/dev/null
wait_param bootevent.param_watcher.started true 15 || true
wait_param bootevent.concurrentTaskService.ready true 15 || true
$B sleep 3

klog "TRIGGER marble.debug.start.account=true"
ohos_param set marble.debug.start.account true >/dev/null
wait_param bootevent.account.ready true 20 || true
$B sleep 2

{
 echo "=== MARBLE PHASE 3 STATUS SNAPSHOT ==="
 echo "--- BOOTEVENTS ---"
 for k in \
  bootevent.samgr.ready \
  bootevent.hdf_devmgr.ready \
  bootevent.param_watcher.started \
  bootevent.concurrentTaskService.ready \
  bootevent.huksService.ready \
  bootevent.account.ready \
  marble.debug.start.coredeps \
  marble.debug.start.huks \
  marble.debug.start.account; do
  printf '%s=%s\n' "$k" "$(ohos_param get "$k" | $B tr -d ' \t\r\n')"
 done

 echo "--- PROCESSES ---"
 for p in /proc/[0-9]*; do
  pid=${p##*/}
  cmd=$($B tr '\0' ' ' < "$p/cmdline" 2>/dev/null || true)
  [ -n "$cmd" ] && printf 'pid=%-6s %s\n' "$pid" "$cmd"
 done

 echo "--- DEV NODES ---"
 $B ls -la "$R/dev" "$R/dev/unix/socket" 2>&1 || true

 echo "--- HIDUMPER -LS ---"
 $B timeout 15 $B chroot "$R" /system/bin/hidumper -ls 2>&1 || true

 echo "--- HILOG TAIL (120 lines) ---"
 $B timeout 10 $B chroot "$R" /system/bin/hilog -x 2>&1 | $B tail -n 120 || true
} > "$LOG" 2>&1

if $B test -d /metadata/hmos-debug; then
 $B cp "$LOG" /metadata/hmos-debug/phase3-status.txt 2>/dev/null || true
fi

# Emit compact summary to kmsg so live COM9 captures service states immediately.
for line in \
 "samgr=$(ohos_param get bootevent.samgr.ready | $B tr -d ' \t\r\n')" \
 "hdf_devmgr=$(ohos_param get bootevent.hdf_devmgr.ready | $B tr -d ' \t\r\n')" \
 "param_watcher=$(ohos_param get bootevent.param_watcher.started | $B tr -d ' \t\r\n')" \
 "concurrent_task=$(ohos_param get bootevent.concurrentTaskService.ready | $B tr -d ' \t\r\n')" \
 "huks=$(ohos_param get bootevent.huksService.ready | $B tr -d ' \t\r\n')" \
 "account=$(ohos_param get bootevent.account.ready | $B tr -d ' \t\r\n')"; do
 klog "SUMMARY $line"
done

sa_count=$($B timeout 10 $B chroot "$R" /system/bin/hidumper -ls 2>/dev/null | $B wc -l || echo 0)
klog "MARBLE_PHASE3_DIAG_COMPLETE sa_lines=$sa_count log=$LOG"
