/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2010 Thomas Martitz
 * Copyright (c) 2020 Solomon Peachy
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/*
 * Based, but heavily modified, on the ALSA test pcm example.
 * HiBy keeps a single poll-thread pump model instead of async signal callbacks.
 */

#include "autoconf.h"

/* HiBy Linux variant of ALSA PCM backend.
 * Kept target-local to avoid changing generic hosted pcm-alsa.c. */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <alsa/asoundlib.h>

//#define LOGF_ENABLE

#include "system.h"
#include "debug.h"
#include "kernel.h"
#include "panic.h"

#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_mixer.h"
#include "pcm_sampr.h"
#include "audiohw.h"
#include "pcm-alsa.h"
#include "fixedpoint.h"

#include "logf.h"

#include <pthread.h>

/* plughw:0,0 works with both, however "default" is recommended.
 * default may not behave consistently on this target, so keep explicit device. */
#define DEFAULT_PLAYBACK_DEVICE "plughw:0,0"
#define DEFAULT_CAPTURE_DEVICE "default"

#if MIX_FRAME_SAMPLES < 512
#error "MIX_FRAME_SAMPLES needs to be at least 512!"
#elif MIX_FRAME_SAMPLES < 1024
#warning "MIX_FRAME_SAMPLES <1024 may cause dropouts!"
#endif

/* PCM_DC_OFFSET_VALUE is a workaround for eros q hardware quirk */
#if !defined(PCM_DC_OFFSET_VALUE)
# define PCM_DC_OFFSET_VALUE 0
#endif

static const snd_pcm_access_t access_ = SND_PCM_ACCESS_RW_INTERLEAVED; /* access mode */
#if defined(HAVE_ALSA_32BIT)
static const snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;    /* sample format */
typedef int32_t sample_t;
#else
static const snd_pcm_format_t format = SND_PCM_FORMAT_S16;    /* sample format */
typedef int16_t sample_t;
#endif
static const int channels = 2;                                /* count of channels */
static unsigned int real_sample_rate;
static unsigned int last_sample_rate;

static snd_pcm_t *handle = NULL;
static snd_pcm_sframes_t buffer_size;
static snd_pcm_sframes_t period_size;
static sample_t *frames = NULL;

static const void  *pcm_data = 0;
static size_t       pcm_size = 0;

static unsigned int xruns = 0;

static pthread_mutex_t pcm_mtx;
static bool pcm_mtx_initialized = false;
static pthread_t poll_thread;
static volatile bool poll_thread_stop = false;
static bool poll_thread_running = false;
static unsigned int poll_interval_us = 10000;

static char playback_dev_buf[128] = DEFAULT_PLAYBACK_DEVICE;
static const char *playback_dev = playback_dev_buf;
static char current_alsa_device_buf[128];

#ifdef HAVE_RECORDING
static void *pcm_data_rec = DEFAULT_CAPTURE_DEVICE;
static const char *capture_dev = NULL;
static snd_pcm_stream_t current_alsa_mode;  /* SND_PCM_STREAM_PLAYBACK / _CAPTURE */
#endif

static const char *current_alsa_device;
static void open_hwdev(const char *device, snd_pcm_stream_t mode);
static void pcm_dma_apply_settings_nolock(void);

static void set_playback_device_string(const char *device)
{
    if (!device || !*device)
        return;

    snprintf(playback_dev_buf, sizeof(playback_dev_buf), "%s", device);
    playback_dev = playback_dev_buf;
}

static void ensure_pcm_mutex_initialized(void)
{
    if (!pcm_mtx_initialized)
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&pcm_mtx, &attr);
        pthread_mutexattr_destroy(&attr);
        pcm_mtx_initialized = true;
    }
}

static bool is_bluealsa_device(const char *device)
{
    return device && strstr(device, "bluealsa:") != NULL;
}

static bool is_bluealsa_active(void)
{
    if (current_alsa_device && *current_alsa_device)
        return is_bluealsa_device(current_alsa_device);
    return is_bluealsa_device(playback_dev);
}

static bool pcm_params_ready(void)
{
    return period_size > 0 && buffer_size > 0 && frames != NULL;
}

static bool is_alsa_transient_error(int err)
{
    return err == -EBUSY || err == -EAGAIN || err == -ENOENT || err == -ENODEV;
}

static void pcm_bt_trace(const char *fmt, ...);

static void pcm_bt_trace(const char *fmt, ...)
{
    FILE *fp;
    time_t now;
    struct tm tm_now;
    va_list ap;

    fp = fopen("/usr/data/mnt/sd_0/.rockbox/pcm_bt_trace.log", "a");
    if (!fp)
        return;

    now = time(NULL);
    localtime_r(&now, &tm_now);
    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
            tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
            tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fclose(fp);
}

