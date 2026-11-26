"""Apply one frozen graphics profile to Mesa ea74a6339917390657db153cb9792f9ccac9f5ae.

Use separate clean source trees for gallium-egl and turnip. Gallium requires
platforms=ohos, gallium-drivers=zink, vulkan-drivers='', shader-cache=disabled;
Turnip requires platforms='', gallium-drivers='', vulkan-drivers=freedreno,
freedreno-kmds=kgsl. Both use the OHOS AArch64 SDK, cpp_rtti=false and
femulated-tls. Pass dmabuf_sync/uapi as an include directory for Gallium.
The donor BufferHandle/display_type headers and surface/hilog/libc++ libraries
are external build inputs. This prepares sources; it does not deploy binaries.

The gallium-egl profile applies a second, ordered patch: the Zink MSAA resolve
re-entrancy guard (mesa-zink-msaa-guard.patch) which must stay on top of
mesa-gallium-egl.patch and is therefore applied last.
"""

import argparse
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
# 顺序固定：主补丁先行，防护补丁依赖其后的代码上下文
PROFILE_PATCHES = {
    'gallium-egl': ('mesa-gallium-egl.patch', 'mesa-zink-msaa-guard.patch'),
    'turnip': ('mesa-turnip.patch',),
}


def patch_chain(profile):
    return [HERE / name for name in PROFILE_PATCHES[profile]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--profile', choices=tuple(PROFILE_PATCHES), required=True)
    args = parser.parse_args()
    source = str(args.source.resolve())
    # 两套已验证构建使用独立源码树，避免混入其他试验分支
    for patch in patch_chain(args.profile):
        subprocess.run(['git', '-C', source, 'apply', '--check', str(patch)], check=True)
        subprocess.run(['git', '-C', source, 'apply', str(patch)], check=True)
        print('applied', patch.name)


if __name__ == '__main__':
    main()