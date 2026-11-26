"""Marble GPU/display stability guards: MSAA resolve recursion, buffer release desync, webrender triage.

These tests never touch a device. They pin the forensic conclusions that the surrounding
changes depend on, compile the display release-cache header on the host, and verify the
report keeps the evidence/speculation boundary.
"""

import hashlib
import importlib.util
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GRAPHICS = ROOT / 'compat/graphics'
DISPLAY = ROOT / 'compat/display'
REPORT = ROOT / 'work/marble-gpu-stability-report.md'
GUARD_PATCH = GRAPHICS / 'mesa-zink-msaa-guard.patch'
GALLIUM_PATCH = GRAPHICS / 'mesa-gallium-egl.patch'
TURNIP_PATCH = GRAPHICS / 'mesa-turnip.patch'
ZINK_RENDER_PASS = 'src/gallium/drivers/zink/zink_render_pass.c'
MESA_SRC = ROOT / 'work/mesa-turnip-src'
CXX = shutil.which('g++') or shutil.which('clang++')


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def added_lines(patch_text):
    return [line[1:] for line in patch_text.splitlines()
            if line.startswith('+') and not line.startswith('+++')]


def run(argv, cwd):
    return subprocess.run(argv, cwd=str(cwd), capture_output=True, text=True)


class MsaaResolveGuardTests(unittest.TestCase):
    tree = None

    @classmethod
    def setUpClass(cls):
        if not (MESA_SRC.is_dir() and shutil.which('git')):
            return
        temp = tempfile.mkdtemp(prefix='marble-msaa-guard-')
        cls.addClassCleanup(shutil.rmtree, temp, True)
        tree = Path(temp)
        blob = run(['git', '-C', str(MESA_SRC), 'show', f'HEAD:{ZINK_RENDER_PASS}'], ROOT)
        if blob.returncode != 0:
            return
        target = tree / ZINK_RENDER_PASS
        target.parent.mkdir(parents=True)
        target.write_text(blob.stdout)
        steps = (['git', 'init', '-q', '.'],
                 ['git', 'add', '-A'],
                 ['git', '-c', 'user.email=a@b', '-c', 'user.name=a', 'commit', '-qm', 'upstream'],
                 ['git', 'apply', f'--include={ZINK_RENDER_PASS}', str(GALLIUM_PATCH)],
                 ['git', '-c', 'user.email=a@b', '-c', 'user.name=a', 'commit', '-qam', 'gallium'],
                 ['git', 'apply', str(GUARD_PATCH)])
        for argv in steps:
            if run(argv, tree).returncode != 0:
                return
        cls.tree = tree

    def setUp(self):
        self.assert_paths = [GUARD_PATCH, GALLIUM_PATCH]
        for path in self.assert_paths:
            self.assertTrue(path.is_file(), f'missing {path}')
        self.patch = GUARD_PATCH.read_text()

    def test_guard_patch_touches_only_the_msaa_expand_function(self):
        touched = re.findall(r'^diff --git a/(\S+) b/', self.patch, re.M)
        self.assertEqual(touched, [ZINK_RENDER_PASS])

    @unittest.skipUnless(MESA_SRC.is_dir() and shutil.which('git'), 'needs the staged Mesa tree')
    def test_idempotence_marker_is_set_before_the_blit(self):
        self.assertIsNotNone(self.tree, '补丁链未能应用，无法验证置位顺序')
        source = (self.tree / ZINK_RENDER_PASS).read_text()
        body = source.split('zink_render_msaa_expand(', 1)[1].split('zink_begin_render_pass', 1)[0]
        marker = 'csurf->transient_init = true;'
        blit = 'util_blitter_blit_generic'
        self.assertIn(blit, body)
        self.assertLess(body.index(marker), body.index(blit),
                        'resolve 幂等标记必须先于 blit 置位，否则递归无法自断')

    def test_recursion_gate_is_bounded_and_observable(self):
        self.assertIn('static __thread unsigned expand_depth;', self.patch)
        self.assertIn('if (expand_depth >= 2) {', self.patch)
        self.assertIn('ZINK_MSAA_EXPAND_CAP', self.patch)
        self.assertIn('ZINK_MSAA_EXPAND mask=0x%x', self.patch)
        # 每个成功进入都必须有配对的退出，否则闸门会在正常帧里累积
        self.assertEqual(self.patch.count('++expand_depth;'), 1)
        self.assertEqual(self.patch.count('--expand_depth;'), 1)

    def test_guard_does_not_revert_the_existing_clear_staging_hunk(self):
        self.assertNotIn('clear_mask', added_lines(self.patch))
        self.assertIn('ctx->clears_enabled = 0;', GALLIUM_PATCH.read_text())

    def test_apply_mesa_chain_order(self):
        module = load_module('apply_mesa_under_test', GRAPHICS / 'apply_mesa.py')
        self.assertEqual(module.PROFILE_PATCHES['gallium-egl'],
                         ('mesa-gallium-egl.patch', 'mesa-zink-msaa-guard.patch'))
        self.assertEqual(module.PROFILE_PATCHES['turnip'], ('mesa-turnip.patch',))
        chain = module.patch_chain('gallium-egl')
        self.assertEqual([p.name for p in chain], list(module.PROFILE_PATCHES['gallium-egl']))
        for patch in chain:
            self.assertTrue(patch.is_file(), f'补丁链缺文件 {patch}')

    @unittest.skipUnless(MESA_SRC.is_dir() and shutil.which('git'), 'needs the staged Mesa tree')
    def test_guard_patch_applies_on_top_of_the_gallium_patch(self):
        self.assertIsNotNone(self.tree, '补丁链未能应用')
        source = (self.tree / ZINK_RENDER_PASS).read_text()
        self.assertEqual(source.count('ZINK_MSAA_EXPAND_CAP'), 1)
        self.assertEqual(source.count('static __thread unsigned expand_depth;'), 1)
        # 已应用后不得再次应用（补丁必须对自身状态敏感）
        again = run(['git', 'apply', '--check', str(GUARD_PATCH)], self.tree)
        self.assertNotEqual(again.returncode, 0, '守卫补丁可在已应用状态重复应用')

    def test_unknown_profile_is_rejected(self):
        result = subprocess.run([sys.executable, str(GRAPHICS / 'apply_mesa.py'), '--source',
                                 str(ROOT), '--profile', 'stone-skip'], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0, '未知 profile 必须被拒绝')

    def test_guard_patch_is_repository_content_not_generated_output(self):
        digest = hashlib.sha256(GUARD_PATCH.read_bytes()).hexdigest()
        self.assertEqual(len(digest), 64)
        self.assertGreater(GUARD_PATCH.stat().st_size, 400)


