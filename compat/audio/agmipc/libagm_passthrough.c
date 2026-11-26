/* SPDX-License-Identifier: MIT
 *
 * AGM in-process passthrough for marble / HarmonyOS NEXT.
 *
 * Why this layer exists (evidence: work/marble-agmipc-report.md):
 *   libagmclient.so is the *marshaller* half of Qualcomm's split AGM build.
 *   Its agm_init()/agm_deinit() are literally `return 0` and every other entry
 *   point short-circuits to -EINVAL while the cached IAGM HIDL proxy is
 *   unusable.  The real implementation lives in libagm.so and is normally
 *   hosted by vendor.qti.hardware.AGMIPC@1.0-service over /dev/hwbinder, which
 *   this device does not have.
 *
 * What this file does:
 *   It exports the same 48-symbol AGM C API and forwards each call to the
 *   matching function in libagm.so, resolved at load time with dlopen/dlsym.
 *   The generated trampolines in agmipc_shim_syms.S are register-transparent,
 *   so no AGM prototype is re-declared and no struct ABI is guessed.
 *
 * Degradation contract:
 *   Never abort.  If libagm.so cannot be loaded or a symbol is missing, print
 *   AGMIPC_SHIM_INIT_FAIL with a reason and point the affected slot at
 *   agmipc_fail_stub (returns -ENODEV) or at the original marshaller
 *   (RTLD_NEXT), so PAL keeps running instead of dying at startup.
 */
typedef unsigned long size_t;
typedef long ssize_t;

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern char *dlerror(void);
extern char *getenv(const char *);
extern int snprintf(char *, size_t, const char *, ...);
extern ssize_t write(int, const void *, size_t);

#include "agmipc_shim_syms.h"
#include "agmipc_shim_names.h"

#define AGMIPC_RTLD_NOW 2
#define AGMIPC_RTLD_LOCAL 0
#define AGMIPC_RTLD_NEXT ((void *)-1L)

/* Implementation library that owns the real GSL/DSP/ACDB code. */
#define AGMIPC_DEFAULT_IMPL "libagm.so"

#define AGMIPC_FAIL_ENODEV (-19)

/* Defined in agmipc_fail_stub.S. */
extern void agmipc_fail_stub(void);

/* Resolved targets.  .data (not .bss) so the array is never a null pointer
 * even if a trampoline somehow runs before the constructor. */
__attribute__((visibility("hidden")))
void *g_agmipc_slots[AGMIPC_SYM_COUNT] = {
#define AGMIPC_SLOT_DEFAULT(name, index) [index] = (void *)&agmipc_fail_stub,
    AGMIPC_SYM_LIST(AGMIPC_SLOT_DEFAULT)
#undef AGMIPC_SLOT_DEFAULT
};

static const char *g_agmipc_impl_name = AGMIPC_DEFAULT_IMPL;

typedef struct {
    int direct;
    int fallback;
    int missing;
    const char *first_missing;
} agmipc_resolve_counts_t;

static agmipc_resolve_counts_t g_agmipc_counts;

static void shim_write(int fd, const char *text)
{
    size_t len = 0;
    while (text[len] != '\0')
        ++len;
    if (len != 0)
        (void)write(fd, text, len);
}

static void shim_line(const char *tag, const char *reason,
                      const agmipc_resolve_counts_t *counts)
{
    char buf[224];
    int n;

    if (reason != 0)
        n = snprintf(buf, sizeof(buf), "%s reason=%s impl=%s direct=%d fallback=%d missing=%d\n",
                     tag, reason, g_agmipc_impl_name, counts->direct, counts->fallback,
                     counts->missing);
    else
        n = snprintf(buf, sizeof(buf), "%s impl=%s direct=%d fallback=%d missing=%d\n",
                     tag, g_agmipc_impl_name, counts->direct, counts->fallback,
                     counts->missing);
    if (n > 0) {
        if (n > (int)sizeof(buf))
            n = (int)sizeof(buf);
        (void)write(2, buf, (size_t)n);
    }
}

