/* SPDX-License-Identifier: MIT
 * Front-end PCM probe for marble.
 *
 * PAL/AGM is blocked on a kernel node this build does not expose
 * (/dev/gpr_channel), so this checks the other audio path the kernel does
 * offer: the ALSA front-end PCMs of ukee-mtp-snd-card.  If pcm_open() and
 * pcm_write() succeed here, a direct tinyalsa path is viable and the OHOS
 * audio HDI does not have to depend on PAL at all.
 *
 * usage: pcm_fe_probe [card] [device] [rate] [channels] [seconds]
 *   exit 0 = opened, configured and wrote frames; 2 = open failed; 3 = write failed
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <tinyalsa/asoundlib.h>

int main(int argc, char **argv)
{
    unsigned int card = argc > 1 ? (unsigned int)atoi(argv[1]) : 0;
    unsigned int device = argc > 2 ? (unsigned int)atoi(argv[2]) : 0;
    unsigned int rate = argc > 3 ? (unsigned int)atoi(argv[3]) : 48000;
    unsigned int channels = argc > 4 ? (unsigned int)atoi(argv[4]) : 2;
    unsigned int seconds = argc > 5 ? (unsigned int)atoi(argv[5]) : 1;

    struct pcm_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.channels = channels;
    cfg.rate = rate;
    cfg.period_size = 1024;
    cfg.period_count = 4;
    cfg.format = PCM_FORMAT_S16_LE;
    cfg.start_threshold = 0;
    cfg.stop_threshold = 0;
    cfg.silence_threshold = 0;

    struct pcm *pcm = pcm_open(card, device, PCM_OUT, &cfg);
    if (pcm == NULL) {
        printf("FE_PROBE_OPEN_FAIL card=%u dev=%u errno=%d (%s)\n", card, device, errno,
               strerror(errno));
        return 2;
    }
    printf("FE_PROBE_OPEN_OK card=%u dev=%u rate=%u ch=%u\n", card, device, rate, channels);
    if (!pcm_is_ready(pcm)) {
        printf("FE_PROBE_NOT_READY errno=%d (%s)\n", errno, strerror(errno));
        pcm_close(pcm);
        return 2;
    }

    /* One period of digital silence; the point is the ioctl path, not sound. */
    size_t frames = (size_t)cfg.period_size * cfg.channels;
    int16_t *buf = calloc(frames, sizeof(int16_t));
    if (buf == NULL) {
        pcm_close(pcm);
        printf("FE_PROBE_NOMEM\n");
        return 3;
    }
    unsigned int periods = seconds * (rate / cfg.period_size);
    if (periods == 0) periods = 1;
    int rc = 0;
    for (unsigned int i = 0; i < periods; i++) {
        rc = pcm_write(pcm, buf, frames * sizeof(int16_t));
        if (rc != 0) {
            printf("FE_PROBE_WRITE_FAIL period=%u rc=%d errno=%d (%s)\n", i, rc, errno,
                   strerror(errno));
            break;
        }
    }
    free(buf);
    pcm_close(pcm);
    if (rc != 0) return 3;
    printf("FE_PROBE_WRITE_OK periods=%u frames_per_period=%zu\n", periods, frames / channels);
    return 0;
}
