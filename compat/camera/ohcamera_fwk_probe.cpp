/* SPDX-License-Identifier: MIT */
/*
 * ohcamera_fwk_probe.cpp - Public OHOS NDK libohcamera.so -> SA 3008 (camera_service)
 * framework verification probe for Xiaomi marble (SM7475).
 *
 * Verifies:
 *   - Camera enumeration (back + front physical sensors)
 *   - Default output capabilities (preview, photo, video, metadata)
 *   - Scene modes & per-mode capabilities: NORMAL_PHOTO (1) & NORMAL_VIDEO (2)
 *   - Flashlight / Torch support + OH_CameraManager_SetTorchMode (ON -> OFF)
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <unistd.h>
#include <ohcamera/camera.h>
#include <ohcamera/camera_manager.h>

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    bool testTorchToggle = true;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-torch") == 0)
            testTorchToggle = false;
    }

    Camera_Manager *mgr = nullptr;
    Camera_ErrorCode rc = OH_Camera_GetCameraManager(&mgr);
    std::printf("OH_CAMERA_MGR rc=%d mgr=%p\n", static_cast<int>(rc), static_cast<void *>(mgr));
    if (rc != CAMERA_OK || mgr == nullptr)
        return 1;

    bool torchSupp = false;
    Camera_ErrorCode trc = OH_CameraManager_IsTorchSupported(mgr, &torchSupp);
    bool torchOnSupp = false;
    Camera_ErrorCode tmr = OH_CameraManager_IsTorchSupportedByTorchMode(mgr, ON, &torchOnSupp);
    std::printf("OH_TORCH_SUPPORTED rc=%d supported=%d mode_on_rc=%d mode_on=%d\n",
                static_cast<int>(trc), torchSupp ? 1 : 0,
                static_cast<int>(tmr), torchOnSupp ? 1 : 0);

    bool torchSetOk = false;
    if (testTorchToggle && torchSupp) {
        Camera_ErrorCode onRc = OH_CameraManager_SetTorchMode(mgr, ON);
        ::usleep(120000); /* 120ms pulse */
        Camera_ErrorCode offRc = OH_CameraManager_SetTorchMode(mgr, OFF);
        torchSetOk = (onRc == CAMERA_OK && offRc == CAMERA_OK);
        std::printf("OH_TORCH_TOGGLE on_rc=%d off_rc=%d ok=%d\n",
                    static_cast<int>(onRc), static_cast<int>(offRc), torchSetOk ? 1 : 0);
    } else if (!testTorchToggle) {
        torchSetOk = true;
        std::printf("OH_TORCH_TOGGLE skipped=1\n");
    }

    Camera_Device *cams = nullptr;
    uint32_t sz = 0;
    rc = OH_CameraManager_GetSupportedCameras(mgr, &cams, &sz);
    std::printf("OH_SUPPORTED_CAMERAS rc=%d count=%u\n", static_cast<int>(rc), sz);
    if (rc != CAMERA_OK || cams == nullptr || sz < 2u) {
        if (cams)
            OH_CameraManager_DeleteSupportedCameras(mgr, cams, sz);
        OH_Camera_DeleteCameraManager(mgr);
        return 2;
    }

    uint32_t capsOk = 0;
    uint32_t metasOk = 0;
    uint32_t photoModeOk = 0;
    uint32_t videoModeOk = 0;
    bool hasBack = false, hasFront = false;
    for (uint32_t i = 0; i < sz; ++i) {
        const char *cid = cams[i].cameraId ? cams[i].cameraId : "null";
        if (cams[i].cameraPosition == CAMERA_POSITION_BACK) hasBack = true;
        if (cams[i].cameraPosition == CAMERA_POSITION_FRONT) hasFront = true;
        std::printf("  CAM[%u] id=%s pos=%d type=%d conn=%d\n",
                    i, cid,
                    static_cast<int>(cams[i].cameraPosition),
                    static_cast<int>(cams[i].cameraType),
                    static_cast<int>(cams[i].connectionType));

        Camera_OutputCapability *cap = nullptr;
        Camera_ErrorCode crc = OH_CameraManager_GetSupportedCameraOutputCapability(mgr, &cams[i], &cap);
        if (crc == CAMERA_OK && cap != nullptr) {
            std::printf("    CAP_DEFAULT rc=0 prev=%u photo=%u video=%u meta=%u\n",
                        cap->previewProfilesSize, cap->photoProfilesSize,
                        cap->videoProfilesSize, cap->metadataProfilesSize);
            if (cap->previewProfilesSize >= 3u && cap->photoProfilesSize >= 3u)
                capsOk++;
            if (cap->metadataProfilesSize >= 1u)
                metasOk++;
            OH_CameraManager_DeleteSupportedCameraOutputCapability(mgr, cap);
        } else {
            std::printf("    CAP_DEFAULT rc=%d\n", static_cast<int>(crc));
        }

        Camera_SceneMode *modes = nullptr;
        uint32_t modeCnt = 0;
        Camera_ErrorCode smrc = OH_CameraManager_GetSupportedSceneModes(&cams[i], &modes, &modeCnt);
        if (smrc == CAMERA_OK && modes != nullptr) {
            std::printf("    SCENE_MODES count=%u:", modeCnt);
            for (uint32_t m = 0; m < modeCnt; ++m)
                std::printf(" %d", static_cast<int>(modes[m]));
            std::printf("\n");
            OH_CameraManager_DeleteSceneModes(mgr, modes);
        } else {
            std::printf("    SCENE_MODES rc=%d\n", static_cast<int>(smrc));
        }

        Camera_OutputCapability *photoCap = nullptr;
        Camera_ErrorCode prc = OH_CameraManager_GetSupportedCameraOutputCapabilityWithSceneMode(
            mgr, &cams[i], NORMAL_PHOTO, &photoCap);
        if (prc == CAMERA_OK && photoCap != nullptr) {
            std::printf("    CAP_PHOTO(1) rc=0 prev=%u photo=%u video=%u\n",
                        photoCap->previewProfilesSize, photoCap->photoProfilesSize,
                        photoCap->videoProfilesSize);
            if (photoCap->previewProfilesSize >= 4u && photoCap->photoProfilesSize >= 4u)
                photoModeOk++;
            OH_CameraManager_DeleteSupportedCameraOutputCapability(mgr, photoCap);
        } else {
            std::printf("    CAP_PHOTO(1) rc=%d\n", static_cast<int>(prc));
        }

        Camera_OutputCapability *videoCap = nullptr;
        Camera_ErrorCode vrc = OH_CameraManager_GetSupportedCameraOutputCapabilityWithSceneMode(
            mgr, &cams[i], NORMAL_VIDEO, &videoCap);
        if (vrc == CAMERA_OK && videoCap != nullptr) {
            std::printf("    CAP_VIDEO(2) rc=0 prev=%u photo=%u video=%u\n",
                        videoCap->previewProfilesSize, videoCap->photoProfilesSize,
                        videoCap->videoProfilesSize);
            if (videoCap->previewProfilesSize >= 4u && videoCap->videoProfilesSize >= 4u)
                videoModeOk++;
            OH_CameraManager_DeleteSupportedCameraOutputCapability(mgr, videoCap);
        } else {
            std::printf("    CAP_VIDEO(2) rc=%d\n", static_cast<int>(vrc));
        }
    }

    OH_CameraManager_DeleteSupportedCameras(mgr, cams, sz);
    OH_Camera_DeleteCameraManager(mgr);

    bool ok = (sz >= 2u) && hasBack && hasFront &&
              (capsOk == sz) && (metasOk == sz) &&
              (photoModeOk == sz) && (videoModeOk == sz) &&
              torchSupp && torchSetOk;
    std::printf("MARBLE_OHCAMERA_FWK_PROBE_SUMMARY ok=%d cams=%u caps_ok=%u metas_ok=%u "
                "photo_mode_ok=%u video_mode_ok=%u torch_supp=%d torch_set_ok=%d back=%d front=%d\n",
                ok ? 1 : 0, sz, capsOk, metasOk, photoModeOk, videoModeOk,
                torchSupp ? 1 : 0, torchSetOk ? 1 : 0,
                hasBack ? 1 : 0, hasFront ? 1 : 0);
    return ok ? 0 : 3;
}
