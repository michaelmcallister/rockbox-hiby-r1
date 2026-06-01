/***************************************************************************
 * HiBy-specific helpers for the hosted ALSA PCM backend
 ****************************************************************************/

#ifndef PCM_ALSA_HIBY_H
#define PCM_ALSA_HIBY_H

#include <stdbool.h>
#include <alsa/asoundlib.h>

/* Bluetooth MAC address management */
void hiby_pcm_set_bt_mac(const char *mac);
const char *hiby_pcm_get_bt_mac(void);

/* Device type detection */
bool hiby_pcm_is_bluealsa_device(const char *device);

/* Runtime device switching for Bluetooth */
int pcm_alsa_switch_playback_device(const char *device);

/* Fallback device selection when snd_pcm_open() fails (strong override of
 * the weak default in pcm-alsa.c). Returns a device to retry, or NULL to
 * let the caller treat the failure as fatal. */
const char *pcm_alsa_open_fallback(const char *device, int err);

#endif /* PCM_ALSA_HIBY_H */
