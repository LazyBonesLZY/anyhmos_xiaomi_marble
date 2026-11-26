#!/busybox sh
set -eu
B=/busybox
R=/hmos-root

i=0
while [ "$i" -lt 20 ]; do
 [ -r /sys/class/fastrpc/adsprpc-smd/dev ] && break
 i=$((i + 1))
 $B sleep 0.1
done

stage_char() {
 sys="$1"
 node="$2"
 mode="$3"
 name=${node##*/}
 [ -r "$sys/dev" ] || return 0
 nums=$($B cat "$sys/dev")
 major=${nums%:*}
 minor=${nums#*:}
 parent=${node%/*}
 $B mkdir -p "$parent"
 if [ ! -c "$node" ] || [ "$($B stat -c '%t:%T' "$node" 2>/dev/null || true)" != "$(printf '%x:%x' "$major" "$minor")" ]; then
  $B rm -f "$node"
  $B mknod "$node" c "$major" "$minor"
 fi
 $B chown 1000:1000 "$node"
 $B chmod "$mode" "$node"
 echo "FASTRPC_NODE name=$name dev=$major:$minor mode=$mode"
}

stage_char /sys/class/fastrpc/adsprpc-smd "$R/dev/adsprpc-smd" 0664
stage_char /sys/class/fastrpc/adsprpc-smd-secure "$R/dev/adsprpc-smd-secure" 0644
echo "MARBLE_FASTRPC_NODES=$([ -c "$R/dev/adsprpc-smd" ] && echo ok || echo missing)"
