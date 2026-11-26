/* SPDX-License-Identifier: MIT
 * Live on-device Batch F11 voice call audio + earpiece/speakerphone + volume verifier.
 * Loads libhmos_audio_vdi.so and queries @hmos.pal.pcm.v1 to confirm:
 *   1. Daytime default speaker hardware volume is unity (1.0f, or 0.03f if quiet flag exists).
 *   2. Earpiece & Speaker render streams open cleanly.
 *   3. Donor SetPhoneCallScene(1) opens & starts PAL_STREAM_VOICE_CALL (VOICEMMODE2).
 *   4. SetVoiceVolume(0.6f) succeeds on active voice call.
 *   5. UpdateAudioRoute switches Earpiece <-> Speakerphone live on PAL_STREAM_VOICE_CALL.
 *   6. SelectScene(AUDIO_VDI_IN_CALL) and SetPhoneCallScene(0) cleanly stop & close voice call.
 *   7. VOICEMMODE1 (Slot 0) also opens/starts/closes cleanly via SetExtraParams("sim_slot=0").
 */
#include "iaudio_manager_vdi.h"
#include "pal_client.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int query_voice_status(uint64_t *status_out)
{
    int fd = hmos_pal_connect();
    if (fd < 0) return fd;
    struct hmos_pal_reply reply = {0};
    int rc = hmos_pal_request(fd, HMOS_PAL_VOICE_STATUS, 0, 0, 0, 0, &reply);
    close(fd);
    if (!rc && status_out) *status_out = reply.written_frames;
    return rc;
}

