#!/busybox sh
# SPDX-License-Identifier: MIT
# Batch F2: manual modem online/LPM, IPA/RMNET kernel module loading, and gated RIL + telephony SA startup.
# Safety: read-only modem/EFS/persist, zero speaker audio changes, zero unauthorized calls/SMS/data.
set -eu

if [ -d /hmos-root/system ]; then
	R=/hmos-root
else
	R=""
fi

if [ -x /busybox ]; then
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

run_in_hmos() {
	if [ -n "$R" ]; then
		bb chroot "$R" "$@"
	else
		"$@"
	fi
}

PROBE=/compat/bin/marble_modem_probe
F1_START=/compat/bin/start-modem-f1-marble.sh
if [ -n "$R" ] && [ ! -x "$PROBE" ] && [ -x "$R/compat/bin/marble_modem_probe" ]; then
	PROBE="$R/compat/bin/marble_modem_probe"
fi
if [ -n "$R" ] && [ ! -x "$F1_START" ] && [ -x "$R/compat/bin/start-modem-f1-marble.sh" ]; then
	F1_START="$R/compat/bin/start-modem-f1-marble.sh"
fi

ensure_mss_and_qrtr() {
	rproc_state=$(bb cat /sys/class/remoteproc/remoteproc2/state 2>/dev/null || echo missing)
	if [ "$rproc_state" != "running" ]; then
		if [ -x "$F1_START" ]; then
			echo "F2_MODEM_ENSURE_F1=starting"
			bb sh "$F1_START"
		else
			echo "F2_MODEM_ERR=missing_f1_script"
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
			echo "F2_QRTR_READY=1 count=$qrtr_cnt wait_s=$i"
			return 0
		fi
		i=$((i + 1))
		bb sleep 1
	done
	echo "F2_QRTR_TIMEOUT=1 count=$qrtr_cnt"
	return 1
}

do_online() {
	ensure_mss_and_qrtr
	"$PROBE" --set-mode online || echo "F2_SET_MODE_ONLINE_RC=$?"
	i=0
	op_ok=0
	while [ "$i" -lt 3 ]; do
		dms_out=$("$PROBE" --dms 2>/dev/null || true)
		if printf '%s\n' "$dms_out" | bb grep -q 'op_mode=0(online)'; then
			op_ok=1
			break
		fi
		i=$((i + 1))
		bb sleep 1
	done
	echo "F2_ONLINE_WAIT ok=$op_ok wait_s=$i"
	"$PROBE" --dms
	"$PROBE" --uim
	"$PROBE" --sim-ef || true
	"$PROBE" --nas-ext || true
	"$PROBE" --loc-rev || true
}

do_lpm() {
	ensure_mss_and_qrtr
	"$PROBE" --set-mode lpm
	"$PROBE" --dms
}

find_ko() {
	ko="$1"
	for dir in \
		"/lib/modules" \
		"${R}/vendor_dlkm/lib/modules" \
		"/vendor_dlkm/lib/modules" \
		"${R}/vendor/lib/modules" \
		"/vendor/lib/modules"; do
		if [ -f "${dir}/${ko}" ]; then
			printf '%s\n' "${dir}/${ko}"
			return 0
		fi
	done
	return 1
}

do_ipa() {
	loaded_list=""
	missing_list=""
	failed_list=""
	for ko in ipa_fmwk.ko gsim.ko ipam.ko ipanetm.ko rmnet_ctl.ko rmnet_core.ko; do
		mod=$(printf '%s' "${ko%.ko}" | bb tr - _)
		if bb grep -q "^${mod} " /proc/modules 2>/dev/null; then
			loaded_list="${loaded_list}${mod}(already),"
			continue
		fi
		kpath=$(find_ko "$ko" || true)
		if [ -z "$kpath" ]; then
			missing_list="${missing_list}${ko},"
			continue
		fi
		if bb insmod "$kpath" 2>&1; then
			loaded_list="${loaded_list}${mod}(new),"
		else
			failed_list="${failed_list}${mod}(rc=$?),"
		fi
	done
	echo "F2_IPA_LOADED=${loaded_list:-none}"
	echo "F2_IPA_MISSING=${missing_list:-none}"
	echo "F2_IPA_FAILED=${failed_list:-none}"
	if [ -d /sys/class/net ]; then
		rmnets=$(bb ls /sys/class/net 2>/dev/null | bb grep -E '^(rmnet|ipa)' | bb tr '\n' ',' || true)
		echo "F2_RMNET_NETDEVS=${rmnets:-none}"
	fi
}