/* Called by agmipc_fail_stub when a slot never resolved. */
__attribute__((visibility("hidden")))
void agmipc_report_unavailable(void)
{
    static int reported;
    if (reported == 0) {
        reported = 1;
        shim_write(2, "AGMIPC_SHIM_CALL_UNRESOLVED rc=-19 (no direct libagm symbol, "
                      "no original marshaller)\n");
    }
}

static void agmipc_resolve_all(void *impl, agmipc_resolve_counts_t *counts)
{
#define AGMIPC_RESOLVE(name, index)                                                    \
    do {                                                                               \
        void *direct = (impl != 0) ? dlsym(impl, #name) : 0;                          \
        if (direct != 0) {                                                             \
            g_agmipc_slots[index] = direct;                                            \
            ++counts->direct;                                                          \
        } else {                                                                       \
            void *original = dlsym(AGMIPC_RTLD_NEXT, #name);                          \
            if (original != 0) {                                                       \
                g_agmipc_slots[index] = original;                                      \
                ++counts->fallback;                                                    \
            } else {                                                                   \
                g_agmipc_slots[index] = (void *)&agmipc_fail_stub;                      \
                ++counts->missing;                                                     \
                if (counts->first_missing == 0)                                        \
                    counts->first_missing = #name;                                    \
            }                                                                          \
        }                                                                              \
    } while (0);
    AGMIPC_SYM_LIST(AGMIPC_RESOLVE);
#undef AGMIPC_RESOLVE
}

__attribute__((constructor))
static void agmipc_shim_init(void)
{
    const char *override = getenv("HMOS_AGMIPC_IMPL_LIB");
    agmipc_resolve_counts_t counts = {0, 0, 0, 0};
    const char *err;
    void *impl;

    if (override != 0 && override[0] != '\0')
        g_agmipc_impl_name = override;

    impl = dlopen(g_agmipc_impl_name, AGMIPC_RTLD_NOW | AGMIPC_RTLD_LOCAL);
    if (impl == 0) {
        err = dlerror();
        if (err == 0)
            err = "dlopen_failed";
        /* Leave the original marshaller in charge so PAL follows the IPC path
         * it was built for instead of the shim.  No abort. */
        agmipc_resolve_all(0, &counts);
        g_agmipc_counts = counts;
        shim_line("AGMIPC_SHIM_INIT_FAIL", err, &counts);
        return;
    }

    agmipc_resolve_all(impl, &counts);
    g_agmipc_counts = counts;
    if (counts.missing == 0) {
        shim_line("AGMIPC_SHIM_INIT_OK", 0, &counts);
        return;
    }
    /* Partial resolution: unresolved entries return -ENODEV, everything else
     * is live.  PAL keeps running. */
    shim_line("AGMIPC_SHIM_INIT_FAIL", counts.first_missing, &counts);
}

/* Exposed for on-device diagnosis: reports the real resolution mix. */
__attribute__((visibility("default")))
int agmipc_shim_selfcheck(void)
{
    char buf[160];
    int n;

    n = snprintf(buf, sizeof(buf),
                 "AGMIPC_SHIM_SELFCHECK impl=%s direct=%d fallback=%d missing=%d\n",
                 g_agmipc_impl_name, g_agmipc_counts.direct, g_agmipc_counts.fallback,
                 g_agmipc_counts.missing);
    if (n > 0) {
        if (n > (int)sizeof(buf))
            n = (int)sizeof(buf);
        (void)write(1, buf, (size_t)n);
    }
    return (g_agmipc_counts.direct == AGMIPC_SYM_COUNT) ? 0 : AGMIPC_FAIL_ENODEV;
}

/* Keep the AGM symbol names reachable for report / nm diffs. */
__attribute__((visibility("default")))
const char *const *agmipc_shim_symbol_names(void)
{
    return g_agmipc_names;
}

__attribute__((visibility("default")))
int agmipc_shim_symbol_count(void)
{
    return AGMIPC_SYM_COUNT;
}