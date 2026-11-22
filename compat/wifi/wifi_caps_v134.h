#ifndef HMOS_WIFI_CAPS_V134_H
#define HMOS_WIFI_CAPS_V134_H
#include <stdint.h>
struct HmosWifiCaps {
    uint32_t ifindex, wiphy;
    unsigned he, eht, records, mask;
};
int HmosReadWifiCaps(const char *ifname, struct HmosWifiCaps *caps);
#endif