void pcm_alsa_set_playback_device(const char *device)
{
    pcm_bt_trace("set_playback_device request=%s", device ? device : "<null>");
    set_playback_device_string(device);
    pcm_bt_trace("set_playback_device applied=%s", playback_dev ? playback_dev : "<null>");
}

int pcm_alsa_switch_playback_device(const char *device)
{
    bool requested_bluealsa;
    bool opened_bluealsa;
    int rc = 0;

    pcm_bt_trace("switch_playback_device request=%s", device ? device : "<null>");
    if (!device || !*device)
        return -1;

    requested_bluealsa = is_bluealsa_device(device);
    set_playback_device_string(device);
    if (!handle)
    {
        pcm_bt_trace("switch_playback_device (no active handle): %s", playback_dev);
        return 0;
    }

    ensure_pcm_mutex_initialized();
    pthread_mutex_lock(&pcm_mtx);
    open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
    pcm_dma_apply_settings_nolock();
    opened_bluealsa = is_bluealsa_device(current_alsa_device);
    if (requested_bluealsa)
    {
        if (!opened_bluealsa)
            rc = -1;
    }
    else if (!current_alsa_device || strcmp(current_alsa_device, playback_dev))
    {
        rc = -1;
    }
    pthread_mutex_unlock(&pcm_mtx);
    pcm_bt_trace("switch_playback_device (active): req=%s opened=%s rc=%d",
                 playback_dev,
                 current_alsa_device ? current_alsa_device : "<null>",
                 rc);
    return rc;
}

#ifdef HAVE_RECORDING
void pcm_alsa_set_capture_device(const char *device)
{
    capture_dev = device;
}
#endif

static int set_hwparams(snd_pcm_t *handle)
{
    int err;
    unsigned int srate;
    snd_pcm_hw_params_t *params;
    snd_pcm_hw_params_malloc(&params);

    /* Size playback buffers based on sample rate.

       Buffer size must be at least 4x period size!

       Note these are in FRAMES, and are sized to be about 8.5ms
       for the buffer and 2.1ms for the period
     */
    if (pcm_sampr > SAMPR_96) {
        buffer_size = MIX_FRAME_SAMPLES * 4 * 4;
        period_size = MIX_FRAME_SAMPLES * 4;
    } else if (pcm_sampr > SAMPR_48) {
        buffer_size = MIX_FRAME_SAMPLES * 2 * 4;
        period_size = MIX_FRAME_SAMPLES * 2;
    } else {
        buffer_size = MIX_FRAME_SAMPLES * 4;
        period_size = MIX_FRAME_SAMPLES;
    }

    /* BlueALSA benefits from a deeper queue and larger periods to absorb
     * transport jitter and userspace scheduling delays. */
    if (is_bluealsa_active())
    {
        period_size *= 2;
        if (period_size < 2048)
            period_size = 2048;
        buffer_size = period_size * 8;
    }

    /* choose all parameters */
    err = snd_pcm_hw_params_any(handle, params);
    if (err < 0)
    {
        panicf("Broken configuration for playback: no configurations available: %s", snd_strerror(err));
        goto error;
    }
    /* set the interleaved read/write format */
    err = snd_pcm_hw_params_set_access(handle, params, access_);
    if (err < 0)
    {
        panicf("Access type not available for playback: %s", snd_strerror(err));
        goto error;
    }
    /* set the sample format */
    err = snd_pcm_hw_params_set_format(handle, params, format);
    if (err < 0)
    {
        logf("Sample format not available for playback: %s", snd_strerror(err));
        goto error;
    }
    /* set the count of channels */
    err = snd_pcm_hw_params_set_channels(handle, params, channels);
    if (err < 0)
    {
        logf("Channels count (%i) not available for playbacks: %s", channels, snd_strerror(err));
        goto error;
    }
    /* set the stream rate */
    srate = pcm_sampr;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &srate, 0);
    if (err < 0)
    {
        logf("Rate %luHz not available for playback: %s", pcm_sampr, snd_strerror(err));
        goto error;
    }
    real_sample_rate = srate;
    if (real_sample_rate != pcm_sampr)
    {
        logf("Rate doesn't match (requested %luHz, get %dHz)", pcm_sampr, real_sample_rate);
        err = -EINVAL;
        goto error;
    }

    /* set the buffer size */
    err = snd_pcm_hw_params_set_buffer_size_near(handle, params, &buffer_size);
    if (err < 0)
    {
        logf("Unable to set buffer size %ld for playback: %s", buffer_size, snd_strerror(err));
        goto error;
    }

    /* set the period size */
    err = snd_pcm_hw_params_set_period_size_near (handle, params, &period_size, NULL);
    if (err < 0)
    {
        logf("Unable to set period size %ld for playback: %s", period_size, snd_strerror(err));
        goto error;
    }

    if (frames) free(frames);
    frames = calloc(1, period_size * channels * sizeof(sample_t));
    if (real_sample_rate > 0)
    {
        unsigned int us = (unsigned int)((period_size * 1000000ULL) / real_sample_rate);
        if (is_bluealsa_active())
            us /= 2;
        if (us < 2000) us = 2000;
        if (us > 50000) us = 50000;
        poll_interval_us = us;
    }

    /* write the parameters to device */
    err = snd_pcm_hw_params(handle, params);
    if (err < 0)
    {
        logf("Unable to set hw params for playback: %s", snd_strerror(err));
        goto error;
    }

    err = 0; /* success */
