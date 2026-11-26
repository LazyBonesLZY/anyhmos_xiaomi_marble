/* SPDX-License-Identifier: MIT
 *
 * libaudioroute.so compatibility shim for Qualcomm's libaudioroute_ext.so.
 *
 * Why this file exists
 * --------------------
 * The vendor audio stack on marble (SM7475 / ukee) splits audio_route into two
 * libraries:
 *
 *   libaudioroute_ext.so  - Qualcomm only  : audio_route_init_ext,
 *                                            audio_route_free_ext,
 *                                            audio_route_update_overlay_dynamic,
 *                                            dump_audio_route
 *   libaudioroute.so      - stock AOSP API : audio_route_init / free /
 *                                            update_mixer /
 *                                            apply_and_update_path /
 *                                            reset_and_update_path
 *
 * libar-pal.so calls audio_route_init_ext(), then hands the returned pointer to
 * audio_route_apply_and_update_path() / _reset_and_update_path() / _free(),
 * which must come from libaudioroute.so.  We cannot ship Qualcomm's original
 * libaudioroute.so, so we compile AOSP audio_route.c and must survive being
 * called with a pointer that was allocated by Qualcomm's _ext library.
 *
 * We deliberately do not rely on that pointer being layout compatible with
 * AOSP's "struct audio_route" (it is a 0x28-byte AOSP shaped allocation in the
 * A15 dump, but that is an implementation detail of one vendor build and QTI is
 * free to extend it).  Instead:
 *
 *   - every exported entry point treats the caller pointer as opaque and never
 *     dereferences it;
 *   - the real AOSP state is a private singleton created on first use from
 *     HMOS_AUDIO_CARD (default "0") and HMOS_AUDIO_MIXER_XML
 *     (default "/vendor/etc/audio/sku_ukee/mixer_paths_ukee_mtp.xml");
 *   - failures are reported as -ENOENT (or -EINVAL for a NULL path name) and a
 *     single "AUDIOROUTE_SHIM_INIT_FAIL" line on stderr.  Nothing in this file
 *     aborts the PAL process.
 *
 * Consequences (documented, not hidden): the overlay state that _ext keeps in
 * its own bookkeeping (audio_route_update_overlay_dynamic / dump_audio_route)
 * is not applied through this shim, and mixer updates issued by _ext on its own
 * struct are not forwarded.  Both structs re-read the same mixer_paths XML, so
 * the routes this shim applies are identical for the marble bring-up.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* audio_route.c is compiled in the same command line with
 *   -Daudio_route_init=hmos_ar_init -Daudio_route_free=hmos_ar_free ...
 * so that AOSP's implementation owns a private hmos_ar_* namespace.  Those same
 * -D macros would also rename the exported wrappers below, therefore this file
 * drops them again; keep this list in sync with tools/build/build_current.py
 * (target "audioroute") and with exports.map. */
#undef audio_route_init
#undef audio_route_free
#undef audio_route_update_mixer
#undef audio_route_apply_and_update_path
#undef audio_route_reset_and_update_path

#include <audio_route/audio_route.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Present for auditing: which target this object was built for. */
__attribute__((used, visibility("hidden")))
static const char hmos_ar_build_target[] =
    "hmos-audioroute-shim target=aarch64-linux-android ndk=r27d";

#define HMOS_AR_DEFAULT_CARD "0"
#define HMOS_AR_DEFAULT_XML "/vendor/etc/audio/sku_ukee/mixer_paths_ukee_mtp.xml"

/* AOSP audio_route.c compiled into this DSO with these symbols renamed. */
extern struct audio_route *hmos_ar_init(unsigned int card, const char *xml_path);
extern void hmos_ar_free(struct audio_route *ar);
extern int hmos_ar_update_mixer(struct audio_route *ar);
extern int hmos_ar_apply_and_update_path(struct audio_route *ar, const char *name);
extern int hmos_ar_reset_and_update_path(struct audio_route *ar, const char *name);

