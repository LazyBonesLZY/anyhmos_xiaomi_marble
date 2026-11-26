/* SPDX-License-Identifier: MIT
 * One-shot Bionic AGM test. No PCM open/read/write, PAL server, or IPC shim.
 * Run only when no other user-space AGM owner is active; agm_init/deinit can
 * change shared DSP state even in a private mount namespace.
 */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>

/* Capture the vendor AGM log without running the PAL playback server. */
int __android_log_print(int priority, const char *tag, const char *format, ...)
{
    char message[512];
    va_list args;
    va_start(args, format);
    int rc = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (rc >= 0)
        fprintf(stderr, "AGM_LOG %d %s: %s\n", priority, tag ? tag : "?", message);
    return rc;
}

int main(void)
{
    void *agm = dlopen("libagm.so", RTLD_NOW | RTLD_LOCAL);
    if (!agm) {
        fprintf(stderr, "AGM_PROBE DLOPEN %s\n", dlerror());
        return 2;
    }
    int (*init)(void) = dlsym(agm, "agm_init");
    int (*deinit)(void) = dlsym(agm, "agm_deinit");
    if (!init || !deinit) {
        fprintf(stderr, "AGM_PROBE symbol missing\n");
        return 2;
    }
    int rc = init();
    fprintf(stderr, "AGM_PROBE INIT rc=%d\n", rc);
    if (rc)
        return 3;
    rc = deinit();
    fprintf(stderr, "AGM_PROBE DEINIT rc=%d\n", rc);
    return rc ? 4 : 0;
}
