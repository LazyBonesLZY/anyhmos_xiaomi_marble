/* SPDX-License-Identifier: MIT
 * Isolated original303 Bionic/PAL PCM + Voice Call service. No vendor files are changed.
 * The diagnostic entry runs after Bionic constructors, outside loader locks.
 */
#include <sys/types.h>
#include <PalApi.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/time.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <sched.h>
#include "protocol.h"

#define MAX_CLIENTS 8
/* Unity hardware gain for daytime playback; HarmonyOS audio_server applies software
 * volume scaling (audio_volume_config.xml). Touching HMOS_AUDIO_QUIET_FLAG restores
 * the 0.03f (-30.46 dB) quiet bench/nighttime attenuation floor. */
#define HMOS_AUDIO_DEFAULT_VOL 1.0f
#define HMOS_AUDIO_QUIET_VOL   0.03f
#define HMOS_AUDIO_QUIET_FLAG  "/data/local/tmp/marble-audio-quiet"
#define HMOS_VOICE_SLOT_FILE   "/data/local/tmp/marble-voice-slot"

extern int *__errno(void);
static int (*stream_open)(struct pal_stream_attributes *, unsigned, struct pal_device *,
    unsigned, struct modifier_kv *, pal_stream_callback, unsigned long, pal_stream_handle_t **);
static int (*stream_close)(pal_stream_handle_t *);
static int (*stream_start)(pal_stream_handle_t *);
static int (*stream_stop)(pal_stream_handle_t *);
static int (*stream_pause)(pal_stream_handle_t *);
static int (*stream_resume)(pal_stream_handle_t *);
static int (*stream_write)(pal_stream_handle_t *, struct pal_buffer *);
static int (*stream_volume)(pal_stream_handle_t *, struct pal_volume_data *);
static int (*stream_buffer)(pal_stream_handle_t *, pal_buffer_config_t *, pal_buffer_config_t *);
static int (*stream_set_device)(pal_stream_handle_t *, uint32_t, struct pal_device *);
static int (*stream_set_param)(pal_stream_handle_t *, uint32_t, pal_param_payload *);
static ssize_t (*stream_read)(pal_stream_handle_t *, struct pal_buffer *);
static int (*stream_set_mute)(pal_stream_handle_t *, bool);
static int (*stream_get_mute)(pal_stream_handle_t *, bool *);
static int (*mic_set_mute)(bool);
static int (*mic_get_mute)(bool *);
static int (*get_param)(uint32_t,void **,size_t *,void *);
static int (*set_param)(uint32_t,void *,size_t);
static int (*register_global)(pal_global_callback,uint64_t);
static volatile sig_atomic_t stopping;

static float initial_playback_volume(void)
{
    return access(HMOS_AUDIO_QUIET_FLAG, F_OK) == 0 ? HMOS_AUDIO_QUIET_VOL : HMOS_AUDIO_DEFAULT_VOL;
}

int __android_log_print(int priority, const char *tag, const char *fmt, ...)
{
    static unsigned count;
    if (__atomic_fetch_add(&count,1,__ATOMIC_RELAXED)>=1500) return 0;
    char b[768];
    int n=snprintf(b,sizeof(b),"PAL_LOG_%d_%s: ",priority,tag?tag:"?");
    if (n<0 || n>=(int)sizeof(b)) return 0;
    va_list ap; va_start(ap,fmt);
    int m=vsnprintf(b+n,sizeof(b)-(size_t)n,fmt,ap);
    va_end(ap);
    if (m<0) return 0;
    size_t used=(size_t)n+(size_t)m;
    if (used>sizeof(b)-2) used=sizeof(b)-2;
    b[used++]='\n';
    (void)write(1,b,used);
    return m;
}

struct client {
    int fd;
    pal_stream_handle_t *stream;
    int state; /* 0 closed, 1 open, 2 running, 3 paused */
    uint32_t out_device;
    float volume;
    uint64_t frames;
    uint64_t nonzero_samples;
    unsigned peak;
    int capture;
    unsigned capture_channels;
    uint64_t capture_epoch;
};

struct voice_session {
    pal_stream_handle_t *stream;
    int state; /* 0 closed, 1 open, 2 running */
    int owner_fd;
    uint32_t rx_dev;
    uint32_t tx_dev;
    uint32_t vsid;
    float volume;
    int muted;
};

static struct voice_session voice = {
    .stream = 0,
    .state = 0,
    .owner_fd = -1,
    .rx_dev = PAL_DEVICE_OUT_HANDSET,
    .tx_dev = PAL_DEVICE_IN_HANDSET_MIC,
    .vsid = VOICEMMODE2,
    .volume = 0.8f,
    .muted = 0,
};

