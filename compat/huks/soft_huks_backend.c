/*
 * Backend probing, routing and logging for the marble software HUKS backend.
 *
 * Three tiers:
 *   qsee -> Qualcomm TEE (libQSEEComAPI_system.so + GPTEE + /dev/qseecom)
 *   soft -> this OpenSSL backend with an on-disk sealed key store
 *   stub -> explicit "this port does not provide it" (attestation chains, ...)
 *
 * The qsee tier is a deliberately fail-safe skeleton: on marble today
 * /dev/qseecom does not exist, so the probe reports `backend=soft` and every
 * request is served by the OpenSSL backend.  No code path assumes a TA exists.
 */

#include "soft_huks.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define QSEE_DEV "/dev/qseecom"
#define QSEE_API_LIB "libQSEEComAPI_system.so"
#define GPTEE_LIB "libGPTEE_system.qti.so"
#define SOFKEYMASTER_LIB "libpuresoftkeymasterdevice.so"

/* ------------------------------------------------------------------ */
/* Logging: stderr always, libhilog when the process has it.           */
/* ------------------------------------------------------------------ */

typedef int (*OhLogPrintFn)(int type, int level, unsigned int domain, const char *tag, const char *fmt, ...);

static pthread_once_t gLogOnce = PTHREAD_ONCE_INIT;
static OhLogPrintFn gOhLogPrint = NULL;

static void soft_log_init(void)
{
    void *handle = dlopen("libhilog.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        handle = dlopen("libhilog_ndk.z.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (handle != NULL) {
        *(void **)(&gOhLogPrint) = dlsym(handle, "OH_LOG_Print");
    }
}

void soft_huks_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_once(&gLogOnce, soft_log_init);
    if (gOhLogPrint != NULL) {
        /* type=1 (app), level=4 (info), domain 0x0, tag SoftHuks */
        (void)gOhLogPrint(1, 4, 0x0, "SoftHuks", "%{public}s", buf);
    }
    (void)fprintf(stderr, "[SoftHuks] %s\n", buf);
    (void)fflush(stderr);
}

const char *soft_backend_name(enum SoftBackend backend)
{
    switch (backend) {
        case SOFT_BACKEND_QSEE:
            return "qsee";
        case SOFT_BACKEND_STUB:
            return "stub";
        case SOFT_BACKEND_SOFT:
        default:
            return "soft";
    }
}

/* ------------------------------------------------------------------ */
/* Qualcomm TEE probe (never crashes, never assumes a TA)              */
/* ------------------------------------------------------------------ */

struct QseeProbeState {
    int probed;
    enum SoftBackend backend;
    char detail[192];
};

static struct QseeProbeState gQsee = { 0, SOFT_BACKEND_SOFT, "not_probed" };
static pthread_mutex_t gQseeMutex = PTHREAD_MUTEX_INITIALIZER;

static void qsee_probe_locked(struct QseeProbeState *st)
{
    char openState[32];
    int fd;

    fd = open(QSEE_DEV, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
        (void)close(fd);
        (void)snprintf(openState, sizeof(openState), "ok");
    } else {
        (void)snprintf(openState, sizeof(openState), "%s", strerror(errno));
    }

    void *api = dlopen(QSEE_API_LIB, RTLD_NOW | RTLD_LOCAL);
    void *gptee = dlopen(GPTEE_LIB, RTLD_NOW | RTLD_LOCAL);
    int haveStartApp = 0;
    if (api != NULL) {
        haveStartApp = dlsym(api, "QSEECom_start_app") != NULL;
    }

    /*
     * The TA is only considered usable when the kernel channel exists AND the
     * userspace QSEECom API loaded AND it exposes the start-app entry point.
     * Today marble fails at the first condition, so we stay on `soft`.
     */
    int devOk = (fd >= 0);
    int haveSof = (access("/system/lib64/" SOFKEYMASTER_LIB, R_OK) == 0);
    if (devOk && api != NULL && haveStartApp) {
        st->backend = SOFT_BACKEND_QSEE;
        (void)snprintf(st->detail, sizeof(st->detail),
            "qseecom=" QSEE_DEV " open=ok gptee=%s api=%s backend=qsee",
            (gptee != NULL) ? "ok" : "missing", QSEE_API_LIB);
    } else {
        st->backend = SOFT_BACKEND_SOFT;
        (void)snprintf(st->detail, sizeof(st->detail),
            "qseecom=" QSEE_DEV " open=%s gptee=%s api=%s start_app=%s soffkeymaster=%s backend=soft",
            openState, (gptee != NULL) ? "ok" : "missing",
            (api != NULL) ? "ok" : "missing", haveStartApp ? "ok" : "missing",
            haveSof ? "ok" : "missing");
    }

    if (api != NULL) {
        (void)dlclose(api);
    }
    if (gptee != NULL) {
        (void)dlclose(gptee);
    }
    st->probed = 1;
}

