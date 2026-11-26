/*
 * marble: hostapd (AP side) entry points for the supplicant-only libwpa.z.so
 *
 * The OpenHarmony wpa_supplicant fork routes every driver event through
 * wpa_supplicant_event() in events.c, which calls the hostapd dispatchers in
 * src/ap/drv_callbacks.c when run_mode == 1.  That file is only meaningful when
 * it is compiled with -DHOSTAPD (which the wpa_supplicant makefile does not
 * do), so its object ends up empty and the two symbols stay undefined.
 *
 * marble brings up the station path first: set_running_wpa() keeps run_mode at
 * 0, so these hooks are never taken.  They are provided here as no-ops instead
 * of linking the whole hostapd object set, which would duplicate every
 * the src/ap objects and pull in radius/eap_server code paths.
 *
 * SoftAP / P2P-GO needs the real implementation: rebuild with -DHOSTAPD for
 * every src/ap translation unit, drop this file from the link.  See
 * work/marble-wifi-supplicant-report.md ("未验证项 / 后续").
 */
#include "includes.h"
#include "common.h"
#include "wpa_supplicant_i.h"

void wpa_supplicant_event_hapd(void *ctx, enum wpa_event_type event,
                               union wpa_event_data *data);
void wpa_supplicant_event_global_hapd(void *ctx, enum wpa_event_type event,
                                      union wpa_event_data *data);

void wpa_supplicant_event_hapd(void *ctx, enum wpa_event_type event,
                               union wpa_event_data *data)
{
	(void)ctx;
	(void)data;
	(void)event;
}

void wpa_supplicant_event_global_hapd(void *ctx, enum wpa_event_type event,
                                      union wpa_event_data *data)
{
	(void)ctx;
	(void)data;
	(void)event;
}