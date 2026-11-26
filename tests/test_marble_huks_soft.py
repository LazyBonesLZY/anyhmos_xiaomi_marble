#!/usr/bin/env python3
"""Tests for the marble software HUKS backend (compat/huks).

Three groups:
  * source/ABI: the aarch64 artifacts export exactly the seam symbols from
    step 1 of the investigation (engine vtable + HDI impl), and the IHuks
    vtable layout matches the 25-slot v1.1 IDL order;
  * behaviour: a real host binary drives generate -> sign -> verify ->
    encrypt -> decrypt -> derive -> random through both seams;
  * honesty: the report must state that this is not hardware isolation, the
    key store must really be sealed, and the probe tool must keep all three
    backend markers.

The aarch64 checks skip (with a reason) when the OHOS SDK or the cross-built
artifacts are unavailable; the host behaviour checks always run.
"""

import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HUKE_DIR = os.path.join(REPO, "compat", "huks")
BUILD_SH = os.path.join(HUKE_DIR, "build_soft_huks.sh")
BUILD_OUT = os.path.join(REPO, "work", "marble-huks-build")
ENGINE_SO = os.path.join(BUILD_OUT, "host", "libsoft_huks_engine.so")
HDI_SO = os.path.join(BUILD_OUT, "host", "libsoft_huks_hdi.so")
SELFTEST = os.path.join(BUILD_OUT, "host", "soft_huks_selftest")
HOST_PROBE = os.path.join(BUILD_OUT, "host", "tee_backend_probe")

CROSS_ENGINE = os.path.join(BUILD_OUT, "arm64", "libhuks_engine_core_standard.z.so")
CROSS_HDI = os.path.join(BUILD_OUT, "arm64", "libhuks_hdi_service_1.1.z.so")
CROSS_PROBE = os.path.join(BUILD_OUT, "arm64", "tee_backend_probe")
SDK_CLANG = os.path.join(REPO, "work", "ohos-sdk", "installed", "native", "llvm", "bin",
                         "aarch64-unknown-linux-ohos-clang")
REPORT = os.path.join(REPO, "work", "marble-huks-report.md")


def run(cmd, **kwargs):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=600, **kwargs)


class BuildOnce(unittest.TestCase):
    build_log = ""
    build_ok = False
    skip_reason = ""

    @classmethod
    def setUpClass(cls):
        if not os.path.exists(BUILD_SH):
            cls.skip_reason = f"missing {BUILD_SH}"
            return
        try:
            os.chmod(BUILD_SH, 0o755)
        except OSError:
            pass
        proc = run(["bash", BUILD_SH, "--all"])
        cls.build_log = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode != 0:
            if "openssl/evp.h" in cls.build_log or "openssl" in cls.build_log.lower():
                cls.skip_reason = "host OpenSSL development headers are not available"
            else:
                raise AssertionError("build_soft_huks.sh --all failed:\n" + cls.build_log[-4000:])
        cls.build_ok = True