static void end_signal(int sig) { (void)sig; __atomic_store_n(&stopping,1,__ATOMIC_RELEASE); }

static int valid_out_device(uint32_t dev)
{
    return dev==PAL_DEVICE_OUT_SPEAKER || dev==PAL_DEVICE_OUT_HANDSET ||
        dev==PAL_DEVICE_OUT_BLUETOOTH_SCO || dev==PAL_DEVICE_OUT_WIRED_HEADSET ||
        dev==PAL_DEVICE_OUT_WIRED_HEADPHONE || dev==PAL_DEVICE_OUT_USB_HEADSET;
}

static int valid_in_device(uint32_t dev)
{
    return dev==PAL_DEVICE_IN_HANDSET_MIC || dev==PAL_DEVICE_IN_SPEAKER_MIC ||
        dev==PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET || dev==PAL_DEVICE_IN_WIRED_HEADSET ||
        dev==PAL_DEVICE_IN_USB_HEADSET;
}

static uint32_t default_tx_for_rx(uint32_t rx_dev)
{
    switch (rx_dev) {
    case PAL_DEVICE_OUT_SPEAKER:          return PAL_DEVICE_IN_SPEAKER_MIC;
    case PAL_DEVICE_OUT_BLUETOOTH_SCO:    return PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
    case PAL_DEVICE_OUT_WIRED_HEADSET:    return PAL_DEVICE_IN_WIRED_HEADSET;
    case PAL_DEVICE_OUT_USB_HEADSET:      return PAL_DEVICE_IN_USB_HEADSET;
    case PAL_DEVICE_OUT_HANDSET:
    case PAL_DEVICE_OUT_WIRED_HEADPHONE:
    default:                              return PAL_DEVICE_IN_HANDSET_MIC;
    }
}

static uint32_t resolve_vsid(uint32_t arg)
{
    if (arg==VOICEMMODE1 || arg==1u) return VOICEMMODE1;
    if (arg==VOICEMMODE2 || arg==2u) return VOICEMMODE2;
    if (arg==VOICELBMMODE1 || arg==VOICELBMMODE2) return arg;
    FILE *f=fopen(HMOS_VOICE_SLOT_FILE,"r");
    if (f) {
        char buf[32]={0};
        if (fgets(buf,sizeof(buf),f)) {
            fclose(f);
            if (buf[0]=='0') return VOICEMMODE1;
            if (buf[0]=='1') return VOICEMMODE2;
        } else {
            fclose(f);
        }
    }
    /* Active China Telecom SIM on marble is Slot 1 -> VOICEMMODE2. */
    return VOICEMMODE2;
}

/* Verified against SM7475 marble libar-pal.so (StreamPCM::StreamPCM 0x19ad1c &
 * Stream::switchDevice 0x1921e8): vendor sizeof(pal_device) == 220 bytes
 * (id + config + address + sndDevName[128], without trailing custom_config[256]). */
struct pal_device_v220 {
    pal_device_id_t id;
    struct pal_media_config config;
    struct pal_usb_device_address address;
    char sndDevName[DEVICE_NAME_MAX_SIZE];
};
_Static_assert(sizeof(struct pal_device_v220) == 220, "SM7475 vendor pal_device ABI");
_Static_assert(sizeof(struct pal_stream_attributes) == 224, "SM7475 vendor pal_stream_attributes ABI");

static void fill_out_device(struct pal_device_v220 *d, uint32_t dev_id, unsigned rate)
{
    memset(d,0,sizeof(*d));
    d->id=(pal_device_id_t)dev_id;
    d->config.bit_width=16;
    d->config.aud_fmt_id=PAL_AUDIO_FMT_PCM_S16_LE;
    if (dev_id==PAL_DEVICE_OUT_BLUETOOTH_SCO) {
        d->config.sample_rate=8000;
        d->config.ch_info.channels=1;
        d->config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    } else if (dev_id==PAL_DEVICE_OUT_HANDSET) {
        d->config.sample_rate=rate;
        d->config.ch_info.channels=1;
        d->config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    } else {
        d->config.sample_rate=rate;
        d->config.ch_info.channels=2;
        d->config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
        d->config.ch_info.ch_map[1]=PAL_CHMAP_CHANNEL_FR;
    }
}