error:
    snd_pcm_hw_params_free(params);
    return err;
}

/* Set sw params: playback start threshold and low buffer watermark */
static int set_swparams(snd_pcm_t *handle)
{
    int err;
    snd_pcm_uframes_t start_threshold = (snd_pcm_uframes_t)(buffer_size / 2);

    snd_pcm_sw_params_t *swparams;
    snd_pcm_sw_params_malloc(&swparams);

    /* get the current swparams */
    err = snd_pcm_sw_params_current(handle, swparams);
    if (err < 0)
    {
        logf("Unable to determine current swparams for playback: %s", snd_strerror(err));
        goto error;
    }
    /* Keep a larger prefill for bluetooth output to avoid immediate XRUN. */
    if (is_bluealsa_active() && buffer_size > period_size)
        start_threshold = (snd_pcm_uframes_t)(buffer_size - period_size);

    err = snd_pcm_sw_params_set_start_threshold(handle, swparams, start_threshold);
    if (err < 0)
    {
        logf("Unable to set start threshold mode for playback: %s", snd_strerror(err));
        goto error;
    }
    /* allow the transfer when at least period_size samples can be processed */
    err = snd_pcm_sw_params_set_avail_min(handle, swparams, period_size);
    if (err < 0)
    {
        logf("Unable to set avail min for playback: %s", snd_strerror(err));
        goto error;
    }
    /* write the parameters to the playback device */
    err = snd_pcm_sw_params(handle, swparams);
    if (err < 0)
    {
        logf("Unable to set sw params for playback: %s", snd_strerror(err));
        goto error;
    }

    err = 0; /* success */
error:
    snd_pcm_sw_params_free(swparams);
    return err;
}

#if defined(HAVE_ALSA_32BIT)
/* Multiplicative factors applied to each sample */
static int32_t dig_vol_mult_l = 0;
static int32_t dig_vol_mult_r = 0;

void pcm_set_mixer_volume(int vol_db_l, int vol_db_r)
{
    dig_vol_mult_l = fp_factor(fp_div(vol_db_l, 10, 16), 16);
    dig_vol_mult_r = fp_factor(fp_div(vol_db_r, 10, 16), 16);
}
#endif

/* copy pcm samples to a spare buffer, suitable for snd_pcm_writei() */
static bool copy_frames(bool first)
{
    ssize_t nframes, frames_left = period_size;
    bool new_buffer = false;

    while (frames_left > 0)
    {
        if (!pcm_size)
        {
            new_buffer = true;
#ifdef HAVE_RECORDING
            switch (current_alsa_mode)
            {
            case SND_PCM_STREAM_PLAYBACK:
#endif
                if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data, &pcm_size))
                {
                    return false;
                }
#ifdef HAVE_RECORDING
                break;
            case SND_PCM_STREAM_CAPTURE:
                if (!pcm_play_dma_complete_callback(PCM_DMAST_OK, &pcm_data, &pcm_size))
                {
                    return false;
                }
                break;
            default:
                break;
            }
#endif
        }

        /* Note:  This assumes stereo 16-bit */
        if (pcm_size % 4)
            panicf("Wrong pcm_size");
        /* the compiler will optimize this test away */
        nframes = MIN((ssize_t)pcm_size/4, frames_left);

