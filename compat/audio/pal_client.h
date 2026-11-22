/* SPDX-License-Identifier: MIT */
#ifndef HMOS_PAL_CLIENT_H
#define HMOS_PAL_CLIENT_H
#include "protocol.h"
int hmos_pal_connect(void);
int hmos_pal_request(int fd, uint32_t op, uint32_t arg0, uint32_t arg1,
    const void *pcm, uint32_t bytes, struct hmos_pal_reply *reply);
int hmos_pal_capture_read(int fd, void *pcm, uint32_t capacity,
    struct hmos_pal_reply *reply);
#endif
