#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch F6: clean 8-module IPA/RMNET ordering before MPSS online + multi-SIM Sub1 NAS + Telephony bring-up.
# Safety: never detach/reattach rmnet_ipa, never write persist/EFS, never alter speaker gain or place calls/SMS.
set -u

if [ -d /hmos-root/system ]; then
	R=/hmos-root
else
	R=""
fi

if [ -x "${MARBLE_BB:-}" ]; then
	B="$MARBLE_BB"
elif [ -x /busybox ]; then
	B=/busybox
elif [ -x /compat/bin/busybox ]; then
	B=/compat/bin/busybox
else
	B=""
fi

bb() {
	if [ -n "$B" ]; then
		"$B" "$@"
	else
		"$@"
	fi
}

PROBE=/compat/bin/marble_modem_probe
if [ -n "$R" ] && [ ! -x "$PROBE" ] && [ -x "$R/compat/bin/marble_modem_probe" ]; then
	PROBE="$R/compat/bin/marble_modem_probe"
fi

F1_START=/compat/bin/start-modem-f1-marble.sh
if [ -n "$R" ] && [ ! -x "$F1_START" ] && [ -x "$R/compat/bin/start-modem-f1-marble.sh" ]; then
	F1_START="$R/compat/bin/start-modem-f1-marble.sh"
fi

F2_START=/compat/bin/start-modem-f2-marble.sh
if [ -n "$R" ] && [ ! -x "$F2_START" ] && [ -x "$R/compat/bin/start-modem-f2-marble.sh" ]; then
	F2_START="$R/compat/bin/start-modem-f2-marble.sh"
fi

say() { echo "$*"; }

find_ko() {
	ko="$1"
	for dir in \
		"/hmos/vendor_dlkm/lib/modules" \
		"${R}/vendor_dlkm/lib/modules" \
		"/vendor_dlkm/lib/modules" \
		"/compat/modules" \
		"${R}/compat/modules" \
		"/vendor/lib/modules"; do
		if [ -f "${dir}/${ko}" ]; then
			printf '%s\n' "${dir}/${ko}"
			return 0
		fi
	done
	return 1
}

load_ko_step() {
	ko="$1"
	mod=$(printf '%s' "${ko%.ko}" | bb tr - _)
	if bb grep -q "^${mod} " /proc/modules 2>/dev/null; then
		loaded_list="${loaded_list}${mod}(already),"
		return 0
	fi
	kpath=$(find_ko "$ko" || true)
	if [ -z "$kpath" ]; then
		missing_list="${missing_list}${ko},"
		return 1
	fi
	if bb insmod "$kpath" 2>&1; then
		loaded_list="${loaded_list}${mod}(new),"
		return 0
	fi
	failed_list="${failed_list}${mod}(rc=$?),"
	return 1
}

do_ipa() {
	loaded_list=""
	missing_list=""
	failed_list=""

	for ko in ipa_fmwk.ko gsim.ko ipam.ko rndisipam.ko ipa_clientsm.ko; do
		load_ko_step "$ko" || true
	done

	i=0
	ipa_ready=0
	while [ "$i" -lt 25 ]; do
		if bb dmesg 2>/dev/null | bb grep -q 'IPA driver is now in ready state'; then
			ipa_ready=1
			break
		fi
		bb usleep 200000 2>/dev/null || bb sleep 1
		i=$((i + 1))
	done

	load_ko_step "ipanetm.ko" || true

	j=0
	rmnet_ready=0
	while [ "$j" -lt 25 ]; do
		if [ -e /sys/class/net/rmnet_ipa0 ]; then
			rmnet_ready=1
			break
		fi
		bb usleep 200000 2>/dev/null || bb sleep 1
		j=$((j + 1))
	done

	for ko in rmnet_ctl.ko rmnet_core.ko; do
		load_ko_step "$ko" || true
	done

	rmnets=$(bb ls /sys/class/net 2>/dev/null | bb grep -E '^(rmnet|ipa)' | bb tr '\n' ',' || true)
	say "F6_IPA_LOADED=${loaded_list:-none}"
	say "F6_IPA_MISSING=${missing_list:-none}"
	say "F6_IPA_FAILED=${failed_list:-none}"
	say "F6_IPA_READY=$ipa_ready wait_iters=$i rmnet_ipa0=$rmnet_ready net_iters=$j"
	say "F6_RMNET_NETDEVS=${rmnets:-none}"

	[ -z "$missing_list" ] && [ -z "$failed_list" ] && [ "$rmnet_ready" -eq 1 ]
}