static pthread_mutex_t hmos_ar_lock = PTHREAD_MUTEX_INITIALIZER;
static struct audio_route *volatile hmos_ar_singleton;
static int hmos_ar_failure_logged;

static int hmos_ar_card(void)
{
    const char *raw = getenv("HMOS_AUDIO_CARD");
    char *end = NULL;
    unsigned long value;

    if (raw == NULL || *raw == '\0') return 0;
    value = strtoul(raw, &end, 0);
    if (end == raw || *end != '\0') return 0;
    return (int)value;
}

static const char *hmos_ar_xml(void)
{
    const char *raw = getenv("HMOS_AUDIO_MIXER_XML");
    return (raw != NULL && *raw != '\0') ? raw : HMOS_AR_DEFAULT_XML;
}

static void hmos_ar_log_line(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;
    size_t len;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    (void)!write(STDERR_FILENO, buf, len);
}

/* Slow path only; the pointer is created once and then never recreated until
 * audio_route_free() clears it.  Returns NULL when the card cannot be opened or
 * the mixer XML cannot be parsed. */
static struct audio_route *hmos_ar_acquire(void)
{
    struct audio_route *ar = hmos_ar_singleton;

    if (ar != NULL) return ar;
    pthread_mutex_lock(&hmos_ar_lock);
    if (hmos_ar_singleton == NULL) {
        int card = hmos_ar_card();
        const char *xml = hmos_ar_xml();
        hmos_ar_singleton = hmos_ar_init((unsigned int)card, xml);
        if (hmos_ar_singleton == NULL && hmos_ar_failure_logged == 0) {
            hmos_ar_failure_logged = 1;
            hmos_ar_log_line("AUDIOROUTE_SHIM_INIT_FAIL card=%d xml=%s\n", card, xml);
        }
    }
    ar = hmos_ar_singleton;
    pthread_mutex_unlock(&hmos_ar_lock);
    return ar;
}

/* The next five symbols are the only ones libar-pal.so and
 * libaudioroute_ext.so import from libaudioroute.so.  Keep this list in sync
 * with exports.map and tests/test_marble_audio_runtime.py. */

__attribute__((visibility("default")))
struct audio_route *audio_route_init(unsigned int card, const char *xml_path)
{
    /* _ext calls this only when it could not open the XML itself; the arguments
     * are therefore not used - the singleton is configured from the environment
     * so that both the _ext fallback and the plain PAL call path agree. */
    (void)card;
    (void)xml_path;
    return hmos_ar_acquire();
}

__attribute__((visibility("default")))
void audio_route_free(struct audio_route *ar)
{
    struct audio_route *mine;

    (void)ar;
    pthread_mutex_lock(&hmos_ar_lock);
    mine = hmos_ar_singleton;
    hmos_ar_singleton = NULL;
    pthread_mutex_unlock(&hmos_ar_lock);
    if (mine != NULL) hmos_ar_free(mine);
}

__attribute__((visibility("default")))
int audio_route_update_mixer(struct audio_route *ar)
{
    struct audio_route *mine = hmos_ar_acquire();

    (void)ar;
    if (mine == NULL) return -ENOENT;
    return hmos_ar_update_mixer(mine);
}

__attribute__((visibility("default")))
int audio_route_apply_and_update_path(struct audio_route *ar, const char *name)
{
    struct audio_route *mine = hmos_ar_acquire();

    (void)ar;
    if (mine == NULL) return -ENOENT;
    if (name == NULL) return -EINVAL;
    return hmos_ar_apply_and_update_path(mine, name);
}

__attribute__((visibility("default")))
int audio_route_reset_and_update_path(struct audio_route *ar, const char *name)
{
    struct audio_route *mine = hmos_ar_acquire();

    (void)ar;
    if (mine == NULL) return -ENOENT;
    if (name == NULL) return -EINVAL;
    return hmos_ar_reset_and_update_path(mine, name);
}