#ifdef HAVE_RECORDING
        switch (current_alsa_mode)
        {
        case SND_PCM_STREAM_PLAYBACK:
#endif
#if defined(HAVE_ALSA_32BIT)
            if (format == SND_PCM_FORMAT_S32_LE)
            {
                /* We have to convert 16-bit to 32-bit, the need to multiply the
                 * sample by some value so the sound is not too low */
                const int16_t *pcm_ptr = pcm_data;
                sample_t *sample_ptr = &frames[2*(period_size-frames_left)];
                for (int i = 0; i < nframes; i++)
                {
                    *sample_ptr++ = (*pcm_ptr++ * dig_vol_mult_l) + PCM_DC_OFFSET_VALUE;
                    *sample_ptr++ = (*pcm_ptr++ * dig_vol_mult_r) + PCM_DC_OFFSET_VALUE;
                }
            }
            else
#endif
            {
                /* Rockbox and PCM have same format: memcopy */
                memcpy(&frames[2*(period_size-frames_left)], pcm_data, nframes * 4);
	    }
#ifdef HAVE_RECORDING
            break;
        case SND_PCM_STREAM_CAPTURE:
            memcpy(pcm_data_rec, &frames[2*(period_size-frames_left)], nframes * 4);
            break;
        default:
            break;
        }
#endif
        pcm_data += nframes*4;
        pcm_size -= nframes*4;
        frames_left -= nframes;

        if (new_buffer && !first)
        {
            new_buffer = false;
#ifdef HAVE_RECORDING
            switch (current_alsa_mode)
            {
            case SND_PCM_STREAM_PLAYBACK:
#endif
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
#ifdef HAVE_RECORDING
                break;
            case SND_PCM_STREAM_CAPTURE:
                pcm_rec_dma_status_callback(PCM_DMAST_STARTED);
                break;
            default:
                break;
            }
#endif
        }
    }

    return true;
}

static void pcm_pump_locked(snd_pcm_t *handle)
{
    int err;
    static unsigned long bt_reopen_backoff = 0;
    if (!handle)
        return;

    snd_pcm_state_t state = snd_pcm_state(handle);

    if (state == SND_PCM_STATE_DISCONNECTED && is_bluealsa_device(playback_dev))
    {
        pcm_bt_trace("pump: disconnected -> reopen/reapply");
        open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
        pcm_dma_apply_settings_nolock();
        return;
    }

    if (state == SND_PCM_STATE_OPEN || !pcm_params_ready())
        return;

    if (state == SND_PCM_STATE_XRUN)
    {
        xruns++;
        logf("initial underrun!");
        pcm_bt_trace("pump: XRUN state detected");
        err = snd_pcm_recover(handle, -EPIPE, 0);
        if (err < 0) {
            logf("XRUN Recovery error: %s", snd_strerror(err));
            pcm_bt_trace("pump: XRUN recover failed: %d (%s)", err, snd_strerror(err));
            goto abort;
        }
        pcm_bt_trace("pump: XRUN recovered");
    }
    else if (state == SND_PCM_STATE_DRAINING)
    {
        logf("draining...");
        goto abort;
    }
    else if (state == SND_PCM_STATE_SETUP)
    {
        goto abort;
    }

#ifdef HAVE_RECORDING
    if (current_alsa_mode == SND_PCM_STREAM_PLAYBACK)
    {
#endif
        while (snd_pcm_avail_update(handle) >= period_size)
        {
            if (copy_frames(false))
            {
            retry:
                err = snd_pcm_writei(handle, frames, period_size);
                if (err == -EPIPE)
                {
                    logf("mid underrun!");
                    xruns++;
                    pcm_bt_trace("pump: write XRUN, recovering");
                    err = snd_pcm_recover(handle, -EPIPE, 0);
                    if (err < 0) {
                       logf("XRUN Recovery error: %s", snd_strerror(err));
                       pcm_bt_trace("pump: write recover failed: %d (%s)", err, snd_strerror(err));
                       goto abort;
                    }
                    pcm_bt_trace("pump: write recover ok");
                    goto retry;
                }
                else if (err != period_size)
                {
                    logf("Write error: written %i expected %li", err, period_size);
                    pcm_bt_trace("pump: write short/error rc=%d expected=%ld state=%d",
                                 err, period_size, snd_pcm_state(handle));
                    if (err == -ENODEV && is_bluealsa_device(playback_dev))
                    {
                        pcm_bt_trace("pump: ENODEV on bluealsa, reopen and reapply settings");
                        open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
                        pcm_dma_apply_settings_nolock();
                        /* Avoid tight reconnect loop when transport is flapping. */
                        bt_reopen_backoff++;
                        usleep((bt_reopen_backoff % 5 == 0) ? 200000 : 80000);
                    }
                    else
                    {
                        bt_reopen_backoff = 0;
                    }
                    break;
                }
                else
                {
                    bt_reopen_backoff = 0;
                }
            }
            else
            {
                logf("%s: No Data (%d).", __func__, state);
                break;
            }
        }
#ifdef HAVE_RECORDING
    }
    else if (current_alsa_mode == SND_PCM_STREAM_CAPTURE)
    {
        while (snd_pcm_avail_update(handle) >= period_size)
        {
            int err = snd_pcm_readi(handle, frames, period_size);
            if (err == -EPIPE)
            {
                logf("rec mid underrun!");
                xruns++;
                err = snd_pcm_recover(handle, -EPIPE, 0);
                if (err < 0) {
                   logf("XRUN Recovery error: %s", snd_strerror(err));
                   goto abort;
                }
		continue;  /* buffer contents trashed, no sense in trying to copy */
            }
            else if (err != period_size)
            {
                logf("Read error: read %i expected %li", err, period_size);
                break;
            }

            /* start the fake DMA transfer */
            if (!copy_frames(false))
            {
                /* do not spam logf */
                /* logf("%s: No Data.", __func__); */
                break;
            }
        }
    }
#endif

    if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED)
    {
        err = snd_pcm_start(handle);
        if (err < 0) {
            logf("cb start error: %s", snd_strerror(err));
            pcm_bt_trace("pump: start failed: %d (%s)", err, snd_strerror(err));
            /* Depending on the error we might be SOL */
        }
                else
                {
                    pcm_bt_trace("pump: start ok");
                }
            }

