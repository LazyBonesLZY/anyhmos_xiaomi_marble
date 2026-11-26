/* SPDX-License-Identifier: MIT */
/*
 * Clean-room hostapd HDI shim for Xiaomi marble dual STA+AP libwpa.z.so.
 *
 * Keeps libwpa.z.so DT_NEEDED minimal (libnl_share.z.so + libc.so) by
 * resolving HostapdEventReport dynamically only when running inside
 * hostapd_host (where libhostapd_hdi_client.z.so is already mapped).
 */
#define _GNU_SOURCE

#include "wpa_softap_hdi_shim.h"

#include <dlfcn.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_shim_lock = PTHREAD_MUTEX_INITIALIZER;
static struct hostapd_data *g_active_hostapd = NULL;
static marble_hostapd_event_report_fn g_override_reporter = NULL;
static marble_hostapd_event_report_fn g_cached_reporter = NULL;
static int g_reporter_resolved = 0;

static int has_prefix(const char *txt, size_t len, const char *prefix)
{
    size_t plen = strlen(prefix);
    return len >= plen && memcmp(txt, prefix, plen) == 0;
}

static marble_hostapd_event_report_fn resolve_event_reporter(void)
{
    marble_hostapd_event_report_fn fn;

    pthread_mutex_lock(&g_shim_lock);
    if (g_override_reporter != NULL) {
        fn = g_override_reporter;
        pthread_mutex_unlock(&g_shim_lock);
        return fn;
    }
    if (g_reporter_resolved) {
        fn = g_cached_reporter;
        pthread_mutex_unlock(&g_shim_lock);
        return fn;
    }

    fn = NULL;
    {
        void *sym = dlsym(RTLD_DEFAULT, "HostapdEventReport");
        if (sym == NULL) {
            void *h = dlopen("libhostapd_hdi_client.z.so",
                             RTLD_NOLOAD | RTLD_LAZY);
            if (h != NULL) {
                sym = dlsym(h, "HostapdEventReport");
                dlclose(h);
            }
        }
        if (sym != NULL) {
            memcpy(&fn, &sym, sizeof(fn));
        }
    }

    g_cached_reporter = fn;
    g_reporter_resolved = 1;
    pthread_mutex_unlock(&g_shim_lock);
    return fn;
}

__attribute__((visibility("default")))
struct hostapd_data *getHostapd(void)
{
    struct hostapd_data *hapd;

    pthread_mutex_lock(&g_shim_lock);
    hapd = g_active_hostapd;
    pthread_mutex_unlock(&g_shim_lock);
    return hapd;
}

void marble_hostapd_set_active(struct hostapd_data *hapd)
{
    pthread_mutex_lock(&g_shim_lock);
    g_active_hostapd = hapd;
    pthread_mutex_unlock(&g_shim_lock);
}

void marble_hostapd_set_event_reporter(marble_hostapd_event_report_fn fn)
{
    pthread_mutex_lock(&g_shim_lock);
    g_override_reporter = fn;
    if (fn == NULL) {
        g_cached_reporter = NULL;
        g_reporter_resolved = 0;
    }
    pthread_mutex_unlock(&g_shim_lock);
}

void marble_hostapd_notify_msg(const char *ifname, const char *txt, size_t len)
{
    uint32_t event;
    struct marble_hostapd_ap_cb_parm parm;
    marble_hostapd_event_report_fn reporter;
    size_t copy_len;

    if (ifname == NULL || ifname[0] == '\0' || txt == NULL || len == 0) {
        return;
    }

    if (has_prefix(txt, len, "AP-STA-CONNECTED") ||
        has_prefix(txt, len, "AP-STA-DISCONNECTED")) {
        event = MARBLE_HOSTAPD_EVENT_STA_JOIN;
    } else if (has_prefix(txt, len, "AP-ENABLED") ||
               has_prefix(txt, len, "AP-DISABLE") ||
               has_prefix(txt, len, "CTRL-EVENT-TERMINATING") ||
               has_prefix(txt, len, "AP-STA-POSSIBLE-PSK-MISMATCH ")) {
        event = MARBLE_HOSTAPD_EVENT_AP_STATE;
    } else {
        return;
    }

    reporter = resolve_event_reporter();
    if (reporter == NULL) {
        return;
    }

    memset(&parm, 0, sizeof(parm));
    copy_len = len;
    if (copy_len >= sizeof(parm.content)) {
        copy_len = sizeof(parm.content) - 1u;
    }
    memcpy(parm.content, txt, copy_len);
    parm.content[copy_len] = '\0';
    parm.id = 0;

    (void)reporter(ifname, event, &parm);
}