do_telephony() {
	ensure_mss_and_qrtr
	vendor_so="${R}/compat/lib64/libril_vendor_qmi.z.so"
	target_so="${R}/vendor/lib64/libril_vendor.z.so"
	if [ ! -f "$vendor_so" ]; then
		echo "F2_TEL_ERR=missing_libril_vendor_qmi"
		return 1
	fi
	if [ ! -f "$target_so" ]; then
		echo "F2_TEL_ERR=missing_target_libril_vendor"
		return 1
	fi
	if ! bb grep -q " ${target_so} " /proc/mounts 2>/dev/null; then
		bb mount --bind "$vendor_so" "$target_so"
		bb mount -o remount,ro,bind "$target_so" 2>/dev/null || true
	fi
	echo "F2_RIL_VENDOR_BOUND=1"

	vlib_val=$(run_in_hmos /system/bin/param get const.sys.radio.vendorlib.path 2>/dev/null | bb tr -d ' \t\r\n' || true)
	if [ "$vlib_val" != "/vendor/lib64/libril_vendor.z.so" ]; then
		run_in_hmos /system/bin/param set const.sys.radio.vendorlib.path /vendor/lib64/libril_vendor.z.so 2>/dev/null || true
		vlib_val=$(run_in_hmos /system/bin/param get const.sys.radio.vendorlib.path 2>/dev/null | bb tr -d ' \t\r\n' || true)
	fi
	sc_val=$(run_in_hmos /system/bin/param get const.telephony.slotCount 2>/dev/null | bb tr -d ' \t\r\n' || true)
	if [ -z "$sc_val" ]; then
		run_in_hmos /system/bin/param set const.telephony.slotCount 2 2>/dev/null || true
		sc_val=$(run_in_hmos /system/bin/param get const.telephony.slotCount 2>/dev/null | bb tr -d ' \t\r\n' || true)
	fi
	echo "F2_TEL_PARAMS vendorlib=${vlib_val:-unset} slotCount=${sc_val:-unset}"

	if [ "$vlib_val" != "/vendor/lib64/libril_vendor.z.so" ]; then
		echo "F2_TEL_ERR=vendorlib_param_mismatch"
		return 1
	fi

	bb mkdir -p "${R}/data/service/el1/public/telephony" "${R}/data/service/el2/public/telephony"
	bb chown 1001:1001 "${R}/data/service/el1/public/telephony" "${R}/data/service/el2/public/telephony" 2>/dev/null || true
	bb chmod 0711 "${R}/data/service/el1/public/telephony" "${R}/data/service/el2/public/telephony"

	run_in_hmos /system/bin/param set marble.debug.start.telephony true

	i=0
	ril_pid=""
	while [ "$i" -lt 10 ]; do
		ril_pid=$(bb ps 2>/dev/null | bb awk '/riladapter_host/ && !/awk/ {print $1}' | bb tr '\n' ' ')
		if [ -n "$ril_pid" ]; then
			break
		fi
		i=$((i + 1))
		bb sleep 1
	done

	if [ -n "$ril_pid" ] && { [ -x "${R}/compat/bin/native_ril_hdi_loader" ] || [ -x /compat/bin/native_ril_hdi_loader ]; }; then
		run_in_hmos /compat/bin/native_ril_hdi_loader --load || echo "F2_RIL_LOADER_RC=$?"
	fi

	i=0
	sa_4010=0
	tel_pid=""
	while [ "$i" -lt 15 ]; do
		ril_pid=$(bb ps 2>/dev/null | bb awk '/riladapter_host/ && !/awk/ {print $1}' | bb tr '\n' ' ')
		tel_pid=$(bb ps 2>/dev/null | bb awk '/telephony-marble/ && !/awk/ {print $1}' | bb tr '\n' ' ')
		if [ -x "${R}/compat/bin/native_sa_check" ] || [ -x /compat/bin/native_sa_check ]; then
			if run_in_hmos /compat/bin/native_sa_check 4010 2>/dev/null | bb grep -q 'NATIVE_SA_READY id=4010'; then
				sa_4010=1
				break
			fi
		elif [ -n "$ril_pid" ] && [ -n "$tel_pid" ]; then
			break
		fi
		i=$((i + 1))
		bb sleep 1
	done

	echo "F2_TEL_PROCS wait_s=$i ril_pid=${ril_pid:-none} tel_pid=${tel_pid:-none} sa4010=$sa_4010"
	for sa in 4010 4007 4008 4009; do
		if [ -x "${R}/compat/bin/native_sa_check" ] || [ -x /compat/bin/native_sa_check ]; then
			res=$(run_in_hmos /compat/bin/native_sa_check "$sa" 2>/dev/null || echo "NATIVE_SA_CHECK_ERR=$sa")
			echo "F2_TEL_SA_${sa}=${res}"
		fi
	done
	if [ -f "${R}/system/lib64/libhmicuuc.z.so" ] || [ -f /system/lib64/libhmicuuc.z.so ]; then
		echo "F2_TEL_ICUUC=present"
	else
		echo "F2_TEL_ICUUC=absent"
	fi
	[ -n "$ril_pid" ] && [ -n "$tel_pid" ] && [ "$sa_4010" -eq 1 ]
}

MODE="${1:---all}"
case "$MODE" in
	--ensure) ensure_mss_and_qrtr ;;
	--online) do_online ;;
	--lpm) do_lpm ;;
	--ipa) do_ipa ;;
	--telephony) do_telephony ;;
	--all)
		do_online
		do_ipa
		do_telephony
		;;
	*)
		echo "Usage: $0 [--ensure|--online|--lpm|--ipa|--telephony|--all]" >&2
		exit 2
		;;
esac
