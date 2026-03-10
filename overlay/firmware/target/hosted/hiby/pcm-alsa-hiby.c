/***************************************************************************
 * HiBy-specific helpers for the hosted ALSA PCM backend
 ****************************************************************************/

#include "autoconf.h"

#include "pcm-alsa-hiby.h"

#include <string.h>

bool hiby_pcm_is_bluealsa_device(const char *device)
{
    return device && strstr(device, "bluealsa:") != NULL;
}

void hiby_pcm_adjust_bt_buffering(snd_pcm_sframes_t *period_size,
                                  snd_pcm_sframes_t *buffer_size,
                                  bool bluealsa_active)
{
    if (!bluealsa_active || !period_size || !buffer_size)
        return;

    *period_size *= 2;
    if (*period_size < 2048)
        *period_size = 2048;
    *buffer_size = *period_size * 8;
}

unsigned int hiby_pcm_calc_poll_interval(unsigned int sample_rate,
                                         snd_pcm_sframes_t period_size,
                                         bool bluealsa_active)
{
    unsigned int interval_us;

    if (sample_rate == 0 || period_size <= 0)
        return 10000;

    interval_us = (unsigned int)((period_size * 1000000ULL) / sample_rate);
    if (bluealsa_active)
        interval_us /= 2;
    if (interval_us < 2000)
        interval_us = 2000;
    if (interval_us > 50000)
        interval_us = 50000;
    return interval_us;
}

snd_pcm_uframes_t hiby_pcm_start_threshold(snd_pcm_sframes_t buffer_size,
                                           snd_pcm_sframes_t period_size,
                                           bool bluealsa_active)
{
    if (bluealsa_active && buffer_size > period_size)
        return (snd_pcm_uframes_t)(buffer_size - period_size);

    return (snd_pcm_uframes_t)(buffer_size / 2);
}
