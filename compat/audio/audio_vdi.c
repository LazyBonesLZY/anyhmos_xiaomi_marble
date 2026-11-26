/* SPDX-License-Identifier: MIT
 * Donor6.1 VDI playback + capture + cellular voice call endpoint.
 * Real PCM, device routing, and PAL_STREAM_VOICE_CALL operations use the isolated
 * stock303 PAL process. Input is advertised only after the backend capability gate.
 */
#include "iaudio_manager_vdi.h"
#include "pal_client.h"
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

_Static_assert(offsetof(struct IAudioRenderVdi,SetVolumeWithRamp)==120,"donor ramp slot");
_Static_assert(offsetof(struct IAudioRenderVdi,Start)==256,"donor start slot");
_Static_assert(offsetof(struct IAudioAdapterVdi,GetPortCapability)==40,"donor capability slot");
_Static_assert(offsetof(struct IAudioAdapterVdi,UpdateAudioRoute)==72,"donor route slot");
_Static_assert(offsetof(struct IAudioAdapterVdi,SetVoiceVolume)==104,"donor voice vol slot");
_Static_assert(offsetof(struct IAudioAdapterVdi,donorExtensions)+5*sizeof(void *)==168,"donor SetPhoneCallScene slot");
_Static_assert(sizeof(struct AudioAdapterDescriptorVdi)==24,"donor descriptors");
_Static_assert(sizeof(struct AudioSampleAttributesVdi)==168,"donor sample attributes");

/* Unity hardware gain for normal daytime use; HarmonyOS audio_server already scales
 * software volume via audio_volume_config.xml. Presence of HMOS_AUDIO_QUIET_FLAG
 * restores the 0.03f (-30.46 dB) quiet bench/nighttime attenuation floor. */
#define HMOS_AUDIO_DEFAULT_VOL 1.0f
#define HMOS_AUDIO_QUIET_VOL   0.03f
#define HMOS_AUDIO_QUIET_FLAG  "/data/local/tmp/marble-audio-quiet"
#define HMOS_MAX_ACTIVE_RENDERS 8
#define HMOS_MAX_ROUTES 16

struct adapter;
struct render {
    struct IAudioRenderVdi api;
    pthread_mutex_t lock;
    struct adapter *owner;
    int fd, running, muted, backend_open;
    uint32_t out_device;
    float volume;
    uint64_t write_calls;
    struct AudioSampleAttributesVdi attrs;
};
struct audio_route_slot {
    int used;
    int32_t handle;
    uint32_t out_dev;
    uint32_t in_dev;
    int is_voice;
};
struct adapter {
    struct IAudioAdapterVdi api;
    pthread_mutex_t lock;
    unsigned renders;
    unsigned captures;
    struct render *active_renders[HMOS_MAX_ACTIVE_RENDERS];
    struct audio_route_slot routes[HMOS_MAX_ROUTES];
    int32_t next_route_handle;
    int voice_fd;
    int voice_active;
    int phone_call_scene;
    uint32_t voice_rx_dev;
    uint32_t voice_vsid;
    float voice_volume;
};

static float default_initial_volume(void)
{
    return access(HMOS_AUDIO_QUIET_FLAG, F_OK) == 0 ? HMOS_AUDIO_QUIET_VOL : HMOS_AUDIO_DEFAULT_VOL;
}

static uint32_t pin_to_pal_out_device(uint32_t pins)
{
    if (!pins) return 0;
    if (pins==PIN_VDI_OUT_SPEAKER) return HMOS_PAL_DEV_OUT_SPEAKER;
    if (pins==PIN_VDI_OUT_EARPIECE) return HMOS_PAL_DEV_OUT_HANDSET;
    if (pins & PIN_VDI_OUT_BLUETOOTH_SCO) return HMOS_PAL_DEV_OUT_BLUETOOTH_SCO;
    if (pins & PIN_VDI_OUT_USB_HEADSET) return HMOS_PAL_DEV_OUT_USB_HEADSET;
    if (pins & PIN_VDI_OUT_HEADSET) return HMOS_PAL_DEV_OUT_WIRED_HEADSET;
    if (pins & PIN_VDI_OUT_HEADPHONE) return HMOS_PAL_DEV_OUT_WIRED_HEADPHONE;
    if (pins & PIN_VDI_OUT_SPEAKER) return HMOS_PAL_DEV_OUT_SPEAKER;
    if ((pins & PIN_VDI_OUT_EARPIECE)==PIN_VDI_OUT_EARPIECE) return HMOS_PAL_DEV_OUT_HANDSET;
    return 0;
}