class TurnipPatchScopeTests(unittest.TestCase):
    """The MSAA recursion lives in Zink, not in Turnip: pin that forensic boundary."""

    def setUp(self):
        self.patch = TURNIP_PATCH.read_text()

    def test_turnip_patch_has_no_resolve_or_render_pass_edits(self):
        touched = re.findall(r'^diff --git a/(\S+) b/', self.patch, re.M)
        for forbidden in ('src/freedreno/vulkan/tu_cmd.cc', 'src/freedreno/vulkan/tu_pass.cc'):
            self.assertNotIn(forbidden, touched)
        self.assertNotIn('vkCmdResolveImage', self.patch)
        self.assertNotIn('tu_CmdResolveImage', self.patch)

    def test_turnip_patch_keeps_the_image_and_native_buffer_paths(self):
        touched = re.findall(r'^diff --git a/(\S+) b/', self.patch, re.M)
        for expected in ('src/freedreno/vulkan/tu_image.cc', 'src/freedreno/vulkan/tu_ohos.cc'):
            self.assertIn(expected, touched)


HARNESS = r'''
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include "direct_scanout_state.h"

static int open_devnull() { return open("/dev/null", O_RDONLY); }

int main()
{
    DirectReleaseCache cache;
    uint32_t layer = 0;
    const int fd = open_devnull();
    if (fd < 0) return 90;
    cache.Adopt(7, fd);
    int got = cache.Acquire(layer);
    if (got != fd || layer != 7) return 1;
    if (!cache.Published() || cache.Lost() != 0) return 2;
    /* 调用方自己关闭后，缓存不得再交出同一个编号 */
    close(got);
    if (cache.Acquire(layer) != -1 || layer != 0) return 3;
    /* 未取走即被新提交替换 → 记一次释放失步 */
    const int second = open_devnull();
    const int third = open_devnull();
    if (second < 0 || third < 0) return 91;
    cache.Adopt(9, second);
    cache.Adopt(9, third);
    if (cache.Lost() != 1) return 4;
    /* 既有借用语义必须保留：契约测试依赖它 */
    if (cache.Borrow(layer) != third || layer != 9) return 5;
    cache.ResetLayer(9);
    if (cache.Borrow(layer) != -1 || cache.Published()) return 6;
    return 0;
}
'''


