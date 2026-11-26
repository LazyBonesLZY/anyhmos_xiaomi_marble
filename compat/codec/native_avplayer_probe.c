/* SPDX-License-Identifier: MIT */
/* Local fd AVPlayer timeline probe: use an external `timeout -k 2 30`.
 * The monotonic deadlines bound observations, not a blocked SDK call/ReleaseSync.
 * A pass is control/timeline evidence, not evidence of audible or decoded PCM. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <multimedia/player_framework/avplayer.h>

enum {
    FIXTURE_MS = 6000, DURATION_TOL_MS = 100,
    FORWARD_MS = 4200, BACKWARD_MS = 1200, SEEK_TOL_MS = 150,
    ACTION_MS = 3000, SAMPLE_MS = 50, SAMPLE_SLACK_MS = 250,
    PLAY_WINDOW_MS = 800, MIN_ADVANCE_MS = 250, MIN_STEP_MS = 80, START_SLACK_MS = 400,
    PAUSE_WINDOW_MS = 500, PAUSE_DRIFT_MS = 80,
    EOS_EARLY_MS = 750, EOS_END_TOL_MS = 250,
};
_Static_assert(BACKWARD_MS > SEEK_TOL_MS && FORWARD_MS - BACKWARD_MS > 2 * SEEK_TOL_MS,
               "seek windows must be positive and disjoint");

typedef struct {
    uint64_t serial, state_serial;
    unsigned seeks, eos;
    AVPlayerState state;
    int32_t seek_ms;
    int64_t eos_at;
    const char *error;
    int32_t error_code;
} Events;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    Events events;
} Observer;

/* One player per process. Keep userData/lock valid through release, even if
 * release reports failure and there is no demonstrated callback barrier. */
static Observer observer = {.lock = PTHREAD_MUTEX_INITIALIZER, .events.state = AV_IDLE};

static bool fail(const char *stage, const char *detail, int code)
{
    fprintf(stderr, "AVPLAYER_FAIL stage=%s detail=%s code=%d\n", stage, detail, code);
    return false;
}

static int64_t now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) {
        perror("CLOCK_MONOTONIC");
        _Exit(1);
    }
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int init_observer(void)
{
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc != 0) return rc;
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc == 0) rc = pthread_cond_init(&observer.changed, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
}

static void record_error(Events *e, const char *reason, int32_t code)
{
    if (e->error == NULL) {
        e->error = reason;
        e->error_code = code;
    }
}

static void on_info(OH_AVPlayer *player, AVPlayerOnInfoType type, OH_AVFormat *body, void *user)
{
    (void)player;
    if (type != AV_INFO_TYPE_STATE_CHANGE && type != AV_INFO_TYPE_SEEKDONE && type != AV_INFO_TYPE_EOS) return;

    /* The SDK owns body; copy only the documented scalar while in callback. */
    int32_t value = 0;
    bool valid = type == AV_INFO_TYPE_EOS || (body != NULL && OH_AVFormat_GetIntValue(body,
        type == AV_INFO_TYPE_STATE_CHANGE ? OH_PLAYER_STATE : OH_PLAYER_SEEK_POSITION, &value));
    int64_t eos_at = type == AV_INFO_TYPE_EOS ? now_ms() : 0;
    Observer *o = user;
    pthread_mutex_lock(&o->lock);
    Events *e = &o->events;
    ++e->serial;
    if (!valid) {
        record_error(e, "callback_payload", type);
    } else if (type == AV_INFO_TYPE_STATE_CHANGE) {
        if (value < AV_IDLE || value > AV_RELEASED) {
            record_error(e, "callback_state", value);
        } else {
            e->state = (AVPlayerState)value;
            e->state_serial = e->serial;
        }
    } else if (type == AV_INFO_TYPE_SEEKDONE) {
        ++e->seeks;
        e->seek_ms = value;
    } else if (e->eos++ == 0) {
        e->eos_at = eos_at;
    }
    pthread_cond_signal(&o->changed);
    pthread_mutex_unlock(&o->lock);
}

static void on_error(OH_AVPlayer *player, int32_t code, const char *message, void *user)
{
    (void)player;
    (void)message;
    Observer *o = user;
    pthread_mutex_lock(&o->lock);
    record_error(&o->events, "callback_error", code);
    ++o->events.serial;
    pthread_cond_signal(&o->changed);
    pthread_mutex_unlock(&o->lock);
}

static Events snapshot(void)
{
    pthread_mutex_lock(&observer.lock);
    Events result = observer.events;
    pthread_mutex_unlock(&observer.lock);
    return result;
}

