# SPDX-License-Identifier: MIT
# Sourced after the bounded SoftBus stage. Original roles and SA checks remain.
marble_late_resource() {
 stage=late_resource
 owner="$R/dev/marble-resource-late-owner"
 if ! $B mkdir "$owner" 2>/dev/null; then
  log 'RESOURCE_LATE_SKIP owner_exists=1'
  return 0
 fi
 deadline=$(($(now) + 60))
 while :; do
  ready=1
  for id in 1301 401 501 4606 1120 4700 4802; do
   if ! $B timeout -k 1 2 $B chroot "$R" /compat/bin/native_sa_check "$id" >/dev/null 2>&1; then ready=0; fi
  done
  carrier=$($B cat /sys/class/net/wlan0/carrier 2>/dev/null || true)
  [ "$carrier" = 1 ] || ready=0
  [ "$ready" = 0 ] || break
  if [ "$(now)" -ge "$deadline" ]; then
   log "RESOURCE_LATE_TIMEOUT carrier=$carrier gate_unchanged=1"
   return 0
  fi
  $B sleep 2
 done
 if [ "$(pget marble.debug.start.resource_trial)" = true ]; then
  log 'RESOURCE_LATE_SKIP gate_already_true=1'
  return 0
 fi
 parent="$R/data/service/el1/public"
 if [ -L "$parent" ] || [ "$($B stat -c %u:%g:%a "$parent" 2>/dev/null)" != 0:0:711 ]; then
  log 'RESOURCE_LATE_DIRECTORY_HOLD parent_mismatch=1'
  return 0
 fi
 # Original init will create absent directories, but never repair unknown owners.
 for spec in ressched:700 efficiency_manager:700 device_standby:700 appnap:700 background_task_mgr:711 WorkScheduler:711 bundle_usage:711; do
  name=${spec%%:*};mode=${spec#*:};path="$parent/$name"
  if [ -L "$path" ]; then
   log "RESOURCE_LATE_DIRECTORY_HOLD name=$name symlink=1"
   return 0
  fi
  if [ -e "$path" ]; then
   if [ ! -d "$path" ] || [ "$($B stat -c %u:%g:%a "$path" 2>/dev/null)" != "1096:1096:$mode" ]; then
    log "RESOURCE_LATE_DIRECTORY_HOLD name=$name identity_mismatch=1"
    return 0
   fi
  fi
 done
 if ! $B timeout -k 1 3 $B chroot "$R" /system/bin/param set marble.debug.start.resource_trial true >/dev/null 2>&1; then
  log 'RESOURCE_LATE_SET_FAILED'
  return 0
 fi
 log 'RESOURCE_LATE_TRIGGERED'
 wait_sa 1901 20 || true
 wait_sa 1903 30 || true
 carrier=$($B cat /sys/class/net/wlan0/carrier 2>/dev/null || true)
 log "RESOURCE_LATE_END carrier=$carrier"
}
marble_late_resource