abort:
    return;
}

static void *poll_thread_fn(void *arg)
{
    (void)arg;
    while (!poll_thread_stop)
    {
        if (handle && pthread_mutex_trylock(&pcm_mtx) == 0)
        {
            pcm_pump_locked(handle);
            pthread_mutex_unlock(&pcm_mtx);
        }
        usleep(poll_interval_us);
    }
    return NULL;
}

static bool is_current_poll_thread(void)
{
    return poll_thread_running && pthread_equal(pthread_self(), poll_thread);
}

static void close_hwdev(void)
{
    logf("closedev (%p)", handle);
    pcm_bt_trace("close_hwdev begin handle=%p poll_thread_running=%d",
                 handle, poll_thread_running ? 1 : 0);

    if (poll_thread_running)
    {
        poll_thread_stop = true;
        if (!is_current_poll_thread())
        {
            pthread_join(poll_thread, NULL);
        }
        else
        {
            pcm_bt_trace("close_hwdev called from poll thread; skipping self-join");
        }
        poll_thread_running = false;
    }

    if (handle) {
        snd_pcm_drain(handle);
#ifdef AUDIOHW_MUTE_ON_STOP
        audiohw_mute(true);
#endif
        snd_pcm_close(handle);

        handle = NULL;
    }
    current_alsa_device = NULL;
    pcm_bt_trace("close_hwdev complete");

#ifdef HAVE_RECORDING
    pcm_data_rec = NULL;
#endif
}

static void alsadev_cleanup(void)
{
    free(frames);
    frames = NULL;
    close_hwdev();
}

