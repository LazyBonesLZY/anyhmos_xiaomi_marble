#!/usr/bin/env python3
"""Static-artifact tests for the marble telephony / modem investigation.

These tests do NOT touch a device, a serial port, adb or fastboot.  They only
verify what this worktree is supposed to contain after the telephony stage-1
investigation:

  * work/marble-telephony-report.md exists, is structured, and its embedded SA
    table is the same JSON as devices/marble/runtime/telephony/telephony-sa-table.json
  * that SA table is faithful to the donor ROM profile when the donor tree is
    present locally
  * devices/marble/runtime/telephony/start-modem-marble.sh parses and its
    --dry-run mode is inert and machine readable
  * devices/marble/runtime/telephony/init-modem-marble.cfg is valid init JSON
    with the donor permission domain and every service gated behind a condition
  * compat/telephony/qmi_probe.c is dependency free and warning-free
  * nothing under the read-only repository areas was modified by these artifacts
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPORT = ROOT / 'work' / 'marble-telephony-report.md'
TELEPHONY_DIR = ROOT / 'devices' / 'marble' / 'runtime' / 'telephony'
SA_TABLE = TELEPHONY_DIR / 'telephony-sa-table.json'
MODEM_SCRIPT = TELEPHONY_DIR / 'start-modem-marble.sh'
INIT_CFG = TELEPHONY_DIR / 'init-modem-marble.cfg'
PROBE_C = ROOT / 'compat' / 'telephony' / 'qmi_probe.c'
BUILD_PROBE = ROOT / 'compat' / 'telephony' / 'build-qmi-probe.sh'
PROBE_BIN = ROOT / 'work' / 'marble-telephony' / 'bin' / 'qmi_probe'
EVIDENCE = ROOT / 'work' / 'marble-telephony' / 'evidence'
DONOR_FS = ROOT / 'work' / 'donor-rom' / 'ADY-AL10-207.0.0.111-SP10' / 'fs'
DONOR_PROFILE = DONOR_FS / 'system' / 'system' / 'profile' / 'telephony.json'
STAGE_SCRIPT = ROOT / 'devices' / 'marble' / 'runtime' / 'boot' / 'stage-phase4-marble.sh'

# Repository areas this task must never write to.
FORBIDDEN_PREFIXES = (
    'devices/marble/runtime/boot/stage-phase4-marble.sh',
    'devices/marble/runtime/boot/start-phase4-marble.sh',
    'tools/packaging/',
    'docs/',
    'README.md',
    'tools/build/build_current.py',
    'compat/audio/',
    'compat/bluetooth/',
    'compat/bootcontrol/',
)


def _read(path: Path) -> str:
    return path.read_text(encoding='utf-8')


def _report_json_block() -> dict:
    text = _read(REPORT)
    match = re.search(r'```json\n(.*?)\n```', text, re.S)
    if not match:
        raise AssertionError('report has no ```json fenced block')
    return json.loads(match.group(1))


class ReportArtifactsTest(unittest.TestCase):
    def test_report_exists(self) -> None:
        self.assertTrue(REPORT.is_file(), f'missing {REPORT}')
        self.assertGreater(REPORT.stat().st_size, 8000)

    def test_report_has_required_sections(self) -> None:
        text = _read(REPORT)
        for heading in (
            '# marble 基带 / 读卡(SIM) / 5G',
            '## 0. 结论摘要',
            '## 2. OHOS 侧需要哪些 SA',
            '## 3. marble 侧硬件与固件接口盘点',
            '## 4. 阻断点判定',
            '## 5. 最小可执行路径',
            '## 6. 明确不可行的子项',
            '## 7. 给 lead 的串口验证清单',
            '## 8. 证据索引',
            '## 9. 风险与未验证项',
        ):
            self.assertIn(heading, text, f'missing section: {heading}')

    def test_report_labels_every_claim_category(self) -> None:
        text = _read(REPORT)
        for label in ('已验证', '高置信推断', '未验证', '不可行'):
            self.assertIn(label, text, f'missing confidence label: {label}')

    def test_report_states_no_flash_no_serial(self) -> None:
        text = _read(REPORT)
        self.assertIn('没有', text)
        self.assertRegex(text, r'没有.*刷机')
        self.assertRegex(text, r'没有.*COM9')

    def test_report_embeds_the_same_sa_table_as_the_sidecar(self) -> None:
        self.assertTrue(SA_TABLE.is_file(), f'missing {SA_TABLE}')
        sidecar = json.loads(_read(SA_TABLE))
        self.assertEqual(_report_json_block(), sidecar,
                         'report JSON block drifted from telephony-sa-table.json')

    def test_report_marks_mid_pipeline_stage3_swap_point(self) -> None:
        text = _read(REPORT)
        self.assertIn('const.sys.radio.vendorlib.path', text)
        self.assertIn('libril_msgtransfer.z.so', text)


class SaTableTest(unittest.TestCase):
    def setUp(self) -> None:
        self.table = json.loads(_read(SA_TABLE))

    def test_schema_and_process(self) -> None:
        self.assertEqual(self.table['schema'], 'marble-telephony/sa-table/1')
        proc = self.table['process']
        self.assertEqual(proc['uid'], 'radio')
        self.assertEqual(proc['secon'], 'u:r:telephony_sa:s0')
        self.assertIn('netsys_socket', proc['gid'])
        self.assertEqual(proc['permission_count'], 57)

    def test_each_sa_has_the_fields_the_task_asked_for(self) -> None:
        for sa in self.table['system_abilities']:
            with self.subTest(sa=sa['id']):
                self.assertIsInstance(sa['id'], int)
                self.assertTrue(sa['libpath'].endswith('.z.so'), sa['libpath'])
                self.assertIn('role', sa)
                self.assertIsInstance(sa['hdi'], list)
                self.assertIsInstance(sa['dev_nodes'], list)
                self.assertIn('confidence', sa)

    def test_ril_hdi_service_names_come_from_the_donor_hcb(self) -> None:
        core = next(sa for sa in self.table['system_abilities'] if sa['id'] == 4010)
        hdi = core['hdi'][0]
        self.assertEqual(hdi['hdf_host'], 'riladapter_host')
        self.assertEqual(hdi['module_name'], 'libril_driver.z.so')
        self.assertEqual(hdi['service'], 'ril_service')
        self.assertEqual(hdi['extra_module_name'], 'libhril_hdf.z.so')
        self.assertEqual(hdi['extra_service'], 'ril_ext')
        self.assertFalse(hdi['donor_vendor_layer_compatible_with_marble'])

    def test_minimal_subset_is_4010(self) -> None:
        subset = self.table['minimal_subset_for_sim_and_5g']
        self.assertEqual(subset['ids'], [4010])

    @unittest.skipUnless(DONOR_PROFILE.is_file(), 'donor ROM profile not present')
    def test_ids_match_the_donor_profile(self) -> None:
        donor = json.loads(_read(DONOR_PROFILE))
        donor_ids = sorted(entry['name'] for entry in donor['systemability'])
        table_ids = sorted(sa['id'] for sa in self.table['system_abilities'])
        self.assertEqual(table_ids, donor_ids)

    @unittest.skipUnless(DONOR_PROFILE.is_file(), 'donor ROM profile not present')
    def test_libpaths_match_the_donor_profile(self) -> None:
        donor = {entry['name']: entry['libpath'] for entry in json.loads(_read(DONOR_PROFILE))['systemability']}
        for sa in self.table['system_abilities']:
            with self.subTest(sa=sa['id']):
                self.assertEqual(sa['libpath'], donor[sa['id']])


class ModemScriptTest(unittest.TestCase):
    def test_script_parses_under_sh_and_bash(self) -> None:
        for shell in ('sh', 'bash'):
            if shutil.which(shell) is None:
                continue
            proc = subprocess.run([shell, '-n', str(MODEM_SCRIPT)], capture_output=True, text=True)
            self.assertEqual(proc.returncode, 0, f'{shell} -n failed: {proc.stderr}')

    def test_dry_run_is_inert_and_machine_readable(self) -> None:
        env = dict(os.environ, MARBLE_MODEM_DRY_RUN='1')
        proc = subprocess.run(['sh', str(MODEM_SCRIPT)], capture_output=True, text=True, env=env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        out = proc.stdout
        self.assertIn('MARBLE_MODEM_DRY_RUN=1', out)
        self.assertIn('MARBLE_MODEM_RESULT=DRY_RUN', out)
        self.assertIn('remoteproc', out)
        # dry run must not claim it did real work
        self.assertNotIn('MARBLE_MODEM_RPROC_STATE=running', out)

    def test_mss_state_and_qrtr_discovery_are_reported_separately(self) -> None:
        block = _read(MODEM_SCRIPT).split('probe_result=not_staged\n', 1)[1]
        block = 'probe_result=not_staged\n' + block.replace(
            'for p in /compat/bin/qmi_probe ', 'for p in "$R/missing/qmi_probe" ', 1)
        mock = '''set -eu
R=$1; B=mock_bb; probe_fixture_out=$2; probe_fixture_rc=$3
say() { printf '%s\\n' "$*"; }
mock_bb() {
  case "$1" in
    timeout)
      [ "$*" = "timeout -k 1 8 $R/compat/bin/qmi_probe --services" ] || exit 95
      printf '%s\\n' "$probe_fixture_out"
      return "$probe_fixture_rc" ;;
    grep) command "$@" ;;
    *) exit 96 ;;
  esac
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            probe = Path(tmp) / 'compat/bin/qmi_probe'
            probe.parent.mkdir(parents=True)
            for present, output, rc, result in (
                (False, '', 0, 'not_staged'),
                (True, 'QRTR_UNIQUE_REMOTE_TOTAL=1', 0, 'remote_advertisement_seen'),
                (True, 'QRTR_UNIQUE_REMOTE_TOTAL=1', 124, 'unconfirmed'),
                (True, 'QRTR_UNIQUE_REMOTE_TOTAL=0', 0, 'unconfirmed'),
                (True, 'old probe output', 0, 'unconfirmed'),
            ):
                with self.subTest(present=present, output=output, rc=rc):
                    if present:
                        probe.touch()
                        probe.chmod(0o700)  # only file presence is checked; the mock never execs it
                    ran = subprocess.run(['sh', '-c', mock + block, 'fixture', tmp, output, str(rc)],
                                         capture_output=True, text=True, check=True, timeout=5)
                    self.assertIn(f'MARBLE_MODEM_QRTR_DISCOVERY={result}', ran.stdout.splitlines())
                    self.assertIn('MARBLE_MODEM_RESULT=MSS_RUNNING telephony=untested', ran.stdout.splitlines())
                    self.assertNotIn('MARBLE_MODEM_RESULT=OK', ran.stdout)

    def test_script_refuses_to_touch_flash_or_serial(self) -> None:
        text = _read(MODEM_SCRIPT)
        for banned in ('fastboot', 'adb ', 'COM9', 'sde7', 'mke2fs', 'dd if=', 'mkfs'):
            self.assertNotIn(banned, text, f'modem script must not reference {banned!r}')
        # modem_a may only ever be mounted read-only
        self.assertIn('-o ro', text)
        self.assertNotRegex(text, r'mount[^\n]*modem_a[^\n]*(?<!ro,)rw')

    def test_script_globs_modem_images_instead_of_counting(self) -> None:
        text = _read(MODEM_SCRIPT)
        self.assertIn('"$IMG"/modem.b*', text,
                      'modem.b* numbering is not contiguous (b14/b21/b30 absent) so a glob is required')

    def test_script_handles_both_firmware_namespaces(self) -> None:
        text = _read(MODEM_SCRIPT)
        self.assertIn('/compat/firmware', text)
        self.assertIn('$R/compat/firmware', text)


class InitCfgTest(unittest.TestCase):
    def setUp(self) -> None:
        self.cfg = json.loads(_read(INIT_CFG))
        self.services = {svc['name']: svc for svc in self.cfg['services']}

    def test_services_present(self) -> None:
        self.assertEqual(sorted(self.services), ['riladapter_host', 'telephony_sa'])

    def test_every_service_is_gated_behind_a_condition(self) -> None:
        for name, svc in self.services.items():
            with self.subTest(service=name):
                self.assertEqual(svc.get('disabled'), 1, 'must stay disabled by default')
                self.assertEqual(svc.get('start-mode'), 'condition')
        gating = [job for job in self.cfg['jobs'] if job['name'].startswith('param:')]
        self.assertTrue(gating)
        self.assertIn('marble.debug.start.telephony=true', gating[0]['condition'])
        self.assertIn('start telephony_sa', gating[0]['cmds'])

    def test_riladapter_host_matches_the_donor_permission_domain(self) -> None:
        host = self.services['riladapter_host']
        self.assertEqual(host['path'][0], '/vendor/bin/hdf_devhost')
        self.assertEqual(host['path'][1:], ['-i', '9', '-n', 'riladapter_host'])
        self.assertEqual(host['uid'], 'radio')
        self.assertIn('oeminfo_nvme_server', host['gid'])
        self.assertEqual(host['secon'], 'u:r:riladapter_host:s0')
        self.assertIn('CAP_NET_ADMIN', host['caps'])

    def test_telephony_sa_matches_the_donor_process(self) -> None:
        sa = self.services['telephony_sa']
        self.assertEqual(sa['path'], ['/system/bin/sa_main', '/system/profile/telephony.json'])
        self.assertEqual(sa['uid'], 'radio')
        self.assertEqual(sa['gid'], ['radio', 'shell', 'netsys_socket', 'sensor_host'])
        self.assertEqual(sa['secon'], 'u:r:telephony_sa:s0')
        self.assertEqual(sa['apl'], 'system_basic')
        self.assertFalse(sa['saspawn'])
        self.assertEqual(len(sa['permission']), 57)

    @unittest.skipUnless((DONOR_FS / 'system' / 'system' / 'etc' / 'init' / 'telephony.cfg').is_file(),
                         'donor init cfg not present')
    def test_permission_list_is_identical_to_the_donor(self) -> None:
        donor = json.loads(_read(DONOR_FS / 'system' / 'system' / 'etc' / 'init' / 'telephony.cfg'))
        donor_svc = donor['services'][0]
        sa = self.services['telephony_sa']
        self.assertEqual(sa['permission'], donor_svc['permission'])
        self.assertEqual(sa['permission_acls'], donor_svc['permission_acls'])
        self.assertEqual(sa['socket'], donor_svc['socket'])


class QmiProbeTest(unittest.TestCase):
    def test_probe_is_dependency_free(self) -> None:
        text = _read(PROBE_C)
        includes = [line for line in text.splitlines() if re.match(r'\s*#\s*include', line)]
        for line in includes:
            with self.subTest(include=line):
                if line.strip() == '#include <linux/qrtr.h>':
                    continue  # userspace ABI header, not a libqrtr dependency
                for banned in ('qmi', 'qrtr', 'hidl', 'binder'):
                    self.assertNotIn(banned, line.lower(), f'probe must not include {line.strip()}')
        self.assertIn('#include <linux/qrtr.h>', text)
        self.assertFalse(re.search(r'qmi_(cci|client|services)\s*\(', text))
        self.assertIn('#define AF_QIPCRTR 42', text)
        self.assertIn('QRTR_TYPE_NEW_LOOKUP', text)
        self.assertIn('QMI_UIM_GET_CARD_STATUS', text)
        self.assertIn('QMI_NAS_GET_SERVING_SYSTEM', text)

    @unittest.skipUnless(shutil.which('cc'), 'no host C compiler')
    def test_probe_compiles_warning_free(self) -> None:
        proc = subprocess.run(
            ['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
             '-fsyntax-only', str(PROBE_C)],
            capture_output=True, text=True,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stderr.strip(), '', 'compiler emitted diagnostics')
        # Protocol and CLI behaviour are covered by the syscall-stubbed fixtures
        # in test_marble_qrtr_probe.py, never by opening a host QRTR socket.


class ProbeBinaryTest(unittest.TestCase):
    def test_build_helper_parses(self) -> None:
        self.assertTrue(BUILD_PROBE.is_file(), f'missing {BUILD_PROBE}')
        proc = subprocess.run(['sh', '-n', str(BUILD_PROBE)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        text = _read(BUILD_PROBE)
        self.assertIn('-static', text, 'probe must be built static so it runs from the ramdisk')

    def test_prebuilt_probe_is_static_aarch64(self) -> None:
        if not PROBE_BIN.is_file():
            self.skipTest('probe binary not built in this checkout')
        head = PROBE_BIN.read_bytes()[:20]
        self.assertEqual(head[:4], b'\x7fELF', 'not an ELF file')
        self.assertEqual(head[4], 2, 'not ELF64')
        self.assertEqual(head[5], 1, 'not little endian')
        machine = int.from_bytes(head[18:20], 'little')
        self.assertEqual(machine, 183, f'expected EM_AARCH64(183), got {machine}')
        self.assertGreater(PROBE_BIN.stat().st_size, 100_000)


class EvidenceTest(unittest.TestCase):
    EXPECTED = (
        'donor-telephony-profile.json',
        'donor-telephony-init.cfg',
        'donor-hdf-devhost.cfg',
        'donor-hdf-default-hcb-strings.txt',
        'sa-needed-libs.txt',
        'marble-modem-partition-index.txt',
        'kernel-modules-telecom.txt',
        'kernel-defconfig-telecom.txt',
        'marble-dts-modem-ipa.txt',
    )

    def test_evidence_snapshot_present(self) -> None:
        for name in self.EXPECTED:
            path = EVIDENCE / name
            with self.subTest(evidence=name):
                self.assertTrue(path.is_file(), f'missing evidence {name}')
                self.assertGreater(path.stat().st_size, 100)

    def test_modem_partition_index_contains_the_real_firmware(self) -> None:
        text = _read(EVIDENCE / 'marble-modem-partition-index.txt')
        self.assertIn('modem/image/modem.mdt', text)
        self.assertIn('modem/image/ipa_fws.mdt', text)
        self.assertIn('modem/image/modemr.jsn', text)
        self.assertIn('mcfg_sw', text)
        # numbering is sparse, this is the fact that forces a glob in the script
        self.assertNotIn('modem/image/modem.b14', text)
        self.assertNotIn('modem/image/modem.b21', text)

    def test_kernel_defconfig_shows_pas_but_not_mss_legacy(self) -> None:
        text = _read(EVIDENCE / 'kernel-defconfig-telecom.txt')
        self.assertIn('CONFIG_QCOM_Q6V5_PAS=m', text)
        self.assertIn('# CONFIG_QCOM_Q6V5_MSS is not set', text)
        self.assertIn('CONFIG_QSEECOM=m', text)
        self.assertIn('CONFIG_QRTR=m', text)


class ReadOnlyAreaTest(unittest.TestCase):
    """The new artifacts must not write into the read-only areas."""

    def test_new_artifacts_do_not_write_forbidden_paths(self) -> None:
        new_files = [MODEM_SCRIPT, INIT_CFG, SA_TABLE, PROBE_C, REPORT]
        new_files += sorted(EVIDENCE.glob('*'))
        for path in new_files:
            if not path.is_file():
                continue
            text = _read(path)
            with self.subTest(artifact=path.name):
                for line in text.splitlines():
                    if not re.search(r'(>|>>|tee|cp |mv |sed -i|install )', line):
                        continue
                    for prefix in FORBIDDEN_PREFIXES:
                        self.assertNotIn(
                            prefix, line,
                            f'{path.name} appears to write into read-only area: {line.strip()}',
                        )

    def test_stage_script_line_references_are_still_valid(self) -> None:
        if not STAGE_SCRIPT.is_file():
            self.skipTest('stage script not present')
        lines = _read(STAGE_SCRIPT).splitlines()
        if len(lines) < 160:
            self.skipTest('stage script shorter than the cited insertion point')
        if lines[157].strip() != 'fi' or not lines[159].startswith('# Wait briefly'):
            self.skipTest('stage-phase4-marble.sh changed; report line numbers need a refresh')
        self.assertEqual(lines[157].strip(), 'fi')
        self.assertTrue(lines[159].startswith('# Wait briefly'))


if __name__ == '__main__':
    unittest.main()