class HostBehaviourTest(BuildOnce):
    def test_10_selftest_all_green(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        self.assertTrue(os.path.exists(SELFTEST), "host selftest binary was not built")
        root = tempfile.mkdtemp(prefix="huks-soft-")
        try:
            env = dict(os.environ, HUKS_SOFT_ROOT=root)
            proc = run([SELFTEST, ENGINE_SO, HDI_SO], env=env)
            out = (proc.stdout or "") + (proc.stderr or "")
            self.assertEqual(proc.returncode, 0, "selftest failed:\n" + out[-4000:])
            self.assertNotIn("SELFTEST FAIL", out, out[-4000:])
            summary = [line for line in out.splitlines() if line.startswith("SELFTEST SUMMARY")]
            self.assertTrue(summary, "no summary line:\n" + out[-2000:])
            passed = int(summary[-1].split("pass=")[1].split()[0])
            self.assertGreaterEqual(passed, 30, f"only {passed} assertions ran")
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_20_route_decisions_are_logged(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        root = tempfile.mkdtemp(prefix="huks-soft-")
        try:
            env = dict(os.environ, HUKS_SOFT_ROOT=root)
            proc = run([SELFTEST, ENGINE_SO, HDI_SO], env=env)
            out = (proc.stdout or "") + (proc.stderr or "")
            self.assertIn("HUKS_BACKEND=soft op=generate", out)
            self.assertIn("reason=qsee_unavailable", out)
            self.assertIn("HUKS_BACKEND=stub op=attest", out)
            self.assertIn("reason=local_attest_deferred_to_dcm", out)
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_30_store_is_sealed_not_plaintext(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        root = tempfile.mkdtemp(prefix="huks-soft-")
        try:
            env = dict(os.environ, HUKS_SOFT_ROOT=root)
            proc = run([SELFTEST, ENGINE_SO, HDI_SO], env=env)
            self.assertEqual(proc.returncode, 0, (proc.stdout or "") + (proc.stderr or ""))

            kek = os.path.join(root, ".kek")
            self.assertTrue(os.path.exists(kek), "device KEK was not created")
            self.assertEqual(os.path.getsize(kek), 32)
            self.assertEqual(stat.S_IMODE(os.stat(kek).st_mode), 0o600, "KEK must be 0600")
            with open(kek, "rb") as fh:
                kek_bytes = fh.read()
            self.assertNotEqual(kek_bytes, b"\x00" * 32, "KEK must come from a CSPRNG")

            key_files = []
            for dirpath, _dirnames, filenames in os.walk(root):
                for name in filenames:
                    if name.endswith(".key"):
                        key_files.append(os.path.join(dirpath, name))
            self.assertGreaterEqual(len(key_files), 6, "expected one sealed file per generated key")

            for path in key_files:
                self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600,
                                 f"{path} must be 0600")
                with open(path, "rb") as fh:
                    blob = fh.read()
                self.assertTrue(blob.startswith(b"SHS1"), f"{path} is not AES-GCM sealed")
                self.assertNotIn(b"rsa-test-alias", blob, "key alias leaked in plaintext")
                self.assertNotIn(b"hdi-ecc-alias", blob, "key alias leaked in plaintext")
                self.assertNotIn(kek_bytes[:16], blob, "KEK material leaked into key file")
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_40_unsupported_algorithm_returns_hks_error(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        root = tempfile.mkdtemp(prefix="huks-soft-")
        try:
            proc = run([SELFTEST, ENGINE_SO, HDI_SO], env=dict(os.environ, HUKS_SOFT_ROOT=root))
            out = proc.stdout or ""
            self.assertIn("SELFTEST ok engine.unsupported_alg_error_code", out)
            self.assertIn("SELFTEST ok engine.attest_defers_to_dcm", out)
        finally:
            shutil.rmtree(root, ignore_errors=True)


class AbiTest(BuildOnce):
    def _abi_lines(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        root = tempfile.mkdtemp(prefix="huks-soft-")
        try:
            proc = run([SELFTEST, ENGINE_SO, HDI_SO], env=dict(os.environ, HUKS_SOFT_ROOT=root))
            return [line for line in (proc.stdout or "").splitlines() if line.startswith("ABI ")]
        finally:
            shutil.rmtree(root, ignore_errors=True)

    def test_01_ihuks_vtable_is_25_slots(self):
        lines = self._abi_lines()
        self.assertIn("ABI IHuks.size=200 slots=25", lines)
        # v1.1 IDL order: ModuleInit, ModuleDestroy, GenerateKey ... GetVersion last
        self.assertIn("ABI offset.GenerateKey=16", lines)
        self.assertIn("ABI offset.Sign=104", lines)
        self.assertIn("ABI offset.Verify=112", lines)
        self.assertIn("ABI offset.GetVersion=192", lines)
        self.assertIn("ABI version=1.1", lines)
        self.assertIn("ABI HuksBlob.size=16 HuksParamSet.size=16", lines)

    def test_02_cross_symbols_match_the_seam(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        if not (os.path.exists(CROSS_ENGINE) and os.path.exists(CROSS_HDI)):
            self.skipTest("aarch64 artifacts not built (OHOS SDK missing?)")

        nm = os.path.join(os.path.dirname(SDK_CLANG), "llvm-nm")
        if not os.path.exists(nm):
            nm = shutil.which("llvm-nm") or shutil.which("aarch64-linux-gnu-nm")
        if not nm:
            self.skipTest("no aarch64 nm available to inspect the cross artifacts")

        engine_syms = run([nm, "-D", "--defined-only", CROSS_ENGINE]).stdout
        self.assertIn("HuksCreateHdiDevicePtr", engine_syms)
        self.assertIn("HuksDestoryHdiDevicePtr", engine_syms)

        hdi_syms = run([nm, "-D", "--defined-only", CROSS_HDI]).stdout
        for symbol in ("HuksImplGetInstance", "HuksImplRelease",
                       "HuksCreateHdiDevicePtr", "HuksDestoryHdiDevicePtr"):
            self.assertIn(symbol, hdi_syms, f"{symbol} missing from the HDI impl library")

    def test_03_cross_links_device_libcrypto(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        if not os.path.exists(CROSS_HDI):
            self.skipTest("aarch64 artifacts not built")
        with open(CROSS_HDI, "rb") as fh:
            head = fh.read(20)
        self.assertEqual(head[:4], b"\x7fELF")
        self.assertEqual(head[18] | (head[19] << 8), 183, "not an aarch64 ELF")
        readelf = shutil.which("readelf")
        if not readelf:
            self.skipTest("readelf not available")
        dyn = run([readelf, "-d", CROSS_HDI]).stdout
        self.assertIn("libcrypto_openssl.z.so", dyn, "must link the device OpenSSL")
        self.assertIn("libhuks_hdi_service_1.1.z.so", dyn, "SONAME must be the HDI 1.1 impl name")

    def test_04_cross_probe_is_bionic_executable(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        if not os.path.exists(CROSS_PROBE):
            self.skipTest("aarch64 probe not built")
        with open(CROSS_PROBE, "rb") as fh:
            head = fh.read(20)
        self.assertEqual(head[18] | (head[19] << 8), 183, "probe must be aarch64")


class ProbeToolTest(BuildOnce):
    def test_01_probe_prints_the_contract_line(self):
        if not self.build_ok:
            self.skipTest(self.skip_reason or "build unavailable")
        self.assertTrue(os.path.exists(HOST_PROBE), "host probe was not built")
        proc = run([HOST_PROBE])
        self.assertEqual(proc.returncode, 0, proc.stderr)
        line = (proc.stdout or "").strip().splitlines()[0]
        self.assertTrue(line.startswith("TEE_PROBE qseecom=/dev/qseecom open="), line)
        self.assertIn("gptee=", line)
        self.assertIn("soffkeymaster=", line)
        self.assertIn("backend=", line)

    def test_02_probe_source_keeps_all_three_backends(self):
        path = os.path.join(HUKE_DIR, "tee_backend_probe.c")
        self.assertTrue(os.path.exists(path))
        with open(path, encoding="utf-8") as fh:
            src = fh.read()
        for marker in ("HUKS_BACKEND=qsee", "HUKS_BACKEND=soft", "HUKS_BACKEND=stub"):
            self.assertIn(marker, src, f"probe source lost the {marker} branch marker")
        self.assertIn("TEE_PROBE qseecom=", src)


class ReportContractTest(unittest.TestCase):
    def test_01_report_states_no_hardware_isolation(self):
        self.assertTrue(os.path.exists(REPORT), f"missing {REPORT}")
        with open(REPORT, encoding="utf-8") as fh:
            report = fh.read()
        for phrase in ("不提供硬件隔离", "不是硬件根信任", "root 可读", "软件 TEE"):
            self.assertIn(phrase, report, f"report must contain the honesty phrase: {phrase}")

    def test_02_report_has_seam_evidence_and_abi_table(self):
        with open(REPORT, encoding="utf-8") as fh:
            report = fh.read()
        for phrase in ("libhuks_engine_core_standard.z.so", "LoadHdiImpl",
                       "HuksCreateHdiDevicePtr", "HuksImplGetInstance",
                       "struct IHuks", "内核侧 blocker"):
            self.assertIn(phrase, report, f"report is missing: {phrase}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