static int unsupported(void *self,...){(void)self; return -ENOTSUP;}
static int command(struct render *r,unsigned op,unsigned arg)
{
    struct hmos_pal_reply reply={0};
    return hmos_pal_request(r->fd,op,arg,0,0,0,&reply);
}
static int backend_ready(void)
{
    int fd=hmos_pal_connect();
    if (fd<0) return fd;
    struct hmos_pal_reply reply={0};
    int rc=hmos_pal_request(fd,HMOS_PAL_STATUS,0,0,0,0,&reply);
    close(fd);
    return rc;
}

/* ==================== Voice Call Controller (caller holds a->lock) ==================== */
static int adapter_voice_stop_locked(struct adapter *a)
{
    int rc=0;
    if (a->voice_fd>=0) {
        struct hmos_pal_reply reply={0};
        rc=hmos_pal_request(a->voice_fd,HMOS_PAL_VOICE_CLOSE,0,0,0,0,&reply);
        close(a->voice_fd);
        a->voice_fd=-1;
    }
    a->voice_active=0;
    fprintf(stderr,"HMOS_AUDIO_VOICE_STOP rc=%d\n",rc);
    return rc;
}

static int adapter_voice_ensure_started_locked(struct adapter *a, uint32_t rx_dev)
{
    if (!rx_dev) rx_dev=a->voice_rx_dev ? a->voice_rx_dev : HMOS_PAL_DEV_OUT_HANDSET;
    if (a->voice_fd<0) {
        a->voice_fd=hmos_pal_connect();
        if (a->voice_fd<0) {
            int rc=a->voice_fd; a->voice_fd=-1;
            fprintf(stderr,"HMOS_AUDIO_VOICE_CONNECT_ERR rc=%d\n",rc);
            return rc;
        }
    }
    struct hmos_pal_reply reply={0};
    int rc;
    if (a->voice_active) {
        if (rx_dev!=a->voice_rx_dev) {
            rc=hmos_pal_request(a->voice_fd,HMOS_PAL_VOICE_SET_DEVICE,rx_dev,0,0,0,&reply);
            if (!rc) a->voice_rx_dev=rx_dev;
            fprintf(stderr,"HMOS_AUDIO_VOICE_SWITCH rx=%u rc=%d\n",rx_dev,rc);
            return rc;
        }
        return 0;
    }
    rc=hmos_pal_request(a->voice_fd,HMOS_PAL_VOICE_OPEN,rx_dev,a->voice_vsid,0,0,&reply);
    if (!rc) rc=hmos_pal_request(a->voice_fd,HMOS_PAL_VOICE_START,0,0,0,0,&reply);
    if (!rc) {
        unsigned vol=(unsigned)(a->voice_volume*1000000.0f);
        (void)hmos_pal_request(a->voice_fd,HMOS_PAL_VOICE_VOLUME,vol,0,0,0,&reply);
        a->voice_active=1;
        a->voice_rx_dev=rx_dev;
    } else {
        (void)adapter_voice_stop_locked(a);
    }
    fprintf(stderr,"HMOS_AUDIO_VOICE_START rx=%u vsid=0x%x vol=%.3f rc=%d\n",
        rx_dev,a->voice_vsid,a->voice_volume,rc);
    return rc;
}

static void adapter_switch_renders_device_locked(struct adapter *a, uint32_t pal_dev)
{
    if (!pal_dev) return;
    for (unsigned i=0;i<HMOS_MAX_ACTIVE_RENDERS;i++) {
        struct render *r=a->active_renders[i];
        if (!r) continue;
        pthread_mutex_lock(&r->lock);
        if (r->out_device!=pal_dev) {
            int rc=r->backend_open ? command(r,HMOS_PAL_SET_DEVICE,pal_dev) : 0;
            if (!rc) r->out_device=pal_dev;
            fprintf(stderr,"HMOS_AUDIO_RENDER_DEVICE dev=%u rc=%d\n",pal_dev,rc);
        }
        pthread_mutex_unlock(&r->lock);
    }
}

/* Caller holds r->lock. A PAL stopped handle can accept writes while its
 * speaker route stays silent. Release that handle and open a fresh real stream.
 * Preserve the native sink object and its requested volume/mute/device state. */
