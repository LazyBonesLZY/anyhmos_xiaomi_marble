"""Static + offline-runtime contracts for the marble USB-gadget / HDC bring-up.

Two layers, both offline (no device, no serial, no flashing):

* ``MarbleUsbStaticTests`` - artifact contracts: shell syntax, init-cfg JSON,
  VID/PID against the marble stock dump, guarded /sys writes, ordering,
  privileges.  Nothing executes the device script.
* ``MarbleUsbMockRuntimeTests`` - runs ``start-usb-marble.sh`` end to end
  against a mock busybox + mock configfs/sysfs/param tree.  The mock reproduces
  the two kernel behaviours this script depends on, both verified in the pinned
  source tree:
    - ``configfs.c``: ``mkdir usb_gadget/<name>`` also creates
      ``functions/ configs/ strings/ os_desc/`` (gadget_info default groups);
    - ``f_fs.c``: functionfs registers lazily inside ``_ffs_alloc_dev()`` and
      ``mount -t functionfs <inst>`` resolves ``fc->source`` against the ffs
      instance, so a mount without the matching ``functions/ffs.<inst>`` fails.
  Therefore an ordering bug (probing functionfs, or mounting ep0, before the
  gadget skeleton exists) makes the mock fail exactly like the kernel would.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import textwrap
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
USB_DIR = ROOT / 'devices/marble/runtime/usb'
BASELINE_PATH = USB_DIR / 'marble-stock-usb-baseline.json'
INIT_CFG = USB_DIR / 'init-usb-marble.cfg'
START_SH = USB_DIR / 'start-usb-marble.sh'
STOP_SH = USB_DIR / 'stop-usb-marble.sh'
HDC_SH = USB_DIR / 'hdc-usb-permit.sh'
STOCK_TARBALL = ROOT / 'work/marble-stock-vendor/marble-stock-vendor.tar.gz'
STOCK_RC_MEMBER = 'vendor/etc/init/hw/init.qcom.usb.rc'

SHELL_ARTIFACTS = (START_SH, STOP_SH, HDC_SH)

# Packages that would turn hdcd into a general-purpose remote control plane.
HDC_FORBIDDEN_PERMISSIONS = (
    'ohos.permission.INSTALL_BUNDLE',
    'ohos.permission.CLEAN_APPLICATION_DATA',
    'ohos.permission.CLEAN_BACKGROUND_PROCESSES',
    'ohos.permission.MANAGE_LOCAL_ACCOUNTS',
    'ohos.permission.INSTALL_ALLOW_DOWNGRADE',
)

UDC_NAME = 'a600000.dwc3'


def read_text(path):
    return path.read_text(encoding='utf-8')


def script_code_lines(text):
    """Script body without comments, for 'must never contain' assertions."""
    return '\n'.join(line for line in text.splitlines()
                     if not line.lstrip().startswith('#'))


class MarbleUsbStaticTests(unittest.TestCase):
    def test_artifacts_exist(self):
        for path in SHELL_ARTIFACTS + (INIT_CFG, BASELINE_PATH):
            self.assertTrue(path.is_file(), f'missing artifact: {path}')

    def test_shell_scripts_parse(self):
        bash = shutil.which('bash')
        self.assertIsNotNone(bash, 'bash is required to syntax-check the artifacts')
        for path in SHELL_ARTIFACTS:
            with self.subTest(script=path.name):
                proc = subprocess.run([bash, '-n', str(path)],
                                      capture_output=True, text=True, check=False)
                self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_start_script_is_posix_sh_compatible(self):
        """The device runs it with busybox ash; dash is a stricter stand-in."""
        sh = shutil.which('dash') or shutil.which('sh')
        self.assertIsNotNone(sh)
        proc = subprocess.run([sh, '-n', str(START_SH)],
                              capture_output=True, text=True, check=False)
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def test_scripts_are_strict_and_staged(self):
        marker = {'start-usb-marble.sh': 'USB_STAGE=',
                  'stop-usb-marble.sh': 'USB_STOP_STAGE=',
                  'hdc-usb-permit.sh': 'USB_HDC_STAGE='}
        for path in SHELL_ARTIFACTS:
            with self.subTest(script=path.name):
                text = read_text(path)
                self.assertIn('set -eu', text)
                self.assertIn(marker[path.name], text,
                              'every step must print its stage marker')
                stages = re.findall(r'^stage=([a-z0-9-]+)$', text, re.M)
                self.assertGreaterEqual(len(stages), 5,
                                        f'expected >=5 stage transitions, got {stages}')
                self.assertEqual(len(stages), len(set(stages)), f'duplicate stages: {stages}')
                self.assertIn('trap', text)

    # ------------------------------------------------------------------ cfg
    def test_init_cfg_is_valid_json(self):
        doc = json.loads(read_text(INIT_CFG))
        self.assertIsInstance(doc, dict)
        self.assertIn('services', doc)
        self.assertIn('jobs', doc)

    def test_init_cfg_services_keep_donor_fields(self):
        doc = json.loads(read_text(INIT_CFG))
        names = {svc['name'] for svc in doc['services']}
        self.assertEqual(names, {'hdcd', 'mtpfs', 'usb_service'})
        by_name = {svc['name']: svc for svc in doc['services']}
        self.assertEqual(by_name['hdcd']['secon'], 'u:r:hdcd:s0')
        self.assertEqual(by_name['usb_service']['secon'], 'u:r:usb_service:s0')
        self.assertEqual(by_name['usb_service']['path'],
                         ['/system/bin/sa_main', '/system/profile/usb_service.json'])
        self.assertEqual(by_name['usb_service']['apl'], 'system_basic')
        self.assertIn('u:r:mtpfs:s0', by_name['mtpfs']['secon'])
        for svc in doc['services']:
            self.assertIn('path', svc)
            self.assertNotEqual(svc.get('uid'), 'root', 'no service may run as root')

    def test_experimental_services_disabled_by_default(self):
        doc = json.loads(read_text(INIT_CFG))
        for svc in doc['services']:
            with self.subTest(service=svc['name']):
                self.assertEqual(svc.get('disabled'), 1,
                                 'experimental USB services must not auto-start')

    def test_hdcd_is_not_critical(self):
        doc = json.loads(read_text(INIT_CFG))
        hdcd = next(s for s in doc['services'] if s['name'] == 'hdcd')
        self.assertNotIn('critical', hdcd)

    def test_hdcd_profile_is_minimal(self):
        doc = json.loads(read_text(INIT_CFG))
        hdcd = next(s for s in doc['services'] if s['name'] == 'hdcd')
        granted = set(hdcd.get('permission', [])) | set(hdcd.get('permission_acls', []))
        leaked = granted & set(HDC_FORBIDDEN_PERMISSIONS)
        self.assertFalse(leaked, f'hdcd must not hold {sorted(leaked)}')

    def test_usb_param_job_disables_shell_and_root(self):
        doc = json.loads(read_text(INIT_CFG))
        job = next(j for j in doc['jobs'] if j['name'] == 'param:marble-usb')
        cmds = ' ; '.join(job['cmds'])
        self.assertIn('setparam persist.hdc.control.shell false', cmds)
        self.assertIn('setparam persist.hdc.control.fport false', cmds)
        self.assertIn('setparam persist.hdc.root 0', cmds)
        self.assertIn('setparam persist.hdc.mode.tcp disable', cmds)
        self.assertIn('setparam persist.hdc.mode.uart disable', cmds)
        self.assertIn('start hdcd', cmds)
        self.assertEqual(job['condition'], 'marble.debug.start.usb=true')

    def test_hdc_data_dirs_match_donor_context_labels(self):
        doc = json.loads(read_text(INIT_CFG))
        dirs = ' ; '.join(cmd for job in doc['jobs'] for cmd in job['cmds'])
        self.assertIn('/data/service/el1/public/hdc', dirs)
        self.assertIn('restorecon /data/service/el1/public/hdc', dirs)

    # ------------------------------------------------------- vid/pid + dump
    def test_vid_pid_match_stock_compositions(self):
        baseline = json.loads(read_text(BASELINE_PATH))
        comps = baseline['compositions']
        vid, pid = self._script_vid_pid()
        pairs = {(c['vid'].lower(), c['pid'].lower()) for c in comps.values()}
        self.assertIn((vid.lower(), pid.lower()), pairs,
                      f'{vid}/{pid} is not a marble stock composition pair')

    def test_mtp_composition_pair_is_the_stock_mtp_one(self):
        baseline = json.loads(read_text(BASELINE_PATH))
        self.assertEqual(baseline['compositions']['mtp'], {'vid': '0x2717', 'pid': '0xff40'})
        self.assertEqual(baseline['compositions']['mtp,adb'], {'vid': '0x2717', 'pid': '0xff48'})
        self.assertEqual(baseline['gadget']['name'], 'g1')
        self.assertEqual(baseline['gadget']['config'], 'b.1')
        self.assertEqual(baseline['gadget']['configfs_mount'], '/config')
        self.assertEqual(baseline['gadget']['udc_name'], UDC_NAME)
        self.assertEqual(baseline['gadget']['os_desc']['qw_sign'], 'MSFT100')

    def test_baseline_records_the_uiaf_style_ffs_names(self):
        baseline = json.loads(read_text(BASELINE_PATH))
        self.assertEqual(baseline['functions']['ffs_mtp'], 'ffs.mtp')
        self.assertEqual(baseline['functions']['ffs_ptp'], 'ffs.ptp')
        self.assertEqual(baseline['functions']['mtp'], 'mtp.gs0')
        self.assertIn('functionfs', baseline['functions']['functionfs_mtp_mount'])

    def test_dump_sha256_matches_local_stock_tarball(self):
        baseline = json.loads(read_text(BASELINE_PATH))
        recorded = baseline['source']['sha256']
        self.assertRegex(recorded, r'^[0-9a-f]{64}$')
        if not STOCK_TARBALL.is_file():
            self.skipTest('local stock vendor tarball not present; only the recorded SHA is checked')
        with tarfile.open(STOCK_TARBALL) as tar:
            member = tar.extractfile(STOCK_RC_MEMBER)
            self.assertIsNotNone(member, f'{STOCK_RC_MEMBER} missing from the tarball')
            body = member.read()
        self.assertEqual(len(body), baseline['source']['bytes'])
        self.assertEqual(hashlib.sha256(body).hexdigest(), recorded)
        text = body.decode('utf-8').lower()
        for name, comp in baseline['compositions'].items():
            with self.subTest(composition=name):
                self.assertIn(f'idvendor {comp["vid"].lower()}', text)
                self.assertIn(f'idproduct {comp["pid"].lower()}', text)

    @staticmethod
    def _script_vid_pid():
        text = read_text(START_SH)
        vid = re.search(r'USB_VID=\$\{USB_VID:-([^}]+)\}', text)
        pid = re.search(r'USB_PID=\$\{USB_PID:-([^}]+)\}', text)
        assert vid and pid, 'USB_VID/USB_PID defaults not found'
        return vid.group(1), pid.group(1)

    def test_udc_name_is_discovered_not_hardcoded(self):
        text = read_text(START_SH)
        self.assertIn('USB_UDC=${USB_UDC:-}', text)
        self.assertIn('/class/udc', text)
        self.assertNotIn(UDC_NAME, script_code_lines(text))

    # ------------------------------------------------------ guarded sysfs IO
    def test_no_unguarded_redirect_into_sys_or_configfs(self):
        pattern = re.compile(r'(?<![0-9])>\s*"?((?:/sys|/config|\$SYSFS|\$GCFG|\$DCONF|\$CFG)[^\s";|]*)')
        for path in SHELL_ARTIFACTS:
            with self.subTest(script=path.name):
                offenders = []
                for lineno, line in enumerate(read_text(path).splitlines(), 1):
                    if line.strip().startswith('#'):
                        continue
                    for match in pattern.finditer(line):
                        target = match.group(1)
                        if target in ('"$1"', '$1'):
                            continue
                        offenders.append((lineno, target))
                self.assertFalse(offenders, f'blind redirect(s) to /sys or configfs: {offenders}')

    def test_write_helpers_check_existence(self):
        text = read_text(START_SH)
        for helper in ('write_attr', 'write_attr_req'):
            with self.subTest(helper=helper):
                body = re.search(rf'^{helper}\(\) \{{(.*?)^\}}', text, re.S | re.M)
                self.assertIsNotNone(body, f'{helper}() not found')
                snippet = body.group(1)
                self.assertIn('[ ! -e', snippet, f'{helper} must test node existence')
                self.assertIn('> "$1"', snippet, f'{helper} must be the only writer')
        req = re.search(r'^write_attr_req\(\) \{(.*?)^\}', text, re.S | re.M).group(1)
        self.assertIn('return 1', req, 'mandatory attributes must abort when missing')

    def test_configfs_dirs_are_created_through_the_mk_helper(self):
        text = read_text(START_SH)
        mk = re.search(r'^mk\(\) \{(.*?)^\}', text, re.S | re.M)
        self.assertIsNotNone(mk)
        self.assertIn('[ -d "$1" ]', mk.group(1))
        for call in re.findall(r'^\s*mk "([^"]+)"', text, re.M):
            with self.subTest(dir=call):
                self.assertTrue(call.startswith(('$GCFG', '$DCONF')),
                                f'unexpected mk() target {call}')

    def test_mount_points_are_checked_before_mounting(self):
        """Both mount helpers go through is_mounted_as() (mount table read)."""
        text = read_text(START_SH)
        probe = re.search(r'^is_mounted_as\(\) \{(.*?)^\}', text, re.S | re.M)
        self.assertIsNotNone(probe, 'is_mounted_as() missing')
        self.assertIn('$MOUNT_TABLE', probe.group(1))
        for helper in ('mount_configfs_at', 'mount_ffs'):
            with self.subTest(helper=helper):
                body = re.search(rf'^{helper}\(\) \{{(.*?)^\}}', text, re.S | re.M)
                self.assertIsNotNone(body)
                snippet = body.group(1)
                self.assertIn('is_mounted_as', snippet,
                              f'{helper} must check the mount table before mounting')
                self.assertIn('ALREADY_MOUNTED', snippet)

    def test_mount_table_is_overridable_for_offline_tests(self):
        text = read_text(START_SH)
        self.assertIn('MOUNT_TABLE=${USB_MOUNT_TABLE:-/proc/mounts}', text)

    # -------------------------------------------------- cold-boot ordering
    def test_functionfs_probe_comes_after_gadget_creation(self):
        """Cold configfs: functions/ only exists after mkdir usb_gadget/g1."""
        text = read_text(START_SH)
        code = script_code_lines(text)
        mk_gadget = code.index('mk "$GCFG"')
        probe = code.index('ffs_probe=')
        probe_mkdir = code.index('$B mkdir "$ffs_probe"')
        self.assertLess(mk_gadget, probe,
                        'probe must run after the gadget skeleton is created')
        self.assertLess(mk_gadget, probe_mkdir)
        # the old bug: probing $CFG/functions/... before /config was even configfs
        self.assertNotIn('$CFG/functions/', code)

    def test_configfs_is_mounted_before_any_gadget_path_is_used(self):
        code = script_code_lines(read_text(START_SH))
        mount_configfs = code.index('mount_configfs_at "$CFG"')
        self.assertLess(mount_configfs, code.index('mk "$GCFG"'))
        self.assertLess(mount_configfs, code.index('ffs_probe='))

    def test_functionfs_support_is_not_inferred_from_proc_filesystems(self):
        """f_fs.c registers lazily, so /proc/filesystems is empty on cold boot."""
        code = script_code_lines(read_text(START_SH))
        self.assertNotIn('grep -q functionfs /proc/filesystems', code)
        self.assertIn('USB_FUNCTIONFS_OK', code)
        self.assertIn('USB_FUNCTIONFS_EVIDENCE', code)

    def test_configfs_support_is_probed_by_mounting(self):
        code = script_code_lines(read_text(START_SH))
        self.assertNotIn('grep -q configfs /proc/filesystems', code)
        self.assertIn('USB_CONFIGFS_MOUNT_FAILED', code)

    # ------------------------------------------------------- host namespace
    def test_refuses_to_run_inside_chroot(self):
        code = script_code_lines(read_text(START_SH))
        self.assertIn('USB_NOT_HOST_NAMESPACE', code)
        self.assertIn('[ ! -d "$R/system/bin" ]', code)

    # --------------------------------------------------- endpoint readiness
    def test_no_bind_without_this_rounds_endpoint(self):
        code = script_code_lines(read_text(START_SH))
        self.assertNotIn('binding anyway', code.lower())
        self.assertIn('USB_FFS_NOT_READY', code)
        self.assertIn('keep_usb ffs-endpoint-not-ready 49', code)
        self.assertLess(code.index('keep_usb ffs-endpoint-not-ready 49'),
                        code.index('stage=udc-bind'))

    def test_stale_ffs_flag_is_reset_on_fresh_mount(self):
        code = script_code_lines(read_text(START_SH))
        self.assertIn('USB_FFS_FLAG_STALE_RESET', code)
        self.assertIn('HDC_FFS_MOUNTED_NOW', code)
        self.assertIn('setp sys.usb.ffs.ready 0', code)
        # the params are reported but never gate: they are echoes whose writer
        # is unproven on marble, and a leftover 1 proves nothing
        readiness = code[code.index('stage=ffs-ready'):code.index('stage=udc-bind')]
        self.assertIn('init.svc.hdcd', readiness)
        self.assertIn('sys.usb.ffs.ready', readiness)
        self.assertIn('ffs_check hdc', readiness)
        self.assertIn('USB_FFS_FLAG_UNTRUSTED', readiness)
        self.assertNotIn('"${ffs_flag:-0}" = 1 ] && [ "$hdcd_state" = running ]',
                         readiness)

    def test_hdcd_is_started_by_the_existing_init_job(self):
        code = script_code_lines(read_text(START_SH))
        self.assertIn('setp marble.debug.start.usb true', code)
        # never exec the daemon directly, and never touch the policy flag
        self.assertNotIn('/system/bin/hdcd', code)
        self.assertNotIn('setp const.security.developermode.state', code)
        self.assertNotIn('setp ctl.start', code)
        self.assertNotIn('param set ctl.start', code)
        self.assertNotIn('nsenter', code)
        self.assertNotIn('su ', code)

    def test_ffs_mount_uses_the_donor_identity(self):
        text = read_text(START_SH)
        self.assertIn('USB_FFS_UID=${USB_FFS_UID:-2000}', text)
        self.assertIn('USB_FFS_GID=${USB_FFS_GID:-2000}', text)
        self.assertIn('USB_FFS_FMODE=${USB_FFS_FMODE:-0660}', text)
        self.assertIn('USB_FFS_RMODE=${USB_FFS_RMODE:-0770}', text)
        self.assertIn('mount -t functionfs -o "$opts" "$inst" "$dir"', text)
        self.assertIn('verify_ep0 hdc "$USB_FFS_UID" "$USB_FFS_GID"', text)
        code = script_code_lines(text)
        for forbidden in ('chown ', 'chmod 0', 'setenforce', 'chcon'):
            self.assertNotIn(forbidden, code)

    # -------------------------------------------------------------- failure
    def test_udc_is_never_stolen_from_another_gadget(self):
        code = script_code_lines(read_text(START_SH))
        self.assertIn('USB_UDC_BUSY_OTHER_GADGET', code)
        self.assertIn('keep_usb udc-held-by-other-gadget 47', code)
        self.assertLess(code.index('udc_owner_scan'), code.index('stage=udc-bind'))

    def test_virtual_udc_is_documented_and_never_bound(self):
        text = read_text(START_SH)
        code = script_code_lines(text)
        self.assertIn("grep -v '^dummy'", code)
        self.assertIn('USB_UDC_VIRTUAL', code)
        self.assertIn('keep_usb virtual-udc 53', code)
        header = text[:text.index('set -eu')]
        self.assertRegex(header, r'(?m)^#\s+53\s')

    def test_a_bound_gadget_must_link_ffs_hdc_before_reporting_hdc(self):
        code = script_code_lines(read_text(START_SH))
        self.assertIn('g1_has_ffs', code)
        self.assertIn('USB_HDC_LINK_MISSING', code)
        self.assertIn('keep_usb bound-without-ffs-hdc 49', code)
        # the link is checked before the endpoint evidence is reported
        self.assertLess(code.index('g1_has_ffs; then'), code.index('USB_FFS_READY=1'))

    def test_bind_gate_is_the_kernel_endpoint_set(self):
        """ep1..epN are kernel evidence; the params only echo."""
        code = script_code_lines(read_text(START_SH))
        # readiness is ep1 AND ep2 inside this instance's functionfs mount
        self.assertIn('ffs_check hdc', code)
        self.assertIn('ep1=', code)
        self.assertIn('ep2=', code)
        self.assertIn('is_mounted_as "$d" functionfs "$1"', code)
        # a failed stale-flag reset must not be papered over
        self.assertIn('USB_FFS_FLAG_RESET_FAILED', code)
        ready = code[code.index('stage=ffs-ready'):code.index('stage=udc-bind')]
        self.assertLess(ready.index('ffs_check hdc'), ready.index('USB_FFS_READY=1'))
        # the gate runs before the bind and its failure keeps the original USB
        self.assertLess(code.index('keep_usb ffs-endpoint-not-ready 49'),
                        code.index('stage=udc-bind'))

    def test_existing_bindings_are_excluded_before_gadget_writes(self):
        code = script_code_lines(read_text(START_SH))
        scan = code.index('stage=udc-scan')
        self.assertLess(scan, code.index('stage=gadget-skeleton'))
        self.assertLess(code.index('udc_owner_scan || keep_usb udc-held-by-other-gadget 47'),
                        code.index('mk "$GCFG"'))
        self.assertIn('USB_UDC_ALREADY_BOUND udc=$own', code)
        self.assertLess(scan, code.index('write_attr_req "$GCFG/idVendor"'))

    def test_proc_endpoint_ownership_probe_is_read_only_and_non_gating(self):
        code = script_code_lines(read_text(START_SH))
        self.assertIn('/proc/[0-9]*/comm', code)
        self.assertIn('USB_FFS_EP0_HOLDER', code)
        for line in code.splitlines():
            if 'USB_FFS_EP0_HOLDER' in line or 'ep0_holder' in line:
                with self.subTest(line=line):
                    self.assertNotIn('keep_usb', line)
                    self.assertNotIn('exit ', line)

    def test_bind_is_the_last_mutation(self):
        text = read_text(START_SH)
        code = script_code_lines(text)
        first_udc_write = code.index('write_attr_req "$GCFG/UDC"')
        for keep in re.finditer(r'keep_usb [a-z-]+ (\d+)', code):
            with self.subTest(code=keep.group(1)):
                self.assertLess(keep.start(), first_udc_write,
                                'every keep_usb path must run before the UDC write')

    def test_exit_codes_are_documented(self):
        text = read_text(START_SH)
        header = text[:text.index('set -eu')]
        documented = {int(code) for code in re.findall(r'^#\s+(\d+)\s', header, re.M)}
        used = {int(code) for code in re.findall(r'^\s*exit (\d+)', text, re.M)}
        used |= {int(code) for code in re.findall(r'keep_usb [a-z-]+ (\d+)', text)}
        self.assertTrue(documented, 'exit code table missing')
        self.assertEqual(used - documented, set(),
                         f'undocumented exit codes: {sorted(used - documented)}')
        self.assertIn(43, used)
        self.assertIn(49, used)

    def test_udc_rebinding_is_guarded_and_retried(self):
        text = read_text(START_SH)
        self.assertIn('USB_UDC_RETRY', text)
        self.assertIn('USB_UDC_ALREADY_BOUND', text)
        self.assertIn('USB_UDC_BIND_RETRY', text)
        self.assertIn('USB_UDC_WRITE_REFUSED', text)
        self.assertIn('write_attr_req "$GCFG/UDC" "$udc"', text)
        # configfs.c: a "" write runs unregister_gadget() and drops the live
        # connection, so no retry may detach a holder (stop-usb-marble.sh owns
        # the deliberate detach path).
        self.assertNotIn('write_attr "$GCFG/UDC" ""', text)
        self.assertIn('USB_NOTHING_TO_DO already-bound', text)
        self.assertIn('write_attr "$GCFG/UDC" ""', read_text(STOP_SH))

    def test_mtp_requires_an_explicit_responder_opt_in(self):
        code = script_code_lines(read_text(START_SH))
        self.assertIn('USB_MTP_RESPONDER=${USB_MTP_RESPONDER:-0}', code)
        self.assertIn('USB_MTP_RESPONDER_ABSENT', code)
        self.assertNotIn('f_generic', code)
        self.assertIn('ffs.mtp', code)

    # -------------------------------------------------------------- rollback
    def test_rollback_unbinds_udc_first(self):
        text = read_text(STOP_SH)
        unbind = text.index('stage=unbind-udc')
        self.assertLess(unbind, text.index('stage=unlink'))
        self.assertLess(unbind, text.index('stage=teardown'))
        self.assertIn('write_attr "$GCFG/UDC" ""', text)

    def test_rollback_defaults_do_not_unload_modules_or_unmount_ffs(self):
        text = read_text(STOP_SH)
        self.assertIn('USB_STOP_UNMOUNT=${USB_STOP_UNMOUNT:-0}', text)
        self.assertIn('USB_STOP_UNLOAD=${USB_STOP_UNLOAD:-0}', text)

    def test_rollback_documents_console_independence(self):
        text = read_text(STOP_SH)
        self.assertIn('serial console', text)
        self.assertIn('dwc3-msm.ko', text)

    # ------------------------------------------------------------ hdc profile
    def test_hdc_profile_defaults_to_no_root_and_no_shell(self):
        text = read_text(HDC_SH)
        self.assertIn('MARBLE_HDC_ALLOW_ROOT=${MARBLE_HDC_ALLOW_ROOT:-0}', text)
        self.assertIn('setp persist.hdc.root 0', text)
        self.assertIn('setp persist.hdc.control.shell false', text)
        self.assertIn('setp persist.hdc.control.fport false', text)
        self.assertIn('setp persist.hdc.mode.uart disable', text)
        root_enable = text.index('setp persist.hdc.root 1')
        opt_in = text.index('MARBLE_HDC_ALLOW_ROOT', 0)
        self.assertLess(opt_in, root_enable,
                        'root may only be enabled behind the explicit opt-in flag')

    def test_hdc_profile_documents_host_prerequisites(self):
        text = read_text(HDC_SH)
        self.assertIn('hdc list targets', text)
        self.assertIn('/dev/usb-ffs/hdc/ep0', text)
        self.assertIn('/data/service/el1/public/hdc/hdc_keys', text)
        self.assertIn('Ver: 3.2.0f', text)


# --------------------------------------------------------------------------- #
# Mock runtime: run the real script against a fake device root.
# --------------------------------------------------------------------------- #

MOCK_BUSYBOX = r'''#!/bin/sh
# Mock busybox for offline tests.  Only the syscall-ish commands that differ
# from a normal userspace are emulated; everything else is exec'd.
ROOT="@@ROOT@@"
LOG="$ROOT/log/calls.log"
PARAMS="$ROOT/params"
MOUNTS="$ROOT/mounts"
mockparam() {
 awk -F= -v k="$1" '$1 == k { sub(/^[^=]*=/, ""); print; exit }' "$PARAMS" 2>/dev/null
}
mockset() {
 if [ "$(mockparam mock.param.readonly)" = "$1" ]; then return 1; fi
 if grep -q "^$1=" "$PARAMS" 2>/dev/null; then
  awk -F= -v k="$1" -v v="$2" 'BEGIN { OFS="=" } $1 == k { $2 = v } { print }' \
   "$PARAMS" > "$PARAMS.tmp" && mv "$PARAMS.tmp" "$PARAMS"
 else
  printf '%s=%s\n' "$1" "$2" >> "$PARAMS"
 fi
}
cmd="$1"
[ $# -gt 0 ] && shift
log() { printf '%s\n' "$*" >> "$LOG"; }

case "$cmd" in
 mount)
  log "mount $*"
  fstype=""
  opts=""
  src=""
  dir=""
  while [ $# -gt 0 ]; do
   case "$1" in
    -t) fstype="$2"; shift 2 ;;
    -o) opts="$2"; shift 2 ;;
    *) src="$1"; dir="$2"; shift 2 ;;
   esac
  done
  if [ -n "${MOCK_FAIL_FSTYPE:-}" ] && [ "$fstype" = "$MOCK_FAIL_FSTYPE" ]; then
   echo "mock: mount $fstype $dir failed" >&2
   exit 32
  fi
  case "$fstype" in
   configfs)
    /bin/mkdir -p "$dir/usb_gadget"
    printf 'configfs %s configfs rw 0 0\n' "$dir" >> "$MOUNTS"
    exit 0
    ;;
   functionfs)
    # f_fs.c: ffs_acquire_dev(fc->source) must find the created instance.
    case "$dir" in
     */usb-ffs/*) inst=$(basename "$dir") ;;
     *) inst="$src" ;;
    esac
    if [ ! -d "$ROOT/config/usb_gadget/g1/functions/ffs.$inst" ]; then
     echo "mock: functionfs instance '$inst' not found" >&2
     exit 19
    fi
    /bin/mkdir -p "$dir"
    : > "$dir/ep0"
    chmod 0660 "$dir/ep0"
    printf '%s %s functionfs rw,uid=%s 0 0\n' "$src" "$dir" "$opts" >> "$MOUNTS"
    exit 0
    ;;
  esac
  echo "mock: unsupported fs '$fstype'" >&2
  exit 32
  ;;
 mkdir)
  log "mkdir $*"
  /bin/mkdir "$@" || exit $?
  target=""
  for a in "$@"; do
   case "$a" in -*) ;; *) target="$a" ;; esac
  done
  parent=$(dirname "$target")
  base=$(basename "$target")
  # configfs creates attribute groups together with their directory, so the
  # mock creates the same attribute files the script writes to.
  case "$parent:$base" in
   */usb_gadget:g1)
    for g in functions configs strings os_desc; do
     /bin/mkdir -p "$target/$g"
    done
    for a in idVendor idProduct bcdUSB bcdDevice UDC; do : > "$target/$a"; done
    for a in use b_vendor_code qw_sign; do : > "$target/os_desc/$a"; done
    ;;
   */usb_gadget/g1/strings:0x409)
    for a in serialnumber manufacturer product; do : > "$target/$a"; done
    ;;
   */configs:b.1)
    # configfs.c: a config group carries its own "strings" subgroup.
    /bin/mkdir -p "$target/strings"
    for a in MaxPower bmAttributes; do : > "$target/$a"; done
    ;;
   */configs/b.1/strings:0x409)
    : > "$target/configuration"
    ;;
   */functions:mass_storage.0)
    /bin/mkdir -p "$target/lun.0"
    for a in file cdrom removable ro; do : > "$target/lun.0/$a"; done
    ;;
  esac
  exit 0
  ;;
 insmod)
  log "insmod $*"
  exit 0
  ;;
 chroot)
  # chroot ROOT /system/bin/param get|set KEY [VALUE]
  bin="$2"
  op="$3"
  key="$4"
  val="${5:-}"
  cur=$(awk -F= -v k="$key" '$1 == k { sub(/^[^=]*=/, ""); print; exit }' "$PARAMS" 2>/dev/null)
  case "$op" in
   get)
    # real devices pad `param get` output with trailing spaces
    if [ -n "$cur" ]; then
     if [ "$(mockparam mock.param.trail)" = 1 ]; then
      printf '%s \n' "$cur"
     else
      printf '%s\n' "$cur"
     fi
    fi
    exit 0
    ;;
   set)
    log "param set $key=$val"
    ro=$(mockparam mock.param.readonly)
    if [ -n "$ro" ] && [ "$ro" = "$key" ]; then
     log "mock: param $key refused"
     echo "mock: param $key is read-only" >&2
     exit 1
    fi
    if [ -f "$PARAMS" ] && grep -q "^$key=" "$PARAMS" 2>/dev/null; then
     awk -F= -v k="$key" -v v="$val" 'BEGIN { OFS="=" } $1 == k { $2 = v } { print }' \
      "$PARAMS" > "$PARAMS.tmp" && mv "$PARAMS.tmp" "$PARAMS"
    else
     printf '%s=%s\n' "$key" "$val" >> "$PARAMS"
    fi
    # Emulate what the existing init job for marble.debug.start.usb would do
    # when the daemon is allowed to come up in this scenario.
    if [ "$key" = "marble.debug.start.usb" ] && [ "$(mockparam mock.hdcd.starts)" = 1 ]; then
     log "mock: init job started hdcd"
     mockset init.svc.hdcd running
     mockset sys.usb.ffs.ready 1
     # f_fs.c: ep1..epN appear only after the daemon wrote descriptors+strings.
     n=$(mockparam mock.hdcd.eps)
     case "$n" in ''|*[!0-9]*) n=2 ;; esac
     d="$ROOT/hmos-root/dev/usb-ffs/hdc"
     if [ -d "$d" ]; then
      i=1
      while [ "$i" -le "$n" ]; do
       : > "$d/ep$i"
       i=$((i + 1))
      done
     fi
    fi
    exit 0
    ;;
  esac
  exit 1
  ;;
esac
exec "$cmd" "$@"
'''



class MockSandbox:
    """A disposable fake device root for start-usb-marble.sh."""

    def __init__(self, tmp: Path, params=None, fail_fstype=None, host_namespace=True,
                 udc_name=UDC_NAME):
        self.root = tmp
        self.ffs_uid = os.getuid()
        self.ffs_gid = os.getgid()
        (tmp / 'log').mkdir(parents=True, exist_ok=True)
        (tmp / 'params').write_text('')
        (tmp / 'mounts').write_text('')
        (tmp / 'sys/class/udc').mkdir(parents=True, exist_ok=True)
        if udc_name:
            udc_dir = tmp / 'sys/class/udc' / udc_name
            udc_dir.mkdir(parents=True, exist_ok=True)
            (udc_dir / 'state').write_text('not attached\n')
            (udc_dir / 'current_speed').write_text('unknown\n')
        self.ohos = tmp / 'hmos-root'
        (self.ohos / 'dev').mkdir(parents=True, exist_ok=True)
        (self.ohos / 'vendor_dlkm/lib/modules').mkdir(parents=True, exist_ok=True)
        if host_namespace:
            (self.ohos / 'system/bin').mkdir(parents=True, exist_ok=True)
        self.config = tmp / 'config'
        self.config.mkdir(parents=True, exist_ok=True)

        stub = tmp / 'busybox'
        body = MOCK_BUSYBOX.replace('@@ROOT@@', str(tmp))
        stub.write_text(body)
        stub.chmod(0o755)

        for key, value in (params or {}).items():
            self.set_param(key, value)
        self.fail_fstype = fail_fstype

    # -- param store -------------------------------------------------------
    def set_param(self, key, value):
        path = self.root / 'params'
        lines = [line for line in path.read_text().splitlines() if not line.startswith(f'{key}=')]
        lines.append(f'{key}={value}')
        path.write_text('\n'.join(lines) + '\n')

    def param(self, key, default=''):
        for line in (self.root / 'params').read_text().splitlines():
            if line.startswith(f'{key}='):
                return line.split('=', 1)[1]
        return default

    def config_path(self, *parts):
        return self.config.joinpath(*parts)

    def link_ffs_hdc(self, gadget, cfg='b.1'):
        """Create the configfs link a real g1 needs to report HDC."""
        func = gadget / 'functions/ffs.hdc'
        func.mkdir(parents=True, exist_ok=True)
        conf = gadget / 'configs' / cfg
        conf.mkdir(parents=True, exist_ok=True)
        link = conf / 'f1'
        if not link.is_symlink():
            # configfs synthesizes a path relative to the config group.
            link.symlink_to(os.path.relpath(func, conf))
        return link

    def pre_mount_ffs(self, inst='hdc', eps=0, source=None):
        """Simulate a previous run in the same boot: ffs already mounted.

        ``source`` overrides the mount-table source field, which must name the
        same instance for the mount to count as this round's ep0.
        """
        d = self.ohos / 'dev/usb-ffs' / inst
        d.mkdir(parents=True, exist_ok=True)
        ep0 = d / 'ep0'
        ep0.write_text('')
        ep0.chmod(0o660)
        for i in range(1, eps + 1):
            (d / f'ep{i}').write_text('')
        with (self.root / 'mounts').open('a') as fh:
            fh.write(f'{source or inst} {d} functionfs rw 0 0\n')
        return ep0

    def write_config_file(self, rel, content):
        path = self.config / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    # -- execution ---------------------------------------------------------
    def run(self, mode='hdc', timeout=60, env_extra=None, script=None):
        env = {
            'PATH': '/usr/bin:/bin',
            'HOME': str(self.root),
            'USB_BUSYBOX': str(self.root / 'busybox'),
            'USB_OHOS_ROOT': str(self.ohos),
            'USB_SYSFS': str(self.root / 'sys'),
            'USB_CONFIGFS': str(self.config),
            'USB_MOUNT_TABLE': str(self.root / 'mounts'),
            'USB_FFS_UID': str(self.ffs_uid),
            'USB_FFS_GID': str(self.ffs_gid),
            'USB_WAIT_FFS_SECONDS': '1',
            'USB_WAIT_UDC_SECONDS': '1',
            'MODE': mode,
        }
        if self.fail_fstype:
            env['MOCK_FAIL_FSTYPE'] = self.fail_fstype
        env.update(env_extra or {})
        proc = subprocess.run(['/bin/sh', str(script or START_SH)],
                              capture_output=True, text=True, env=env, timeout=timeout)
        return proc

    def calls(self):
        log = self.root / 'log/calls.log'
        return log.read_text() if log.is_file() else ''

    def udc_file(self):
        return self.config / 'usb_gadget/g1/UDC'


class MarbleUsbMockRuntimeTests(unittest.TestCase):
    def setUp(self):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory(prefix='marble-usb-mock-')
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    # -- helpers -----------------------------------------------------------
    def sandbox(self, **kwargs):
        # Realistic first run: the daemon is not up yet, the script asks the
        # existing init job for it, and that job brings it up on this round's
        # ep0 (mock.hdcd.starts=1 models the init job).
        params = {
            'const.security.developermode.state': 'true',
            'init.svc.hdcd': 'stopped',
            'sys.usb.ffs.ready': '0',
            'mock.hdcd.starts': '1',
        }
        params.update(kwargs.pop('params', {}))
        return MockSandbox(self.tmp, params=params, **kwargs)

    def assert_not_bound(self, sb):
        """configfs always exposes g1/UDC; unbound means the file is empty."""
        udc = sb.udc_file()
        if udc.exists():
            self.assertEqual(udc.read_text().strip(), '',
                             'UDC must stay unbound when the run keeps the original USB')

    def assert_keep_original(self, proc):
        self.assertIn('USB_KEEP_ORIGINAL', proc.stdout,
                      f'expected a keep-original exit, got:\n{proc.stdout}\n{proc.stderr}')
        sb = None
        return sb

    # -- ordering ----------------------------------------------------------
    def test_cold_boot_probes_functionfs_after_gadget_creation(self):
        sb = self.sandbox()
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('USB_FUNCTIONFS_OK', proc.stdout)
        self.assertNotIn('USB_NO_FUNCTIONFS_SUPPORT', proc.stdout)
        gadget = proc.stdout.index('USB_STAGE=gadget-skeleton')
        probe = proc.stdout.index('USB_FUNCTIONFS_OK')
        self.assertLess(gadget, probe, 'probe must follow the gadget skeleton')
        calls = sb.calls()
        g1 = calls.index('mkdir %s/usb_gadget/g1\n' % sb.config)
        probe_mkdir = calls.index('ffs.hmos_probe')
        self.assertLess(g1, probe_mkdir)

    def test_mutation_of_the_old_probe_path_is_caught(self):
        """Negative control: the pre-fix probe path must fail on cold configfs.

        The old code probed ``$CFG/functions/ffs.*`` (missing the
        ``usb_gadget/g1`` level) and reported "kernel has no functionfs" on a
        cold boot.  The mock reproduces that failure, so the passing run above
        is evidence that the current ordering fixed it.
        """
        src = read_text(START_SH)
        mutated = src.replace('ffs_probe="$GCFG/functions/ffs.hmos_probe"',
                              'ffs_probe="$CFG/functions/ffs.hmos_probe"')
        self.assertNotEqual(src, mutated, 'mutation anchor not found')
        path = self.tmp / 'start-usb-marble-old-probe.sh'
        path.write_text(mutated)
        sb = self.sandbox()
        proc = sb.run(script=path)
        self.assertEqual(proc.returncode, 52, proc.stdout + proc.stderr)
        self.assertIn('USB_NO_FUNCTIONFS_SUPPORT', proc.stdout)
        self.assert_not_bound(sb)

    def test_functionfs_probe_proves_lazy_registration(self):
        """The mock only accepts a functionfs mount with a real ffs instance."""
        sb = self.sandbox()
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue((sb.config / 'usb_gadget/g1/functions/ffs.hdc').is_dir())
        # the probe instance must be gone again
        self.assertFalse((sb.config / 'usb_gadget/g1/functions/ffs.hmos_probe').exists())
        # and the mount was accepted only because ffs.hdc existed at mount time
        self.assertIn('USB_FFS_MOUNTED inst=hdc', proc.stdout)

    # -- happy path --------------------------------------------------------
    def test_binds_only_after_endpoint_is_ready(self):
        sb = self.sandbox()
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('USB_UDC_BOUND', proc.stdout)
        self.assertIn('USB_FFS_READY=1 mount=1 ep1=1 ep2=1 inst=hdc', proc.stdout)
        ready = proc.stdout.index('USB_FFS_READY=1')
        bound = proc.stdout.index('USB_UDC_BOUND')
        self.assertLess(ready, bound, 'UDC bind must come after endpoint readiness')
        self.assertEqual(sb.udc_file().read_text().strip(), UDC_NAME)
        self.assertIn(f'USB_FFS_EP0_OWNER={sb.ffs_uid}:{sb.ffs_gid}', proc.stdout)
        # no process holds ep0 in the mock: that probe is evidence, not a gate
        self.assertIn('USB_FFS_EP0_HOLDER inst=hdc held_by=none', proc.stdout)

    def test_mounts_functionfs_with_donor_identity(self):
        sb = self.sandbox()
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        calls = sb.calls()
        self.assertIn('mount -t functionfs -o', calls)
        self.assertIn(f'uid={sb.ffs_uid},gid={sb.ffs_gid}', calls)
        self.assertIn('fmode=0660', calls)
        self.assertIn('rmode=0770', calls)
        self.assertIn('hdc', calls)
        # no post-mount identity fixups
        for forbidden in ('chown', 'chmod 0', 'chcon', 'setenforce'):
            self.assertNotIn(f'\n{forbidden} ', '\n' + calls)

    # -- keep-original failure paths --------------------------------------
    def test_endpoint_never_ready_keeps_original_usb(self):
        sb = self.sandbox(params={'sys.usb.ffs.ready': '0', 'init.svc.hdcd': 'stopped',
                                  'mock.hdcd.starts': '0'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assert_keep_original(proc)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_stale_flag_is_reset_and_not_trusted(self):
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'stopped',
                                  'mock.hdcd.starts': '0'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_FLAG_STALE_RESET prev=1', proc.stdout)
        self.assertIn('USB_HDC_START_REQUEST', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_init_job_start_makes_the_endpoint_ready(self):
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'stopped',
                                  'mock.hdcd.starts': '1'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_FLAG_STALE_RESET prev=1', proc.stdout)
        self.assertIn('USB_FFS_READY=1', proc.stdout)
        self.assertIn('USB_UDC_BOUND', proc.stdout)
        self.assertEqual(sb.param('marble.debug.start.usb'), 'true')

    def test_second_run_with_existing_ep0_binds_without_restart(self):
        """Idempotent re-run: ffs already mounted, daemon already serving it."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'running'})
        sb.pre_mount_ffs('hdc', eps=2)
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_ALREADY_MOUNTED', proc.stdout)
        self.assertNotIn('USB_HDC_START_REQUEST', proc.stdout)
        self.assertNotIn('USB_FFS_FLAG_STALE_RESET', proc.stdout)
        self.assertIn('USB_UDC_BOUND', proc.stdout)

    def test_running_daemon_on_a_fresh_ep0_is_not_trusted(self):
        """Fresh mount + already-running hdcd: that daemon owns a dead ep0."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'running'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_EP0_DAEMON_STALE', proc.stdout)
        self.assert_keep_original(proc)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_other_gadget_holding_udc_is_not_stolen(self):
        sb = self.sandbox()
        sb.write_config_file('usb_gadget/g2/UDC', UDC_NAME + '\n')
        proc = sb.run()
        self.assertEqual(proc.returncode, 47, proc.stdout + proc.stderr)
        self.assertIn('USB_UDC_BUSY_OTHER_GADGET name=g2', proc.stdout)
        self.assert_keep_original(proc)
        self.assert_not_bound(sb)

    def test_fake_running_daemon_without_endpoints_does_not_bind(self):
        """Negative control: params say running/ready, the kernel has no ep1..N."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'stopped',
                                  'mock.hdcd.starts': '1', 'mock.hdcd.eps': '0'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_NOT_READY mount=1 ep1=0 ep2=0', proc.stdout)
        self.assertIn('hdcd=running', proc.stdout)
        self.assertNotIn('USB_FFS_READY=1', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_keep_original(proc)
        self.assert_not_bound(sb)

    def test_failed_stale_flag_reset_is_not_trusted(self):
        """Negative control: the flag stays 1, so only ep files may gate."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'stopped',
                                  'mock.hdcd.starts': '1', 'mock.hdcd.eps': '0',
                                  'mock.param.readonly': 'sys.usb.ffs.ready'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_FLAG_STALE_RESET prev=1', proc.stdout)
        self.assertIn('USB_FFS_FLAG_RESET_FAILED', proc.stdout)
        self.assertIn('USB_FFS_FLAG_UNTRUSTED gate=endpoint-files', proc.stdout)
        self.assertEqual(sb.param('sys.usb.ffs.ready'), '1')
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_endpoint_evidence_outranks_a_late_flag(self):
        """The happy path binds on ep files even when the flag never turns 1."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '0', 'init.svc.hdcd': 'stopped',
                                  'mock.hdcd.starts': '1',
                                  'mock.param.readonly': 'sys.usb.ffs.ready'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_READY=1 mount=1 ep1=1 ep2=1 inst=hdc', proc.stdout)
        self.assertEqual(sb.param('sys.usb.ffs.ready'), '0')
        self.assertIn('USB_UDC_BOUND', proc.stdout)

    def test_already_bound_gadget_is_reported_not_rewritten(self):
        """Idempotent re-run: an existing binding is never detached or rewritten."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'running'})
        sb.pre_mount_ffs('hdc', eps=2)
        gadget = sb.config / 'usb_gadget/g1'
        gadget.mkdir(parents=True)
        (gadget / 'UDC').write_text(UDC_NAME + '\n')
        (gadget / 'UDC').chmod(0o444)
        sb.link_ffs_hdc(gadget)
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn(f'USB_UDC_ALREADY_BOUND udc={UDC_NAME}', proc.stdout)
        self.assertIn('USB_NOTHING_TO_DO already-bound', proc.stdout)
        # the phase4 wrapper greps these two markers; keep them on this path too
        self.assertIn('USB_FFS_READY=1 mount=1 ep1=1 ep2=1 inst=hdc already_bound=1', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assertEqual((gadget / 'UDC').read_text(), UDC_NAME + '\n')
        # nothing about the bound gadget was touched: no attributes, no extra
        # links and no MaxPower write on the live config
        for name in ('idVendor', 'idProduct', 'bcdUSB', 'strings'):
            self.assertFalse((gadget / name).exists(), name)
        self.assertEqual(sorted(p.name for p in (gadget / 'configs/b.1').iterdir()), ['f1'])
        self.assertFalse((gadget / 'configs/b.1/MaxPower').exists())

    def test_bound_gadget_without_endpoints_keeps_the_original_usb(self):
        """Bound but the daemon is gone: report it, detach nothing."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'running'})
        sb.pre_mount_ffs('hdc')
        gadget = sb.config / 'usb_gadget/g1'
        gadget.mkdir(parents=True)
        (gadget / 'UDC').write_text(UDC_NAME + '\n')
        sb.link_ffs_hdc(gadget)
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_NOT_READY mount=1 ep1=0 ep2=0 inst=hdc', proc.stdout)
        self.assertIn('reason=bound-ffs-not-active', proc.stdout)
        self.assertEqual((gadget / 'UDC').read_text(), UDC_NAME + '\n')
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)

    def test_foreign_binding_blocks_before_the_gadget_is_created(self):
        sb = self.sandbox()
        sb.write_config_file('usb_gadget/g2/UDC', UDC_NAME + '\n')
        proc = sb.run()
        self.assertEqual(proc.returncode, 47, proc.stdout + proc.stderr)
        self.assertIn('USB_UDC_BUSY_OTHER_GADGET name=g2', proc.stdout)
        self.assertFalse((sb.config / 'usb_gadget/g1').exists(),
                         'the gadget must not be created while another holds the UDC')
        self.assert_keep_original(proc)

    def test_ep0_holder_is_read_from_proc(self):
        """Read-only /proc proof of who holds ep0; informational only."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'running'})
        ep0 = sb.pre_mount_ffs('hdc', eps=2)
        fake = self.tmp / 'bin/hdcd'
        fake.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(os.path.realpath(sys.executable), fake)
        holder = subprocess.Popen(
            [str(fake), '-c',
             'import os, sys, time; os.open(sys.argv[1], os.O_RDONLY); time.sleep(20)',
             str(ep0)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(holder.wait)
        self.addCleanup(holder.kill)
        time.sleep(0.5)
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertRegex(proc.stdout, r'USB_FFS_EP0_HOLDER inst=hdc held_by= hdcd:\d+')

    def test_probe_acm_gadget_holding_the_udc_is_left_alone(self):
        """Machine F: hmos_probe (ACM/COM9) owns a600000.dwc3. Nothing may move."""
        sb = self.sandbox()
        probe = sb.config / 'usb_gadget/hmos_probe'
        (probe / 'functions/acm.usb0').mkdir(parents=True)
        (probe / 'UDC').write_text(UDC_NAME + '\n')
        conf = probe / 'configs/c.1'
        conf.mkdir(parents=True)
        (conf / 'acm.usb0').symlink_to(probe / 'functions/acm.usb0')
        proc = sb.run()
        self.assertEqual(proc.returncode, 47, proc.stdout + proc.stderr)
        self.assertIn('USB_UDC_BUSY_OTHER_GADGET name=hmos_probe', proc.stdout)
        self.assert_keep_original(proc)
        # the console gadget and its binding are byte-identical afterwards
        self.assertEqual((probe / 'UDC').read_text(), UDC_NAME + '\n')
        self.assertTrue((conf / 'acm.usb0').is_symlink())
        self.assertFalse((sb.config / 'usb_gadget/g1').exists())
        self.assertFalse((sb.ohos / 'dev/usb-ffs').exists())
        self.assertFalse((sb.ohos / 'config').exists())
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)

    def test_virtual_udc_is_never_a_physical_success(self):
        """An explicit dummy_udc.0 must fail closed, not "bind" invisibly."""
        sb = self.sandbox()
        proc = sb.run(env_extra={'USB_UDC': 'dummy_udc.0'})
        self.assertEqual(proc.returncode, 53, proc.stdout + proc.stderr)
        self.assertIn('USB_UDC_VIRTUAL name=dummy_udc.0', proc.stdout)
        self.assert_keep_original(proc)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assertFalse((sb.config / 'usb_gadget/g1').exists())

    def test_prebound_dummy_is_not_reported_as_physical_hdc(self):
        sb = self.sandbox(udc_name='dummy_udc.0')
        sb.pre_mount_ffs('hdc', eps=2)
        sb.write_config_file('usb_gadget/g1/UDC', 'dummy_udc.0\n')
        sb.link_ffs_hdc(sb.config / 'usb_gadget/g1')
        proc = sb.run(env_extra={'USB_UDC': UDC_NAME})
        self.assertEqual(proc.returncode, 53, proc.stdout + proc.stderr)
        self.assertNotIn('USB_FFS_READY=1', proc.stdout)
        self.assertEqual(sb.udc_file().read_text(), 'dummy_udc.0\n')

    def test_virtual_udc_is_skipped_when_discovering(self):
        """Only dummy_hcd present: no physical UDC, so no bind at all."""
        sb = self.sandbox(udc_name='dummy_udc.0')
        proc = sb.run()
        self.assertEqual(proc.returncode, 41, proc.stdout + proc.stderr)
        self.assertIn('USB_NO_UDC', proc.stdout)
        self.assertNotIn('USB_UDC_NAME=dummy_udc.0', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_single_endpoint_is_not_readiness(self):
        """ep1 alone is not the donor contract (hdcd needs ep1=out and ep2=in)."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'running',
                                  'mock.hdcd.starts': '0'})
        sb.pre_mount_ffs('hdc', eps=1)
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_NOT_READY mount=1 ep1=1 ep2=0', proc.stdout)
        self.assertNotIn('USB_FFS_READY=1', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_endpoints_under_a_foreign_mount_source_are_not_readiness(self):
        """The mount must be this instance's; files under another source lie."""
        sb = self.sandbox(params={'sys.usb.ffs.ready': '1', 'init.svc.hdcd': 'running'})
        sb.pre_mount_ffs('hdc', eps=2, source='mtp')
        proc = sb.run()
        self.assertEqual(proc.returncode, 48, proc.stdout + proc.stderr)
        self.assertIn('reason=ffs-hdc-mount', proc.stdout)
        self.assertNotIn('USB_FFS_READY=1', proc.stdout)
        self.assertNotIn('param set marble.debug.start.usb', sb.calls())
        self.assert_not_bound(sb)

    def test_stacked_mounts_do_not_prove_hdc(self):
        sb = self.sandbox()
        ep0 = sb.pre_mount_ffs('hdc', eps=2)
        with (sb.root / 'mounts').open('a') as f:
            f.write(f'tmpfs {ep0.parent} tmpfs rw 0 0\n')
        sb.write_config_file('usb_gadget/g1/UDC', UDC_NAME + '\n')
        sb.link_ffs_hdc(sb.config / 'usb_gadget/g1')
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_NOT_READY mount=0', proc.stdout)
        self.assertNotIn('USB_FFS_READY=1', proc.stdout)
        self.assertEqual(sb.udc_file().read_text(), UDC_NAME + '\n')

    def test_symlink_is_not_an_endpoint(self):
        sb = self.sandbox(params={'mock.hdcd.starts': '0'})
        ep0 = sb.pre_mount_ffs('hdc', eps=1)
        (ep0.parent / 'ep2').symlink_to('ep1')
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_NOT_READY mount=1 ep1=1 ep2=0', proc.stdout)
        self.assert_not_bound(sb)

    def test_padded_param_values_still_pass_the_developer_gate(self):
        """`param get` pads values on F; a padded true must not block."""
        sb = self.sandbox(params={'mock.param.trail': '1'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn('USB_HDC_DEVMODE_BLOCKED', proc.stdout)
        self.assertIn('USB_UDC_BOUND', proc.stdout)

    def test_padded_param_values_still_detect_a_stale_daemon(self):
        """A padded `running` must still trip the stale-ep0 guard."""
        sb = self.sandbox(params={'init.svc.hdcd': 'running', 'sys.usb.ffs.ready': '1',
                                  'mock.param.trail': '1'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 49, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_EP0_DAEMON_STALE', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_ep0_owner_mismatch_keeps_original_usb(self):
        sb = self.sandbox()
        proc = sb.run(env_extra={'USB_FFS_UID': '4321'})
        self.assertEqual(proc.returncode, 48, proc.stdout + proc.stderr)
        self.assertIn('USB_FFS_EP0_OWNER=', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_configfs_mount_failure_keeps_original_usb(self):
        sb = self.sandbox(fail_fstype='configfs')
        proc = sb.run()
        self.assertEqual(proc.returncode, 51, proc.stdout + proc.stderr)
        self.assertIn('USB_CONFIGFS_MOUNT_FAILED', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)

    def test_developer_mode_gate_keeps_original_usb(self):
        sb = self.sandbox(params={'const.security.developermode.state': 'false',
                                  'init.svc.hdcd': 'stopped'})
        proc = sb.run()
        self.assertEqual(proc.returncode, 50, proc.stdout + proc.stderr)
        self.assertIn('USB_HDC_DEVMODE_BLOCKED', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)
        # the script must never flip the policy flag itself
        self.assertNotIn('const.security.developermode.state=true', sb.calls().replace(' ', ''))

    def test_not_host_namespace_is_refused(self):
        sb = self.sandbox(host_namespace=False)
        proc = sb.run()
        self.assertEqual(proc.returncode, 46, proc.stdout + proc.stderr)
        self.assertIn('USB_NOT_HOST_NAMESPACE', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)

    def test_mtp_without_responder_keeps_original_usb(self):
        sb = self.sandbox()
        # The mock cannot chown ep0 to the stock mtp uid/gid (1024), so the
        # test drives the check with the real owner and keeps its focus on the
        # responder gate.
        proc = sb.run(mode='mtp',
                      env_extra={'USB_FFS_MTP_UID': str(sb.ffs_uid),
                                 'USB_FFS_MTP_GID': str(sb.ffs_gid)})
        self.assertEqual(proc.returncode, 45, proc.stdout + proc.stderr)
        self.assertIn('USB_MTP_RESPONDER_ABSENT', proc.stdout)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)
        self.assert_not_bound(sb)

    def test_storage_mode_without_image_keeps_original_usb(self):
        sb = self.sandbox()
        proc = sb.run(mode='storage')
        self.assertEqual(proc.returncode, 44, proc.stdout + proc.stderr)
        self.assertNotIn('USB_UDC_BOUND', proc.stdout)


if __name__ == '__main__':
    unittest.main()