/* SPDX-License-Identifier: MIT */
/*
 * Clean-room hostapd HDI shim for Xiaomi marble dual STA+AP libwpa.z.so.
 *
 * Bridges the OpenHarmony 4.1 wpa_supplicant-2.9 hostapd runtime to the
 * donor ADY-AL10 libhostapd_interface_service_1.0.z.so contract:
 *   1. Exports getHostapd() (versioned getHostapd@@1.0) returning the active
 *      hostapd_data instance for HostApdInterfaceShellCmd().
 *   2. Forwards hostapd control events (AP-ENABLED, AP-DISABLED,
 *      CTRL-EVENT-TERMINATING, AP-STA-POSSIBLE-PSK-MISMATCH,
 *      AP-STA-CONNECTED, AP-STA-DISCONNECTED) to HostapdEventReport() when
 *      libhostapd_hdi_client.z.so is resident in hostapd_host.
 */
#ifndef MARBLE_WPA_SOFTAP_HDI_SHIM_H
#define MARBLE_WPA_SOFTAP_HDI_SHIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hostapd_data;

#define MARBLE_HOSTAPD_CB_CONTENT_LEN 1024u

enum marble_hostapd_event {
    MARBLE_HOSTAPD_EVENT_STA_JOIN = 0u,
    MARBLE_HOSTAPD_EVENT_AP_STATE = 1u,
};

struct marble_hostapd_ap_cb_parm {
    char content[MARBLE_HOSTAPD_CB_CONTENT_LEN];
    int32_t id;
};

typedef int32_t (*marble_hostapd_event_report_fn)(const char *ifname,
                                                  uint32_t event,
                                                  void *data);

/* Exported HDI symbol required by libhostapd_interface_service_1.0.z.so. */
struct hostapd_data *getHostapd(void);

/* Internal lifecycle & event hooks invoked from ap_main.c / ap_ctrl_iface.c. */
void marble_hostapd_set_active(struct hostapd_data *hapd);
void marble_hostapd_notify_msg(const char *ifname, const char *txt, size_t len);

/* Test seam for offline host unit verification (NULL restores dlsym lookup). */
void marble_hostapd_set_event_reporter(marble_hostapd_event_report_fn fn);

#ifdef __cplusplus
}
#endif

#endif /* MARBLE_WPA_SOFTAP_HDI_SHIM_H */
