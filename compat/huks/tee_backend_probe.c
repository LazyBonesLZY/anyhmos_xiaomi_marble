/*
 * tee_backend_probe: one-line TEE backend self-check for marble.
 *
 * Build (Bionic, device):
 *   compat/huks/build_soft_huks.sh --probe-only
 *   adb push / serial: /compat/bin/tee_backend_probe
 *
 * Output contract (always one line, always exit 0):
 *   TEE_PROBE qseecom=/dev/qseecom open=ok|ENOENT gptee=ok|missing \
 *             soffkeymaster=ok|missing backend=soft
 *
 * The three tiers the router can pick (hard-coded in soft_huks_backend.c):
 *   HUKS_BACKEND=qsee   Qualcomm TEE via /dev/qseecom + GPTEE TA
 *   HUKS_BACKEND=soft   OpenSSL backend + sealed key store (current default)
 *   HUKS_BACKEND=stub   explicit "this port does not provide it"
 *
 * With -v it also prints the routing table and the library paths it looked at.
 * No writes, no ioctl, no TA call: the probe must be safe to run on a device
 * whose kernel qseecom channel is half-initialised.
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define QSEE_DEV "/dev/qseecom"
#define QSEE_API_LIB "libQSEEComAPI_system.so"
#define GPTEE_LIB "libGPTEE_system.qti.so"
#define SOFKEYMASTER_LIB "libpuresoftkeymasterdevice.so"

static int file_present(const char *path)
{
    return access(path, R_OK) == 0;
}

static int lib_loadable(const char *name)
{
    void *h = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        return 0;
    }
    (void)dlclose(h);
    return 1;
}

int main(int argc, char **argv)
{
    int verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
    int fd;
    const char *openState;
    int haveDev;
    int haveGptee;
    int haveSof;
    int haveQseeApi;
    const char *backend;

    fd = open(QSEE_DEV, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        (void)close(fd);
        openState = "ok";
        haveDev = 1;
    } else {
        openState = (errno == ENOENT) ? "ENOENT" : strerror(errno);
        haveDev = 0;
    }

    haveQseeApi = lib_loadable(QSEE_API_LIB);
    haveGptee = lib_loadable(GPTEE_LIB);
    haveSof = file_present("/system/lib64/" SOFKEYMASTER_LIB) || lib_loadable(SOFKEYMASTER_LIB);

    /*
     * Same precedence as soft_route(): the hardware tier only wins when the
     * kernel channel and the userspace API are both there.  Otherwise the
     * OpenSSL tier serves the request, and attestation stays on `stub`.
     */
    if (haveDev && haveQseeApi) {
        backend = "qsee";
    } else {
        backend = "soft";
    }

    (void)printf("TEE_PROBE qseecom=%s open=%s gptee=%s soffkeymaster=%s backend=%s\n",
        QSEE_DEV, openState, haveGptee ? "ok" : "missing", haveSof ? "ok" : "missing", backend);

    if (verbose) {
        (void)printf("TEE_PROBE_DETAIL qsee_api=%s:%s\n", QSEE_API_LIB, haveQseeApi ? "ok" : "missing");
        (void)printf("TEE_PROBE_ROUTE op=attest -> HUKS_BACKEND=stub reason=no_attestation_chain\n");
        (void)printf("TEE_PROBE_ROUTE op=* alg=HKDF|PBKDF2|SM2|SM4 -> HUKS_BACKEND=soft reason=alg_not_in_qsee_ta\n");
        (void)printf("TEE_PROBE_ROUTE op=import_wrapped -> HUKS_BACKEND=soft reason=huawei_wrap_format\n");
        (void)printf("TEE_PROBE_ROUTE op=* -> HUKS_BACKEND=%s reason=%s\n", backend,
            (strcmp(backend, "qsee") == 0) ? "qsee_ta_ready" : "qsee_unavailable");
    }
    return 0;
}