static bool wait_change(const char *stage, uint64_t serial, int64_t until)
{
    struct timespec deadline = {(time_t)(until / 1000), (long)(until % 1000) * 1000000};
    pthread_mutex_lock(&observer.lock);
    int rc = 0;
    if (serial == observer.events.serial) {
        rc = pthread_cond_timedwait(&observer.changed, &observer.lock, &deadline);
    }
    pthread_mutex_unlock(&observer.lock);
    return rc == 0 || rc == ETIMEDOUT || fail(stage, "cond_wait", rc);
}

static bool healthy(const char *stage, Events e)
{
    return e.error == NULL || fail(stage, e.error, e.error_code);
}

static bool before_eos(const char *stage, Events e, unsigned seeks)
{
    return healthy(stage, e) && (e.eos == 0 || fail(stage, "unexpected_eos", (int)e.eos)) &&
        (e.seeks == seeks || fail(stage, "unexpected_seekdone", (int)e.seeks));
}

static bool sdk_ok(const char *stage, OH_AVErrCode rc)
{
    return rc == AV_ERR_OK || fail(stage, "sdk", rc);
}

static bool near_ms(int32_t actual, int32_t expected, int tol)
{
    return llabs((int64_t)actual - expected) <= tol;
}

static bool get_position(OH_AVPlayer *p, const char *stage, int32_t max_ms, int32_t *ms)
{
    *ms = -1;
    return sdk_ok(stage, OH_AVPlayer_GetCurrentTime(p, ms)) &&
        ((*ms >= 0 && *ms <= max_ms) || fail(stage, "position_range", *ms));
}

static bool prior_state(AVPlayerState actual, AVPlayerState from)
{
    /* SetFDSource may still be publishing INITIALIZED when we submit Prepare. */
    return actual == from || (from == AV_IDLE && actual == AV_INITIALIZED);
}

static bool transition(OH_AVPlayer *p, const char *stage, OH_AVErrCode (*call)(OH_AVPlayer *),
                       AVPlayerState from, AVPlayerState to, unsigned seeks)
{
    printf("AVPLAYER_STEP stage=%s\n", stage);
    Events before = snapshot();
    if (!before_eos(stage, before, seeks)) return false;
    if (!prior_state(before.state, from)) return fail(stage, "prior_state", before.state);
    int64_t deadline = now_ms() + ACTION_MS;
    if (!sdk_ok(stage, call(p))) return false;
    for (;;) {
        Events e = snapshot();
        if (!before_eos(stage, e, seeks)) return false;
        if (now_ms() >= deadline) return fail(stage, "state_timeout", e.state);
        if (e.state == to && e.state_serial > before.serial) return true;
        if (!prior_state(e.state, from)) return fail(stage, "unexpected_state", e.state);
        if (!wait_change(stage, e.serial, deadline)) return false;
    }
}

typedef struct {
    int32_t start_ms, end_ms;
    int64_t start_at;
} Window;

static bool observe(OH_AVPlayer *p, const char *stage, int32_t duration, int32_t origin,
                    AVPlayerState state, unsigned seeks, Window *window)
{
    bool playing = state == AV_PLAYING;
    if (!get_position(p, stage, duration, &window->start_ms)) return false;
    const int32_t base = window->start_ms;
    if (base < origin - PAUSE_DRIFT_MS || base > origin + START_SLACK_MS) {
        return fail(stage, "window_origin", base);
    }
    /* Open a fresh wall-time and media-time window after each playing callback. */
    const int64_t start = window->start_at = now_ms();
    const int64_t end = start + (playing ? PLAY_WINDOW_MS : PAUSE_WINDOW_MS);
    int32_t peak = base, progress_mark = base;
    unsigned advances = 0;
    for (;;) {
        Events e = snapshot();
        if (!before_eos(stage, e, seeks)) return false;
        if (e.state != state) return fail(stage, "unexpected_state", e.state);
        int32_t ms;
        if (!get_position(p, stage, duration, &ms)) return false;
        int64_t at = now_ms();
        if (at > end + SAMPLE_SLACK_MS) return fail(stage, "sample_timeout", 0);
        if (playing) {
            if (ms < peak - PAUSE_DRIFT_MS || ms - base > at - start + START_SLACK_MS) {
                return fail(stage, "play_jump", ms);
            }
            if (ms > peak) peak = ms;
            /* One isolated jump is not sustained progress. */
            if (ms - progress_mark >= MIN_STEP_MS) {
                progress_mark = ms;
                ++advances;
            }
        } else if (!near_ms(ms, base, PAUSE_DRIFT_MS)) {
            return fail(stage, "pause_drift", ms - base);
        }
        if (at >= end) {
            if (playing && (ms - base < MIN_ADVANCE_MS || advances < 2)) {
                return fail(stage, "no_advance", ms - base);
            }
            window->end_ms = ms;
            printf("AVPLAYER_WINDOW stage=%s from_ms=%d to_ms=%d elapsed_ms=%" PRId64 "\n",
                stage, base, ms, at - start);
            return true;
        }
        if (!wait_change(stage, e.serial, at + SAMPLE_MS < end ? at + SAMPLE_MS : end)) return false;
    }
}