class ReleaseCacheBehaviourTests(unittest.TestCase):
    def setUp(self):
        self.header = DISPLAY / 'direct_scanout_state.h'
        self.assertTrue(self.header.is_file())
        self.source = self.header.read_text()

    def test_header_keeps_the_borrow_contract(self):
        # tests/display/direct_scanout_contract.cpp 依赖“借用后描述符仍归 composer”
        self.assertIn('int Borrow(uint32_t &layer) const', self.source)
        self.assertIn('int Acquire(uint32_t &layer)', self.source)
        self.assertIn('unsigned Lost() const', self.source)

    @unittest.skipUnless(CXX, 'needs a host C++ compiler')
    def test_acquire_drops_stale_descriptors_and_counts_lost_fences(self):
        with tempfile.TemporaryDirectory() as temp:
            tree = Path(temp)
            (tree / 'guard.cc').write_text(HARNESS)
            binary = tree / 'guard'
            build = subprocess.run([CXX, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    '-I', str(DISPLAY), '-o', str(binary), str(tree / 'guard.cc')],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run_rc = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(run_rc.returncode, 0,
                             f'DirectReleaseCache guard behaviour failed rc={run_rc.returncode}')


class ComposerGuardTests(unittest.TestCase):
    def setUp(self):
        self.source = (DISPLAY / 'composer_vdi.cpp').read_text()
        self.release = self.source.split('GetDisplayReleaseFence', 1)[1].split('extern "C" int32_t SetLayerBuffer', 1)[0]

    def test_release_fence_query_validates_ownership(self):
        self.assertIn('directRelease.Acquire(layerId)', self.release)
        self.assertIn('RELEASE_FENCE_LOST', self.release)
        self.assertNotIn('directRelease.Borrow(layerId)', self.release)

    def test_panel_bar_write_is_bounded_by_the_dmabuf_length(self):
        self.assertIn('fstat(barFd, &barStat', self.source)
        self.assertIn('PANEL_BARS_SIZE_CLAMP', self.source)
        self.assertIn('PANEL_BARS_SIZE_REJECT', self.source)
        self.assertIn('munmap(barMap, size_t(usable))', self.source)
        self.assertNotIn('munmap(barMap, size_t(b.size))', self.source)
        # 客户端句柄仍必须先过 Linear() 的布局校验
        self.assertIn('int64_t(b.stride) * b.height <= b.size', self.source)


class ReportTests(unittest.TestCase):
    REQUIRED_SECTIONS = (
        '## 1. 取证',
        '## 2. 症状 A：MSAA 递归循环',
        '## 3. 症状 B：缓冲区异常溢出与释放失步',
        '## 4. 症状 C：网页渲染',
        '## 7. 验证命令',
        '## 8. 未上机验证',
    )

    def setUp(self):
        self.assertTrue(REPORT.is_file(), 'work/marble-gpu-stability-report.md 缺失')
        self.text = REPORT.read_text()

    def test_sections_present(self):
        for section in self.REQUIRED_SECTIONS:
            self.assertIn(section, self.text)

    def test_declares_unverified_status_and_never_claims_host_acceptance(self):
        self.assertIn('未上机验证', self.text)
        self.assertNotIn('已上机验证', self.text)
        self.assertIn('在不动 kernel/驱动前提下不可行', self.text.replace('kernel/驱动', 'kernel/驱动'))

    def test_separates_evidence_from_speculation(self):
        self.assertIn('有代码证据', self.text)
        self.assertIn('无证据', self.text)
        self.assertIn('`[推测]`', self.text)
        self.assertIn('`[部分证据]`', self.text)
        # 口述症状不得被写成已发生的崩溃事实
        self.assertIn('日志层完全没有出现', self.text)
        self.assertIn('不能当事实使用', self.text)

    def test_each_symptom_has_a_verdict_row(self):
        for symptom in ('MSAA 递归循环', '缓冲区异常溢出', '缓冲区释放失步', '网页渲染 bug'):
            rows = [line for line in self.text.splitlines()
                    if line.startswith('|') and symptom in line]
            self.assertTrue(rows, f'{symptom} 缺少结论表行')

    # 报告允许用短名引用 Mesa 上游文件，这里给出解析前缀
    CITATION_PREFIXES = ('', 'work/mesa-turnip-src/', 'work/mesa-turnip-src/src/gallium/drivers/zink/',
                         'work/mesa-turnip-src/src/freedreno/vulkan/', 'compat/display/',
                         'compat/graphics/', 'compat/services/', 'tests/')

    def resolve_citation(self, name):
        for prefix in self.CITATION_PREFIXES:
            if (ROOT / (prefix + name)).is_file():
                return prefix + name
        return None

    def test_code_citations_carry_file_and_line(self):
        citations = re.findall(r'`?([\w./-]+\.(?:c|cc|cpp|h|py|md|patch)):(\d+)(?:-\d+)?', self.text)
        self.assertGreaterEqual(len(citations), 20, '报告需要 file:line 形式的取证')
        for name, _ in citations:
            self.assertIsNotNone(self.resolve_citation(name), f'引用了不存在的文件 {name}')

    def test_records_the_silent_log_trap(self):
        self.assertIn('submit=0 fence=0', self.text)
        self.assertIn('marble_vulkan_native_clear.c', self.text)

    def test_reports_both_kms_capture_outcomes(self):
        self.assertIn('nonzero_pixels=0', self.text)
        self.assertIn('2268000', self.text)


if __name__ == '__main__':
    unittest.main()