static void open_hwdev(const char *device, snd_pcm_stream_t mode)
{
    int err;
    int attempts = is_bluealsa_device(device) ? 25 : 1;
    int delay_us = 120000;
    int i;
    char open_device_copy[160];
    const char *open_device = device;
    snd_pcm_state_t existing_state = SND_PCM_STATE_OPEN;

    ensure_pcm_mutex_initialized();

    if (!open_device || !*open_device)
    {
        panicf("%s(): Invalid empty ALSA device", __func__);
    }

    /* Keep a stable copy so later updates to global buffers can't invalidate this path. */
    snprintf(open_device_copy, sizeof(open_device_copy), "%s", open_device);
    open_device = open_device_copy;

    logf("opendev %s (%p)", open_device, handle);
    pcm_bt_trace("open_hwdev begin device=%s mode=%d handle=%p", open_device, mode, handle);

    if (handle)
        existing_state = snd_pcm_state(handle);

    if (handle && current_alsa_device && open_device &&
        strcmp(open_device, current_alsa_device) == 0
#ifdef HAVE_RECORDING
        && current_alsa_mode == mode
#endif
        )
    {
        if (is_bluealsa_device(open_device))
        {
            pcm_bt_trace("open_hwdev forcing reopen for same bluealsa device %s (state=%d)",
                         open_device, existing_state);
        }
        else if (existing_state != SND_PCM_STATE_DISCONNECTED)
        {
            return;
        }
        else
        {
            pcm_bt_trace("open_hwdev forcing reopen for disconnected handle on %s", open_device);
        }
    }

    /* Close old handle */
    close_hwdev();

    for (i = 0; i < attempts; i++)
    {
        err = snd_pcm_open(&handle, open_device, mode, 0);
        if (err >= 0)
            break;

        if (!is_bluealsa_device(open_device) || !is_alsa_transient_error(err) || i == attempts - 1)
            break;

        logf("opendev retry %d/%d for %s: %s",
             i + 1, attempts, open_device, snd_strerror(err));
        pcm_bt_trace("open_hwdev retry %d/%d device=%s err=%d (%s)",
                     i + 1, attempts, open_device, err, snd_strerror(err));
        usleep(delay_us);
    }

    if (err < 0)
    {
        if (mode == SND_PCM_STREAM_PLAYBACK &&
            is_bluealsa_device(open_device) &&
            (err == -ENOENT || err == -ENODEV))
        {
            pcm_bt_trace("open_hwdev bluealsa unavailable (%d: %s), fallback to %s",
                         err, snd_strerror(err), DEFAULT_PLAYBACK_DEVICE);
            logf("bluealsa unavailable, fallback to %s", DEFAULT_PLAYBACK_DEVICE);
            set_playback_device_string(DEFAULT_PLAYBACK_DEVICE);
            snprintf(open_device_copy, sizeof(open_device_copy), "%s",
                     DEFAULT_PLAYBACK_DEVICE);
            open_device = open_device_copy;

            err = snd_pcm_open(&handle, open_device, mode, 0);
            if (err >= 0)
                goto open_ok;

            pcm_bt_trace("open_hwdev fallback failed device=%s err=%d (%s)",
                         open_device, err, snd_strerror(err));
        }

        pcm_bt_trace("open_hwdev failed device=%s err=%d (%s)",
                     open_device, err, snd_strerror(err));
        panicf("%s(): Cannot open device %s: %s", __func__, open_device, snd_strerror(err));
    }

open_ok:
    last_sample_rate = 0;

    if (!poll_thread_running)
    {
        poll_thread_stop = false;
        err = pthread_create(&poll_thread, NULL, poll_thread_fn, NULL);
        if (err == 0)
            poll_thread_running = true;
        else
            panicf("Unable to start ALSA poll thread: %s", strerror(err));
    }

#ifdef HAVE_RECORDING
    current_alsa_mode = mode;
#else
    (void)mode;
#endif
    snprintf(current_alsa_device_buf, sizeof(current_alsa_device_buf), "%s", open_device);
    current_alsa_device = current_alsa_device_buf;
    pcm_bt_trace("open_hwdev complete device=%s poll_thread_running=%d",
                 current_alsa_device, poll_thread_running ? 1 : 0);

    atexit(alsadev_cleanup);
}

void pcm_play_dma_init(void)
{
    logf("PCM DMA Init");

    audiohw_preinit();
    ensure_pcm_mutex_initialized();

    open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);

    return;
}

void pcm_play_lock(void)
{
    ensure_pcm_mutex_initialized();
    pthread_mutex_lock(&pcm_mtx);
}

void pcm_play_unlock(void)
{
    pthread_mutex_unlock(&pcm_mtx);
}