static void fill_in_device(struct pal_device_v220 *d, uint32_t dev_id, unsigned rate)
{
    memset(d,0,sizeof(*d));
    d->id=(pal_device_id_t)dev_id;
    d->config.bit_width=16;
    d->config.aud_fmt_id=PAL_AUDIO_FMT_PCM_S16_LE;
    if (dev_id==PAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET) {
        d->config.sample_rate=8000;
        d->config.ch_info.channels=1;
        d->config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    } else {
        d->config.sample_rate=rate;
        d->config.ch_info.channels=1;
        d->config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    }
}

static int apply_playback_volume(pal_stream_handle_t *s, uint32_t dev_id, float vol)
{
    uint32_t mask=(dev_id==PAL_DEVICE_OUT_HANDSET || dev_id==PAL_DEVICE_OUT_BLUETOOTH_SCO) ? 1u : 3u;
    struct { unsigned count; struct pal_channel_vol_kv pair; } v={1,{mask,vol}};
    int rc=stream_volume(s,(struct pal_volume_data *)&v);
    if (rc && mask!=1u) {
        v.pair.channel_mask=1u;
        rc=stream_volume(s,(struct pal_volume_data *)&v);
    }
    return rc;
}

static int close_stream(struct client *c)
{
    if (!c->stream) return 0;
    int rc=0;
    if (c->state>=2) rc=stream_stop(c->stream);
    int crc=stream_close(c->stream);
    printf("PAL_SERVER_PCM_SUMMARY frames=%llu nonzero_samples=%llu peak_s16=%u stop_rc=%d close_rc=%d\n",
        (unsigned long long)c->frames,(unsigned long long)c->nonzero_samples,c->peak,rc,crc);
    c->stream=0; c->state=0;
    return rc ? rc : crc;
}

static int open_stream(struct client *c, unsigned rate, unsigned arg1)
{
    if (c->stream) return -16;
    unsigned channels=arg1 & 0xFFFFu;
    unsigned dev_id=(arg1 >> 16) & 0xFFFFu;
    if (!dev_id) dev_id=c->out_device ? c->out_device : PAL_DEVICE_OUT_SPEAKER;
    if (rate!=48000 || channels!=2 || !valid_out_device(dev_id)) return -95;
    struct { struct pal_stream_attributes value; unsigned char tail[128]; } a={0};
    struct { struct pal_device_v220 value; unsigned char tail[256]; } d={0};
    a.value.type=PAL_STREAM_LOW_LATENCY;
    a.value.direction=PAL_AUDIO_OUTPUT;
    a.value.out_media_config.sample_rate=rate;
    a.value.out_media_config.bit_width=16;
    a.value.out_media_config.aud_fmt_id=PAL_AUDIO_FMT_PCM_S16_LE;
    a.value.out_media_config.ch_info.channels=2;
    a.value.out_media_config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    a.value.out_media_config.ch_info.ch_map[1]=PAL_CHMAP_CHANNEL_FR;
    fill_out_device(&d.value,dev_id,rate);
    int rc=stream_open(&a.value,1,(struct pal_device *)&d.value,0,0,0,0,&c->stream);
    if (rc) return rc;
    if (!c->stream) return -5;
    c->state=1; c->out_device=dev_id; c->frames=0; c->nonzero_samples=0; c->peak=0;
    c->volume=initial_playback_volume();
    pal_buffer_config_t out={.buf_count=2,.buf_size=960};
    rc=stream_buffer(c->stream,0,&out);
    if (!rc) rc=apply_playback_volume(c->stream,dev_id,c->volume);
    if (rc) (void)close_stream(c);
    else printf("PAL_SERVER_OPEN dev=%u vol=%.4f\n",dev_id,c->volume);
    return rc;
}

static int set_playback_device(struct client *c, uint32_t dev_id)
{
    if (!valid_out_device(dev_id)) return -95;
    if (!c->stream) { c->out_device=dev_id; return 0; }
    if (c->out_device==dev_id) return 0;
    int rc=-95;
    if (stream_set_device) {
        struct { struct pal_device_v220 value; unsigned char tail[256]; } d={0};
        fill_out_device(&d.value,dev_id,48000);
        rc=stream_set_device(c->stream,1,(struct pal_device *)&d.value);
    }
    if (!rc) {
        c->out_device=dev_id;
        (void)apply_playback_volume(c->stream,dev_id,c->volume);
        printf("PAL_SERVER_SET_DEVICE dev=%u mode=switch rc=0\n",dev_id);
        return 0;
    }
    /* Fallback: cleanly reopen stream on new output device preserving state & volume. */
    int was_running=(c->state==2);
    float saved_vol=c->volume;
    (void)close_stream(c);
    rc=open_stream(c,48000,2u | (dev_id << 16));
    if (!rc) {
        c->volume=saved_vol;
        (void)apply_playback_volume(c->stream,dev_id,saved_vol);
        if (was_running) {
            rc=stream_start(c->stream);
            if (!rc) c->state=2;
        }
    }
    printf("PAL_SERVER_SET_DEVICE dev=%u mode=reopen rc=%d\n",dev_id,rc);
    return rc;
}

