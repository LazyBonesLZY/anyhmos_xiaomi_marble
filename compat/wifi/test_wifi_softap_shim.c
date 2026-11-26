/* SPDX-License-Identifier: MIT */
/*
 * Offline host unit test for compat/wifi/wpa_softap_hdi_shim.c.
 */
#include "wpa_softap_hdi_shim.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

struct hostapd_data {
    int magic;
};

struct captured_event {
    int count;
    char ifname[64];
    uint32_t event;
    char content[MARBLE_HOSTAPD_CB_CONTENT_LEN];
    int32_t id;
};

static struct captured_event g_cap;

static int32_t mock_event_report(const char *ifname, uint32_t event, void *data)
{
    const struct marble_hostapd_ap_cb_parm *parm =
        (const struct marble_hostapd_ap_cb_parm *)data;

    g_cap.count++;
    snprintf(g_cap.ifname, sizeof(g_cap.ifname), "%s", ifname);
    g_cap.event = event;
    memcpy(g_cap.content, parm->content, sizeof(g_cap.content));
    g_cap.id = parm->id;
    return 0;
}

static void reset_cap(void)
{
    memset(&g_cap, 0, sizeof(g_cap));
}

int main(void)
{
    /* Verify ABI struct size & offset match donor libhostapd_interface_service. */
    assert(sizeof(struct marble_hostapd_ap_cb_parm) == 1028u);
    assert(offsetof(struct marble_hostapd_ap_cb_parm, content) == 0u);
    assert(offsetof(struct marble_hostapd_ap_cb_parm, id) == 1024u);

    /* 1. getHostapd() lifecycle */
    assert(getHostapd() == NULL);
    struct hostapd_data dummy = { .magic = 0x5A17 };
    marble_hostapd_set_active(&dummy);
    assert(getHostapd() == &dummy);
    assert(getHostapd()->magic == 0x5A17);
    marble_hostapd_set_active(NULL);
    assert(getHostapd() == NULL);

    /* 2. Null / absent reporter safety */
    marble_hostapd_set_event_reporter(NULL);
    marble_hostapd_notify_msg("wlan0", "AP-ENABLED", 10);

    /* 3. AP state events */
    marble_hostapd_set_event_reporter(mock_event_report);

    reset_cap();
    marble_hostapd_notify_msg("wlan0", "AP-ENABLED ", 11);
    assert(g_cap.count == 1);
    assert(strcmp(g_cap.ifname, "wlan0") == 0);
    assert(g_cap.event == MARBLE_HOSTAPD_EVENT_AP_STATE);
    assert(strcmp(g_cap.content, "AP-ENABLED ") == 0);
    assert(g_cap.id == 0);

    reset_cap();
    marble_hostapd_notify_msg("wlan0", "AP-DISABLED ", 12);
    assert(g_cap.count == 1);
    assert(g_cap.event == MARBLE_HOSTAPD_EVENT_AP_STATE);
    assert(strcmp(g_cap.content, "AP-DISABLED ") == 0);

    reset_cap();
    marble_hostapd_notify_msg("wlan1", "CTRL-EVENT-TERMINATING", 22);
    assert(g_cap.count == 1);
    assert(strcmp(g_cap.ifname, "wlan1") == 0);
    assert(g_cap.event == MARBLE_HOSTAPD_EVENT_AP_STATE);

    reset_cap();
    marble_hostapd_notify_msg("wlan0", "AP-STA-POSSIBLE-PSK-MISMATCH aa:bb:cc:dd:ee:ff", 46);
    assert(g_cap.count == 1);
    assert(g_cap.event == MARBLE_HOSTAPD_EVENT_AP_STATE);

    /* 4. STA join / leave events preserve MAC payload */
    reset_cap();
    const char *join_msg = "AP-STA-CONNECTED 11:22:33:44:55:66";
    marble_hostapd_notify_msg("wlan0", join_msg, strlen(join_msg));
    assert(g_cap.count == 1);
    assert(g_cap.event == MARBLE_HOSTAPD_EVENT_STA_JOIN);
    assert(strcmp(g_cap.content, join_msg) == 0);

    reset_cap();
    const char *leave_msg = "AP-STA-DISCONNECTED 11:22:33:44:55:66";
    marble_hostapd_notify_msg("wlan0", leave_msg, strlen(leave_msg));
    assert(g_cap.count == 1);
    assert(g_cap.event == MARBLE_HOSTAPD_EVENT_STA_JOIN);
    assert(strcmp(g_cap.content, leave_msg) == 0);

    /* 5. Unrelated messages and invalid inputs are ignored */
    reset_cap();
    marble_hostapd_notify_msg("wlan0", "CTRL-EVENT-SCAN-RESULTS", 23);
    marble_hostapd_notify_msg(NULL, "AP-ENABLED", 10);
    marble_hostapd_notify_msg("", "AP-ENABLED", 10);
    marble_hostapd_notify_msg("wlan0", NULL, 10);
    marble_hostapd_notify_msg("wlan0", "AP-ENABLED", 0);
    assert(g_cap.count == 0);

    /* 6. Oversized message clamps safely with NUL terminator */
    char huge[1500];
    memcpy(huge, "AP-STA-CONNECTED ", 17);
    memset(huge + 17, 'A', sizeof(huge) - 17);
    reset_cap();
    marble_hostapd_notify_msg("wlan0", huge, sizeof(huge));
    assert(g_cap.count == 1);
    assert(g_cap.event == MARBLE_HOSTAPD_EVENT_STA_JOIN);
    assert(strlen(g_cap.content) == MARBLE_HOSTAPD_CB_CONTENT_LEN - 1u);
    assert(g_cap.content[MARBLE_HOSTAPD_CB_CONTENT_LEN - 1u] == '\0');

    printf("TEST_WIFI_SOFTAP_SHIM=PASS\n");
    return 0;
}