ensure_mss_qrtr() {
	rproc_state=$(bb cat /sys/class/remoteproc/remoteproc2/state 2>/dev/null || echo missing)
	if [ "$rproc_state" != "running" ]; then
		if [ -x "$F1_START" ]; then
			say "F6_MODEM_ENSURE_F1=starting"
			bb sh "$F1_START" || return $?
		else
			say "F6_ERR=missing_f1_script"
			return 1
		fi
	fi

	i=0
	qrtr_cnt=0
	while [ "$i" -lt 30 ]; do
		svc_out=$("$PROBE" --services 2>/dev/null || true)
		qrtr_cnt=$(printf '%s\n' "$svc_out" | bb awk -F'QRTR_REMOTE_COUNT=' 'NF>1 {print $2; exit}' | bb awk '{print $1}')
		case "$qrtr_cnt" in
			""|*[!0-9]*) qrtr_cnt=0 ;;
		esac
		if [ "$qrtr_cnt" -ge 8 ] &&
		   printf '%s\n' "$svc_out" | bb grep -q 'name=dms' &&
		   printf '%s\n' "$svc_out" | bb grep -q 'name=nas' &&
		   printf '%s\n' "$svc_out" | bb grep -q 'name=uim'; then
			say "F6_QRTR_READY=1 count=$qrtr_cnt wait_s=$i"
			return 0
		fi
		i=$((i + 1))
		bb sleep 1
	done
	say "F6_QRTR_TIMEOUT=1 count=${qrtr_cnt:-0}"
	return 1
}

detect_active_sub() {
	uim_out=$("$PROBE" --uim 2>/dev/null || true)
	if printf '%s\n' "$uim_out" | bb grep -q 'slot=1 state=1(present)'; then
		echo "1"
	elif printf '%s\n' "$uim_out" | bb grep -q 'slot=0 state=1(present)'; then
		echo "0"
	else
		echo "1"
	fi
}

do_online() {
	do_ipa || { rc=$?; say "F6_IPA_ERR_RC=$rc"; return "$rc"; }
	ensure_mss_qrtr || return $?

	"$PROBE" --set-mode online || say "F6_SET_MODE_ONLINE_RC=$?"
	i=0
	op_ok=0
	while [ "$i" -lt 15 ]; do
		if "$PROBE" --dms 2>/dev/null | bb grep -q 'op_mode=0(online)'; then
			op_ok=1
			break
		fi
		i=$((i + 1))
		bb sleep 1
	done
	say "F6_MODEM_ONLINE=$op_ok wait_s=$i"

	active_sub=$(detect_active_sub)
	say "F6_ACTIVE_SUB=$active_sub"
	"$PROBE" --sub "$active_sub" --nas-ext || true
	[ "$op_ok" -eq 1 ]
}

do_telephony() {
	do_online || return $?
	[ -x "$F2_START" ] || { say "F6_ERR=missing_f2_script"; return 1; }
	bb sh "$F2_START" --telephony || { rc=$?; say "F6_F2_TEL_RC=$rc"; return "$rc"; }

	if [ -n "$R" ] && [ -x "${R}/compat/bin/native_sa_check" ]; then
		bb chroot "$R" /compat/bin/native_sa_check 66250 2>/dev/null || say "F6_SA_66250_CHECK=pending"
	elif [ -x /compat/bin/native_sa_check ]; then
		/compat/bin/native_sa_check 66250 2>/dev/null || say "F6_SA_66250_CHECK=pending"
	fi
	say "F6_TELEPHONY_READY=1"
	return 0
}

do_status() {
	say "F6_RPROC2=$(bb cat /sys/class/remoteproc/remoteproc2/state 2>/dev/null || echo missing)"
	rmnets=$(bb ls /sys/class/net 2>/dev/null | bb grep -E '^(rmnet|ipa)' | bb tr '\n' ',' || true)
	say "F6_RMNET_NETDEVS=${rmnets:-none}"
	if [ -x "$PROBE" ]; then
		"$PROBE" --dms || true
		active_sub=$(detect_active_sub)
		say "F6_ACTIVE_SUB=$active_sub"
		"$PROBE" --sub "$active_sub" --nas-ext || true
	fi
	return 0
}

MODE="${1:---all}"
case "$MODE" in
	--ipa) do_ipa ;;
	--online) do_online ;;
	--telephony) do_telephony ;;
	--all) do_telephony ;;
	--status) do_status ;;
	*)
		echo "usage: $0 [--ipa|--online|--telephony|--all|--status]" >&2
		exit 2
		;;
esac