/* ==================== Batch F11: PAL_STREAM_VOICE_CALL ==================== */
static uint64_t voice_status_word(void)
{
    return ((uint64_t)(voice.state & 0xFFu)) |
           ((uint64_t)(voice.rx_dev & 0xFFu) << 8) |
           ((uint64_t)(voice.tx_dev & 0xFFu) << 16) |
           ((uint64_t)(voice.muted ? 1u : 0u) << 24) |
           ((uint64_t)voice.vsid << 32);
}

static int voice_apply_volume(float vol)
{
    if (!voice.stream || voice.state < 2) return 0;
    uint32_t mask=(voice.rx_dev==PAL_DEVICE_OUT_SPEAKER) ? 3u : 1u;
    struct { unsigned count; struct pal_channel_vol_kv pair; } v={1,{mask,vol}};
    int rc=stream_volume(voice.stream,(struct pal_volume_data *)&v);
    if (rc && mask!=1u) {
        v.pair.channel_mask=1u;
        rc=stream_volume(voice.stream,(struct pal_volume_data *)&v);
    }
    printf("PAL_SERVER_VOICE_VOL vol=%.4f mask=%u rc=%d\n",vol,v.pair.channel_mask,rc);
    return rc;
}

static int voice_close(void)
{
    if (!voice.stream) {
        voice.state=0; voice.owner_fd=-1;
        return 0;
    }
    int stop_rc=0;
    if (voice.state>=2) stop_rc=stream_stop(voice.stream);
    int close_rc=stream_close(voice.stream);
    printf("PAL_SERVER_VOICE_CLOSE rx=%u tx=%u vsid=0x%x stop_rc=%d close_rc=%d\n",
        voice.rx_dev,voice.tx_dev,voice.vsid,stop_rc,close_rc);
    voice.stream=0; voice.state=0; voice.owner_fd=-1;
    return stop_rc ? stop_rc : close_rc;
}

static int voice_open_internal(int owner_fd, uint32_t rx_dev, uint32_t tx_dev, uint32_t vsid)
{
    if (!valid_out_device(rx_dev)) return -95;
    if (!tx_dev) tx_dev=default_tx_for_rx(rx_dev);
    if (!valid_in_device(tx_dev)) return -95;
    unsigned rate=(rx_dev==PAL_DEVICE_OUT_BLUETOOTH_SCO) ? 8000u : 48000u;
    struct { struct pal_stream_attributes value; unsigned char tail[128]; } a={0};
    struct { struct pal_device_v220 devs[2]; unsigned char tail[256]; } d={0};
    a.value.type=PAL_STREAM_VOICE_CALL;
    a.value.direction=PAL_AUDIO_INPUT_OUTPUT;
    a.value.info.voice_call_info.VSID=vsid;
    a.value.info.voice_call_info.tty_mode=PAL_TTY_OFF;
    a.value.in_media_config.sample_rate=rate;
    a.value.in_media_config.bit_width=16;
    a.value.in_media_config.aud_fmt_id=PAL_AUDIO_FMT_PCM_S16_LE;
    a.value.in_media_config.ch_info.channels=1;
    a.value.in_media_config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    a.value.out_media_config.sample_rate=rate;
    a.value.out_media_config.bit_width=16;
    a.value.out_media_config.aud_fmt_id=PAL_AUDIO_FMT_PCM_S16_LE;
    if (rx_dev==PAL_DEVICE_OUT_SPEAKER || rx_dev==PAL_DEVICE_OUT_WIRED_HEADSET ||
        rx_dev==PAL_DEVICE_OUT_WIRED_HEADPHONE || rx_dev==PAL_DEVICE_OUT_USB_HEADSET) {
        a.value.out_media_config.ch_info.channels=2;
        a.value.out_media_config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
        a.value.out_media_config.ch_info.ch_map[1]=PAL_CHMAP_CHANNEL_FR;
    } else {
        a.value.out_media_config.ch_info.channels=1;
        a.value.out_media_config.ch_info.ch_map[0]=PAL_CHMAP_CHANNEL_FL;
    }
    fill_out_device(&d.devs[0],rx_dev,rate);
    fill_in_device(&d.devs[1],tx_dev,rate);
    int rc=stream_open(&a.value,2,(struct pal_device *)d.devs,0,0,0,0,&voice.stream);
    if (rc || !voice.stream) {
        printf("PAL_SERVER_VOICE_OPEN rx=%u tx=%u vsid=0x%x rc=%d\n",rx_dev,tx_dev,vsid,rc?rc:-5);
        voice.stream=0; voice.state=0;
        return rc ? rc : -5;
    }
    voice.state=1; voice.owner_fd=owner_fd;
    voice.rx_dev=rx_dev; voice.tx_dev=tx_dev; voice.vsid=vsid;
    printf("PAL_SERVER_VOICE_OPEN rx=%u tx=%u vsid=0x%x rc=0\n",rx_dev,tx_dev,vsid);
    return 0;
}