static bool seek_paused(OH_AVPlayer *p, const char *stage, int32_t duration, int32_t target,
                        unsigned prior_seeks, int32_t *landed)
{
    int32_t before;
    if (!get_position(p, stage, duration, &before)) return false;
    if ((prior_seeks == 0 && before >= target - SEEK_TOL_MS) ||
        (prior_seeks != 0 && before <= target + SEEK_TOL_MS)) return fail(stage, "seek_direction", before);

    printf("AVPLAYER_STEP stage=%s target_ms=%d\n", stage, target);
    Events mark = snapshot();
    if (!before_eos(stage, mark, prior_seeks)) return false;
    if (mark.state != AV_PAUSED) return fail(stage, "prior_state", mark.state);
    int64_t deadline = now_ms() + ACTION_MS;
    if (!sdk_ok(stage, OH_AVPlayer_Seek(p, target, AV_SEEK_CLOSEST))) return false;

    /* The SDK has no request ID. Serialize seeks, require exactly one new
     * SEEKDONE, match its payload to this (disjoint) target, and check the
     * actual paused position separately; rc or a position alone is not enough. */
    *landed = -1;
    for (;;) {
        Events e = snapshot();
        if (!healthy(stage, e)) return false;
        if (e.state != AV_PAUSED || e.eos != 0) return fail(stage, "seek_state", e.state);
        if (e.seeks > prior_seeks + 1) return fail(stage, "unexpected_seekdone", (int)e.seeks);
        int64_t at = now_ms();
        if (at >= deadline) return fail(stage, e.seeks == prior_seeks ? "seekdone_timeout" : "seek_position", *landed);
        if (e.seeks == prior_seeks + 1) {
            if (!near_ms(e.seek_ms, target, SEEK_TOL_MS)) return fail(stage, "seekdone_position", e.seek_ms);
            if (!get_position(p, stage, duration, landed)) return false;
            at = now_ms();
            if (at < deadline && near_ms(*landed, target, SEEK_TOL_MS)) {
                printf("AVPLAYER_SEEK stage=%s from_ms=%d target_ms=%d event_ms=%d actual_ms=%d tolerance_ms=%d\n",
                    stage, before, target, e.seek_ms, *landed, SEEK_TOL_MS);
                return true;
            }
        }
        int64_t next = e.seeks == prior_seeks ? deadline : at + SAMPLE_MS;
        if (!wait_change(stage, e.serial, next < deadline ? next : deadline)) return false;
    }
}

static bool await_eos(OH_AVPlayer *p, int32_t duration, Window resume)
{
    const char *stage = "eos";
    const int32_t remaining = duration - resume.start_ms;
    const int64_t deadline = resume.start_at + remaining + ACTION_MS;
    printf("AVPLAYER_STEP stage=%s\n", stage);
    for (;;) {
        Events e = snapshot();
        if (!healthy(stage, e)) return false;
        if (e.seeks != 2) return fail(stage, "unexpected_seekdone", (int)e.seeks);
        if (e.state != AV_PLAYING && e.state != AV_COMPLETED) return fail(stage, "unexpected_state", e.state);
        if (e.eos > 1) return fail(stage, "duplicate_eos", (int)e.eos);
        if (e.eos == 1 && e.eos_at < resume.start_at + remaining - EOS_EARLY_MS) {
            return fail(stage, "early_eos", (int)(e.eos_at - resume.start_at));
        }
        if (now_ms() >= deadline) return fail(stage, "eos_timeout", e.state);
        if (e.eos == 1 && e.state == AV_COMPLETED) {
            int32_t end_ms;
            if (!get_position(p, stage, duration + EOS_END_TOL_MS, &end_ms)) return false;
            if (now_ms() >= deadline) return fail(stage, "eos_timeout", end_ms);
            if (!near_ms(end_ms, duration, EOS_END_TOL_MS)) return fail(stage, "eos_position", end_ms);
            printf("AVPLAYER_EOS callback=1 completed=1 position_ms=%d elapsed_ms=%" PRId64 "\n",
                end_ms, e.eos_at - resume.start_at);
            return true;
        }
        if (!wait_change(stage, e.serial, deadline)) return false;
    }
}

