"""Build the selected implementations directly; never regenerate older versions.

--list and --dry-run do not compile or touch a device. External
SDK/upstream headers, libdrm and donor libraries must be supplied separately.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
DEPS = Path(os.environ.get('HMOS_DEPS', os.environ.get('SHENG_DEPS', '/root/hmos-work/graphics-deps')))
SDK = Path(os.environ.get('HMOS_SDK', os.environ.get('SHENG_SDK', '/root/hmos-work/sdk-6.0.0.47/installed/native')))
UPSTREAM = Path(os.environ.get('HMOS_UPSTREAM', os.environ.get('SHENG_UPSTREAM', str(DEPS/'source'))))
LINK = Path(os.environ.get('HMOS_DONOR_LINK', os.environ.get('SHENG_DONOR_LINK', str(ROOT/'work/donor-hdi-link'))))
STAGE = Path(os.environ.get('HMOS_STAGE', os.environ.get('SHENG_STAGE', str(DEPS/'stage'))))


def includes(paths):
    return ['-I'+str(p) for p in paths]


def commands(out, default_display_route=0, *, single_dsi=False):
    if default_display_route not in (0, 2):
        raise ValueError('Default display route must be 0 or 2')
    # 路由和面板拓扑独立；双 plane 直扫契约仍固定使用原拓扑。
    route_flags = ['-DHMOS_DEFAULT_DISPLAY_ROUTE=2'] if default_display_route == 2 else []
    composer_flags = route_flags + (['-DHMOS_SPLIT_SCANOUT=0'] if single_dsi else [])
    # 按宿主系统选择 SDK 工具名，保留目标平台参数
    suffix = '.exe' if os.name == 'nt' else ''
    clang = str(SDK/('llvm/bin/clang'+suffix))
    clangpp = str(SDK/('llvm/bin/clang++'+suffix))
    cflags = ['--target=aarch64-linux-ohos', '-O2', '-Wall', '-Wextra']
    cppflags = cflags+['-std=c++17', '-fno-exceptions', '-fno-rtti', '-nostdlib++',
        '-include', str(ROOT/'compat/platform/platform_cpp_config.h')]
    hdf = UPSTREAM/'drivers_hdf_core'
    shared_inc = [hdf/'interfaces/inner_api'/p for p in
                  ('hdi','ipc','hdi/base','utils','osal/uhdf','core')]
    shared_inc += [UPSTREAM/'commonlibrary_c_utils/base/include',
        UPSTREAM/'communication_ipc/interfaces/innerkits/ipc_core/include',
        UPSTREAM/'communication_ipc/interfaces/innerkits/ipc_single/include',
        UPSTREAM/'hiviewdfx_hilog/interfaces/native/innerkits/include']
    cpp_runtime = [str(LINK/'libc++.so')]
    hdi_libs = [str(LINK/n) for n in ('libhdi.z.so','libhdi_base.z.so',
        'libutils.z.so','libipc_single.z.so','libipc_common.z.so','libc++.so')]
    result = {}

    def add(key, output, src, flags=(), libs=(), inc=(), cpp=False):
        compiler = clangpp if cpp else clang
        result[key] = [compiler]+(cppflags if cpp else cflags)+includes(inc)+list(flags)+[
            str(ROOT/s) for s in src]+list(libs)+['-o', str(out/output)]

    for key, output, source in [
        ('bootstrap','hmos-probe','compat/bootstrap/hmos_probe_v121.c'),
        ('init-entry','init','compat/bootstrap/init_entry.c'),
    ]:
        flags = ['-static','-O2','-Wall','-Wextra','-Werror']
        if key=='init-entry':
            flags=['-nostdlib','-static','-Os','-fno-stack-protector','-fno-builtin',
                   '-Wl,-e,_start,-z,noexecstack,--build-id=none']
        result[key]=['aarch64-linux-gnu-gcc',*flags,str(ROOT/source),'-o',str(out/output)]
    drm_inc = [STAGE/'include', STAGE/'include/libdrm']
    display_inc = [ROOT/'compat/display',ROOT/'compat/display/include/vdi-only',
        ROOT/'compat/display/include',ROOT/'compat/display/include/display/buffer',
        UPSTREAM/'drivers_hdf_core/interfaces/inner_api/hdi/base',
        UPSTREAM/'drivers_peripheral/display/buffer/hdi_service/include',
        UPSTREAM/'drivers_interface/display/composer/common/include',
        UPSTREAM/'hiviewdfx_hilog/interfaces/native/innerkits/include',
        *drm_inc]
    add('composer','libdisplay_composer_vdi_impl.z.so',
        ['compat/display/composer_vdi.cpp','compat/display/composer_unsupported.cpp'],
        ['-g','-shared','-fPIC','-Wl,-z,defs','-Wl,-soname,libdisplay_composer_vdi_impl.z.so']+composer_flags,
        [str(STAGE/'lib/libdrm.a'),*cpp_runtime],display_inc,True)
    # 使用模拟 DRM 提交验证模式切换，不访问物理显示设备
    add('mode-contract','mode_switch_contract',
        ['compat/display/composer_vdi.cpp','compat/display/composer_unsupported.cpp',
         'tests/display/mode_switch_contract.cpp'],
        ['-DACE5_MODE_TEST','-Wl,--wrap=drmModeCreatePropertyBlob',
         '-Wl,--wrap=drmModeDestroyPropertyBlob','-Wl,--wrap=drmModeAtomicCommit']+composer_flags,
        [str(STAGE/'lib/libdrm.a'),*cpp_runtime],display_inc,True)
    add('vblank-probe','vblank_probe',['tests/display/vblank_probe.c'],
        [],[str(STAGE/'lib/libdrm.a')],display_inc)
    add('present-contract','present_fence_contract',
        ['compat/display/composer_vdi.cpp','compat/display/composer_unsupported.cpp',
         'tests/display/present_fence_contract.cpp'],
        ['-DACE5_MODE_TEST','-DACE5_SUBMIT_TEST',
         '-Wl,--wrap=drmModeAtomicAlloc','-Wl,--wrap=drmModeAtomicAddProperty',
         '-Wl,--wrap=drmModeAtomicCommit','-Wl,--wrap=drmPrimeFDToHandle',
         '-Wl,--wrap=drmModeAddFB2','-Wl,--wrap=drmModeRmFB']+composer_flags,
        [str(STAGE/'lib/libdrm.a'),*cpp_runtime],display_inc,True)
    # 使用模拟 DRM 验证直扫提交、释放同步和失败回退
    add('direct-contract','direct_scanout_contract',
        ['compat/display/composer_vdi.cpp','compat/display/composer_unsupported.cpp',
         'tests/display/direct_scanout_contract.cpp'],
        ['-DACE5_MODE_TEST','-DACE5_SUBMIT_TEST','-DACE5_DIRECT_TEST',
         '-Wl,--wrap=drmModeAtomicAlloc','-Wl,--wrap=drmModeAtomicAddProperty',
         '-Wl,--wrap=drmModeAtomicCommit','-Wl,--wrap=drmPrimeFDToHandle',
         '-Wl,--wrap=drmModeAddFB2','-Wl,--wrap=drmModeRmFB',
         '-Wl,--wrap=fopen','-Wl,--wrap=ioctl',
         '-Wl,--wrap=drmModeObjectGetProperties','-Wl,--wrap=drmModeGetProperty',
         '-Wl,--wrap=drmModeDestroyPropertyBlob']+route_flags,
        [str(STAGE/'lib/libdrm.a'),*cpp_runtime],display_inc,True)
    # 模拟控制文件及读取错误，验证启动默认与显式覆盖
    add('route-policy-contract','display_route_policy_contract',
        ['compat/display/composer_vdi.cpp','compat/display/composer_unsupported.cpp',
         'tests/display/display_route_policy_contract.cpp'],
        ['-DACE5_DIRECT_TEST','-Wl,--wrap=open',
         '-Wl,--wrap=read','-Wl,--wrap=close']+composer_flags,
        [str(STAGE/'lib/libdrm.a'),*cpp_runtime],display_inc,True)
    add('allocator','libdisplay_buffer_vdi_impl.z.so',['compat/display/buffer_vdi.cpp'],
        ['-g','-DACE5_LINEAR_STRIDE_ALIGNMENT=128','-shared','-fPIC','-Wl,-z,defs',
         '-Wl,-soname,libdisplay_buffer_vdi_impl.z.so'],cpp_runtime,display_inc,True)
    add('allocator-contract','alpha_metadata_probe',['tests/display/alpha_metadata_probe.cpp'],
        [],cpp_runtime,display_inc,True)
    # 使用非正方形图案验证公开平面步长和无效布局
    add('plane-contract','plane_layout_contract',
        ['compat/display/buffer_vdi.cpp','tests/display/plane_layout_contract.cpp'],
        ['-DACE5_LINEAR_STRIDE_ALIGNMENT=128'],cpp_runtime,display_inc,True)
    # 沿用已验证的原电源工具唤醒实现及构建参数
    result['display-wake'] = [clangpp,'--target=aarch64-linux-ohos',
        '--sysroot='+str(SDK/'sysroot'),'-O2','-Wall','-Wextra','-Werror',
        '-std=c++17','-nostdlib++',str(ROOT/'compat/display/native_display_wake.cpp'),
        '-Wl,-z,defs','-Wl,-z,relro','-Wl,-z,now','-Wl,-z,noexecstack',
        '-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1','-ldl',
        '-o',str(out/'native_display_wake')]
    add('display-mode','native_refresh_rate',['compat/display/native_refresh_rate.cpp'],
        ['-Os'],cpp_runtime,[],True)
    diagnostic_flags = ['--sysroot='+str(SDK/'sysroot'),'-Wl,-z,defs']
    # The VDI client shares the composer's C++ ABI and loads both VDI libs at runtime.
    add('display-scanout','hmos_display_scanout',['compat/services/hmos_display_scanout.cpp'],
        diagnostic_flags,[*cpp_runtime,'-ldl'],display_inc,True)
    add('kms-diagnostic','hmos_kms_diagnostic',['compat/services/hmos_kms_diagnostic.cpp'],
        diagnostic_flags,[str(STAGE/'lib/libdrm.a'),*cpp_runtime],drm_inc,True)
    # The capture tool only uses the SDK's DRM UAPI, not libdrm.
    add('capture-scanout','capture_scanout',['compat/services/capture_scanout.c'],diagnostic_flags)
    add('touch','hmos_thp_bridge',
        ['compat/touch/bridge.cpp','compat/touch/nvt_touch_core.cpp','compat/touch/nvt_finger_filter.cpp'],
        ['-std=c++20','-fexceptions'],cpp_runtime,[ROOT/'compat/touch'],True)
    add('touch-launcher','native_thp_launcher',['compat/touch/launcher.c'])

    audio=ROOT/'compat/audio'
    add('audio-vdi','libhmos_audio_vdi.so',['compat/audio/audio_vdi.c','compat/audio/pal_client.c'],
        ['-D_GNU_SOURCE','-shared','-fPIC','-Wl,-z,defs','-Wl,-soname,libhmos_audio_vdi.so'],
        ['-pthread'],[audio,audio/'include'])
    for key,source,libs in [
        ('audio-loader','audio_vdi_loader.c',['-ldl']),
        ('audio-manager','audio_manager_service.c',['-ldl','-pthread']),
    ]:
        output='libhmos_audio_vdi_loader.so' if key=='audio-loader' else 'libhmos_audio_manager_service.so'
        add(key,output,['compat/audio/'+source],['-shared','-fPIC','-Wl,-z,defs'],libs)
    add('audio-hdi','native_audio_hdi_publisher',['compat/audio/native_audio_hdi_publisher.c'],
        [],['-ldl'],shared_inc)
    add('audio-launcher','native_audio_service_launcher',['compat/audio/native_audio_service_launcher.c'])
    add('audio-check','native_system_audio_check',['compat/audio/native_system_audio_check.c'],[],['-lohaudio','-lm'])
    # PAL headers are kept in-tree (BSD-3, Qualcomm arpal) so the shims rebuild without
    # the old /root/hmos-reference checkout.  See compat/audio/pal-headers/PROVENANCE.md.
    palinc=Path(os.environ.get('HMOS_PAL_INCLUDE', os.environ.get('SHENG_PAL_INCLUDE',
        str(ROOT/'compat/audio/pal-headers/inc'))))
    bionic=Path(os.environ.get('HMOS_BIONIC_LINK', os.environ.get('SHENG_BIONIC_LINK',str(ROOT/'work/audio-bionic-v1/lib64'))))
    bflags=['--target=aarch64-linux-android33','--sysroot='+str(SDK/'sysroot'),
        '-isystem',str(SDK/'sysroot/usr/include/aarch64-linux-ohos'),
        '-fuse-ld=lld','-fPIC','-shared','-nostdlib','-D_GNU_SOURCE','-mno-outline-atomics',
        '-O2','-Wall','-Wextra','-Werror','-Wl,-z,defs','-L'+str(bionic)]
    # 为原厂充电进程构建独立属性与日志适配库
    result['batterysecret-process']=[clang,*bflags,'-fno-builtin',
        '-Wl,-soname,libhmos_batterysecret_process.so',
        str(ROOT/'compat/power/batterysecret_process_compat.c'),'-lc','-ldl',
        '-o',str(out/'libhmos_batterysecret_process.so')]
    # 采集后端与鸿蒙输入接口配套构建
    result['pal-server']=[clang,*bflags,'-DHMOS_CAPTURE_EXPERIMENTAL=1',
        '-I'+str(palinc),'-Wl,-soname,libhmos_pal_server.so',
        str(audio/'pal_server.c'),'-lc','-ldl','-o',str(out/'libhmos_pal_server.so')]
    # The lifecycle shim is Bionic and must not silently pick the OHOS libc.
    result['pal-lifecycle']=[clang,*bflags,'-Wl,-soname,libhmos_pal_lifecycle.so',str(audio/'pal_lifecycle.c'),'-lc','-ldl',
        '-o',str(out/'libhmos_pal_lifecycle.so')]
    # Audio route library for the marble vendor PAL.  AOSP audio_route.c is compiled into a
    # private hmos_ar_* namespace and compat/audio/audioroute/shim.c exports only the five
    # symbols libar-pal.so / libaudioroute_ext.so import, treating their struct pointer as
    # opaque.  Built with the Android NDK because the result must run on marble Bionic.
    ndk=Path(os.environ.get('HMOS_NDK', '/home/lazybones/android-ndk-r27d'))
    ndk_sysroot=ndk/'toolchains/llvm/prebuilt/linux-x86_64/sysroot'
    audioroute=audio/'audioroute'
    ar_bionic=Path(os.environ.get('HMOS_AUDIO_BIONIC',
        os.environ.get('HMOS_BIONIC_LINK', str(ROOT/'work/marble-bionic/lib64'))))
    ar_renames=[f'-D{name}=hmos_ar_{name[len("audio_route_"):]}' for name in
        ('audio_route_init','audio_route_free','audio_route_update_mixer',
         'audio_route_apply_and_update_path','audio_route_reset_and_update_path')]
    result['audioroute']=[str(ndk/'toolchains/llvm/prebuilt/linux-x86_64/bin/clang'),
        '--target=aarch64-linux-android'+str(os.environ.get('HMOS_NDK_API', '33')),
        '--sysroot='+str(ndk_sysroot),'-O2','-Wall','-Wextra','-Werror','-fPIC','-shared',
        '-fvisibility=hidden','-D_GNU_SOURCE',*ar_renames,
        '-I'+str(audioroute/'include'),'-I'+str(audioroute/'include/expat'),
        '-Wl,-soname,libaudioroute.so','-Wl,-z,defs',
        '-Wl,--version-script='+str(audioroute/'exports.map'),
        str(audioroute/'audio_route.c'),str(audioroute/'shim.c'),
        '-L'+str(ar_bionic),'-ltinyalsa','-lexpat','-llog','-ldl','-lc',
        '-o',str(out/'libaudioroute.so')]
    pd=audio/'pd-mapper'
    result['pd-mapper']=['aarch64-linux-gnu-gcc','-static','-O2','-D_GNU_SOURCE',
        '-I'+str(pd),*[str(pd/n) for n in ('pd-mapper.c','assoc.c','json.c','servreg_loc.c',
          'qrtr.c','qmi.c','logging.c')],'-o',str(out/'hmos-pd-mapper')]

    add('hisysevent-sink','hmos_hisysevent_sink',['compat/services/hisysevent_sink.c'],[],['-std=gnu11'])
    add('brightness-persist','hmos_brightness_persist',['compat/services/brightness_persist.c'],[],['-std=gnu11'])
    add('wifi-category','libhmos_wifi_category_v134.so',
        ['compat/wifi/wifi_caps_v134.c','compat/wifi/wifi_category_ioctl_v134.c'],
        ['-std=gnu11','-fPIC','-fvisibility=hidden','-shared',
         '-Wl,-soname,libhmos_wifi_category_v134.so'],[],[ROOT/'compat/wifi/include',ROOT/'compat/wifi'])
    add('wifi-contract','native_wifi_category_probe_v134',
        ['compat/wifi/wifi_caps_v134.c','compat/wifi/native_wifi_category_probe_v134.c'],
        ['-std=gnu11'],['-ldl'],[ROOT/'compat/wifi/include',ROOT/'compat/wifi'])
    for key,source in [('storage-launcher','storage_ram_launcher.c'),('storage-no-tee','storage_ram_no_tee.c')]:
        shared=key=='storage-no-tee'
        add(key,'libhmos_ram_no_tee.so' if shared else 'storage-ram-launcher',
            ['compat/framework/account/'+source],['-shared','-fPIC','-Wl,-z,relro,-z,now'] if shared else [])
    huks=UPSTREAM/'security_huks/interfaces/inner_api/huks_standard/main/include'
    add('account-hmac','libhmos_account_hmac.so',['compat/framework/account/account_hmac.c'],
        ['-std=gnu11','-Wno-misleading-indentation','-shared','-fPIC','-Wl,-z,relro,-z,now',
         '-Wl,--version-script='+str(ROOT/'compat/framework/account/exports.map')],
        [str(LINK/'libcrypto_openssl.z.so'),'-Wl,--allow-shlib-undefined'],[huks])
    add('database','libdbkeys.so',['compat/framework/database/db_key.c'],
        ['-std=gnu11','-Wno-misleading-indentation','-shared','-fPIC','-Wl,-z,relro,-z,now',
         '-Wl,--version-script='+str(ROOT/'compat/framework/database/exports.map'),'-Wl,-soname,libdbkeys.so'],
        [str(LINK/'libhukssdk.z.so'),str(LINK/'libcrypto_openssl.z.so'),'-Wl,--allow-shlib-undefined'],[huks])
    for key,source,output in [
        ('battery-publisher','compat/battery/native_battery_hdi_publisher_v131.cpp','native_battery_hdi_publisher_v131'),
        ('bluetooth-publisher','compat/bluetooth/native_bt_hdi_publisher_v136.cpp','native_bt_hdi_publisher_v136'),
    ]:
        group='battery' if key.startswith('battery') else 'bluetooth'
        inc=[ROOT/'compat'/group/'include',ROOT/'compat'/group/'include'/('bluetooth/hci' if group=='bluetooth' else 'battery')]
        libs=hdi_libs.copy()
        if group=='bluetooth':
            btlibs=Path(os.environ.get('HMOS_BT_VENDOR_LINK', os.environ.get('SHENG_BT_VENDOR_LINK',str(ROOT/'work/donor-bluetooth-v136/vendor/lib64'))))
            libs+=[str(btlibs/'libbluetooth_hci_stub_1.0.z.so')]
        sources=[source]
        if group=='battery':
            sources.append('compat/battery/battery_charge_adapter.cpp')
        add(key,output,sources,[],libs+['-ldl','-Wl,--allow-shlib-undefined'],inc+shared_inc,True)
    result['bluetooth-vendor']=[clangpp,*cflags,
        '-I'+str(UPSTREAM/'drivers_peripheral/bluetooth/hci/hdi_service/implement'),
        '-shared','-fPIC',str(ROOT/'compat/bluetooth/bluetooth_vendor_v136.cpp'),
        '-ldl','-o',str(out/'libbt_vendor_hmos_v136.so')]
    headers=Path(os.environ.get('HMOS_ANDROID_CPP_HEADERS', os.environ.get('SHENG_ANDROID_CPP_HEADERS','/root/hmos-work/clang-prebuilt/clang-r450784e/include/c++/v1')))
    for name in ('bridge','identity','initialize'):
        result['bluetooth-'+name]=[str(SDK/'llvm/bin/clang++'),*bflags,
            '-nostdinc++','-isystem',str(headers),'-std=c++17','-fno-exceptions',
            str(ROOT/f'compat/bluetooth/bluetooth_{name}_v136.cpp'),'-lc++','-lc','-ldl',
            '-o',str(out/f'libhmos_bt_{name}_v136.so')]
    # v139 host check is a two-translation-unit helper, not the old v137 binary.
    result['bluetooth-current-c']=[clang,*cflags,'-DHMOS_CURRENT_BT_API','-c',
        str(ROOT/'compat/bluetooth/native_bt_host_check_v137.c'),'-o',str(out/'bt-current.o')]
    # marble: reuse the same donor HDI sources under marble artifact names.
    # marble_bt_bridge.c is plain C (H4/TTY + btpower + bluetooth_a firmware) and
    # is built both as the daemon and as a shared library for the symbol contract.
    result['bluetooth-bridge-marble']=[clang,*cflags,'-std=gnu11',
        str(ROOT/'compat/bluetooth/marble_bt_bridge.c'),
        '-o',str(out/'hmos_bt_bridge_marble')]
    result['bluetooth-bridge-marble-lib']=[clang,*cflags,'-std=gnu11','-fPIC','-shared',
        '-DHMOS_BT_BRIDGE_NO_MAIN','-Wl,-soname,libhmos_bt_bridge_marble.so',
        str(ROOT/'compat/bluetooth/marble_bt_bridge.c'),
        '-o',str(out/'libhmos_bt_bridge_marble.so')]
    # Explicit diagnostic target only; not a boot payload or service.
    add('bluetooth-rom-probe-marble','marble_bt_rom_probe',
        ['compat/bluetooth/marble_bt_rom_probe.c'],['-std=c11','-Werror'])
    add('bluetooth-fw-probe-marble','marble_bt_fw_probe',
        ['compat/bluetooth/marble_bt_fw_probe.c'],['-std=c11','-Werror'])
    result['bluetooth-vendor-marble']=[clangpp,*cflags,
        '-I'+str(UPSTREAM/'drivers_peripheral/bluetooth/hci/hdi_service/implement'),
        '-shared','-fPIC',str(ROOT/'compat/bluetooth/bluetooth_vendor_v136.cpp'),
        '-ldl','-Wl,-soname,libbt_vendor.z.so','-o',str(out/'libbt_vendor.z.so')]
    bt_publish=Path(os.environ.get('HMOS_BT_VENDOR_LINK', os.environ.get('SHENG_BT_VENDOR_LINK',str(ROOT/'work/donor-bluetooth-v136/vendor/lib64'))))
    add('bluetooth-hdi-marble','hmos_bt_hdi_publisher_marble',
        ['compat/bluetooth/native_bt_hdi_publisher_v136.cpp'],[],
        hdi_libs+[str(bt_publish/'libbluetooth_hci_stub_1.0.z.so'),'-ldl','-Wl,--allow-shlib-undefined'],
        [ROOT/'compat/bluetooth/include',ROOT/'compat/bluetooth/include'/('bluetooth/hci')]+shared_inc,True)
    add('bluetooth-current','native_bt_host_check_v139',
        ['compat/bluetooth/native_bt_current_v139.cpp'],[],
        [str(out/'bt-current.o'),*cpp_runtime,'-ldl','-Wl,--allow-shlib-undefined'],[],True)
    # Common service launchers do not require the removed per-round builders.
    for source in ('native_render_launcher.c','native_brightness_launcher.c','native_bundle_cli.c'):
        add(source[:-2],source[:-2],['compat/services/'+source],[],['-ldl'])
    add('boot-launcher','bootanimation-launcher',['compat/services/bootanimation_launcher.c'])
    add('boot-vulkan','libhmos_boot_vulkan.so',['compat/services/bootanimation_vulkan_adapter.cpp'],
        ['-shared','-fPIC','-Wl,-z,defs'],cpp_runtime,[],True)
    add('surface-post','libhmos_surface_diagnostic.so',
        ['compat/services/surface_post.cpp','compat/services/native_buffer_import_observer.S'],
        ['-shared','-fPIC'],cpp_runtime,[],True)
    add('sceneboard-optional-sa','libhmos_sceneboard_optional_sa.so',
        ['compat/framework/fallbacks/sceneboard_optional_sa.cpp'],
        ['-shared','-fPIC','-Wl,-z,relro,-z,now',
         '-Wl,--version-script='+str(ROOT/'compat/framework/fallbacks/sceneboard_optional_sa.map'),
         '-Wl,-soname,libhmos_sceneboard_optional_sa.so'],
        ['-ldl'],[],True)
    # marble DCM SA 65866 software-TEE replacement. The token is derived from
    # this device's own serial/model/UDID and signed with a per-device key; it is
    # a local stand-in for the Huawei cloud token, not a Huawei attestation.
    add('dcm-token','dcm_token.o',['compat/dcm/dcm_token.c'],['-c'],['-std=gnu11'],[ROOT/'compat/dcm'])
    add('dcm-local-ca','dcm_local_ca.o',['compat/dcm/dcm_local_ca.c'],['-c'],['-std=gnu11'],
        [ROOT/'compat/dcm',ROOT/'work/openssl-inc'])
    add('dcm-probe','hmos_dcm_token_probe',
        ['compat/dcm/hmos_dcm_token_probe.cpp'],[],
        [str(out/'dcm_token.o'),str(LINK/'libipc_single.z.so'),str(LINK/'libipc_common.z.so'),
         str(LINK/'libutils.z.so'),str(LINK/'libhilog.so'),str(LINK/'libc++.so'),
         '-ldl','-Wl,--allow-shlib-undefined'],
        [ROOT/'compat/dcm']+shared_inc,True)
    add('dcm-service','hmos_dcm_service',
        ['compat/dcm/hmos_dcm_service.cpp'],[],
        [str(out/'dcm_token.o'),str(out/'dcm_local_ca.o'),str(LINK/'libipc_single.z.so'),
         str(LINK/'libipc_common.z.so'),str(LINK/'libutils.z.so'),str(LINK/'libhilog.so'),
         str(LINK/'libcrypto_openssl.z.so'),str(LINK/'libc++.so'),
         '-ldl','-Wl,--allow-shlib-undefined'],
        [ROOT/'compat/dcm',ROOT/'work/openssl-inc']+shared_inc,True)
    add('wifi-ui-unavailable','libhmos_wifi_ui_unavailable.so',
        ['compat/framework/fallbacks/wifi_ui_unavailable.c'],
        ['-shared','-fPIC','-Wl,-z,relro,-z,now',
         '-Wl,--version-script='+str(ROOT/'compat/framework/fallbacks/wifi_ui_unavailable.map'),
         '-Wl,-soname,libhmos_wifi_ui_unavailable.so'],
        ['-ldl'])
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('targets',nargs='*')
    parser.add_argument('--list',action='store_true')
    parser.add_argument('--default-display-route',type=int,choices=(0,2),default=0)
    parser.add_argument('--single-dsi',action='store_true',
        help='Build composer and compatible contracts for a single DSI panel. Default: sheng split '
             'scanout. The direct-contract target always tests split scanout.')
    parser.add_argument('--dry-run',action='store_true')
    parser.add_argument('--out',type=Path,default=ROOT/'work/current-build')
    args=parser.parse_args()
    out=args.out.resolve()
    plan=commands(out,args.default_display_route,single_dsi=args.single_dsi)
    if args.list:
        print('\n'.join(plan));return
    if not args.targets:parser.error('Select targets explicitly, or use --list.')
    if any(name not in plan for name in args.targets):parser.error('Unknown target')
    targets=list(dict.fromkeys(args.targets))
    if 'bluetooth-current' in targets and 'bluetooth-current-c' not in targets:
        targets.insert(targets.index('bluetooth-current'),'bluetooth-current-c')
    if ('dcm-service' in targets or 'dcm-probe' in targets) and 'dcm-token' not in targets:
        targets.insert(0,'dcm-token')
    if 'dcm-service' in targets and 'dcm-local-ca' not in targets:
        targets.insert(0,'dcm-local-ca')
    if args.dry_run:
        print(json.dumps({name:plan[name] for name in targets},indent=2));return
    out.mkdir(parents=True,exist_ok=True)
    report={}
    for name in targets:
        command=plan[name]
        subprocess.run(command,check=True)
        artifact=Path(command[command.index('-o')+1])
        if artifact.suffix!='.o':
            suffix = '.exe' if os.name == 'nt' else ''
            strip='aarch64-linux-gnu-strip' if command[0]=='aarch64-linux-gnu-gcc' else str(SDK/('llvm/bin/llvm-strip'+suffix))
            subprocess.run([strip,str(artifact)],check=True)
        report[name]=dict(command=command,bytes=artifact.stat().st_size,
            sha256=hashlib.sha256(artifact.read_bytes()).hexdigest())
    (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:{'bytes':v['bytes'],'sha256':v['sha256']} for k,v in report.items()},indent=2))


if __name__=='__main__':main()
