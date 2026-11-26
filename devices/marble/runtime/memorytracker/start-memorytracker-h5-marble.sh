#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch H5: Bring up OHOS MemoryTracker HDI v1.0 host-14 (memorytracker_host) backed by
# SM7475 /sys/kernel/dmabuf/buffers, /proc/<pid>/fdinfo and /proc/<pid>/smaps* (100% read-only).
# Strictly zero audio/modem/persist side effects.
set -u

B=${MARBLE_BB:-/busybox}
R=${MARBLE_ROOT:-/hmos-root}

say() { echo "$*"; }

SKIP=$($B cat "$R/data/local/tmp/marble-skip-stages" 2>/dev/null | $B tr -d ' \t\r\n' || true)
case ",$SKIP," in
  *,memorytracker,*|*,memtracker,*|*,memorytracker_hdi,*)
    say "MEMORYTRACKER_H5_SKIP=requested"
    exit 0
    ;;
esac

if [ ! -f "$R/compat/lib64/libmarble_memory_tracker_interface_service_1.0.z.so" ]; then
  say "MEMORYTRACKER_H5_ERR=missing_memorytracker_hdi_lib"
  exit 0
fi
dmabuf_cnt=$($B ls /sys/kernel/dmabuf/buffers 2>/dev/null | $B wc -l || echo 0)
say "MEMORYTRACKER_H5_SYSFS_READY dmabuf_buffers=$dmabuf_cnt"

if [ -x "$R/system/bin/param" ]; then
  $B chroot "$R" /system/bin/param set marble.memorytracker.ready true >/dev/null 2>&1 || say "MEMORYTRACKER_H5_PARAM_RC=$?"
else
  say "MEMORYTRACKER_H5_ERR=missing_param_bin"
  exit 0
fi

host_pid=""
i=0
while [ "$i" -lt 40 ]; do
  host_pid=$($B pgrep -f 'memorytracker_host.*-i 14|hdf_devhost.*memorytracker_host' 2>/dev/null | $B head -n 1 || true)
  if [ -n "$host_pid" ]; then
    break
  fi
  i=$((i + 1))
  $B sleep 0.2
done

say "MEMORYTRACKER_H5_PROCS host_pid=${host_pid:-none} wait_steps=$i"
say "MEMORYTRACKER_H5_DONE=1"
exit 0