static int voice_set_device(uint32_t rx_dev, uint32_t tx_dev)
{
    if (!valid_out_device(rx_dev)) return -95;
    if (!tx_dev) tx_dev=default_tx_for_rx(rx_dev);
    if (!valid_in_device(tx_dev)) return -95;
    if (!voice.stream) {
        voice.rx_dev=rx_dev; voice.tx_dev=tx_dev;
        return 0;
    }
    if (voice.rx_dev==rx_dev && voice.tx_dev==tx_dev) return 0;
    int rc=-95;
    if (stream_set_device) {
        unsigned rate=(rx_dev==PAL_DEVICE_OUT_BLUETOOTH_SCO) ? 8000u : 48000u;
        struct { struct pal_device_v220 devs[2]; unsigned char tail[256]; } d={0};
        fill_out_device(&d.devs[0],rx_dev,rate);
        fill_in_device(&d.devs[1],tx_dev,rate);
        rc=stream_set_device(voice.stream,2,(struct pal_device *)d.devs);
    }
    if (!rc) {
        voice.rx_dev=rx_dev; voice.tx_dev=tx_dev;
        (void)voice_apply_volume(voice.muted ? 0.0f : voice.volume);
        printf("PAL_SERVER_VOICE_DEVICE rx=%u tx=%u mode=switch rc=0\n",rx_dev,tx_dev);
        return 0;
    }
    /* Fallback: reopen voice call graph on the requested RX+TX pair. */
    int was_running=(voice.state>=2);
    int owner=voice.owner_fd;
    uint32_t vsid=voice.vsid;
    (void)voice_close();
    rc=voice_open_internal(owner,rx_dev,tx_dev,vsid);
    if (!rc && was_running) {
        rc=stream_start(voice.stream);
        if (!rc) {
            voice.state=2;
            (void)voice_apply_volume(voice.muted ? 0.0f : voice.volume);
        }
    }
    printf("PAL_SERVER_VOICE_DEVICE rx=%u tx=%u mode=reopen rc=%d\n",rx_dev,tx_dev,rc);
    return rc;
}

static int voice_open(int owner_fd, uint32_t rx_arg, uint32_t vsid_arg)
{
    uint32_t rx_dev=rx_arg ? rx_arg : PAL_DEVICE_OUT_HANDSET;
    uint32_t vsid=resolve_vsid(vsid_arg);
    if (!valid_out_device(rx_dev)) return -95;
    if (voice.stream) {
        if (voice.vsid==vsid) {
            if (voice.owner_fd<0) voice.owner_fd=owner_fd;
            return voice_set_device(rx_dev,0);
        }
        (void)voice_close();
    }
    if (access(HMOS_AUDIO_QUIET_FLAG, F_OK) == 0 && voice.volume > HMOS_AUDIO_QUIET_VOL) {
        voice.volume=HMOS_AUDIO_QUIET_VOL;
    }
    return voice_open_internal(owner_fd,rx_dev,0,vsid);
}

static int voice_start(void)
{
    if (!voice.stream) return -19;
    if (voice.state==2) return 0;
    int rc=stream_start(voice.stream);
    if (!rc) {
        voice.state=2;
        (void)voice_apply_volume(voice.muted ? 0.0f : voice.volume);
        if (voice.muted && stream_set_mute) (void)stream_set_mute(voice.stream,true);
    }
    printf("PAL_SERVER_VOICE_START rx=%u tx=%u vsid=0x%x rc=%d\n",
        voice.rx_dev,voice.tx_dev,voice.vsid,rc);
    return rc;
}