static int open_backend(struct render *r)
{
    if (r->backend_open) return 0;
    struct hmos_pal_reply reply={0};
    uint32_t dev=r->out_device ? r->out_device : HMOS_PAL_DEV_OUT_SPEAKER;
    int rc=hmos_pal_request(r->fd,HMOS_PAL_OPEN,48000,2u | (dev << 16),0,0,&reply);
    if (rc) return rc;
    r->backend_open=1;
    rc=command(r,HMOS_PAL_VOLUME,r->muted?0:(unsigned)(r->volume*1000000.0f));
    fprintf(stderr,"HMOS_AUDIO_BACKEND_REOPEN dev=%u rc=%d volume=%.4f muted=%d\n",
        dev,rc,r->volume,r->muted);
    return rc;
}
static int stop_backend(struct render *r)
{
    if (!r->backend_open) { r->running=0; return 0; }
    int rc=command(r,HMOS_PAL_STOP,0);
    if (rc) return rc;
    r->running=0;
    rc=command(r,HMOS_PAL_CLOSE,0);
    if (!rc) r->backend_open=0;
    fprintf(stderr,"HMOS_AUDIO_BACKEND_RELEASE rc=%d\n",rc);
    return rc;
}
static int start(struct IAudioRenderVdi *self)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=open_backend(r);
    if (!rc) rc=command(r,HMOS_PAL_START,0);
    if (!rc) r->running=1;
    pthread_mutex_unlock(&r->lock); return rc;
}
static int stop(struct IAudioRenderVdi *self)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=stop_backend(r);
    pthread_mutex_unlock(&r->lock); return rc;
}
static int pause_render(struct IAudioRenderVdi *self)
{
    return stop(self);
}
static int resume_render(struct IAudioRenderVdi *self)
{
    return start(self);
}
static int flush(struct IAudioRenderVdi *self)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int was_running=r->running, rc=stop_backend(r);
    if (!rc) rc=open_backend(r);
    if (!rc && was_running) rc=command(r,HMOS_PAL_START,0);
    if (!rc) r->running=was_running;
    pthread_mutex_unlock(&r->lock); return rc;
}
static int set_volume(struct IAudioRenderVdi *self,float volume)
{
    if (!(volume>=0 && volume<=1)) return -EINVAL;
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=r->backend_open?command(r,HMOS_PAL_VOLUME,r->muted?0:(unsigned)(volume*1000000.0f)):0;
    if (!rc) r->volume=volume;
    pthread_mutex_unlock(&r->lock); return rc;
}
static int set_ramp(struct IAudioRenderVdi *self,float volume,uint32_t duration)
{
    return duration ? -ENOTSUP : set_volume(self,volume);
}
static int get_volume(struct IAudioRenderVdi *self,float *volume)
{
    if (!volume) return -EINVAL;
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    *volume=r->volume; pthread_mutex_unlock(&r->lock); return 0;
}
static int set_mute(struct IAudioRenderVdi *self,bool mute)
{
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    int rc=r->backend_open?command(r,HMOS_PAL_VOLUME,mute?0:(unsigned)(r->volume*1000000.0f)):0;
    if (!rc) r->muted=mute;
    pthread_mutex_unlock(&r->lock); return rc;
}
static int get_mute(struct IAudioRenderVdi *self,bool *mute)
{
    if (!mute) return -EINVAL;
    struct render *r=(void *)self; pthread_mutex_lock(&r->lock);
    *mute=r->muted; pthread_mutex_unlock(&r->lock); return 0;
}
static int render_frame(struct IAudioRenderVdi *self,const int8_t *data,uint32_t len,uint64_t *written)
{
    if (!data || !written) return -EINVAL;
    *written=0;
    struct render *r=(void *)self;
    unsigned sample_bytes=r->attrs.format==AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT?4:2;
    if (len%(sample_bytes*2)) return -EINVAL;
    pthread_mutex_lock(&r->lock);
    int rc=0;
    while (*written<len) {
        unsigned char pcm[HMOS_PAL_MAX_DATA];
        unsigned input=len-(unsigned)*written;
        unsigned max_input=HMOS_PAL_MAX_DATA*(sample_bytes/2);
        if (input>max_input) input=max_input;
        const unsigned char *p=(const unsigned char *)data+*written;
        unsigned output=input;
        const void *payload=p;
        if (sample_bytes==4) {
            output=input/2;
            for (unsigned i=0;i<output/2;i++) {
                /* Signed little-endian S32 -> S16, preserving the high bits. */
                pcm[2*i]=p[4*i+2]; pcm[2*i+1]=p[4*i+3];
            }
            payload=pcm;
        }
        r->write_calls++;
        if (r->write_calls==1 || r->write_calls==2 || r->write_calls==8 || r->write_calls==32) {
            unsigned peak=0,nonzero=0;
            const unsigned char *samples=payload;
            for (unsigned i=0;i<output;i+=2) {
                int v=(int16_t)((unsigned)samples[i]|((unsigned)samples[i+1]<<8));
                unsigned mag=(unsigned)(v<0?-v:v);
                if(mag) nonzero++;
                if(mag>peak) peak=mag;
            }
            fprintf(stderr,"HMOS_AUDIO_PCM call=%llu input_bytes=%u output_bytes=%u peak_s16=%u nonzero=%u volume=%.4f muted=%d\n",
                (unsigned long long)r->write_calls,input,output,peak,nonzero,r->volume,r->muted);
        }
        struct hmos_pal_reply reply={0};
        rc=hmos_pal_request(r->fd,HMOS_PAL_WRITE,0,0,payload,output,&reply);
        *written+=(uint64_t)reply.bytes*(sample_bytes/2);
        if (rc || reply.bytes!=output) { if (!rc) rc=-EIO; break; }
    }
    pthread_mutex_unlock(&r->lock);
    return rc;
}
static int get_size(struct IAudioRenderVdi *self,uint64_t *size)
{
    if (!size) return -EINVAL;
    struct render *r=(void *)self;
    *size=r->attrs.format==AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT?8:4; return 0;
}
static int get_count(struct IAudioRenderVdi *self,uint64_t *count)
{ (void)self; if (!count) return -EINVAL; *count=240; return 0; }
static int get_buffer_size(struct IAudioRenderVdi *self,uint64_t *bytes)
{ int rc=get_size(self,bytes); if (!rc) *bytes*=240; return rc; }
static int get_attrs(struct IAudioRenderVdi *self,struct AudioSampleAttributesVdi *attrs)
{ if (!attrs) return -EINVAL; *attrs=((struct render *)self)->attrs; return 0; }
static int no_drain(struct IAudioRenderVdi *self,bool *supported)
{ (void)self; if (!supported) return -EINVAL; *supported=false; return 0; }
static int no_pause(struct IAudioRenderVdi *self,bool *pause,bool *resume)
{ (void)self; if (!pause || !resume) return -EINVAL; *pause=false; *resume=false; return 0; }
static int scene_supported(struct IAudioRenderVdi *self,const struct AudioSceneDescriptorVdi *scene,bool *supported)
{
    (void)self;
    if (!scene || !supported) return -EINVAL;
    *supported = pin_to_pal_out_device((uint32_t)scene->desc.pins) != 0;
    return 0;
}
static int select_scene(struct IAudioRenderVdi *self,const struct AudioSceneDescriptorVdi *scene)
{
    if (!self || !scene) return -EINVAL;
    struct render *r=(void *)self;
    uint32_t out_dev=pin_to_pal_out_device((uint32_t)scene->desc.pins);
    if (!out_dev) return -ENOTSUP;
    fprintf(stderr,"HMOS_AUDIO_SELECT_SCENE scene_id=%u pins=%u out_dev=%u\n",
        scene->scene.id,(unsigned)scene->desc.pins,out_dev);
    int rc=0;
    struct adapter *a=r->owner;
    if (a) {
        pthread_mutex_lock(&a->lock);
        if (scene->scene.id==AUDIO_VDI_IN_CALL) {
            a->phone_call_scene=1;
            rc=adapter_voice_ensure_started_locked(a,out_dev);
        } else if (a->voice_active) {
            a->phone_call_scene=0;
            (void)adapter_voice_stop_locked(a);
        }
        pthread_mutex_unlock(&a->lock);
    }
    if (!rc && scene->scene.id!=AUDIO_VDI_IN_CALL) {
        pthread_mutex_lock(&r->lock);
        if (r->out_device!=out_dev) {
            rc=r->backend_open ? command(r,HMOS_PAL_SET_DEVICE,out_dev) : 0;
            if (!rc) r->out_device=out_dev;
        }
        pthread_mutex_unlock(&r->lock);
    }
    return rc;
}
static int render_set_extra(struct IAudioRenderVdi *self,const char *kv_list)
{
    (void)self;
    fprintf(stderr,"HMOS_AUDIO_RENDER_SET_EXTRA kv=%s\n",kv_list?kv_list:"(null)");
    return 0;
}

