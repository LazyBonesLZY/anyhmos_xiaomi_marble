/* Preserve the bootloader SN for the donor's cmdline-only parameter reader. */
#ifndef HMOS_BOOT_SERIAL_BRIDGE_H
#define HMOS_BOOT_SERIAL_BRIDGE_H
#include <stddef.h>
#include <string.h>

static int hmos_serial_valid(const char *s, size_t n)
{
    if (!n || n > 95) return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}
static int hmos_sn_key(const char *s, size_t n)
{
    return (n == 2 && !memcmp(s, "sn", 2)) ||
           (n == 8 && !memcmp(s, "serialno", 8)) ||
           (n > 3 && !memcmp(s + n - 3, ".sn", 3)) ||
           (n > 9 && !memcmp(s + n - 9, ".serialno", 9));
}
/* 1 appended, 0 same existing SN or no source, -1 ambiguous/invalid/capacity.
 * Failure leaves cmdline unchanged; do not synthesize identity if no source. */
static int hmos_bridge_boot_serial(char *cmdline, size_t cap, const char *config)
{
    char serial[96] = {0};
    size_t serial_len = 0;
    const char *p = config;
    while (*p) {
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        const char *v = p;
        while (v < end && (*v == ' ' || *v == '\t')) ++v;
        static const char key[] = "androidboot.serialno";
        if ((size_t)(end-v) >= sizeof(key)-1 && !memcmp(v,key,sizeof(key)-1)) {
            v += sizeof(key)-1;
            if (v < end && *v != ' ' && *v != '\t' && *v != '=') goto next;
            while (v < end && (*v == ' ' || *v == '\t')) ++v;
            if (v == end || *v++ != '=') return -1;
            while (v < end && (*v == ' ' || *v == '\t')) ++v;
            const char *last = end;
            while (last > v && (last[-1] == ' ' || last[-1] == '\t' || last[-1] == '\r')) --last;
            if (v < last && *v == '"') {
                if (last-v < 2 || last[-1] != '"') return -1;
                ++v; --last;
            }
            size_t n = (size_t)(last-v);
            if (!hmos_serial_valid(v,n)) return -1;
            if (serial_len && (n != serial_len || memcmp(serial,v,n))) return -1;
            memcpy(serial,v,n); serial[n]=0; serial_len=n;
        }
next:
        p = *end ? end+1 : end;
    }
    if (!serial_len) return 0;
    size_t len = 0;
    while (len < cap && cmdline[len]) ++len;
    if (len == cap) return -1;
    p = cmdline;
    int existing = 0;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        const char *end = p;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') ++end;
        const char *eq = memchr(p,'=',(size_t)(end-p));
        if (eq && hmos_sn_key(p,(size_t)(eq-p))) {
            const char *v = eq+1;
            size_t n = (size_t)(end-v);
            if (!hmos_serial_valid(v,n) || n != serial_len || memcmp(v,serial,n)) return -1;
            existing = 1;
        }
        p = end;
    }
    if (existing) return 0;
    static const char prefix[] = " ohos.boot.sn=";
    if (len + sizeof(prefix)-1 + serial_len + 1 > cap) return -1;
    memcpy(cmdline+len,prefix,sizeof(prefix)-1);
    memcpy(cmdline+len+sizeof(prefix)-1,serial,serial_len+1);
    return 1;
}
#endif
