#!/busybox sh
# Marble PAL/AGM runtime self-test.  Runs ON THE DEVICE, read-only: no partition
# writes, no adb/fastboot.  Safe as a single serial command:
#   sh /compat/audio-runtime/pal_selftest.sh
# Env: RT=/compat/audio-runtime  LOG=/hmos-root/data/local/tmp/marble-audio.log
set -u
RT=${RT:-/compat/audio-runtime}
LD=$RT/bin/linker64
LOG=${LOG:-/hmos-root/data/local/tmp/marble-audio.log}
G=$(command -v grep 2>/dev/null || echo /busybox/grep)
FAIL=0
say() { echo "PAL_SELFTEST $*"; }
bad() { say "FAIL $*"; FAIL=1; }

[ -x "$LD" ] || { bad "no-linker:$LD"; exit 1; }
say "RT=$RT linker=ok"

# 1) Every shipped object must resolve its DT_NEEDED inside lib64/.
n=0; miss=""
for so in "$RT"/lib64/*.so; do
  n=$((n+1))
  out=$("$LD" --list "$so" 2>&1)
  case "$out" in
    *"not found"*|*"cannot locate symbol"*|*"CANNOT LINK"*) miss="$miss $(basename "$so")" ;;
    *"unknown option"*|*"Usage"*) say "LINKER_NO_LIST_SUPPORT"; break ;;
  esac
done
say "LIBS=$n UNRESOLVED=${miss:-none}"
[ -z "$miss" ] || FAIL=1

# 2) End-to-end relocation through the vendor host binary (libar-pal/AGM closure).
out=$("$LD" --list "$RT/bin/agmplay" 2>&1)
case "$out" in
  *"not found"*|*"cannot locate symbol"*|*"CANNOT LINK"*) bad "agmplay-link" ;;
  *) say "AGMPLAY_LINK=ok" ;;
esac

# 3) The two route libraries the PAL stack imports must both be present.
for so in libaudioroute.so libaudioroute_ext.so libtinyalsa.so; do
  [ -f "$RT/lib64/$so" ] || bad "missing:$so"
done

# 4) PAL server handshake.  A missing log means "not started yet", not a failure.
if [ -f "$LOG" ]; then
  if $G -q 'PAL_SERVER_READY=1' "$LOG"; then
    say "PAL_SERVER_READY=1"
  else
    bad "PAL_SERVER_READY missing in $LOG"
  fi
  $G -m1 'AUDIOROUTE_SHIM_INIT_FAIL' "$LOG" 2>/dev/null && FAIL=1
else
  say "PAL_SERVER_READY=skip (no $LOG)"
fi

say "RESULT=$( [ "$FAIL" -eq 0 ] && echo PASS || echo FAIL )"
exit "$FAIL"
