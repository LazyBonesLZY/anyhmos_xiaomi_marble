#!/usr/bin/env python3
"""Offline host-contract & packaging verification for Batch F11 (Voice Call ADSP Audio + Earpiece/Speakerphone/BT SCO + Daytime Volume Fix)."""
from __future__ import annotations

import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools/packaging'
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import build_marble_h20_f11_voice_audio as pkg

AUDIO_DIR = ROOT / 'compat/audio'
AUDIO_VDI_C = AUDIO_DIR / 'audio_vdi.c'
PAL_SERVER_C = AUDIO_DIR / 'pal_server.c'
PROTOCOL_H = AUDIO_DIR / 'protocol.h'
PROBE_C = AUDIO_DIR / 'native_voice_audio_probe.c'
FLASH_SH = ROOT / 'work/marble-dual-20261123/flash-h20-f11-control.sh'
LLVM_READELF = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-readelf'
LLVM_NM = ROOT / 'work/ohos-sdk/installed/native/llvm/bin/llvm-nm'

HOST_HARNESS_C = r'''
#include "iaudio_manager_vdi.h"
#include "pal_client.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
extern struct IAudioManagerVdi *AudioManagerCreateIfInstance(void);

/* Mock SOCK_SEQPACKET PAL server speaking HMOS_PAL_* protocol on a private socket */
static char mock_sock_name[64];
static volatile int mock_stop = 0;
static int mock_listen_fd = -1;

struct mock_state {
    uint32_t last_open_dev;
    uint32_t last_set_dev;
    uint32_t last_vol_u;
    int voice_open_cnt;
    int voice_start_cnt;
    int voice_stop_cnt;
    int voice_close_cnt;
    int voice_set_dev_cnt;
    int voice_vol_cnt;
    uint32_t voice_state;
    uint32_t voice_rx;
    uint32_t voice_tx;
    uint32_t voice_vsid;
    uint32_t voice_vol_u;
    uint32_t last_capture_dev;
    int capture_open_cnt;
};
static struct mock_state ms;

/* Override hmos_pal_connect() so audio_vdi talks to our private mock socket */
int hmos_pal_connect(void) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    size_t n = strlen(mock_sock_name);
    memcpy(a.sun_path + 1, mock_sock_name, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    if (connect(fd, (void *)&a, len) != 0) { close(fd); return -1; }
    return fd;
}

static uint32_t tx_for_rx(uint32_t rx) {
    if (rx == HMOS_PAL_DEV_OUT_SPEAKER) return HMOS_PAL_DEV_IN_SPEAKER_MIC;
    if (rx == HMOS_PAL_DEV_OUT_BLUETOOTH_SCO) return HMOS_PAL_DEV_IN_BT_SCO_HEADSET;
    return HMOS_PAL_DEV_IN_HANDSET_MIC;
}

static uint64_t voice_word(void) {
    return (uint64_t)(ms.voice_state & 0xFFu) |
           ((uint64_t)(ms.voice_rx & 0xFFu) << 8) |
           ((uint64_t)(ms.voice_tx & 0xFFu) << 16) |
           ((uint64_t)ms.voice_vsid << 32);
}

static void *mock_server_thread(void *arg) {
    (void)arg;
    int cfd[16];
    for (int i = 0; i < 16; i++) cfd[i] = -1;
    while (!mock_stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(mock_listen_fd, &rfds);
        int mx = mock_listen_fd;
        for (int i = 0; i < 16; i++) {
            if (cfd[i] >= 0) { FD_SET(cfd[i], &rfds); if (cfd[i] > mx) mx = cfd[i]; }
        }
        struct timeval tv = { 0, 20000 };
        int sel = select(mx + 1, &rfds, 0, 0, &tv);
        if (sel <= 0) continue;
        if (FD_ISSET(mock_listen_fd, &rfds)) {
            int nfd = accept4(mock_listen_fd, 0, 0, SOCK_CLOEXEC);
            if (nfd >= 0) {
                for (int i = 0; i < 16; i++) {
                    if (cfd[i] < 0) { cfd[i] = nfd; nfd = -1; break; }
                }
                if (nfd >= 0) close(nfd);
            }
        }
        for (int i = 0; i < 16; i++) {
            if (cfd[i] < 0 || !FD_ISSET(cfd[i], &rfds)) continue;
            struct hmos_pal_request req = {0};
            ssize_t n = recv(cfd[i], &req, sizeof(req), 0);
            if (n <= 0) { close(cfd[i]); cfd[i] = -1; continue; }
            struct hmos_pal_reply rep = { .magic = HMOS_PAL_MAGIC, .sequence = req.sequence, .rc = 0 };
            switch (req.op) {
            case HMOS_PAL_STATUS:
            case HMOS_PAL_CAPTURE_STATUS:
                rep.rc = 0;
                break;
            case HMOS_PAL_OPEN:
                ms.last_open_dev = (req.arg1 >> 16) ? (req.arg1 >> 16) : HMOS_PAL_DEV_OUT_SPEAKER;
                break;
            case HMOS_PAL_VOLUME:
                ms.last_vol_u = req.arg0;
                break;
            case HMOS_PAL_SET_DEVICE:
                ms.last_set_dev = req.arg0;
                break;
            case HMOS_PAL_VOICE_OPEN:
                ms.voice_open_cnt++;
                ms.voice_rx = req.arg0 ? req.arg0 : HMOS_PAL_DEV_OUT_HANDSET;
                ms.voice_tx = tx_for_rx(ms.voice_rx);
                ms.voice_vsid = req.arg1 ? req.arg1 : HMOS_PAL_VSID_MMODE2;
                ms.voice_state = 1;
                rep.written_frames = voice_word();
                break;
            case HMOS_PAL_VOICE_START:
                ms.voice_start_cnt++;
                ms.voice_state = 2;
                rep.written_frames = voice_word();
                break;
            case HMOS_PAL_VOICE_STOP:
                ms.voice_stop_cnt++;
                ms.voice_state = 1;
                rep.written_frames = voice_word();
                break;
            case HMOS_PAL_VOICE_CLOSE:
                ms.voice_close_cnt++;
                ms.voice_state = 0;
                rep.written_frames = voice_word();
                break;
            case HMOS_PAL_VOICE_SET_DEVICE:
                ms.voice_set_dev_cnt++;
                ms.voice_rx = req.arg0;
                ms.voice_tx = req.arg1 ? req.arg1 : tx_for_rx(req.arg0);
                rep.written_frames = voice_word();
                break;
            case HMOS_PAL_VOICE_VOLUME:
                ms.voice_vol_cnt++;
                ms.voice_vol_u = req.arg0;
                rep.written_frames = voice_word();
                break;
            case HMOS_PAL_VOICE_STATUS:
                rep.written_frames = voice_word();
                break;
            case HMOS_PAL_CAPTURE_OPEN:
                ms.capture_open_cnt++;
                ms.last_capture_dev = (req.arg1 >> 16) ? (req.arg1 >> 16) : HMOS_PAL_DEV_IN_HANDSET_MIC;
                break;
            case HMOS_PAL_CLOSE:
            case HMOS_PAL_STOP:
            case HMOS_PAL_START:
            case HMOS_PAL_CAPTURE_CLOSE:
                break;
            default:
                rep.rc = -95;
                break;
            }
            (void)send(cfd[i], &rep, sizeof(rep), MSG_NOSIGNAL);
        }
    }
    for (int i = 0; i < 16; i++) if (cfd[i] >= 0) close(cfd[i]);
    return 0;
}

int main(void) {
    snprintf(mock_sock_name, sizeof(mock_sock_name), "hmos.pal.f11.test.%d", (int)getpid());
    mock_listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(mock_listen_fd >= 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    size_t n = strlen(mock_sock_name);
    memcpy(a.sun_path + 1, mock_sock_name, n);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    assert(bind(mock_listen_fd, (void *)&a, len) == 0);
    assert(listen(mock_listen_fd, 8) == 0);

    pthread_t th;
    assert(pthread_create(&th, 0, mock_server_thread, 0) == 0);

    struct IAudioManagerVdi *mgr = AudioManagerCreateIfInstance();
    assert(mgr != 0);
    struct AudioAdapterDescriptorVdi ad = { .adapterName = "primary" };
    struct IAudioAdapterVdi *adapter = 0;
    assert(mgr->LoadAdapter(mgr, &ad, &adapter) == 0 && adapter != 0);

    /* 1. Default daytime volume = 1.0f on Speaker + Earpiece & BT SCO CreateRender */
    struct AudioSampleAttributesVdi attrs = {0};
    attrs.type = AUDIO_VDI_IN_MEDIA;
    attrs.interleaved = true;
    attrs.format = AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT;
    attrs.sampleRate = 48000;
    attrs.channelCount = 2;
    struct AudioDeviceDescriptorVdi spk = { 0, PIN_VDI_OUT_SPEAKER, "speaker" };
    struct AudioDeviceDescriptorVdi ear = { 0, PIN_VDI_OUT_EARPIECE, "earpiece" };
    struct AudioDeviceDescriptorVdi bt  = { 0, PIN_VDI_OUT_BLUETOOTH_SCO, "bt_sco" };

    struct IAudioRenderVdi *r_spk = 0;
    assert(adapter->CreateRender(adapter, &spk, &attrs, &r_spk) == 0);
    float vol = 0.0f;
    assert(r_spk->GetVolume(r_spk, &vol) == 0);
    assert(fabsf(vol - 1.0f) < 0.001f);
    assert(ms.last_open_dev == HMOS_PAL_DEV_OUT_SPEAKER);
    assert(ms.last_vol_u == 1000000u);

    struct IAudioRenderVdi *r_ear = 0;
    assert(adapter->CreateRender(adapter, &ear, &attrs, &r_ear) == 0);
    assert(ms.last_open_dev == HMOS_PAL_DEV_OUT_HANDSET);
    assert(adapter->DestroyRender(adapter, r_ear) == 0);

    struct IAudioRenderVdi *r_bt = 0;
    assert(adapter->CreateRender(adapter, &bt, &attrs, &r_bt) == 0);
    assert(ms.last_open_dev == HMOS_PAL_DEV_OUT_BLUETOOTH_SCO);
    assert(adapter->DestroyRender(adapter, r_bt) == 0);

    /* 2. Donor SetPhoneCallScene(1) via offset 168 starts VOICEMMODE2 on HANDSET */
    int32_t (*set_call_scene)(struct IAudioAdapterVdi *, int32_t) =
        (int32_t (*)(struct IAudioAdapterVdi *, int32_t))adapter->donorExtensions[5];
    assert(set_call_scene(adapter, 1) == 0);
    assert(ms.voice_state == 2 && ms.voice_rx == HMOS_PAL_DEV_OUT_HANDSET &&
           ms.voice_tx == HMOS_PAL_DEV_IN_HANDSET_MIC && ms.voice_vsid == HMOS_PAL_VSID_MMODE2);

    /* 3. SetVoiceVolume updates active voice call */
    assert(adapter->SetVoiceVolume(adapter, 0.55f) == 0);
    assert(ms.voice_vol_u == 550000u);
    assert(adapter->SetVoiceVolume(adapter, 1.5f) == -22);

    /* 4. UpdateAudioRoute switches HANDSET -> SPEAKER -> BT_SCO -> HANDSET */
    struct AudioRouteNodeVdi src = { 1, AUDIO_VDI_PORT_SOURCE_ROLE, AUDIO_VDI_PORT_DEVICE_TYPE,
        .ext = { .device = { 0, PIN_VDI_IN_MIC, "mic" } } };
    struct AudioRouteNodeVdi sink = { 0, AUDIO_VDI_PORT_SINK_ROLE, AUDIO_VDI_PORT_DEVICE_TYPE,
        .ext = { .device = { 0, PIN_VDI_OUT_SPEAKER, "spk" } } };
    struct AudioRouteVdi rt = { &src, 1, &sink, 1 };
    int32_t h1 = 0, h2 = 0, h3 = 0;
    assert(adapter->UpdateAudioRoute(adapter, &rt, &h1) == 0 && h1 > 0);
    assert(ms.voice_rx == HMOS_PAL_DEV_OUT_SPEAKER && ms.voice_tx == HMOS_PAL_DEV_IN_SPEAKER_MIC);

    sink.ext.device.type = PIN_VDI_OUT_BLUETOOTH_SCO;
    assert(adapter->UpdateAudioRoute(adapter, &rt, &h2) == 0 && h2 > h1);
    assert(ms.voice_rx == HMOS_PAL_DEV_OUT_BLUETOOTH_SCO && ms.voice_tx == HMOS_PAL_DEV_IN_BT_SCO_HEADSET);

    sink.ext.device.type = PIN_VDI_OUT_EARPIECE;
    assert(adapter->UpdateAudioRoute(adapter, &rt, &h3) == 0 && h3 > h2);
    assert(ms.voice_rx == HMOS_PAL_DEV_OUT_HANDSET && ms.voice_tx == HMOS_PAL_DEV_IN_HANDSET_MIC);

    assert(adapter->ReleaseAudioRoute(adapter, h1) == 0);
    assert(adapter->ReleaseAudioRoute(adapter, h2) == 0);
    assert(adapter->ReleaseAudioRoute(adapter, h3) == 0);

    /* 5. SetPhoneCallScene(0) stops voice call */
    assert(set_call_scene(adapter, 0) == 0);
    assert(ms.voice_state == 0);

    /* 6. SetExtraParams(sim_slot=0) selects VOICEMMODE1 */
    assert(adapter->SetExtraParams(adapter, AUDIO_VDI_EXT_PARAM_KEY_STATUS, "voice", "sim_slot=0") == 0);
    assert(set_call_scene(adapter, 1) == 0);
    assert(ms.voice_state == 2 && ms.voice_vsid == HMOS_PAL_VSID_MMODE1);
    assert(set_call_scene(adapter, 0) == 0);
    assert(ms.voice_state == 0);

    /* 7. SelectScene(AUDIO_VDI_IN_CALL -> AUDIO_VDI_IN_MEDIA) */
    struct AudioSceneDescriptorVdi sc = { .scene = { .id = AUDIO_VDI_IN_CALL },
        .desc = { 0, PIN_VDI_OUT_SPEAKER, "speaker" } };
    bool sup = false;
    assert(r_spk->CheckSceneCapability(r_spk, &sc, &sup) == 0 && sup);
    assert(r_spk->SelectScene(r_spk, &sc) == 0);
    assert(ms.voice_state == 2 && ms.voice_rx == HMOS_PAL_DEV_OUT_SPEAKER);
    sc.scene.id = AUDIO_VDI_IN_MEDIA;
    sc.desc.pins = PIN_VDI_OUT_EARPIECE;
    assert(r_spk->SelectScene(r_spk, &sc) == 0);
    assert(ms.voice_state == 0 && ms.last_set_dev == HMOS_PAL_DEV_OUT_HANDSET);

    /* 8. Capture accepts AUDIO_VDI_IN_COMMUNICATION & BT SCO mic */
    struct AudioDeviceDescriptorVdi cap_d = { 1, PIN_VDI_IN_BLUETOOTH_SCO_HEADSET, "bt_mic" };
    struct AudioSampleAttributesVdi cap_a = {0};
    cap_a.type = AUDIO_VDI_IN_COMMUNICATION;
    cap_a.interleaved = true;
    cap_a.format = AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT;
    cap_a.sampleRate = 48000;
    cap_a.channelCount = 1;
    struct IAudioCaptureVdi *cap = 0;
    assert(adapter->CreateCapture(adapter, &cap_d, &cap_a, &cap) == 0 && cap != 0);
    assert(ms.last_capture_dev == HMOS_PAL_DEV_IN_BT_SCO_HEADSET);
    assert(adapter->DestroyCapture(adapter, cap) == 0);

    assert(adapter->DestroyRender(adapter, r_spk) == 0);
    assert(mgr->UnloadAdapter(mgr, adapter) == 0);

    mock_stop = 1;
    pthread_join(th, 0);
    close(mock_listen_fd);
    printf("F11_AUDIO_VDI_HOST_TEST_OK=1\n");
    return 0;
}
'''