enum SoftBackend soft_probe_qsee(char *detail, size_t detailLen)
{
    enum SoftBackend result;

    (void)pthread_mutex_lock(&gQseeMutex);
    if (!gQsee.probed) {
        qsee_probe_locked(&gQsee);
    }
    result = gQsee.backend;
    if (detail != NULL && detailLen > 0) {
        (void)snprintf(detail, detailLen, "%s", gQsee.detail);
    }
    (void)pthread_mutex_unlock(&gQseeMutex);
    return result;
}

/* ------------------------------------------------------------------ */
/* Routing table (hard-coded; every decision is logged)                */
/* ------------------------------------------------------------------ */

static int is_huawei_only_alg(uint32_t alg)
{
    switch (alg) {
        case HKS_ALG_HKDF:
        case HKS_ALG_PBKDF2:
        case HKS_ALG_SM2:
        case HKS_ALG_SM4:
            return 1;
        default:
            return 0;
    }
}

static int env_forced_backend(enum SoftBackend *out)
{
    const char *v = getenv("HUKS_FORCE_BACKEND");
    if (v == NULL || v[0] == '\0') {
        return 0;
    }
    if (strcmp(v, "soft") == 0) {
        *out = SOFT_BACKEND_SOFT;
        return 1;
    }
    if (strcmp(v, "qsee") == 0) {
        *out = SOFT_BACKEND_QSEE;
        return 1;
    }
    if (strcmp(v, "stub") == 0) {
        *out = SOFT_BACKEND_STUB;
        return 1;
    }
    return 0;
}

enum SoftBackend soft_route(const char *op, uint32_t alg, uint32_t origin, const char **reason)
{
    static pthread_mutex_t routeMutex = PTHREAD_MUTEX_INITIALIZER;
    enum SoftBackend forced;
    enum SoftBackend backend;
    const char *why;
    char detail[192];

    (void)pthread_mutex_lock(&routeMutex);

    if (env_forced_backend(&forced)) {
        backend = forced;
        why = "forced_by_env";
    } else if (strcmp(op, "attest") == 0) {
        /* Real attestation needs a hardware cert chain; never fake it. */
        backend = SOFT_BACKEND_STUB;
        why = "no_attestation_chain";
    } else if (soft_probe_qsee(detail, sizeof(detail)) != SOFT_BACKEND_QSEE) {
        backend = SOFT_BACKEND_SOFT;
        why = "qsee_unavailable";
    } else if (origin == SOFT_KEY_ORIGIN_HUAWEI_EXT) {
        backend = SOFT_BACKEND_SOFT;
        why = "huawei_proprietary_origin";
    } else if (is_huawei_only_alg(alg)) {
        backend = SOFT_BACKEND_SOFT;
        why = "alg_not_in_qsee_ta";
    } else if (strcmp(op, "import_wrapped") == 0) {
        backend = SOFT_BACKEND_SOFT;
        why = "huawei_wrap_format";
    } else {
        backend = SOFT_BACKEND_QSEE;
        why = "qsee_ta_ready";
    }

    (void)pthread_mutex_unlock(&routeMutex);

    if (reason != NULL) {
        *reason = why;
    }
    soft_huks_log("HUKS_BACKEND=%s op=%s alg=%u origin=%u reason=%s",
        soft_backend_name(backend), op, alg, origin, why);
    return backend;
}

/* ------------------------------------------------------------------ */
/* qsee tier skeleton: every call fails closed with a clear reason.     */
/* ------------------------------------------------------------------ */

int32_t soft_qsee_execute(const char *op)
{
    /*
     * Wire-up plan once the kernel side registers /dev/qseecom and a keymaster
     * TA exists:
     *   1. QSEECom_start_app(&handle, "keymaster", 4096)
     *   2. send the serialized HKS paramSet/key blob to the TA
     *   3. map the TA status to HKS_* codes
     * Until then: fail closed, never touch /dev/qseecom.
     */
    soft_huks_log("HUKS_BACKEND=qsee op=%s result=not_wired reason=ta_not_implemented", op);
    return HKS_ERROR_NOT_SUPPORTED;
}

int32_t soft_stub_execute(const char *op)
{
    soft_huks_log("HUKS_BACKEND=stub op=%s result=not_supported reason=port_does_not_provide_it", op);
    return HKS_ERROR_NOT_SUPPORTED;
}
