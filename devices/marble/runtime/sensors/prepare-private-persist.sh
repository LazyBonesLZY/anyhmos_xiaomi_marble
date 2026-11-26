#!/busybox sh
set -eu
B=/busybox
R=/hmos-root
DST="${MARBLE_SENSOR_PERSIST_DST:-$R/data/service/el1/public/marble_sensors/sensors}"
STAMP="$DST/.seeded_v1"

if [ ! -f "$STAMP" ] || [ ! -d "$DST/registry" ]; then
 MNT="$R/dev/.marble-persist-ro"
 cleanup_persist() {
  $B umount "$MNT" 2>/dev/null || true
  $B rmdir "$MNT" 2>/dev/null || true
 }
 trap cleanup_persist EXIT INT TERM
 $B umount "$MNT" 2>/dev/null || true
 $B mkdir -p "$MNT" "$DST"
 if ! $B mount -t ext4 -o ro,noload /dev/block/by-name/persist "$MNT" 2>/dev/null; then
  $B mount -o ro /dev/block/by-name/persist "$MNT"
 fi
 if [ -d "$MNT/sensors" ]; then
  $B cp -a "$MNT/sensors/." "$DST/"
 fi
 $B touch "$STAMP"
 $B chown -R 1000:1000 "$DST"
 $B chmod 0755 "$DST"
 cleanup_persist
 trap - EXIT INT TERM
fi

if $B grep -q persist /proc/mounts 2>/dev/null; then
 echo "MARBLE_PERSIST_LEAK=1" >&2
 exit 1
fi
echo "MARBLE_SENSOR_PERSIST_SEEDED=1 files=$($B ls "$DST" 2>/dev/null | $B wc -l)"