class MarbleBatchF11Tests(unittest.TestCase):
    def test_f11_host_audio_vdi_mock_pal_contract(self):
        with tempfile.TemporaryDirectory() as td:
            tdp = Path(td)
            harness = tdp / 'harness.c'
            pal_client_mock = tdp / 'pal_client_req.c'
            # Extract hmos_pal_request + hmos_pal_capture_read without hmos_pal_connect (overridden by harness)
            src = (AUDIO_DIR / 'pal_client.c').read_text(encoding='utf-8')
            idx = src.index('int hmos_pal_request(')
            pal_client_mock.write_text(src[:src.index('int hmos_pal_connect(')] + src[idx:], encoding='utf-8')
            harness.write_text(HOST_HARNESS_C, encoding='utf-8')
            exe = tdp / 'f11_host_test'
            subprocess.run(
                [
                    'gcc', '-O2', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE',
                    '-I', str(AUDIO_DIR), '-I', str(AUDIO_DIR / 'include'),
                    str(AUDIO_VDI_C), str(pal_client_mock), str(harness),
                    '-lpthread', '-lm', '-o', str(exe),
                ],
                check=True,
            )
            res = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn('F11_AUDIO_VDI_HOST_TEST_OK=1', res.stdout)

    def test_f11_volume_and_voice_markers(self):
        vdi = AUDIO_VDI_C.read_text(encoding='utf-8')
        pal = PAL_SERVER_C.read_text(encoding='utf-8')
        proto = PROTOCOL_H.read_text(encoding='utf-8')
        for marker in (
            'HMOS_AUDIO_DEFAULT_VOL 1.0f',
            'HMOS_AUDIO_QUIET_VOL   0.03f',
            'HMOS_AUDIO_QUIET_FLAG',
            'update_audio_route',
            'release_audio_route',
            'set_voice_volume',
            'set_extra_params',
            'set_phone_call_scene_typed',
            'donorExtensions[5]',
            'PIN_VDI_OUT_EARPIECE',
            'PIN_VDI_OUT_BLUETOOTH_SCO',
            'AUDIO_VDI_IN_CALL',
        ):
            self.assertIn(marker, vdi)
        for marker in (
            'HMOS_AUDIO_DEFAULT_VOL 1.0f',
            'HMOS_AUDIO_QUIET_VOL   0.03f',
            'PAL_STREAM_VOICE_CALL',
            'PAL_AUDIO_INPUT_OUTPUT',
            'VOICEMMODE1',
            'VOICEMMODE2',
            'pal_stream_set_device',
            'HMOS_PAL_VOICE_OPEN',
            'HMOS_PAL_VOICE_SET_DEVICE',
            'HMOS_PAL_VOICE_VOLUME',
            'packet.h.op>=HMOS_PAL_CAPTURE_OPEN && packet.h.op<=HMOS_PAL_MIC_GET_MUTE',
        ):
            self.assertIn(marker, pal)
        for marker in (
            'HMOS_PAL_SET_DEVICE',
            'HMOS_PAL_VOICE_OPEN',
            'HMOS_PAL_VOICE_STATUS',
            'HMOS_PAL_VSID_MMODE2',
        ):
            self.assertIn(marker, proto)

    def test_f11_stage_patch_helper(self):
        sample = (
            b'#!/bin/sh\n'
            b'# === MARBLE_F10_5G_IMS_CALL_SMS_STAGE_BEGIN ===\n'
            + pkg.STAGE_ANCHOR + b'\n'
        )
        patched = pkg.patch_stage(sample)
        self.assertIn(b'MARBLE_F11_VOICE_AUDIO_STAGE_BEGIN', patched)
        self.assertIn(b'/graphics/native_voice_audio_probe', patched)
        self.assertNotIn(b'$SRC', patched)

    def test_f11_built_binaries_elf_contract(self):
        for rel, want_needed, want_soname, want_sym in (
            ('libhmos_audio_vdi.so', {'libc.so'}, ['libhmos_audio_vdi.so'], 'AudioManagerCreateIfInstance'),
            ('libhmos_pal_server.so', {'libc.so', 'libdl.so'}, ['libhmos_pal_server.so'], '__libc_init'),
            ('native_voice_audio_probe', {'libc.so'}, [], None),
        ):
            p = pkg.F11_BUILD / rel
            self.assertTrue(p.is_file(), f'Missing built F11 binary: {p}')
            dyn = subprocess.check_output([str(LLVM_READELF), '-d', str(p)], text=True)
            needed = set(re.findall(r'\(NEEDED\)\s+Shared library:\s+\[([^\]]+)\]', dyn))
            soname = re.findall(r'\(SONAME\)\s+Library soname:\s+\[([^\]]+)\]', dyn)
            self.assertEqual(needed, want_needed)
            self.assertEqual(soname, want_soname)
            if want_sym:
                syms = subprocess.check_output([str(LLVM_NM), '-D', '--defined-only', str(p)], text=True)
                self.assertIn(want_sym, syms)

    def test_f11_package_report_and_guards(self):
        self.assertEqual(len(pkg.GUARD_MEMBERS), 85)
        report_path = pkg.F11_BUILD / 'f11-build-report.json'
        if not report_path.is_file():
            self.skipTest('f11-build-report.json not generated yet')
        report = json.loads(report_path.read_text(encoding='utf-8'))
        cand = Path(report['candidate']['path'])
        if not cand.is_file():
            self.skipTest(f'Candidate image missing: {cand}')

        self.assertEqual(report['base']['sha256'], pkg.BASE_SHA)
        self.assertEqual(len(report['guard_members_verified']), 85)
        self.assertEqual(report['entries_after'], report['entries_before'] + 1)
        archive, r = pkg.load_oracle()
        img = r.read_original(cand)
        layout = r.layout(img)
        avb = r.verify_existing_avb(img, layout['payload_end'])
        self.assertIn('footer_masked', avb)
        with tempfile.TemporaryDirectory() as td:
            got, *_ = r.extract_entries(cand, img, layout, Path(td))
            for rel in (
                'graphics/libhmos_audio_vdi.so',
                'graphics/audio-runtime/lib64/libhmos_pal_server.so',
                'graphics/native_voice_audio_probe',
            ):
                self.assertEqual(r.sha(got[pkg.PREFIX + rel][1]), report['staged'][rel]['sha256'])
            self.assertIn(b'MARBLE_F11_VOICE_AUDIO_STAGE_BEGIN', got[pkg.STAGE_MEMBER][1])
            manifest = archive.parse(got[r.MANIFEST][1])
            r.manifest_holds(got, manifest)

    def test_f11_flash_script_safety(self):
        if not FLASH_SH.is_file():
            self.skipTest('flash-h20-f11-control.sh not created yet')
        subprocess.run(['bash', '-n', str(FLASH_SH)], check=True)
        txt = FLASH_SH.read_text(encoding='utf-8')
        self.assertIn('flash vendor_boot_a', txt)
        self.assertNotIn('flash boot_a', txt)


if __name__ == '__main__':
    unittest.main()