static void pcm_dma_apply_settings_nolock(void)
{
    int err = 0;
    int retries = is_bluealsa_device(current_alsa_device) ? 20 : 1;
    int reopen_tries = is_bluealsa_device(current_alsa_device) ? 8 : 1;
    int i;
    bool params_uninitialized = !pcm_params_ready();
    char reopen_device[160] = {0};

    logf("PCM DMA Settings %d %lu", last_sample_rate, pcm_sampr);

    if (pcm_sampr == 0)
    {
        if (params_uninitialized)
        {
            pcm_bt_trace("pcm_dma_apply_settings deferred: samplerate=0 device=%s",
                         current_alsa_device ? current_alsa_device : "<null>");
        }
        return;
    }

    if (last_sample_rate != pcm_sampr || params_uninitialized)
    {
        last_sample_rate = pcm_sampr;

#ifdef AUDIOHW_MUTE_ON_SRATE_CHANGE
        audiohw_mute(true);
#endif
        snd_pcm_drop(handle);

        for (i = 0; i < retries; i++)
        {
            err = set_hwparams(handle);
            if (err == 0)
                err = set_swparams(handle);
            if (err == 0)
                break;

            if (!is_bluealsa_device(current_alsa_device) || !is_alsa_transient_error(err) || i == retries - 1)
                break;

            logf("PCM DMA apply retry %d/%d for %s: %s",
                 i + 1, retries,
                 current_alsa_device ? current_alsa_device : "<null>",
                 snd_strerror(err));
            snd_pcm_prepare(handle);
            usleep(120000);
        }

        if (err < 0 && is_bluealsa_device(current_alsa_device))
        {
            if (current_alsa_device && *current_alsa_device)
                snprintf(reopen_device, sizeof(reopen_device), "%s", current_alsa_device);
            else if (playback_dev && *playback_dev)
                snprintf(reopen_device, sizeof(reopen_device), "%s", playback_dev);

            for (i = 0; i < reopen_tries; i++)
            {
                pcm_bt_trace("pcm_dma_apply_settings bluealsa reopen try %d/%d",
                             i + 1, reopen_tries);
                open_hwdev(reopen_device[0] ? reopen_device : DEFAULT_PLAYBACK_DEVICE,
                           SND_PCM_STREAM_PLAYBACK);

                err = set_hwparams(handle);
                if (err == 0)
                    err = set_swparams(handle);
                if (err == 0)
                    break;

                usleep(120000);
            }
        }

        if (err < 0)
        {
            logf("PCM DMA apply failed for %s: %s",
                 current_alsa_device ? current_alsa_device : "<null>",
                 snd_strerror(err));
            pcm_bt_trace("pcm_dma_apply_settings failed device=%s err=%d (%s)",
                         current_alsa_device ? current_alsa_device : "<null>",
                         err, snd_strerror(err));
        }
        else
        {
            pcm_bt_trace("pcm_dma_apply_settings ok device=%s srate=%lu period=%ld buffer=%ld",
                         current_alsa_device ? current_alsa_device : "<null>",
                         pcm_sampr, period_size, buffer_size);
        }

#if defined(HAVE_NWZ_LINUX_CODEC)
        /* Sony NWZ linux driver uses a nonstandard mecanism to set the sampling rate */
        audiohw_set_frequency(pcm_sampr);
#endif
        /* (Will be unmuted by pcm resuming) */
    }
}

void pcm_dma_apply_settings(void)
{
    pcm_play_lock();
    pcm_dma_apply_settings_nolock();
    pcm_play_unlock();
}

void pcm_play_dma_stop(void)
{
    logf("PCM DMA stop (%d)", snd_pcm_state(handle));

    int err = snd_pcm_drain(handle);
    if (err < 0)
        logf("Drain failed: %s", snd_strerror(err));
#ifdef AUDIOHW_MUTE_ON_STOP
    audiohw_mute(true);
#endif
}

void pcm_play_dma_start(const void *addr, size_t size)
{
    logf("PCM DMA start (%p %d) poll=1", addr, size);
    pcm_bt_trace("pcm_play_dma_start addr=%p size=%u poll=1 device=%s",
                 addr, (unsigned)size,
                 current_alsa_device ? current_alsa_device : "<null>");

    /* BlueALSA transports can become stale between route switch and first
     * playback write; reopen to bind a fresh transport at stream start.
     */
    if (is_bluealsa_device(playback_dev))
        open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
    else
        pcm_bt_trace("pcm_play_dma_start using non-bluealsa playback_dev=%s",
                     playback_dev ? playback_dev : "<null>");

    pcm_dma_apply_settings_nolock();
    if (!pcm_params_ready())
    {
        pcm_bt_trace("pcm_play_dma_start abort: params not ready srate=%lu period=%ld buffer=%ld frames=%p",
                     pcm_sampr, period_size, buffer_size, frames);
        return;
    }

    pcm_data = addr;
    pcm_size = size;

#if !defined(AUDIOHW_MUTE_ON_STOP) && defined(AUDIOHW_MUTE_ON_SRATE_CHANGE)
    audiohw_mute(false);
#endif

    while (1)
    {
        snd_pcm_state_t state = snd_pcm_state(handle);
        logf("PCM State %d", state);

        switch (state)
        {
            case SND_PCM_STATE_RUNNING:
#if defined(AUDIOHW_MUTE_ON_STOP)
                audiohw_mute(false);
#endif
                pcm_bt_trace("pcm_play_dma_start -> RUNNING");
                return;
            case SND_PCM_STATE_XRUN:
            {
                logf("Trying to recover from underrun");
                int err = snd_pcm_recover(handle, -EPIPE, 0);
                if (err < 0)
                    logf("Recovery failed: %s", snd_strerror(err));
                pcm_bt_trace("pcm_play_dma_start XRUN recover err=%d", err);
                continue;
            }
            case SND_PCM_STATE_SETUP:
            {
                int err = snd_pcm_prepare(handle);
                if (err < 0)
                    logf("Prepare error: %s", snd_strerror(err));
                pcm_bt_trace("pcm_play_dma_start prepare err=%d", err);
            }
                /* fall through */
            case SND_PCM_STATE_PREPARED:
            {
                int err;
#if 0
                /* fill buffer with silence to initiate playback without noisy click */
                snd_pcm_sframes_t sample_size = buffer_size;
                sample_t *samples = calloc(1, sample_size * channels * sizeof(sample_t));

                snd_pcm_format_set_silence(format, samples, sample_size);
                err = snd_pcm_writei(handle, samples, sample_size);
                free(samples);

                if (err != (ssize_t)sample_size)
                {
                    logf("Initial write error: written %i expected %li", err, sample_size);
                    return;
                }
#else
                /* Fill buffer with proper sample data */
                while (snd_pcm_avail_update(handle) >= period_size)
                {
                    if (copy_frames(true))
                    {
                        err = snd_pcm_writei(handle, frames, period_size);
                        if (err < 0 && err != period_size && err != -EAGAIN)
                        {
                            logf("Write error: written %i expected %li", err, period_size);
                            pcm_bt_trace("pcm_play_dma_start initial write err=%d expected=%ld",
                                         err, period_size);
                            break;
                        }
                    }
                }
#endif
                err = snd_pcm_start(handle);
                if (err < 0) {
                    logf("start error: %s", snd_strerror(err));
                    pcm_bt_trace("pcm_play_dma_start snd_pcm_start err=%d (%s)",
                                 err, snd_strerror(err));
                    /* We will recover on the next iteration */
                }
                else
                {
                    pcm_bt_trace("pcm_play_dma_start snd_pcm_start ok");
                }

                break;
            }
            case SND_PCM_STATE_DRAINING:
                /* run until drained */
                continue;
            default:
                logf("Unhandled state: %s", snd_pcm_state_name(state));
                return;
        }
    }
}