static int voice_stop(void)
{
    if (!voice.stream) return 0;
    if (voice.state<2) return 0;
    int rc=stream_stop(voice.stream);
    if (!rc) voice.state=1;
    printf("PAL_SERVER_VOICE_STOP rc=%d\n",rc);
    return rc;
}

#include "pal_capture_backend.inc"
#include "pal_capture_worker.inc"

static int dispatch(struct client *c, const struct hmos_pal_request *h,
                    unsigned char *data, uint32_t *written, uint64_t *value)
{
    *value=c->frames;
    if (h->op==HMOS_PAL_OPEN) return open_stream(c,h->arg0,h->arg1);
    if (h->op==HMOS_PAL_CLOSE) return close_stream(c);
    if (h->op==HMOS_PAL_STATUS) return 0;
    if (h->op==HMOS_PAL_SET_DEVICE) return set_playback_device(c,h->arg0);
    /* Voice call operations */
    if (h->op==HMOS_PAL_VOICE_STATUS) {
        *value=voice_status_word();
        return stream_open ? 0 : -95;
    }
    if (h->op==HMOS_PAL_VOICE_OPEN) {
        int rc=voice_open(c->fd,h->arg0,h->arg1);
        *value=voice_status_word();
        return rc;
    }
    if (h->op==HMOS_PAL_VOICE_START) {
        int rc=voice_start();
        *value=voice_status_word();
        return rc;
    }
    if (h->op==HMOS_PAL_VOICE_STOP) {
        int rc=voice_stop();
        *value=voice_status_word();
        return rc;
    }
    if (h->op==HMOS_PAL_VOICE_CLOSE) {
        int rc=voice_close();
        *value=voice_status_word();
        return rc;
    }
    if (h->op==HMOS_PAL_VOICE_SET_DEVICE) {
        int rc=voice_set_device(h->arg0,h->arg1);
        *value=voice_status_word();
        return rc;
    }
    if (h->op==HMOS_PAL_VOICE_VOLUME) {
        if (h->arg0>1000000u) return -22;
        voice.volume=h->arg0/1000000.0f;
        int rc=voice_apply_volume(voice.muted ? 0.0f : voice.volume);
        *value=voice_status_word();
        return rc;
    }
    if (h->op==HMOS_PAL_VOICE_MUTE) {
        if (h->arg0>1u) return -22;
        voice.muted=(int)h->arg0;
        int rc=0;
        if (voice.stream && voice.state>=2) {
            if (stream_set_mute) rc=stream_set_mute(voice.stream,(bool)h->arg0);
            else rc=voice_apply_volume(voice.muted ? 0.0f : voice.volume);
        }
        *value=voice_status_word();
        return rc;
    }
    if (!c->stream) return -19;
    int rc;
    switch (h->op) {
    case HMOS_PAL_START:
        if (c->state==2) return 0;
        if (c->state!=1) return -22;
        rc=stream_start(c->stream);
        if (!rc) c->state=2;
        return rc;
    case HMOS_PAL_STOP:
        if (c->state==1) return 0;
        rc=stream_stop(c->stream);
        if (!rc) c->state=1;
        return rc;
    case HMOS_PAL_PAUSE:
        if (c->state!=2) return -22;
        rc=stream_pause(c->stream);
        if (!rc) c->state=3;
        return rc;
    case HMOS_PAL_RESUME:
        if (c->state!=3) return -22;
        rc=stream_resume(c->stream);
        if (!rc) c->state=2;
        return rc;
    case HMOS_PAL_VOLUME: {
        if (h->arg0>1000000u) return -22;
        c->volume=h->arg0/1000000.0f;
        return apply_playback_volume(c->stream,c->out_device,c->volume);
    }
    case HMOS_PAL_WRITE:
        if (c->state!=2 || !h->bytes || (h->bytes&3)) return -22;
        while (*written<h->bytes) {
            unsigned n=h->bytes-*written;
            if (n>960) n=960;
            struct { struct pal_buffer value; unsigned char tail[64]; } b={0};
            b.value.buffer=data+*written;
            b.value.size=n;
            b.value.alloc_info.alloc_handle=-1;
            rc=stream_write(c->stream,&b.value);
            if (rc<0) return rc;
            if (!rc || (unsigned)rc>n || (rc&3)) return -5;
            for (int i=0;i<rc;i+=2) {
                int value_s16=(int16_t)((unsigned)data[*written+(unsigned)i] |
                    ((unsigned)data[*written+(unsigned)i+1]<<8));
                unsigned magnitude=(unsigned)(value_s16<0?-value_s16:value_s16);
                if (magnitude) c->nonzero_samples++;
                if (magnitude>c->peak) c->peak=magnitude;
            }
            if (!c->frames) printf("PAL_SERVER_FIRST_PCM_WRITE bytes=%d\n",rc);
            *written+=(unsigned)rc;
            c->frames+=(unsigned)rc/4;
        }
        *value=c->frames;
        return 0;
    default: return -95;
    }
}