static int create_render(struct IAudioAdapterVdi *self,const struct AudioDeviceDescriptorVdi *desc,
    const struct AudioSampleAttributesVdi *attrs,struct IAudioRenderVdi **out)
{
    if (!desc || !attrs || !out) return -EINVAL;
    *out=0;
    uint32_t out_dev=pin_to_pal_out_device((uint32_t)desc->pins);
    fprintf(stderr,"HMOS_AUDIO_CREATE pins=%u out_dev=%u type=%u rate=%u channels=%u format=%u\n",
        (unsigned)desc->pins,out_dev,(unsigned)attrs->type,attrs->sampleRate,attrs->channelCount,(unsigned)attrs->format);
    if (!out_dev || attrs->sampleRate!=48000 || attrs->channelCount!=2 ||
        !attrs->interleaved || attrs->isBigEndian ||
        (attrs->type!=AUDIO_VDI_IN_MEDIA && attrs->type!=AUDIO_VDI_IN_RINGTONE &&
         attrs->type!=AUDIO_VDI_IN_COMMUNICATION && attrs->type!=AUDIO_VDI_IN_CALL) ||
        (attrs->format!=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT && attrs->format!=AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT)) return -ENOTSUP;
    struct render *r=calloc(1,sizeof(*r));
    if (!r) return -ENOMEM;
    r->fd=hmos_pal_connect();
    if (r->fd<0) { int rc=r->fd; free(r); return rc; }
    struct hmos_pal_reply reply={0};
    int rc=hmos_pal_request(r->fd,HMOS_PAL_OPEN,48000,2u | (out_dev << 16),0,0,&reply);
    if (rc) { close(r->fd); free(r); return rc; }
    rc=pthread_mutex_init(&r->lock,0);
    if (rc) { (void)command(r,HMOS_PAL_CLOSE,0); close(r->fd); free(r); return -rc; }
    struct adapter *a=(struct adapter *)self;
    r->owner=a;
    r->attrs=*attrs;
    r->out_device=out_dev;
    r->volume=default_initial_volume();
    r->backend_open=1;
    (void)command(r,HMOS_PAL_VOLUME,(unsigned)(r->volume*1000000.0f));
    /* Every optional method returns a real unsupported error until implemented. */
#define OPTIONAL(field) r->api.field=(__typeof__(r->api.field))unsupported
    OPTIONAL(GetLatency); OPTIONAL(GetRenderPosition); OPTIONAL(SetRenderSpeed); OPTIONAL(GetRenderSpeed);
    OPTIONAL(SetChannelMode); OPTIONAL(GetChannelMode); OPTIONAL(RegCallback); OPTIONAL(DrainBuffer);
    OPTIONAL(GetGainThreshold); OPTIONAL(GetGain); OPTIONAL(SetGain); OPTIONAL(SetSampleAttributes);
    OPTIONAL(GetCurrentChannelId); OPTIONAL(GetExtraParams);
    OPTIONAL(ReqMmapBuffer); OPTIONAL(GetMmapPosition); OPTIONAL(AddAudioEffect); OPTIONAL(RemoveAudioEffect);
    OPTIONAL(AudioDevDump); OPTIONAL(SetBufferSize);
#undef OPTIONAL
    r->api.SetExtraParams=render_set_extra;
    r->api.RenderFrame=render_frame; r->api.Start=start; r->api.Stop=stop;
    r->api.Pause=pause_render; r->api.Resume=resume_render; r->api.Flush=flush; r->api.TurnStandbyMode=stop;
    r->api.SetVolume=set_volume; r->api.GetVolume=get_volume; r->api.SetVolumeWithRamp=set_ramp;
    r->api.SetMute=set_mute; r->api.GetMute=get_mute;
    r->api.GetFrameSize=get_size; r->api.GetFrameCount=get_count; r->api.GetFrameBufferSize=get_buffer_size;
    r->api.GetSampleAttributes=get_attrs; r->api.IsSupportsDrain=no_drain;
    r->api.IsSupportsPauseAndResume=no_pause;
    r->api.CheckSceneCapability=scene_supported; r->api.SelectScene=select_scene;
    pthread_mutex_lock(&a->lock);
    a->renders++;
    for (unsigned i=0;i<HMOS_MAX_ACTIVE_RENDERS;i++) {
        if (!a->active_renders[i]) { a->active_renders[i]=r; break; }
    }
    pthread_mutex_unlock(&a->lock);
    *out=&r->api;
    return 0;
}
static int destroy_render(struct IAudioAdapterVdi *self,struct IAudioRenderVdi *render)
{
    if (!render) return -EINVAL;
    struct render *r=(void *)render;
    struct adapter *a=(struct adapter *)self;
    if (a) {
        pthread_mutex_lock(&a->lock);
        for (unsigned i=0;i<HMOS_MAX_ACTIVE_RENDERS;i++) {
            if (a->active_renders[i]==r) a->active_renders[i]=0;
        }
        if (a->renders) a->renders--;
        pthread_mutex_unlock(&a->lock);
    }
    pthread_mutex_lock(&r->lock);
    int rc=r->backend_open?command(r,HMOS_PAL_CLOSE,0):0; close(r->fd); r->fd=-1;
    pthread_mutex_unlock(&r->lock); pthread_mutex_destroy(&r->lock); free(r);
    return rc;
}
static int init_ports(struct IAudioAdapterVdi *self){(void)self; return backend_ready();}
static int capability(struct IAudioAdapterVdi *self,const struct AudioPortVdi *port,
    struct AudioPortCapabilityVdi *cap)
{
    (void)self;
    if (!port || !cap || port->dir!=PORT_VDI_OUT) return -EINVAL;
    memset(cap,0,sizeof(*cap));
    cap->formats=calloc(2,sizeof(*cap->formats));
    cap->subPorts=calloc(1,sizeof(*cap->subPorts));
    cap->supportSampleFormats=calloc(2,sizeof(*cap->supportSampleFormats));
    if (!cap->formats || !cap->subPorts || !cap->supportSampleFormats) {
        free(cap->formats); free(cap->subPorts); free(cap->supportSampleFormats); memset(cap,0,sizeof(*cap)); return -ENOMEM;
    }
    cap->subPorts[0].desc=strdup("Hmos stock303 speaker");
    if (!cap->subPorts[0].desc) {
        free(cap->formats); free(cap->subPorts); free(cap->supportSampleFormats); memset(cap,0,sizeof(*cap)); return -ENOMEM;
    }
    cap->deviceId=port->portId; cap->hardwareMode=true; cap->formatNum=2; cap->formatsLen=2;
    cap->formats[0]=AUDIO_VDI_FORMAT_TYPE_PCM_16_BIT; cap->formats[1]=AUDIO_VDI_FORMAT_TYPE_PCM_32_BIT;
    cap->sampleRateMasks=AUDIO_VDI_SAMPLE_RATE_MASK_48000;
    cap->channelMasks=AUDIO_VDI_CHANNEL_STEREO; cap->channelCount=2;
    cap->subPortsLen=1; cap->subPorts[0].portId=port->portId; cap->subPorts[0].mask=PORT_VDI_PASSTHROUGH_LPCM;
    cap->supportSampleFormatsLen=2; cap->supportSampleFormats[0]=AUDIO_VDI_SAMPLE_FORMAT_S16;
    cap->supportSampleFormats[1]=AUDIO_VDI_SAMPLE_FORMAT_S32;
    return 0;
}
static int get_extra(struct IAudioAdapterVdi *self,enum AudioExtParamKeyVdi key,
    const char *condition,char *value,uint32_t len)
{
    (void)self;
    fprintf(stderr,"HMOS_AUDIO_GET_EXTRA key=%u condition=%s\n",(unsigned)key,condition?condition:"(null)");
    if (value && len) value[0]=0;
    return -ENOTSUP;
}

