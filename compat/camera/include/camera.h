/* SPDX-License-Identifier: MIT */
#ifndef MARBLE_CAMERA_LOG_SHIM_H
#define MARBLE_CAMERA_LOG_SHIM_H

#include <cstdio>
#include <cstdint>

#ifndef CAMERA_LOGE
#define CAMERA_LOGE(fmt, ...) std::fprintf(stderr, "[MARBLE_CAM_VDI E] " fmt "\n", ##__VA_ARGS__)
#define CAMERA_LOGW(fmt, ...) std::fprintf(stderr, "[MARBLE_CAM_VDI W] " fmt "\n", ##__VA_ARGS__)
#define CAMERA_LOGI(fmt, ...) std::fprintf(stderr, "[MARBLE_CAM_VDI I] " fmt "\n", ##__VA_ARGS__)
#define CAMERA_LOGD(fmt, ...) ((void)0)
#endif

#endif /* MARBLE_CAMERA_LOG_SHIM_H */