static void cleanup_client(struct client *c)
{
    if (voice.stream && voice.owner_fd==c->fd) (void)voice_close();
    (void)close_client_stream(c);
    close(c->fd);
    c->fd=-1;
}

static int serve(void)
{
    signal(SIGTERM,end_signal); signal(SIGINT,end_signal); signal(SIGPIPE,SIG_IGN);
    int fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if (fd<0) return -(*__errno());
    struct sockaddr_un a={.sun_family=AF_UNIX};
    memcpy(a.sun_path+1,HMOS_PAL_SOCKET,sizeof(HMOS_PAL_SOCKET)-1);
    socklen_t len=offsetof(struct sockaddr_un,sun_path)+sizeof(HMOS_PAL_SOCKET);
    if (bind(fd,(void *)&a,len) || listen(fd,MAX_CLIENTS)) {
        int rc=-(*__errno()); close(fd); return rc;
    }
    struct client cs[MAX_CLIENTS];
    for (int i=0;i<MAX_CLIENTS;i++) cs[i]=(struct client){.fd=-1};
    struct { struct hmos_pal_request h; unsigned char data[HMOS_PAL_MAX_DATA]; } packet;
    printf("PAL_SERVER_READY=1 pid=%d socket=@%s voice_api=%d default_vol=%.2f\n",
        getpid(),HMOS_PAL_SOCKET,stream_set_device?1:0,initial_playback_volume());
    fflush(stdout);
    while (!__atomic_load_n(&stopping,__ATOMIC_ACQUIRE)) {
        if (capture_watchdog_expired()) {
            _exit(88);
        }
        struct pollfd fds[MAX_CLIENTS+1]={{fd,POLLIN,0}};
        for (int i=0;i<MAX_CLIENTS;i++) fds[i+1]=(struct pollfd){cs[i].fd,POLLIN,0};
        if (poll(fds,MAX_CLIENTS+1,200)<0) continue;
        if (fds[0].revents&POLLIN) {
            int nfd=accept4(fd,0,0,SOCK_CLOEXEC);
            if (nfd>=0) {
                struct ucred cred={0}; socklen_t n=sizeof(cred);
                /* Verified donor /system/etc/passwd: audio1041/audio_host3127. */
                if (getsockopt(nfd,SOL_SOCKET,SO_PEERCRED,&cred,&n) ||
                    (cred.uid!=0 && cred.uid!=1041 && cred.uid!=3127)) close(nfd);
                else {
                    struct timeval send_timeout={1,0};
                    if (setsockopt(nfd,SOL_SOCKET,SO_SNDTIMEO,&send_timeout,sizeof(send_timeout))) {
                        close(nfd); continue;
                    }
                    int slot;
                    for (slot=0;slot<MAX_CLIENTS && cs[slot].fd>=0;slot++) {}
                    if (slot==MAX_CLIENTS) close(nfd);
                    else cs[slot]=(struct client){.fd=nfd,.out_device=PAL_DEVICE_OUT_SPEAKER};
                }
            }
        }
        for (int i=0;i<MAX_CLIENTS;i++) {
            struct client *c=&cs[i];
            if (c->fd<0 || !fds[i+1].revents) continue;
            ssize_t n=recv(c->fd,&packet,sizeof(packet),MSG_DONTWAIT|MSG_TRUNC);
            if (n<=0) { cleanup_client(c); continue; }
            if ((size_t)n<sizeof(packet.h) || packet.h.magic!=HMOS_PAL_MAGIC ||
                packet.h.bytes>HMOS_PAL_MAX_DATA || (size_t)n!=sizeof(packet.h)+packet.h.bytes ||
                (packet.h.op!=HMOS_PAL_WRITE && packet.h.bytes)) {
                cleanup_client(c); continue;
            }
            struct hmos_pal_reply reply={.magic=HMOS_PAL_MAGIC,.sequence=packet.h.sequence};
            if (packet.h.op>=HMOS_PAL_CAPTURE_OPEN && packet.h.op<=HMOS_PAL_MIC_GET_MUTE) {
                reply.rc=route_capture_request(c,&packet.h);
                if (!reply.rc) { c->fd=-1; continue; }
            } else {
                reply.rc=c->capture?-95:dispatch(c,&packet.h,packet.data,&reply.bytes,&reply.written_frames);
            }
            struct iovec response[2]={{&reply,sizeof(reply)},{packet.data,reply.bytes}};
            int capture_data=packet.h.op==HMOS_PAL_CAPTURE_READ && !reply.rc && reply.bytes;
            struct msghdr message={.msg_iov=response,.msg_iovlen=capture_data?2:1};
            size_t response_size=sizeof(reply)+(capture_data?reply.bytes:0);
            if (sendmsg(c->fd,&message,MSG_NOSIGNAL)!=(ssize_t)response_size) {
                cleanup_client(c);
            }
            if (packet.h.op!=HMOS_PAL_WRITE || reply.rc)
                printf("PAL_SERVER_REQUEST op=%u rc=%d bytes=%u frames=%llu\n",packet.h.op,
                    reply.rc,reply.bytes,(unsigned long long)reply.written_frames);
        }
    }
    (void)voice_close();
    for (int i=0;i<MAX_CLIENTS;i++) if (cs[i].fd>=0) cleanup_client(&cs[i]);
    if (finish_capture_workers()) {
        _exit(88);
    }
    close(fd);
    return 0;
}