#include "audio_capture_vdi.inc"

static int update_audio_route(struct IAudioAdapterVdi *self,
    const struct AudioRouteVdi *route, int32_t *routeHandle)
{
    if (!self || !route || !routeHandle) return -EINVAL;
    struct adapter *a=(struct adapter *)self;
    uint32_t out_dev=0, in_dev=0;
    int has_mix_src=0, has_voice_mix=0;
    for (uint32_t i=0;i<route->sinksLen && route->sinks;i++) {
        if (route->sinks[i].type==AUDIO_VDI_PORT_DEVICE_TYPE) {
            uint32_t d=pin_to_pal_out_device((uint32_t)route->sinks[i].ext.device.type);
            if (d) out_dev=d;
        }
    }
    for (uint32_t i=0;i<route->sourcesLen && route->sources;i++) {
        if (route->sources[i].type==AUDIO_VDI_PORT_DEVICE_TYPE) {
            uint32_t d=pin_to_pal_in_device((uint32_t)route->sources[i].ext.device.type);
            if (d) in_dev=d;
        } else if (route->sources[i].type==AUDIO_VDI_PORT_MIX_TYPE) {
            has_mix_src=1;
            if (route->sources[i].ext.mix.source==AUDIO_VDI_INPUT_VOICE_CALL_TYPE) has_voice_mix=1;
        }
    }
    int is_modem_route=(out_dev && in_dev && !has_mix_src) || has_voice_mix;
    pthread_mutex_lock(&a->lock);
    int is_voice=(a->phone_call_scene || a->voice_active || is_modem_route) && (out_dev!=0);
    int32_t handle=a->next_route_handle++;
    if (a->next_route_handle<=0) a->next_route_handle=1;
    for (unsigned i=0;i<HMOS_MAX_ROUTES;i++) {
        if (!a->routes[i].used) {
            a->routes[i]=(struct audio_route_slot){1,handle,out_dev,in_dev,is_voice};
            break;
        }
    }
    int rc=0;
    if (out_dev) {
        if (is_voice) {
            rc=adapter_voice_ensure_started_locked(a,out_dev);
        } else {
            a->voice_rx_dev=out_dev;
            adapter_switch_renders_device_locked(a,out_dev);
        }
    }
    fprintf(stderr,"HMOS_AUDIO_ROUTE_UPDATE handle=%d out_dev=%u in_dev=%u call_scene=%d voice_active=%d rc=%d\n",
        handle,out_dev,in_dev,a->phone_call_scene,a->voice_active,rc);
    pthread_mutex_unlock(&a->lock);
    *routeHandle=handle;
    return rc;
}