void pcm_play_dma_postinit(void)
{
    audiohw_postinit();

#ifdef AUDIOHW_NEEDS_INITIAL_UNMUTE
    audiohw_mute(false);
#endif
}

unsigned int pcm_alsa_get_rate(void)
{
    return real_sample_rate;
}

unsigned int pcm_alsa_get_xruns(void)
{
    return xruns;
}

const char *pcm_alsa_get_playback_device(void)
{
    return playback_dev;
}

#ifdef HAVE_RECORDING
void pcm_rec_lock(void)
{
    pcm_play_lock();
}

void pcm_rec_unlock(void)
{
    pcm_play_unlock();
}

void pcm_rec_dma_init(void)
{
    logf("PCM REC DMA Init");

    open_hwdev(capture_dev, SND_PCM_STREAM_CAPTURE);
}

void pcm_rec_dma_close(void)
{
    logf("Rec DMA Close");
    // close_hwdev();
    open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
}

void pcm_rec_dma_start(void *start, size_t size)
{
    logf("PCM REC DMA start (%p %d)", start, size);
    pcm_dma_apply_settings_nolock();
    pcm_data_rec = start;
    pcm_size = size;

    if (!handle) return;

    while (1)
    {
        snd_pcm_state_t state = snd_pcm_state(handle);

        switch (state)
        {
            case SND_PCM_STATE_RUNNING:
                return;
            case SND_PCM_STATE_XRUN:
            {
                logf("Trying to recover from error");
                int err = snd_pcm_recover(handle, -EPIPE, 0);
                if (err < 0)
                    panicf("Recovery failed: %s", snd_strerror(err));
                continue;
            }
            case SND_PCM_STATE_SETUP:
            {
                int err = snd_pcm_prepare(handle);
                if (err < 0)
                    panicf("Prepare error: %s", snd_strerror(err));
            }
                /* fall through */
            case SND_PCM_STATE_PREPARED:
            {
                int err = snd_pcm_start(handle);
                if (err < 0)
                    panicf("Start error: %s", snd_strerror(err));
                return;
            }
            case SND_PCM_STATE_DRAINING:
                /* run until drained */
                continue;
            default:
                logf("Unhandled state: %s", snd_pcm_state_name(state));
                return;
        }
    }
}

void pcm_rec_dma_stop(void)
{
    logf("Rec DMA Stop");
    close_hwdev();
}

const void * pcm_rec_dma_get_peak_buffer(void)
{
    uintptr_t addr = (uintptr_t)pcm_data_rec;
    return (void*)((addr + 3) & ~3);
}

#ifdef SIMULATOR
void audiohw_set_recvol(int left, int right, int type)
{
    (void)left;
    (void)right;
    (void)type;
}
#endif

#endif /* HAVE_RECORDING */
