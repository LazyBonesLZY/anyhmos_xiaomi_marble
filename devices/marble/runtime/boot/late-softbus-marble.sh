# SPDX-License-Identifier: MIT
# Sourced after PHASE4_ORCHESTRATOR_DONE; no fake readiness or blind retry.
marble_late_softbus() {
 stage=late_softbus
 # One owner per boot. A timeout leaves the gate closed until next boot.
 owner="$R/dev/marble-softbus-late-owner"
 if ! $B mkdir "$owner" 2>/dev/null; then
  log 'SOFTBUS_LATE_SKIP owner_exists=1'
  return 0
 fi
 deadline=$(($(now) + 60))
 while :; do
  ready=1
  for id in 1301 401 4606 1120; do
   if ! $B timeout -k 1 2 $B chroot "$R" /compat/bin/native_sa_check "$id" >/dev/null 2>&1; then ready=0; fi
  done
  carrier=$($B cat /sys/class/net/wlan0/carrier 2>/dev/null || true)
  [ "$carrier" = 1 ] || ready=0
  if [ "$ready" = 1 ]; then break; fi
  if [ "$(now)" -ge "$deadline" ]; then
   log "SOFTBUS_LATE_TIMEOUT carrier=$carrier gate_unchanged=1"
   return 0
  fi
  $B sleep 2
 done
 # Do not collapse an already-started/manual instance into an automatic test.
 if [ "$(pget marble.debug.start.softbus_control)" = true ]; then
  log 'SOFTBUS_LATE_SKIP gate_already_true=1'
  return 0
 fi
 if ! $B timeout -k 1 3 $B chroot "$R" /system/bin/param set marble.debug.start.softbus_control true >/dev/null 2>&1; then
  log 'SOFTBUS_LATE_SET_FAILED'
  return 0
 fi
 log 'SOFTBUS_LATE_TRIGGERED'
 wait_sa 4700 20 || true
 wait_sa 4802 30 || true
 carrier=$($B cat /sys/class/net/wlan0/carrier 2>/dev/null || true)
 log "SOFTBUS_LATE_END carrier=$carrier"
}
marble_late_softbus