static int release_audio_route(struct IAudioAdapterVdi *self, int32_t routeHandle)
{
    if (!self || routeHandle<=0) return -EINVAL;
    struct adapter *a=(struct adapter *)self;
    pthread_mutex_lock(&a->lock);
    int released_voice=0, other_voice=0;
    for (unsigned i=0;i<HMOS_MAX_ROUTES;i++) {
        if (!a->routes[i].used) continue;
        if (a->routes[i].handle==routeHandle) {
            released_voice=a->routes[i].is_voice;
            a->routes[i].used=0;
        } else if (a->routes[i].is_voice) {
            other_voice=1;
        }
    }
    int rc=0;
    if (released_voice && !other_voice && !a->phone_call_scene && a->voice_active) {
        rc=adapter_voice_stop_locked(a);
    }
    fprintf(stderr,"HMOS_AUDIO_ROUTE_RELEASE handle=%d released_voice=%d rc=%d\n",
        routeHandle,released_voice,rc);
    pthread_mutex_unlock(&a->lock);
    return rc;
}

static int set_voice_volume(struct IAudioAdapterVdi *self, float volume)
{
    if (!self || !(volume>=0.0f && volume<=1.0f)) return -EINVAL;
    struct adapter *a=(struct adapter *)self;
    pthread_mutex_lock(&a->lock);
    a->voice_volume=volume;
    int rc=0;
    if (a->voice_active && a->voice_fd>=0) {
        struct hmos_pal_reply reply={0};
        rc=hmos_pal_request(a->voice_fd,HMOS_PAL_VOICE_VOLUME,
            (unsigned)(volume*1000000.0f),0,0,0,&reply);
    }
    fprintf(stderr,"HMOS_AUDIO_VOICE_VOL vol=%.4f active=%d rc=%d\n",volume,a->voice_active,rc);
    pthread_mutex_unlock(&a->lock);
    return rc;
}