static bool probe(OH_AVPlayer *p, int fd, int64_t size)
{
    if (!sdk_ok("on_error", OH_AVPlayer_SetOnErrorCallback(p, on_error, &observer)) ||
        !sdk_ok("on_info", OH_AVPlayer_SetOnInfoCallback(p, on_info, &observer)) ||
        !sdk_ok("fd_source", OH_AVPlayer_SetFDSource(p, fd, 0, size)) ||
        !transition(p, "prepare", OH_AVPlayer_Prepare, AV_IDLE, AV_PREPARED, 0)) return false;

    int32_t duration = -1;
    if (!sdk_ok("duration", OH_AVPlayer_GetDuration(p, &duration))) return false;
    if (!near_ms(duration, FIXTURE_MS, DURATION_TOL_MS) || FORWARD_MS + SEEK_TOL_MS >= duration) {
        return fail("duration", "outside_fixture_window", duration);
    }
    printf("AVPLAYER_DURATION actual_ms=%d expected_ms=%d tolerance_ms=%d\n",
        duration, FIXTURE_MS, DURATION_TOL_MS);

    Window initial, window;
    int32_t landed;
    return transition(p, "play", OH_AVPlayer_Play, AV_PREPARED, AV_PLAYING, 0) &&
        observe(p, "play_window", duration, 0, AV_PLAYING, 0, &initial) &&
        transition(p, "pause", OH_AVPlayer_Pause, AV_PLAYING, AV_PAUSED, 0) &&
        observe(p, "pause_window", duration, initial.end_ms, AV_PAUSED, 0, &window) &&
        seek_paused(p, "seek_forward", duration, FORWARD_MS, 0, &landed) &&
        seek_paused(p, "seek_backward", duration, BACKWARD_MS, 1, &landed) &&
        transition(p, "resume", OH_AVPlayer_Play, AV_PAUSED, AV_PLAYING, 2) &&
        observe(p, "resume_window", duration, landed, AV_PLAYING, 2, &window) &&
        await_eos(p, duration, window);
}

static void usage(FILE *stream, const char *name)
{
    fprintf(stream, "Usage: timeout -k 2 30 %s /path/to/tones.wav|tones.m4a\n"
        "Use one local 6 s audio-only file (duration %d +/- %d ms). Paused seeks: %d then %d ms (+/- %d ms).\n",
        name, FIXTURE_MS, DURATION_TOL_MS, FORWARD_MS, BACKWARD_MS, SEEK_TOL_MS);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout, argv[0]);
        return 0;
    }
    const char *ext = argc == 2 ? strrchr(argv[1], '.') : NULL;
    if (ext == NULL || (strcasecmp(ext, ".wav") != 0 && strcasecmp(ext, ".m4a") != 0)) {
        usage(stderr, argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        fail("input", "open", errno);
        return 2;
    }
    struct stat st;
    int rc = fstat(fd, &st);
    if (rc != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        fail("input", "nonempty_regular_file_required", rc == 0 ? 0 : errno);
        close(fd);
        return 2;
    }
    rc = init_observer();
    if (rc != 0) {
        fail("observer", "monotonic_cond_init", rc);
        close(fd);
        return 1;
    }
    OH_AVPlayer *player = OH_AVPlayer_Create();
    if (player == NULL) {
        fail("create", "null_player", 0);
        close(fd);
        return 1;
    }

    bool ok = probe(player, fd, st.st_size);
    printf("AVPLAYER_STEP stage=release\n");
    /* Do not close the original fd until the SDK has released its player.
     * On release failure leave it live until the immediate process exit. */
    if (!sdk_ok("release", OH_AVPlayer_ReleaseSync(player))) return 1;
    if (close(fd) != 0) {
        fail("input", "close", errno);
        return 1;
    }
    Events final = snapshot();
    if (!ok || !healthy("release", final)) return 1;
    if (final.seeks != 2 || final.eos != 1) {
        fail("release", "extra_completion", 0);
        return 1;
    }
    puts("AVPLAYER_PROBE_PASS evidence=timeline_only");
    return 0;
}