int main(int argc, char **argv)
{
    const char *vdi_path = (argc > 1 && argv[1] && argv[1][0])
        ? argv[1]
        : "/vendor/lib64/libhmos_audio_vdi.so";

    void *h = dlopen(vdi_path, RTLD_NOW);
    if (!h) {
        fprintf(stderr, "VOICE_PROBE_ERR dlopen=%s err=%s\n", vdi_path, dlerror());
        return 2;
    }
    struct IAudioManagerVdi *(*create_mgr)(void) = dlsym(h, "AudioManagerCreateIfInstance");
    if (!create_mgr) {
        fprintf(stderr, "VOICE_PROBE_ERR missing AudioManagerCreateIfInstance\n");
        return 3;
    }
    struct IAudioManagerVdi *mgr = create_mgr();
    if (!mgr) return 4;

    uint64_t st0 = 0;
    int voice_status_rc = query_voice_status(&st0);
    printf("VOICE_PROBE_INIT status_rc=%d state=%u rx=%u tx=%u vsid=0x%x\n",
        voice_status_rc, (unsigned)(st0 & 0xFFu), (unsigned)((st0 >> 8) & 0xFFu),
        (unsigned)((st0 >> 16) & 0xFFu), (unsigned)(st0 >> 32));

    struct AudioAdapterDescriptorVdi desc = { .adapterName = "primary" };
    struct IAudioAdapterVdi *adapter = 0;
    int load_rc = mgr->LoadAdapter(mgr, &desc, &adapter);
    if (load_rc || !adapter) {
        fprintf(stderr, "VOICE_PROBE_ERR LoadAdapter rc=%d\n", load_rc);
        return 5;
    }

    /* 1. Check default speaker volume and earpiece render creation */
    struct AudioDeviceDescriptorVdi spk_desc = { .portId = 0, .pins = PIN_VDI_OUT_SPEAKER, .desc = "speaker" };
    struct AudioDeviceDescriptorVdi ear_desc = { .portId = 0, .pins = PIN_VDI_OUT_EARPIECE, .desc = "earpiece" };
    struct AudioSampleAttributesVdi attrs = {0};
    attrs.type = AUDIO_VDI_IN_MEDIA;
    attrs.interleaved = true;
    attrs.format = AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT;
    attrs.sampleRate = 48000;
    attrs.channelCount = 2;
    attrs.channelLayout = 3;

    struct IAudioRenderVdi *spk_render = 0;
    int spk_rc = adapter->CreateRender(adapter, &spk_desc, &attrs, &spk_render);
    float spk_vol = -1.0f;
    if (!spk_rc && spk_render) {
        (void)spk_render->GetVolume(spk_render, &spk_vol);
        (void)adapter->DestroyRender(adapter, spk_render);
    }

    struct IAudioRenderVdi *ear_render = 0;
    int ear_rc = adapter->CreateRender(adapter, &ear_desc, &attrs, &ear_render);
    if (!ear_rc && ear_render) {
        (void)adapter->DestroyRender(adapter, ear_render);
    }
    int quiet_flag = (access("/data/local/tmp/marble-audio-quiet", F_OK) == 0);
    float want_vol = quiet_flag ? 0.03f : 1.0f;
    int vol_ok = (!spk_rc && fabsf(spk_vol - want_vol) < 0.005f);
    printf("VOICE_PROBE_RENDER spk_rc=%d spk_vol=%.4f want_vol=%.2f vol_ok=%d ear_rc=%d\n",
        spk_rc, spk_vol, want_vol, vol_ok, ear_rc);

    /* 2. Start cellular voice call via donor SetPhoneCallScene(1) (VOICEMMODE2 default) */
    int32_t (*set_call_scene)(struct IAudioAdapterVdi *, int32_t) =
        (int32_t (*)(struct IAudioAdapterVdi *, int32_t))adapter->donorExtensions[5];
    int call_on_rc = set_call_scene(adapter, 1);
    uint64_t st_call = 0;
    (void)query_voice_status(&st_call);
    int call_active_ok = (!call_on_rc && (st_call & 0xFFu) == 2u &&
                          ((st_call >> 8) & 0xFFu) == HMOS_PAL_DEV_OUT_HANDSET &&
                          ((st_call >> 16) & 0xFFu) == HMOS_PAL_DEV_IN_HANDSET_MIC &&
                          (unsigned)(st_call >> 32) == HMOS_PAL_VSID_MMODE2);
    printf("VOICE_PROBE_MMODE2 call_on_rc=%d state=%u rx=%u tx=%u vsid=0x%x ok=%d\n",
        call_on_rc, (unsigned)(st_call & 0xFFu), (unsigned)((st_call >> 8) & 0xFFu),
        (unsigned)((st_call >> 16) & 0xFFu), (unsigned)(st_call >> 32), call_active_ok);

    /* 3. SetVoiceVolume on active call */
    int vvol_rc = adapter->SetVoiceVolume(adapter, 0.6f);
    printf("VOICE_PROBE_VOL rc=%d\n", vvol_rc);

    /* 4. Live route switch: Earpiece -> Speakerphone -> Earpiece via UpdateAudioRoute */
    struct AudioRouteNodeVdi src_mic = { .portId = 1, .role = AUDIO_VDI_PORT_SOURCE_ROLE,
        .type = AUDIO_VDI_PORT_DEVICE_TYPE, .ext = { .device = { 0, PIN_VDI_IN_MIC, "mic" } } };
    struct AudioRouteNodeVdi sink_spk = { .portId = 0, .role = AUDIO_VDI_PORT_SINK_ROLE,
        .type = AUDIO_VDI_PORT_DEVICE_TYPE, .ext = { .device = { 0, PIN_VDI_OUT_SPEAKER, "speaker" } } };
    struct AudioRouteNodeVdi sink_ear = { .portId = 0, .role = AUDIO_VDI_PORT_SINK_ROLE,
        .type = AUDIO_VDI_PORT_DEVICE_TYPE, .ext = { .device = { 0, PIN_VDI_OUT_EARPIECE, "earpiece" } } };

    struct AudioRouteVdi route_spk = { &src_mic, 1, &sink_spk, 1 };
    int32_t handle_spk = 0;
    int sw_spk_rc = adapter->UpdateAudioRoute(adapter, &route_spk, &handle_spk);
    uint64_t st_spk = 0;
    (void)query_voice_status(&st_spk);
    int spk_sw_ok = (!sw_spk_rc && handle_spk > 0 &&
                     (st_spk & 0xFFu) == 2u &&
                     ((st_spk >> 8) & 0xFFu) == HMOS_PAL_DEV_OUT_SPEAKER &&
                     ((st_spk >> 16) & 0xFFu) == HMOS_PAL_DEV_IN_SPEAKER_MIC);

    struct AudioRouteVdi route_ear = { &src_mic, 1, &sink_ear, 1 };
    int32_t handle_ear = 0;
    int sw_ear_rc = adapter->UpdateAudioRoute(adapter, &route_ear, &handle_ear);
    uint64_t st_ear = 0;
    (void)query_voice_status(&st_ear);
    int ear_sw_ok = (!sw_ear_rc && handle_ear > 0 &&
                     (st_ear & 0xFFu) == 2u &&
                     ((st_ear >> 8) & 0xFFu) == HMOS_PAL_DEV_OUT_HANDSET &&
                     ((st_ear >> 16) & 0xFFu) == HMOS_PAL_DEV_IN_HANDSET_MIC);

    int rel_spk_rc = adapter->ReleaseAudioRoute(adapter, handle_spk);
    int rel_ear_rc = adapter->ReleaseAudioRoute(adapter, handle_ear);
    printf("VOICE_PROBE_SWITCH spk_rc=%d(rx=%u,tx=%u,ok=%d) ear_rc=%d(rx=%u,tx=%u,ok=%d) rel=(%d,%d)\n",
        sw_spk_rc, (unsigned)((st_spk >> 8) & 0xFFu), (unsigned)((st_spk >> 16) & 0xFFu), spk_sw_ok,
        sw_ear_rc, (unsigned)((st_ear >> 8) & 0xFFu), (unsigned)((st_ear >> 16) & 0xFFu), ear_sw_ok,
        rel_spk_rc, rel_ear_rc);

    /* 5. Stop voice call via SetPhoneCallScene(0) */
    int call_off_rc = set_call_scene(adapter, 0);
    uint64_t st_off = 0;
    (void)query_voice_status(&st_off);
    int call_off_ok = (!call_off_rc && (st_off & 0xFFu) == 0u);
    printf("VOICE_PROBE_STOP call_off_rc=%d state=%u ok=%d\n",
        call_off_rc, (unsigned)(st_off & 0xFFu), call_off_ok);

    /* 6. Also verify VOICEMMODE1 (Slot 0) via SetExtraParams("sim_slot=0") */
    int extra_rc = adapter->SetExtraParams(adapter, AUDIO_VDI_EXT_PARAM_KEY_STATUS, "voice", "sim_slot=0");
    int mm1_on_rc = set_call_scene(adapter, 1);
    uint64_t st_mm1 = 0;
    (void)query_voice_status(&st_mm1);
    int mm1_ok = (!extra_rc && !mm1_on_rc && (st_mm1 & 0xFFu) == 2u &&
                  (unsigned)(st_mm1 >> 32) == HMOS_PAL_VSID_MMODE1);
    int mm1_off_rc = set_call_scene(adapter, 0);
    /* Restore Slot 1 default */
    (void)adapter->SetExtraParams(adapter, AUDIO_VDI_EXT_PARAM_KEY_STATUS, "voice", "sim_slot=1");
    printf("VOICE_PROBE_MMODE1 extra_rc=%d on_rc=%d off_rc=%d vsid=0x%x ok=%d\n",
        extra_rc, mm1_on_rc, mm1_off_rc, (unsigned)(st_mm1 >> 32), mm1_ok);

    /* 7. Verify SelectScene(AUDIO_VDI_IN_CALL -> AUDIO_VDI_IN_MEDIA) on a render handle */
    struct IAudioRenderVdi *scene_render = 0;
    int sr_rc = adapter->CreateRender(adapter, &spk_desc, &attrs, &scene_render);
    int sel_call_rc = -1, sel_media_rc = -1;
    uint64_t st_sel_call = 0, st_sel_media = 0;
    if (!sr_rc && scene_render) {
        struct AudioSceneDescriptorVdi sc_call = { .scene = { .id = AUDIO_VDI_IN_CALL },
            .desc = { .portId = 0, .pins = PIN_VDI_OUT_EARPIECE, .desc = "earpiece" } };
        struct AudioSceneDescriptorVdi sc_media = { .scene = { .id = AUDIO_VDI_IN_MEDIA },
            .desc = { .portId = 0, .pins = PIN_VDI_OUT_SPEAKER, .desc = "speaker" } };
        sel_call_rc = scene_render->SelectScene(scene_render, &sc_call);
        (void)query_voice_status(&st_sel_call);
        sel_media_rc = scene_render->SelectScene(scene_render, &sc_media);
        (void)query_voice_status(&st_sel_media);
        (void)adapter->DestroyRender(adapter, scene_render);
    }
    int scene_ok = (!sr_rc && !sel_call_rc && (st_sel_call & 0xFFu) == 2u &&
                    !sel_media_rc && (st_sel_media & 0xFFu) == 0u);
    printf("VOICE_PROBE_SCENE call_rc=%d(state=%u) media_rc=%d(state=%u) ok=%d\n",
        sel_call_rc, (unsigned)(st_sel_call & 0xFFu),
        sel_media_rc, (unsigned)(st_sel_media & 0xFFu), scene_ok);

    int unload_rc = mgr->UnloadAdapter(mgr, adapter);
    int all_ok = (!voice_status_rc && vol_ok && !ear_rc && call_active_ok && !vvol_rc &&
                  spk_sw_ok && ear_sw_ok && !rel_spk_rc && !rel_ear_rc &&
                  call_off_ok && mm1_ok && !mm1_off_rc && scene_ok && !unload_rc);
    printf("MARBLE_VOICE_AUDIO_PROBE_SUMMARY status=%d vol_ok=%d ear=%d mm2=%d vvol=%d sw_spk=%d sw_ear=%d stop=%d mm1=%d scene=%d unload=%d all_ok=%d\n",
        voice_status_rc == 0, vol_ok, ear_rc == 0, call_active_ok, vvol_rc == 0,
        spk_sw_ok, ear_sw_ok, call_off_ok, mm1_ok, scene_ok, unload_rc == 0, all_ok);
    dlclose(h);
    return all_ok ? 0 : 1;
}