static int set_extra_params(struct IAudioAdapterVdi *self, enum AudioExtParamKeyVdi key,
    const char *condition, const char *value)
{
    if (!self) return -EINVAL;
    struct adapter *a=(struct adapter *)self;
    fprintf(stderr,"HMOS_AUDIO_SET_EXTRA key=%u condition=%s value=%s\n",
        (unsigned)key,condition?condition:"(null)",value?value:"(null)");
    if (!value) return 0;
    pthread_mutex_lock(&a->lock);
    int rc=0;
    if (strstr(value,"sim_slot=0") || strstr(value,"vsid=0x11c05000") || strstr(value,"vsid=0x11C05000")) {
        a->voice_vsid=HMOS_PAL_VSID_MMODE1;
    } else if (strstr(value,"sim_slot=1") || strstr(value,"vsid=0x11dc5000") || strstr(value,"vsid=0x11DC5000")) {
        a->voice_vsid=HMOS_PAL_VSID_MMODE2;
    }
    if (strstr(value,"call_state=active")) {
        a->phone_call_scene=1;
        rc=adapter_voice_ensure_started_locked(a,a->voice_rx_dev);
    } else if (strstr(value,"call_state=idle") || strstr(value,"call_state=off")) {
        a->phone_call_scene=0;
        rc=adapter_voice_stop_locked(a);
    }
    pthread_mutex_unlock(&a->lock);
    return rc;
}

/* Donor 6.1 extension at offset 168 (donorExtensions[5]): AudioSetPhoneCallSceneVdi */
static int set_phone_call_scene_typed(struct IAudioAdapterVdi *self, int32_t scene_type)
{
    if (!self) return -EINVAL;
    struct adapter *a=(struct adapter *)self;
    fprintf(stderr,"HMOS_AUDIO_PHONE_CALL_SCENE type=%d\n",scene_type);
    pthread_mutex_lock(&a->lock);
    int rc=0;
    if (scene_type==1) {
        a->phone_call_scene=1;
        rc=adapter_voice_ensure_started_locked(a,a->voice_rx_dev);
    } else if (scene_type==0) {
        a->phone_call_scene=0;
        rc=adapter_voice_stop_locked(a);
    } else {
        rc=-EINVAL;
    }
    pthread_mutex_unlock(&a->lock);
    return rc;
}