static int server_main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    setbuf(stdout,0);
    void *pal=dlopen("libar-pal.so",RTLD_NOW), *agm=dlopen("libagm.so",RTLD_NOW);
    if (!pal || !agm) { printf("PAL_SERVER_DLOPEN_ERROR=%s\n",dlerror()); return 80; }
    int (*agm_init_fn)(void)=dlsym(agm,"agm_init");
    int (*agm_deinit_fn)(void)=dlsym(agm,"agm_deinit");
    int (*pal_init_fn)(void)=dlsym(pal,"pal_init");
    void (*pal_deinit_fn)(void)=dlsym(RTLD_DEFAULT,"pal_deinit");
#define LOAD(field, name) field=dlsym(pal,name); if (!field) return 81
    LOAD(stream_open,"pal_stream_open"); LOAD(stream_close,"pal_stream_close");
    LOAD(stream_start,"pal_stream_start"); LOAD(stream_stop,"pal_stream_stop");
    LOAD(stream_pause,"pal_stream_pause"); LOAD(stream_resume,"pal_stream_resume");
    LOAD(stream_write,"pal_stream_write"); LOAD(stream_volume,"pal_stream_set_volume");
    LOAD(stream_buffer,"pal_stream_set_buffer_size");
#undef LOAD
    /* 输入与语音扩展缺少时保留原播放服务，查询明确返回不支持 */
    stream_set_device=dlsym(pal,"pal_stream_set_device");
    stream_set_param=dlsym(pal,"pal_stream_set_param");
    stream_read=dlsym(pal,"pal_stream_read");
    stream_set_mute=dlsym(pal,"pal_stream_set_mute");
    stream_get_mute=dlsym(pal,"pal_stream_get_mute");
    mic_set_mute=dlsym(pal,"pal_set_mic_mute");
    mic_get_mute=dlsym(pal,"pal_get_mic_mute");
    get_param=dlsym(pal,"pal_get_param");
    set_param=dlsym(pal,"pal_set_param");
    register_global=dlsym(pal,"pal_register_global_callback");
    (void)stream_set_param; (void)set_param;
    if (!agm_init_fn || !agm_deinit_fn || !pal_init_fn || !pal_deinit_fn) return 82;
    int rc=agm_init_fn(); printf("PAL_SERVER_AGM_INIT_RC=%d\n",rc);
    if (rc) return 83;
    rc=pal_init_fn(); printf("PAL_SERVER_PAL_INIT_RC=%d\n",rc);
    if (!rc) { capture_monitor_init(); rc=serve(); pal_deinit_fn(); }
    printf("PAL_SERVER_AGM_DEINIT_RC=%d\n",agm_deinit_fn());
    printf("PAL_SERVER_EXIT=%d\n",rc);
    return rc ? 84 : 0;
}

__attribute__((noreturn)) void __libc_init(void *args, void (*onexit)(void),
    int (*original_main)(int,char **,char **), const void *structors)
{
    (void)original_main;
    void (*real)(void *,void (*)(void),int (*)(int,char **,char **),const void *)=dlsym(RTLD_NEXT,"__libc_init");
    if (real) real(args,onexit,server_main,structors);
    _exit(90);
    __builtin_unreachable();
}
