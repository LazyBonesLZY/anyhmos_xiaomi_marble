#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch F7: IPA/QMAP rmnet_data0 setup + QMI NAS/DMS/UIM + OHOS RIL/Data readiness.
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

F6_START=/compat/bin/start-modem-f6-marble.sh
if [ -n "$R" ] && [ ! -x "$F6_START" ] && [ -x "$R/compat/bin/start-modem-f6-marble.sh" ]; then
	F6_START="$R/compat/bin/start-modem-f6-marble.sh"
fi

say() { echo "$*"; }

wait_qmi_ipa_init() {
	w=0
	while [ "$w" -lt 20 ]; do
		if [ -e /sys/class/net/rmnet_ipa0 ] &&
		   bb dmesg 2>/dev/null | bb grep -q 'QMI_IPA_INIT_MODEM_DRIVER_REQ_V01 response received'; then
			say "F7_QMI_IPA_INIT=1 wait_s=$w"
			return 0
		fi
		w=$((w + 1))
		bb sleep 1
	done
	say "F7_QMI_IPA_INIT=0 wait_s=$w"
	return 1
}

do_ipa_data() {
	[ -x "$F6_START" ] || { say "F7_ERR=missing_f6_script"; return 1; }
	[ -x "$PROBE" ] || { say "F7_ERR=missing_probe"; return 1; }

	bb sh "$F6_START" --ipa || { rc=$?; say "F7_IPA_KO_RC=$rc"; return "$rc"; }
	wait_qmi_ipa_init || true

	orig_v6=$(bb cat /proc/sys/net/ipv6/conf/default/disable_ipv6 2>/dev/null || echo 0)
	echo 1 > /proc/sys/net/ipv6/conf/default/disable_ipv6 2>/dev/null || true
	echo 1 > /proc/sys/net/ipv6/conf/rmnet_ipa0/disable_ipv6 2>/dev/null || true
	setup_out=$("$PROBE" --ipa-setup --ifname rmnet0 --mux-id 1 --mtu 1500 2>&1)
	setup_rc=$?
	echo "$orig_v6" > /proc/sys/net/ipv6/conf/default/disable_ipv6 2>/dev/null || true
	printf '%s\n' "$setup_out"
	if [ "$setup_rc" -eq 0 ] && [ -e /sys/class/net/rmnet0 ]; then
		flags=$(bb cat /sys/class/net/rmnet0/flags 2>/dev/null || echo 0x0)
		mtu=$(bb cat /sys/class/net/rmnet0/mtu 2>/dev/null || echo 0)
		say "F7_IPA_DATA_READY=1 dev=rmnet0 mux_id=1 flags=$flags mtu=$mtu"
		return 0
	fi
	say "F7_IPA_DATA_READY=0 rc=$setup_rc"
	return 1
}

ensure_mss_qrtr() {
	rproc_state=$(bb cat /sys/class/remoteproc/remoteproc2/state 2>/dev/null || echo missing)
	if [ "$rproc_state" != "running" ]; then
		if [ -x "$F1_START" ]; then
			say "F7_MODEM_ENSURE_F1=starting"
			bb sh "$F1_START" || return $?
		else
			say "F7_ERR=missing_f1_script"
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
			say "F7_QRTR_READY=1 count=$qrtr_cnt wait_s=$i"
			return 0
		fi
		i=$((i + 1))
		bb sleep 1
	done
	say "F7_QRTR_TIMEOUT=1 count=${qrtr_cnt:-0}"
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
	ensure_mss_qrtr || return $?
	do_ipa_data || return $?

	attempt=0
	op_ok=0
	while [ "$attempt" -lt 3 ] && [ "$op_ok" -eq 0 ]; do
		attempt=$((attempt + 1))
		ensure_mss_qrtr || true
		"$PROBE" --set-mode online || say "F7_SET_MODE_ONLINE_RC=$? attempt=$attempt"
		i=0
		while [ "$i" -lt 10 ]; do
			if "$PROBE" --dms 2>/dev/null | bb grep -q 'op_mode=0(online)'; then
				op_ok=1
				break
			fi
			i=$((i + 1))
			bb sleep 1
		done
		if [ "$op_ok" -eq 0 ] && [ "$attempt" -lt 3 ]; then
			bb sleep 2
		fi
	done
	say "F7_MODEM_ONLINE=$op_ok attempt=$attempt wait_s=${i:-0}"

	# If MPSS experienced SSR during bring-up and unregistered rmnet_ipa0, recreate rmnet0.
	if [ ! -e /sys/class/net/rmnet0 ]; then
		say "F7_IPA_DATA_RECREATE=1"
		do_ipa_data || true
	fi
	"$PROBE" --dpm-open --ep-type 4 --iface-id 1 || say "F7_DPM_OPEN_RC=$?"

	active_sub=$(detect_active_sub)
	say "F7_ACTIVE_SUB=$active_sub"
	"$PROBE" --sub "$active_sub" --nas-ext || true
	[ "$op_ok" -eq 1 ]
}

do_telephony() {
	do_online || return $?
	[ -x "$F2_START" ] || { say "F7_ERR=missing_f2_script"; return 1; }
	bb sh "$F2_START" --telephony || { rc=$?; say "F7_F2_TEL_RC=$rc"; return "$rc"; }

	if [ -n "$R" ] && [ -x "${R}/compat/bin/native_sa_check" ]; then
		bb chroot "$R" /compat/bin/native_sa_check 66250 2>/dev/null || say "F7_SA_66250_CHECK=pending"
	elif [ -x /compat/bin/native_sa_check ]; then
		/compat/bin/native_sa_check 66250 2>/dev/null || say "F7_SA_66250_CHECK=pending"
	fi
	say "F7_TELEPHONY_READY=1"
	return 0
}

do_status() {
	say "F7_RPROC2=$(bb cat /sys/class/remoteproc/remoteproc2/state 2>/dev/null || echo missing)"
	for dev in rmnet_ipa0 rmnet0; do
		if [ -e "/sys/class/net/$dev" ]; then
			fl=$(bb cat "/sys/class/net/$dev/flags" 2>/dev/null || echo 0)
			mtu=$(bb cat "/sys/class/net/$dev/mtu" 2>/dev/null || echo 0)
			say "F7_NETDEV dev=$dev flags=$fl mtu=$mtu"
		else
			say "F7_NETDEV dev=$dev missing=1"
		fi
	done
	if [ -x "$PROBE" ]; then
		"$PROBE" --dms || true
		active_sub=$(detect_active_sub)
		say "F7_ACTIVE_SUB=$active_sub"
		"$PROBE" --sub "$active_sub" --nas-ext || true
	fi
	return 0
}

MODE="${1:---all}"
case "$MODE" in
	--ipa)
		[ -x "$F6_START" ] || exit 1
		bb sh "$F6_START" --ipa
		;;
	--ipa-data) do_ipa_data ;;
	--online) do_online ;;
	--telephony) do_telephony ;;
	--all) do_telephony ;;
	--status) do_status ;;
	*)
		echo "usage: $0 [--ipa|--ipa-data|--online|--telephony|--all|--status]" >&2
		exit 2
		;;
esac