static int load_adapter(struct IAudioManagerVdi *self,const struct AudioAdapterDescriptorVdi *desc,
    struct IAudioAdapterVdi **out)
{
    (void)self;
    if (!desc || !desc->adapterName || !out || strcmp(desc->adapterName,"primary")) return -EINVAL;
    *out=0; int rc=backend_ready();
    fprintf(stderr,"HMOS_MIC_LOAD backend_status=%d\n",rc);
    if (rc) return rc;
    struct adapter *a=calloc(1,sizeof(*a)); if (!a) return -ENOMEM;
    rc=pthread_mutex_init(&a->lock,0);
    if (rc) { free(a); return -rc; }
    a->voice_fd=-1;
    a->next_route_handle=1;
    a->voice_rx_dev=HMOS_PAL_DEV_OUT_HANDSET;
    a->voice_vsid=0; /* auto: VOICEMMODE2 on marble Slot 1 unless overridden */
    a->voice_volume=0.8f;
#define OPTIONAL(field) a->api.field=(__typeof__(a->api.field))unsupported
    OPTIONAL(SetPassthroughMode); OPTIONAL(GetPassthroughMode);
    OPTIONAL(GetDeviceStatus);
#undef OPTIONAL
    for (unsigned i=0;i<sizeof(a->api.donorExtensions)/sizeof(a->api.donorExtensions[0]);i++) a->api.donorExtensions[i]=unsupported;
    a->api.donorExtensions[5]=(int32_t (*)(void *, ...))set_phone_call_scene_typed;
    a->api.InitAllPorts=init_ports; a->api.CreateRender=create_render; a->api.DestroyRender=destroy_render;
    a->api.GetPortCapability=capture_capability; a->api.GetExtraParams=get_extra;
    a->api.CreateCapture=create_capture; a->api.DestroyCapture=destroy_capture;
    a->api.SetMicMute=adapter_mic_mute; a->api.GetMicMute=adapter_mic_get_mute;
    a->api.UpdateAudioRoute=update_audio_route; a->api.ReleaseAudioRoute=release_audio_route;
    a->api.SetVoiceVolume=set_voice_volume; a->api.SetExtraParams=set_extra_params;
    *out=&a->api; return 0;
}
static int unload_adapter(struct IAudioManagerVdi *self,struct IAudioAdapterVdi *adapter)
{
    (void)self;
    if (!adapter) return -EINVAL;
    struct adapter *a=(struct adapter *)adapter;
    pthread_mutex_lock(&a->lock);
    if (a->renders || __atomic_load_n(&a->captures,__ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&a->lock);
        return -EBUSY;
    }
    (void)adapter_voice_stop_locked(a);
    pthread_mutex_unlock(&a->lock);
    pthread_mutex_destroy(&a->lock);
    free(a);
    return 0;
}
static int get_adapters(struct IAudioManagerVdi *self,struct AudioAdapterDescriptorVdi *out,uint32_t *count)
{
    (void)self;
    if (!out || !count || !*count) return -EINVAL;
    int rc=backend_ready(); if (rc) { *count=0; return rc; }
    char *name=strdup("primary"), *port_name=strdup("speaker");
    struct AudioPortVdi *ports=calloc(1,sizeof(*ports));
    if (!name || !port_name || !ports) { free(name); free(port_name); free(ports); return -ENOMEM; }
    *ports=(struct AudioPortVdi){PORT_VDI_OUT,0,port_name};
    unsigned port_count=1;
    int capture_rc=capture_available();
    fprintf(stderr,"HMOS_MIC_ENUM capture_status=%d\n",capture_rc);
    if (!capture_rc) {
        struct AudioPortVdi *both=realloc(ports,2*sizeof(*ports));
        char *mic=strdup("microphone");
        if (!both || !mic) {
            free(mic); free(name); free(port_name); free(both?both:ports); return -ENOMEM;
        }
        ports=both; ports[1]=(struct AudioPortVdi){PORT_VDI_IN,1,mic}; port_count=2;
    }
    *out=(struct AudioAdapterDescriptorVdi){name,ports,port_count}; *count=1;
    fprintf(stderr,"HMOS_MIC_ENUM adapters=1 ports=%u\n",port_count);
    return 0;
}
static int release_manager(struct IAudioManagerVdi *self){(void)self; return 0;}
struct IAudioManagerVdi *AudioManagerCreateIfInstance(void)
{
    static struct IAudioManagerVdi manager={get_adapters,load_adapter,unload_adapter,release_manager};
    return &manager;
}
