/***************************************************************************
 * HiBy-specific hooks for the shared hosted ALSA backend.
 *
 * This header is included directly by pcm-alsa.c so the poll-thread
 * scheduler can reuse the shared pump logic without forking the backend.
 ****************************************************************************/

#ifndef PCM_ALSA_HIBY_HOOKS_H
#define PCM_ALSA_HIBY_HOOKS_H

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "pcm-internal.h"
#include "pcm_sink.h"
#include "pcm-alsa-hiby.h"

static void open_hwdev(const char *device, snd_pcm_stream_t mode);
static void pcm_pump_locked(snd_pcm_t *handle);
static void sink_dma_init(void);
static void sink_dma_postinit(void);
static void sink_lock(void);
static void sink_unlock(void);
static void sink_set_freq_nolock(uint16_t freq);
static void sink_dma_start(const void *addr, size_t size);
static void sink_dma_stop(void);

static pthread_t hiby_pcm_poll_thread;
static volatile bool hiby_pcm_poll_thread_stop = false;
static bool hiby_pcm_poll_thread_running = false;
static bool hiby_pcm_mutex_initialized = false;
static unsigned int hiby_pcm_poll_interval_us = 10000;

static void hiby_pcm_mutex_init_once(void)
{
    pthread_mutexattr_t attr;

    if (hiby_pcm_mutex_initialized)
        return;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&pcm_mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    hiby_pcm_mutex_initialized = true;
}

static bool hiby_pcm_bt_active(void)
{
    const char *device = current_alsa_device ? current_alsa_device : playback_dev;
    return hiby_pcm_is_bluealsa_device(device);
}

static bool hiby_pcm_params_ready(void)
{
    return period_size > 0 && buffer_size > 0 && frames != NULL;
}

static const char *hiby_pcm_target_device(bool bt_sink)
{
    const char *device = bt_sink ? hiby_pcm_get_bt_device() : NULL;

    if (device && device[0])
        return device;

    return DEFAULT_PLAYBACK_DEVICE;
}

static void hiby_sink_set_freq(bool bt_sink, uint16_t freq)
{
    const char *device = hiby_pcm_target_device(bt_sink);

    sink_lock();
    if (!(handle && current_alsa_device == device
#ifdef HAVE_RECORDING
          && current_alsa_mode == SND_PCM_STREAM_PLAYBACK
#endif
         ))
    {
        open_hwdev(device, SND_PCM_STREAM_PLAYBACK);
    }
    sink_set_freq_nolock(freq);
    sink_unlock();
}

static void hiby_bt_sink_init(void)
{
}

static void hiby_bt_sink_postinit(void)
{
}

static void hiby_builtin_sink_set_freq(uint16_t freq)
{
    hiby_sink_set_freq(false, freq);
}

static void hiby_bt_sink_set_freq(uint16_t freq)
{
    hiby_sink_set_freq(true, freq);
}

struct pcm_sink hiby_bt_pcm_sink = {
    .caps = {
        .samprs       = hw_freq_sampr,
        .num_samprs   = HW_NUM_FREQ,
        .default_freq = HW_FREQ_DEFAULT,
    },
    .ops = {
        .init     = hiby_bt_sink_init,
        .postinit = hiby_bt_sink_postinit,
        .set_freq = hiby_bt_sink_set_freq,
        .lock     = sink_lock,
        .unlock   = sink_unlock,
        .play     = sink_dma_start,
        .stop     = sink_dma_stop,
    },
};

static bool hiby_pcm_keep_hwdev(const char *device, snd_pcm_stream_t mode)
{
#ifndef HAVE_RECORDING
    (void)mode;
#endif

    if (!(handle && device == current_alsa_device
#ifdef HAVE_RECORDING
          && current_alsa_mode == mode
#endif
         ))
    {
        return false;
    }

    return snd_pcm_state(handle) != SND_PCM_STATE_DISCONNECTED;
}

static void hiby_pcm_stop_poll_thread(void)
{
    if (!hiby_pcm_poll_thread_running)
        return;

    hiby_pcm_poll_thread_stop = true;
    pthread_join(hiby_pcm_poll_thread, NULL);
    hiby_pcm_poll_thread_running = false;
}

static void *hiby_pcm_poll_thread_fn(void *arg)
{
    (void)arg;

    while (!hiby_pcm_poll_thread_stop)
    {
        if (handle && pthread_mutex_trylock(&pcm_mtx) == 0)
        {
            pcm_pump_locked(handle);
            pthread_mutex_unlock(&pcm_mtx);
        }
        usleep(hiby_pcm_poll_interval_us);
    }

    return NULL;
}

static void hiby_pcm_start_poll_thread(void)
{
    int err;

    hiby_pcm_mutex_init_once();
    if (hiby_pcm_poll_thread_running)
        return;

    hiby_pcm_poll_thread_stop = false;
    err = pthread_create(&hiby_pcm_poll_thread, NULL, hiby_pcm_poll_thread_fn, NULL);
    if (err == 0)
        hiby_pcm_poll_thread_running = true;
    else
        panicf("Unable to start ALSA poll thread: %s", strerror(err));
}

#endif /* PCM_ALSA_HIBY_HOOKS_H */
