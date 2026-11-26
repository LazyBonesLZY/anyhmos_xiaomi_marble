/* SPDX-License-Identifier: MIT
 * Interposition tracer for the AGM/GSL initialisation chain.
 *
 * libagm's gsl_init() calls four sub-initialisers and returns the first
 * non-zero code; libagm then maps it through a table (index 9 -> -114), which
 * hides *which* step failed.  Preloading this library prints every return
 * value on stderr so the failing step is visible instead of guessed:
 *
 *   AGMTRACE gpr_init=0
 *   AGMTRACE acdb_init=9        <- this one
 *   AGMTRACE gsl_sg_pool_init=0
 *   AGMTRACE gsl_global_persist_cal_pool_init=0
 *   AGMTRACE gsl_init=-... (only if gsl_init itself is interposable)
 *
 * Every wrapper calls the next implementation through dlsym(RTLD_NEXT), so the
 * real code still runs; a missing symbol prints -999 and refuses to guess.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <stdarg.h>

/* The audio libraries are dlopen()ed into a local scope, so RTLD_NEXT cannot
 * always see them; fall back to the already-loaded module itself. */
static void *lookup_loaded(const char *lib, const char *name)
{
    void *h = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD);
    if (h == NULL) return NULL;
    void *p = dlsym(h, name);
    dlclose(h);
    return p;
}

static void *next_sym(const char *name)
{
    void *p = dlsym(RTLD_NEXT, name);
    if (p == NULL) p = lookup_loaded("libar-gpr.so", name);
    if (p == NULL) p = lookup_loaded("libar-acdb.so", name);
    if (p == NULL) p = lookup_loaded("libar-gsl.so", name);
    if (p == NULL) p = lookup_loaded("libar-pal.so", name);
    if (p == NULL) {
        char msg[128];
        int n = snprintf(msg, sizeof(msg), "AGMTRACE missing symbol %s\n", name);
        if (n > 0) (void)!write(2, msg, (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg));
    }
    return p;
}

int gpr_init(void)
{
    static int (*real)(void);
    if (real == NULL) real = (int (*)(void))next_sym("gpr_init");
    if (real == NULL) { dprintf(2, "AGMTRACE gpr_init=-999\n"); return 0; }
    int rc = real();
    dprintf(2, "AGMTRACE gpr_init=%d\n", rc);
    return rc;
}

int acdb_init(void *a, void *b)
{
    static int (*real)(void *, void *);
    if (real == NULL) real = (int (*)(void *, void *))next_sym("acdb_init");
    if (real == NULL) { dprintf(2, "AGMTRACE acdb_init=-999\n"); return 0; }
    int rc = real(a, b);
    dprintf(2, "AGMTRACE acdb_init=%d\n", rc);
    return rc;
}

int gsl_sg_pool_init(void)
{
    static int (*real)(void);
    if (real == NULL) real = (int (*)(void))next_sym("gsl_sg_pool_init");
    if (real == NULL) { dprintf(2, "AGMTRACE gsl_sg_pool_init=-999\n"); return 0; }
    int rc = real();
    dprintf(2, "AGMTRACE gsl_sg_pool_init=%d\n", rc);
    return rc;
}

int gsl_global_persist_cal_pool_init(void)
{
    static int (*real)(void);
    if (real == NULL) real = (int (*)(void))next_sym("gsl_global_persist_cal_pool_init");
    if (real == NULL) { dprintf(2, "AGMTRACE gsl_global_persist_cal_pool_init=-999\n"); return 0; }
    int rc = real();
    dprintf(2, "AGMTRACE gsl_global_persist_cal_pool_init=%d\n", rc);
    return rc;
}

/* Later checkpoints inside gsl_init() (source lines 1084 / 1124 / 1182).  They
 * are exported by libar-gsl, so the same wrapper pattern applies.  Arguments
 * are forwarded verbatim in x0..x3; unused registers are ignored by the callee.
 */
#define TRACE4(sym) \
    int sym(void *a, void *b, void *c, void *d) \
    { \
        static int (*real)(void *, void *, void *, void *); \
        if (real == NULL) real = (int (*)(void *, void *, void *, void *))next_sym(#sym); \
        if (real == NULL) { dprintf(2, "AGMTRACE " #sym "=-999\n"); return 0; } \
        int rc = real(a, b, c, d); \
        dprintf(2, "AGMTRACE " #sym "=%d\n", rc); \
        return rc; \
    }

TRACE4(gsl_mdf_utils_init)
TRACE4(gsl_spf_ss_state_init)
TRACE4(gsl_rtc_init)

/* Remaining exported checkpoints after gsl_spf_ss_state_init(). */
TRACE4(gsl_shmem_init)
TRACE4(gsl_dynamic_module_mgr_init)
TRACE4(gsl_do_load_bootup_dyn_modules)
TRACE4(gsl_mdf_utils_shmem_alloc)
TRACE4(gsl_rtc_internal_init)

/* ar_log() is imported by libar-gsl, so wrapping it exposes every checkpoint
 * message regardless of the module's own log level (which is not queryable).
 * The real implementation is intentionally not forwarded: we only need text.
 */
int ar_log(int level, const char *module, const char *func, int line, const char *fmt, ...)
{
    (void)level;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    va_end(ap);
    if (n < 0) n = 0;
    dprintf(2, "AGMLOG %s:%d %s.%s\n", func ? func : "?", line,
            module ? module : "?", buf);
    return 0;
}

/* libar-gsl asks ACDB for the bootup dynamic-module list through acdb_ioctl.
 * Dumping the reply for that command shows which module IDs the DSP is asked to
 * register, which is the only way to compare them with dsp_a/adsp/*.so.1.
 */
int acdb_ioctl(unsigned int cmd, void *in, unsigned long inlen, void *out, unsigned long outlen)
{
    static int (*real)(unsigned int, void *, unsigned long, void *, unsigned long);
    if (real == NULL) real = (int (*)(unsigned int, void *, unsigned long, void *, unsigned long))next_sym("acdb_ioctl");
    if (real == NULL) { dprintf(2, "AGMTRACE acdb_ioctl=-999\n"); return 0; }
    int rc = real(cmd, in, inlen, out, outlen);
    if (cmd == 0xACE000D2u || cmd == 2900033554u) {
        unsigned char *b = (unsigned char *)out;
        dprintf(2, "AGMACDB ioctl=0x%x rc=%d outlen=%lu first=%02x%02x%02x%02x\n", cmd, rc, outlen,
                b ? b[0] : 0, b ? b[1] : 0, b ? b[2] : 0, b ? b[3] : 0);
    }
    return rc;
}
