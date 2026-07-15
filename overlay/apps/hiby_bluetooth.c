/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/_ \ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026
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

#include "config.h"

#ifdef HIBY_LINUX

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>

#include "kernel.h"
#include "audio.h"
#include "metadata.h"   /* full struct mp3entry (elapsed/offset) for route restart */
#include "menu.h"
#include "splash.h"
#include "gui/list.h"
#include "pcm-alsa.h"

/* HiBy hosted build provides dynamic output routing helper in its
 * target-specific PCM implementation. */
int pcm_alsa_switch_playback_device(const char *device);
void hiby_pcm_set_bt_mac(const char *mac);

#define BT_MAX_DEVICES 32
#define BT_NAME_LEN 80
#define BT_LOCAL_PLAYBACK_DEVICE "plughw:0,0"
#define BT_SYS_SOCKET "/var/run/sys_server"
#define BT_LIST_FILE "/data/bt_list.txt"
#define BT_SCAN_FILE "/data/bt_scan.txt"
#define BT_SYS_REPLY_MAX 128
#define BT_DEVICE_PICK_CANCEL (-1)
#define BT_DEVICE_PICK_SCAN (-2)
#define BT_SCAN_MENU_LABEL "Scan for new devices"

struct bt_device
{
    char mac[18];
    char name[BT_NAME_LEN];
    bool paired;
};

struct bt_device_menu_data
{
    struct bt_device *devices;
    int count;
    bool include_scan_item;
};

static char bt_selected_mac[18];
static const char *bt_playback_dev = BT_LOCAL_PLAYBACK_DEVICE;
/* Single stable buffer for the bluealsa device string. (Was a 2-slot
 * rotating buffer, which gave the same logical device a different pointer
 * between calls and confused pointer-based device comparisons downstream.
 * We only ever route one device at a time, so one buffer is correct.) */
static char bt_bt_playback_dev[96];

static bool bt_wait_for_bluealsa_pcm(const char *mac, int timeout_ticks);
static bool bt_bluealsa_pcm_ready(const char *mac);
static bool bt_is_connected(const char *mac);
static bool bt_prepare_stack(void);
static void bt_connect_device(const struct bt_device *device);

static const char *bt_make_bt_playback_dev(const char *mac)
{
    snprintf(bt_bt_playback_dev, sizeof(bt_bt_playback_dev),
             "bluealsa:DEV=%s,PROFILE=a2dp", mac);
    return bt_bt_playback_dev;
}

static int bt_simplelist_ok_cancel(int action, struct gui_synclist *lists)
{
    (void)lists;
    if (action == ACTION_STD_OK)
        return ACTION_STD_CANCEL;
    return action;
}

static const char *bt_action_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    const char **items = data;
    snprintf(buffer, buffer_len, "%s", items[selected_item]);
    return buffer;
}

static const char *bt_device_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    struct bt_device_menu_data *ctx = data;

    if (ctx->include_scan_item)
    {
        if (selected_item == 0)
        {
            snprintf(buffer, buffer_len, "%s", BT_SCAN_MENU_LABEL);
            return buffer;
        }
        selected_item--;
    }

    if (selected_item < 0 || selected_item >= ctx->count)
    {
        buffer[0] = '\0';
        return buffer;
    }

    snprintf(buffer, buffer_len, "%s (%s)%s",
        ctx->devices[selected_item].name,
        ctx->devices[selected_item].mac,
        ctx->devices[selected_item].paired ? " [Paired]" : "");
    return buffer;
}

static void bt_trim(char *s)
{
    size_t len;

    if (!s)
        return;

    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || isspace((unsigned char)s[len - 1])))
        s[--len] = '\0';
}

static bool bt_has_mac_pattern(const char *p, char sep)
{
    int i;

    for (i = 0; i < 17; i++)
    {
        if ((i % 3) == 2)
        {
            if (p[i] != sep)
                return false;
        }
        else if (!isxdigit((unsigned char)p[i]))
        {
            return false;
        }
    }

    return true;
}

static bool bt_extract_mac_from_line(const char *line, char *mac_out, size_t mac_out_len)
{
    size_t i, len;

    if (!line || mac_out_len < 18)
        return false;

    len = strlen(line);
    if (len < 17)
        return false;

    for (i = 0; i + 17 <= len; i++)
    {
        char sep = line[i + 2];
        if (sep != ':' && sep != '_')
            continue;
        if (!bt_has_mac_pattern(&line[i], sep))
            continue;

        snprintf(mac_out, mac_out_len, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c",
            toupper((unsigned char)line[i + 0]), toupper((unsigned char)line[i + 1]),
            toupper((unsigned char)line[i + 3]), toupper((unsigned char)line[i + 4]),
            toupper((unsigned char)line[i + 6]), toupper((unsigned char)line[i + 7]),
            toupper((unsigned char)line[i + 9]), toupper((unsigned char)line[i + 10]),
            toupper((unsigned char)line[i + 12]), toupper((unsigned char)line[i + 13]),
            toupper((unsigned char)line[i + 15]), toupper((unsigned char)line[i + 16]));
        return true;
    }

    return false;
}

static void bt_mac_to_underscore(const char *mac, char *out, size_t out_len)
{
    size_t i, n = 0;

    for (i = 0; mac[i] != '\0' && n + 1 < out_len; i++)
    {
        out[n++] = (mac[i] == ':') ? '_' : mac[i];
    }
    out[n] = '\0';
}

/* Resolve a writable directory for persistent BT state (codec choice,
 * debug log). Prefers the SD .rockbox directory where the rest of the
 * Rockbox config lives, falling back to the persistent data partition. */
static const char *bt_state_dir(void)
{
    static char dir[128];
    static bool resolved = false;
    static const char *const candidates[] = {
        "/usr/data/mnt/sd_0/.rockbox",
        "/mnt/sd_0/.rockbox",
        "/data",
        NULL,
    };

    if (!resolved)
    {
        struct stat st;
        int i;

        dir[0] = '\0';
        for (i = 0; candidates[i]; i++)
        {
            if (stat(candidates[i], &st) == 0 && S_ISDIR(st.st_mode))
            {
                snprintf(dir, sizeof(dir), "%s", candidates[i]);
                break;
            }
        }
        if (!dir[0])
            snprintf(dir, sizeof(dir), "/data");
        resolved = true;
    }

    return dir;
}

static void bt_dbg(const char *fmt, ...)
{
    char path[160];
    FILE *fp;
    va_list ap;

    snprintf(path, sizeof(path), "%s/bt_debug.log", bt_state_dir());
    fp = fopen(path, "a");
    if (!fp)
        return;

    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

/* Persist the most recently connected device so the user can reconnect with
 * one tap (mirrors the stock app's /data/bt_lastused.txt behaviour). Stored as
 * a single "MAC\tName" line in the resolved state dir. */
static void bt_save_last_device(const char *mac, const char *name)
{
    char path[160];
    FILE *fp;

    if (!mac || !mac[0])
        return;

    snprintf(path, sizeof(path), "%s/bt_lastused.txt", bt_state_dir());
    fp = fopen(path, "w");
    if (!fp)
        return;

    fprintf(fp, "%s\t%s\n", mac, (name && name[0]) ? name : "Bluetooth");
    fclose(fp);
}

/* Load the last connected device. Returns false if none recorded. The device
 * is marked paired=true: it connected successfully before, so the connect path
 * skips the pair step. */
static bool bt_load_last_device(struct bt_device *out)
{
    char path[160];
    char line[BT_NAME_LEN + 24];
    char *tab;
    FILE *fp;

    if (!out)
        return false;

    snprintf(path, sizeof(path), "%s/bt_lastused.txt", bt_state_dir());
    fp = fopen(path, "r");
    if (!fp)
        return false;

    if (!fgets(line, sizeof(line), fp))
    {
        fclose(fp);
        return false;
    }
    fclose(fp);

    line[strcspn(line, "\r\n")] = '\0';
    tab = strchr(line, '\t');
    if (tab)
        *tab = '\0';

    if (!line[0])
        return false;

    memset(out, 0, sizeof(*out));
    snprintf(out->mac, sizeof(out->mac), "%s", line);
    snprintf(out->name, sizeof(out->name), "%s",
             (tab && tab[1]) ? tab + 1 : "Bluetooth");
    out->paired = true;
    return true;
}

static bool bt_is_codec_word_char(char c)
{
    return isalnum((unsigned char)c) || c == '-';
}

/* Read the codec bluealsa has actually negotiated for the sink, e.g.
 * from a "Selected codec: SBC" line. Returns false if unavailable. */
static bool bt_read_active_codec(const char *mac, char *out, size_t out_len)
{
    char mac_u[18];
    char pcm_path[96];
    char cmd[256];
    char line[256];
    FILE *fp;
    bool found = false;

    if (!mac || !*mac || !out || out_len == 0)
        return false;

    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(pcm_path, sizeof(pcm_path),
             "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", mac_u);
    snprintf(cmd, sizeof(cmd),
             "bluealsa-cli codec '%s' 2>/dev/null", pcm_path);

    fp = popen(cmd, "r");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp))
    {
        char *colon;

        if (!strstr(line, "elected"))   /* "Selected codec:" */
            continue;

        colon = strchr(line, ':');
        if (!colon)
            continue;

        colon++;
        while (*colon == ' ' || *colon == '\t')
            colon++;

        {
            char *end = colon;
            while (*end && bt_is_codec_word_char(*end))
                end++;
            *end = '\0';
        }

        if (*colon)
        {
            snprintf(out, out_len, "%s", colon);
            found = true;
        }
        break;
    }

    pclose(fp);
    return found;
}


static int bt_add_device_unique_ex(struct bt_device *devices, int count, int max_devices,
    const char *mac, const char *name, bool paired)
{
    int i;

    if (!mac || !mac[0] || count >= max_devices)
        return count;

    for (i = 0; i < count; i++)
    {
        if (!strcasecmp(devices[i].mac, mac))
        {
            if (paired)
                devices[i].paired = true;
            if (name && name[0] &&
                (!devices[i].name[0] || !strcasecmp(devices[i].name, devices[i].mac)))
            {
                snprintf(devices[i].name, sizeof(devices[i].name), "%s", name);
            }
            return count;
        }
    }

    snprintf(devices[count].mac, sizeof(devices[count].mac), "%s", mac);
    if (name && name[0])
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", name);
    else
        snprintf(devices[count].name, sizeof(devices[count].name), "%s", mac);
    devices[count].paired = paired;

    return count + 1;
}

static int bt_device_sort_cmp(const void *a, const void *b)
{
    const struct bt_device *da = a;
    const struct bt_device *db = b;

    if (da->paired != db->paired)
        return da->paired ? -1 : 1;

    return strcasecmp(da->name, db->name);
}

static int bt_sys_command(const char *command, char *reply, size_t reply_size)
{
    struct sockaddr_un addr;
    int fd = -1;
    ssize_t n;

    if (!command || !command[0])
        return -1;

    if (reply && reply_size > 0)
        reply[0] = '\0';

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", BT_SYS_SOCKET);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (send(fd, command, strlen(command), 0) < 0)
    {
        close(fd);
        return -1;
    }

    if (reply && reply_size > 1)
    {
        n = recv(fd, reply, reply_size - 1, 0);
        if (n < 0)
        {
            close(fd);
            return -1;
        }

        reply[n] = '\0';
        bt_trim(reply);
    }

    close(fd);
    return 0;
}

static bool bt_sys_reply_ok(const char *reply, const char *command_prefix)
{
    char ok_reply[48];
    char wait_reply[48];

    if (!reply || !reply[0])
        return false;

    if (!strcasecmp(reply, "OK") || !strcasecmp(reply, "WAITINIT"))
        return true;

    if (strstr(reply, "FAIL"))
        return false;

    if (!command_prefix || !command_prefix[0])
        return false;

    snprintf(ok_reply, sizeof(ok_reply), "%s:OK", command_prefix);
    snprintf(wait_reply, sizeof(wait_reply), "%s:WAITINIT", command_prefix);

    if (strstr(reply, ok_reply))
        return true;
    if (strstr(reply, wait_reply))
        return true;

    return false;
}

static bool bt_json_get_string_value(const char *line, const char *key,
                                     char *out, size_t out_len)
{
    const char *start;
    const char *colon;
    const char *q1;
    const char *q2;
    size_t len;

    if (!line || !key || !out || out_len == 0)
        return false;

    start = strstr(line, key);
    if (!start)
        return false;

    colon = strchr(start, ':');
    if (!colon)
        return false;

    q1 = strchr(colon, '"');
    if (!q1)
        return false;
    q1++;

    q2 = strchr(q1, '"');
    if (!q2)
        return false;

    len = (size_t)(q2 - q1);
    if (len >= out_len)
        len = out_len - 1;
    memcpy(out, q1, len);
    out[len] = '\0';
    return true;
}

static bool bt_json_get_int_value(const char *line, const char *key, int *value)
{
    const char *start;
    const char *colon;
    const char *p;

    if (!line || !key || !value)
        return false;

    start = strstr(line, key);
    if (!start)
        return false;

    colon = strchr(start, ':');
    if (!colon)
        return false;

    p = colon + 1;
    while (*p == ' ' || *p == '\t')
        p++;

    if (!isdigit((unsigned char)*p) && *p != '-')
        return false;

    *value = atoi(p);
    return true;
}

static bool bt_get_file_stamp(const char *path, long *mtime, long *size)
{
    struct stat st;

    if (!path || !mtime || !size)
        return false;

    if (stat(path, &st) < 0)
        return false;

    *mtime = (long)st.st_mtime;
    *size = (long)st.st_size;
    return true;
}

static int bt_load_devices_from_json_file(const char *path,
                                          struct bt_device *devices,
                                          int max_devices,
                                          bool *ready)
{
    FILE *fp;
    char line[512];
    char mac[18] = "";
    char name[BT_NAME_LEN] = "";
    int paired = 0;
    int count = 0;
    bool has_device_key = false;

    if (ready)
        *ready = false;

    fp = fopen(path, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, "\"DEVICE\""))
            has_device_key = true;

        if (strstr(line, "\"MAC\""))
            bt_extract_mac_from_line(line, mac, sizeof(mac));

        if (bt_json_get_string_value(line, "\"Name\"", name, sizeof(name)))
            bt_trim(name);

        bt_json_get_int_value(line, "\"Paired\"", &paired);

        if (mac[0] && strchr(line, '}'))
        {
            count = bt_add_device_unique_ex(devices, count, max_devices,
                                            mac, name, paired != 0);
            mac[0] = '\0';
            name[0] = '\0';
            paired = 0;
        }
    }

    fclose(fp);

    if (ready)
        *ready = has_device_key;

    return count;
}

static int bt_load_devices_from_bt_list_file(struct bt_device *devices, int max_devices,
                                             bool *ready)
{
    return bt_load_devices_from_json_file(BT_LIST_FILE, devices, max_devices, ready);
}

static int bt_merge_devices_from_bt_scan_file(struct bt_device *devices, int count,
                                              int max_devices, bool *ready)
{
    struct bt_device scanned[BT_MAX_DEVICES];
    int scanned_count;
    int i;

    scanned_count = bt_load_devices_from_json_file(BT_SCAN_FILE, scanned,
                                                   BT_MAX_DEVICES, ready);
    for (i = 0; i < scanned_count; i++)
    {
        count = bt_add_device_unique_ex(devices, count, max_devices,
                                        scanned[i].mac, scanned[i].name,
                                        scanned[i].paired);
    }

    return count;
}

static int bt_load_devices_via_sys_list(struct bt_device *devices, int max_devices)
{
    char reply[BT_SYS_REPLY_MAX];
    long old_mtime = 0;
    long old_size = 0;
    bool had_old_stamp = false;
    bool ready = false;
    bool changed = false;
    int count = 0;
    int waited = 0;

    had_old_stamp = bt_get_file_stamp(BT_LIST_FILE, &old_mtime, &old_size);
    if (bt_sys_command("BT:LIST", reply, sizeof(reply)) < 0)
        return 0;

    while (waited < HZ * 3)
    {
        long mtime = 0;
        long size = 0;

        count = bt_load_devices_from_bt_list_file(devices, max_devices, &ready);

        if (bt_get_file_stamp(BT_LIST_FILE, &mtime, &size))
        {
            if (!had_old_stamp || mtime != old_mtime || size != old_size)
                changed = true;
        }

        if (ready && (changed || waited >= HZ))
            break;

        sleep(HZ / 5);
        waited += HZ / 5;
    }

    if (count > 1)
        qsort(devices, count, sizeof(devices[0]), bt_device_sort_cmp);
    return count;
}

static int bt_scan_and_merge_devices(struct bt_device *devices, int count, int max_devices)
{
    char reply[BT_SYS_REPLY_MAX];
    long old_mtime = 0;
    long old_size = 0;
    bool had_old_stamp;
    bool changed = false;
    bool ready = false;
    int waited = 0;
    int post_waited = 0;

    had_old_stamp = bt_get_file_stamp(BT_SCAN_FILE, &old_mtime, &old_size);

    if (bt_sys_command("BT:SCAN", reply, sizeof(reply)) < 0 ||
        !bt_sys_reply_ok(reply, "BT:SCAN"))
        return count;

    while (waited < HZ * 8)
    {
        long mtime = 0;
        long size = 0;

        if (bt_get_file_stamp(BT_SCAN_FILE, &mtime, &size))
        {
            if (!had_old_stamp || mtime != old_mtime || size != old_size)
            {
                changed = true;
                break;
            }
        }

        sleep(HZ / 5);
        waited += HZ / 5;
    }

    bt_sys_command("BT:CANCEL_SCAN", reply, sizeof(reply));

    while (post_waited < HZ * 3)
    {
        long mtime = 0;
        long size = 0;

        if (bt_get_file_stamp(BT_SCAN_FILE, &mtime, &size))
        {
            if (!had_old_stamp || mtime != old_mtime || size != old_size)
                changed = true;
            if (changed)
            {
                count = bt_merge_devices_from_bt_scan_file(devices, count,
                                                           max_devices, &ready);
                if (ready)
                    break;
            }
        }

        sleep(HZ / 5);
        post_waited += HZ / 5;
    }

    if (!changed)
        count = bt_merge_devices_from_bt_scan_file(devices, count, max_devices, NULL);

    if (count > 1)
        qsort(devices, count, sizeof(devices[0]), bt_device_sort_cmp);
    return count;
}

static int bt_choose_device(const char *title, struct bt_device *devices, int count,
                            bool include_scan_item)
{
    struct bt_device_menu_data data;
    struct simplelist_info info;
    int total_count = count + (include_scan_item ? 1 : 0);

    if (total_count <= 0)
    {
        splash(HZ, "No devices");
        return BT_DEVICE_PICK_CANCEL;
    }

    data.devices = devices;
    data.count = count;
    data.include_scan_item = include_scan_item;

    simplelist_info_init(&info, (char *)title, total_count, &data);
    info.get_name = bt_device_name_cb;
    info.action_callback = bt_simplelist_ok_cancel;
    info.selection = -1;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);
    if (info.selection < 0 || info.selection >= total_count)
        return BT_DEVICE_PICK_CANCEL;

    if (include_scan_item && info.selection == 0)
        return BT_DEVICE_PICK_SCAN;

    return include_scan_item ? info.selection - 1 : info.selection;
}

static void bt_set_selected_mac(const char *mac)
{
    if (mac && mac[0])
        snprintf(bt_selected_mac, sizeof(bt_selected_mac), "%s", mac);
    else
        bt_selected_mac[0] = '\0';
}

/* Apply a playback-device change to any in-progress playback.
 *
 * pcm_alsa_switch_playback_device() only records the new device; the ALSA
 * handle is reopened on the new device by the normal sink stop->start path
 * (sink_dma_stop closes, sink_dma_start opens playback_dev). audio_pause/
 * audio_resume do NOT reopen the device (they only pause the mixer channel),
 * so to actually move a live stream to the new device we must stop and
 * restart playback. Save the current elapsed position and restart there so
 * the listener keeps their place (brief gap, same spot in the track). */
static void bt_kick_audio_if_playing(void)
{
    struct mp3entry *id3;
    unsigned long elapsed, offset;
    int status = audio_status();

    if (!(status & AUDIO_STATUS_PLAY))
        return;

    id3 = audio_current_track();
    elapsed = id3 ? id3->elapsed : 0;
    offset  = id3 ? id3->offset  : 0;

    audio_stop();
    sleep(HZ / 5);
    audio_play(elapsed, offset);
}

static void bt_route_to_local(bool show_message)
{
    bt_playback_dev = BT_LOCAL_PLAYBACK_DEVICE;
    hiby_pcm_set_bt_mac(NULL);
    pcm_alsa_switch_playback_device(bt_playback_dev);
    bt_kick_audio_if_playing();
    if (show_message)
        splash(HZ, "Output: Local");
}

/* Force the A2DP transport onto a codec that can actually be opened for
 * playback. LDAC sinks (e.g. the WH-1000XM5) negotiate LDAC by default, giving
 * a 96 kHz transport that fails to open ("connected, no audio"); on this
 * bluealsa build the `-c SBC -c AAC` offer-restriction does NOT suppress it
 * (LDAC stays selectable and /usr/data/alsa.conf re-adds it). So switch the
 * live transport here -- AAC (48 kHz) preferred, SBC (mandatory A2DP codec) as
 * fallback. Called after connect and before any playback handle exists, so the
 * renegotiation cannot race a live stream. Verified on-device: on AAC the PCM
 * opens and audio reaches the sink. */
static bool bt_force_playable_codec(const char *mac)
{
    static const char *const want[] = { "AAC", "SBC" };
    char cur[24];
    char path[96];
    char cmd[200];
    char mac_u[18];
    unsigned w;
    int i;

    if (!mac || !*mac)
        return false;

    if (bt_read_active_codec(mac, cur, sizeof(cur)) &&
        (strcmp(cur, "AAC") == 0 || strcmp(cur, "SBC") == 0))
        return true;                    /* already on a playable codec */

    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(path, sizeof(path),
             "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", mac_u);

    for (w = 0; w < sizeof(want) / sizeof(want[0]); w++)
    {
        snprintf(cmd, sizeof(cmd),
                 "bluealsa-cli codec '%s' %s >/dev/null 2>&1", path, want[w]);
        system(cmd);

        for (i = 0; i < 20; i++)        /* wait for the transport to settle */
        {
            sleep(HZ / 5);
            if (bt_read_active_codec(mac, cur, sizeof(cur)) &&
                strcmp(cur, want[w]) == 0)
            {
                bt_dbg("codec: switched %s to %s after %d ticks",
                       mac, want[w], i);
                return true;
            }
        }
        bt_dbg("codec: %s did not settle for %s (now=%s)", want[w], mac, cur);
    }
    return false;
}

static bool bt_route_to_bluetooth(const char *mac)
{
    if (!mac || !mac[0])
        return false;

    bt_playback_dev = bt_make_bt_playback_dev(mac);

    if (!bt_wait_for_bluealsa_pcm(mac, HZ * 6))
    {
        bt_dbg("route: no PCM for %s within 6s", mac);
        bt_route_to_local(false);
        return false;
    }

    /* The link may have come up on LDAC (96 kHz, unopenable). Switch the
     * transport to a playable codec now -- before the device is handed to the
     * audio engine and before any playback handle exists, so the codec
     * renegotiation cannot race a live stream. */
    bt_force_playable_codec(mac);

    /* Record bluealsa as the playback device and apply it via a clean
     * stop/start of the audio engine (bt_kick_audio_if_playing). The device
     * is opened lazily on the next sink_dma_start, on the audio thread --
     * we no longer open/close the live handle from here, which is what
     * caused the route-switch races. The PCM is confirmed present above, so
     * the open will succeed; if the sink later vanishes, the pump's
     * DISCONNECTED handling + open fallback keep it from crashing. */
    hiby_pcm_set_bt_mac(mac);
    pcm_alsa_switch_playback_device(bt_playback_dev);
    bt_kick_audio_if_playing();
    bt_dbg("route: ok for %s (device=%s)", mac, bt_playback_dev);
    return true;
}

static bool bt_is_connected(const char *mac)
{
    if (!mac || !*mac)
        return false;

    return bt_bluealsa_pcm_ready(mac);
}

static bool bt_bluealsa_pcm_ready(const char *mac)
{
    char line[256];
    char mac_u[18];
    FILE *fp;

    if (!mac || !*mac)
        return false;

    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    fp = popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (!fp)
        return false;

    while (fgets(line, sizeof(line), fp))
    {
        if (strstr(line, mac_u) && strstr(line, "/a2dpsrc/sink"))
        {
            pclose(fp);
            return true;
        }
    }

    pclose(fp);
    return false;
}

static bool bt_get_active_mac(char *mac_out, size_t mac_out_len)
{
    FILE *fp;
    char line[256];

    if (!mac_out || mac_out_len < 18)
        return false;

    mac_out[0] = '\0';
    fp = popen("bluealsa-cli list-pcms 2>/dev/null", "r");
    if (fp)
    {
        while (fgets(line, sizeof(line), fp))
        {
            if (!strstr(line, "/a2dpsrc/sink"))
                continue;
            if (bt_extract_mac_from_line(line, mac_out, mac_out_len))
            {
                pclose(fp);
                return true;
            }
        }
        pclose(fp);
    }

    if (bt_selected_mac[0] && bt_is_connected(bt_selected_mac))
    {
        snprintf(mac_out, mac_out_len, "%s", bt_selected_mac);
        return true;
    }

    return false;
}

/* Slow path: poll list-pcms on a fixed cadence. Used when the event
 * monitor cannot be started (fork/exec/pipe failure). */
static bool bt_wait_for_bluealsa_pcm_poll(const char *mac, int timeout_ticks)
{
    long deadline = current_tick + timeout_ticks;

    while (TIME_BEFORE(current_tick, deadline))
    {
        if (bt_bluealsa_pcm_ready(mac))
            return true;

        sleep(HZ / 5);
    }

    return bt_bluealsa_pcm_ready(mac);
}

/* Wait for the BlueALSA A2DP sink PCM for `mac` to become available.
 *
 * `bluealsa-cli monitor` streams PCMAdded/PCMRemoved/PropertyChanged
 * events on stdout, so instead of polling on a timer we block until the
 * monitor wakes us, then re-check list-pcms. This collapses the window
 * after a codec re-negotiation (where the transport briefly disappears
 * and reappears) without busy-waiting. Falls back to timed polling if
 * the monitor process cannot be spawned. A wall-clock deadline bounds
 * the wait regardless of event activity. */
static bool bt_wait_for_bluealsa_pcm(const char *mac, int timeout_ticks)
{
    int pipefd[2];
    pid_t pid;
    long deadline;
    bool ready = false;

    if (timeout_ticks < HZ / 2)
        timeout_ticks = HZ / 2;
    deadline = current_tick + timeout_ticks;

    /* Fast path: it may already be up. */
    if (bt_bluealsa_pcm_ready(mac))
        return true;

    if (pipe(pipefd) != 0)
        return bt_wait_for_bluealsa_pcm_poll(mac, timeout_ticks);

    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return bt_wait_for_bluealsa_pcm_poll(mac, timeout_ticks);
    }

    if (pid == 0)
    {
        /* Child: stream events to the pipe. */
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execlp("bluealsa-cli", "bluealsa-cli", "monitor", (char *)NULL);
        _exit(127);
    }

    /* Parent: wake on each event line (or a 1s tick) and re-check. */
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    while (TIME_BEFORE(current_tick, deadline))
    {
        fd_set rfds;
        struct timeval tv;
        int rc;

        if (bt_bluealsa_pcm_ready(mac))
        {
            ready = true;
            break;
        }

        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        rc = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
        if (rc > 0)
        {
            char buf[256];
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0 && !(n < 0 && errno == EAGAIN))
                break; /* monitor exited or pipe error: stop early, poll below */
            /* drain and loop back to re-check readiness */
        }
        else if (rc < 0 && errno != EINTR)
        {
            break;
        }
    }

    /* Tear down the monitor child. */
    close(pipefd[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    if (ready)
        return true;

    /* If the monitor died early, finish out the remaining budget polling. */
    if (TIME_BEFORE(current_tick, deadline))
        return bt_wait_for_bluealsa_pcm_poll(mac, deadline - current_tick);

    return bt_bluealsa_pcm_ready(mac);
}

/* --- bluealsa helpers & instrumentation --------------------------------- *
 * We do NOT restart bluealsa to restrict codecs -- the `-c SBC -c AAC` flag
 * does not suppress LDAC on this build. The stock bt_init already runs a
 * bluealsa a2dp-source daemon; we only ensure one is running and let
 * bt_force_playable_codec() switch the live transport to AAC after connect. */

/* Log the bluealsa daemon pid so its lifecycle across a connect is visible. */
static void bt_log_bluealsa_state(const char *when)
{
    char pid[16] = "none";
    FILE *fp = popen("pidof bluealsa 2>/dev/null", "r");
    if (fp)
    {
        if (fgets(pid, sizeof(pid), fp))
            pid[strcspn(pid, " \r\n")] = '\0';
        pclose(fp);
    }
    bt_dbg("bluealsa[%s] pid=%s", when, pid);
}

/* Lightweight load/memory snapshot read straight from /proc (no process
 * spawn), to correlate BT activity with the reported sluggishness. */
static void bt_log_sys_stats(const char *when)
{
    char load[48] = "?";
    char line[80];
    long avail = -1;
    FILE *fp = fopen("/proc/loadavg", "r");
    if (fp)
    {
        if (fgets(load, sizeof(load), fp))
            load[strcspn(load, "\r\n")] = '\0';
        fclose(fp);
    }
    fp = fopen("/proc/meminfo", "r");
    if (fp)
    {
        while (fgets(line, sizeof(line), fp))
            if (sscanf(line, "MemAvailable: %ld kB", &avail) == 1)
                break;
        fclose(fp);
    }
    bt_dbg("sysstat[%s] load=%s memavail=%ldkB", when, load, avail);
}

/* Ensure a bluealsa a2dp-source daemon is running. The stock one (from
 * bt_init) normally is; we only start one if none exists -- no kill/relaunch,
 * so there is no daemon thrash. If we do start it, wait for hci0 UP first or
 * it logs "Network is down" and never attaches. */
static void bt_ensure_bluealsa_running(void)
{
    int i;

    if (system("pidof bluealsa >/dev/null 2>&1") == 0)
        return;                         /* already running -- leave it alone */

    for (i = 0; i < 25; i++)
    {
        if (system("hciconfig hci0 2>/dev/null | grep -q 'UP RUNNING'") == 0)
            break;
        if (i == 0)
            system("/usr/bin/bt_enable >/tmp/rb_bt_enable.log 2>&1");
        sleep(HZ / 5);
    }

    system("setsid /usr/bin/bluealsa -p a2dp-source --a2dp-volume "
           ">/tmp/rb_bluealsa.log 2>&1 </dev/null &");
    sleep(HZ);
    bt_log_bluealsa_state("started");
    bt_dbg("bluealsa started (none was running)");
}

static bool bt_prepare_stack(void)
{
    char reply[BT_SYS_REPLY_MAX];
    int i;

    /* Make sure a bluealsa a2dp-source daemon is up (it normally is). */
    bt_ensure_bluealsa_running();

    /* If the control socket already answers, the stack is up; skip the
     * heavyweight bt_enable which tears down and re-initialises the whole
     * stack (a frequent source of the connect-retry storms). */
    reply[0] = '\0';
    if (bt_sys_command("BT:LIST", reply, sizeof(reply)) == 0 &&
        bt_sys_reply_ok(reply, "BT:LIST"))
        return true;

    system("/usr/bin/bt_enable >/tmp/rb_bt_enable.log 2>&1");

    for (i = 0; i < 12; i++)
    {
        reply[0] = '\0';
        if (bt_sys_command("BT:LIST", reply, sizeof(reply)) == 0 &&
            bt_sys_reply_ok(reply, "BT:LIST"))
            return true;

        reply[0] = '\0';
        bt_sys_command("BT:ON", reply, sizeof(reply));

        sleep(HZ / 5);
    }

    return false;
}

/* Connect via bluetoothctl. The HiBy sys_server "BT:CONNECT" establishes
 * a link but does not reliably bring up the A2DP source profile, so the
 * BlueALSA sink PCM never appears ("connected, no route to audio"). A
 * plain `bluetoothctl connect` does bring A2DP up (it is also what HiBy's
 * own bt_connect_last.sh uses), so use it as the connect mechanism. */
/* Connect tuning. The hold must outlast the wait, so bluetoothctl is still
 * alive for the whole window in which the link may come up. */
#define BT_CONNECT_ATTEMPTS  2
#define BT_CONNECT_WAIT_SECS 10
#define BT_CONNECT_HOLD_SECS 12

static bool bt_connect_via_bluetoothctl(const char *mac)
{
    char cmd[224];
    int attempt;

    if (!mac || !*mac)
        return false;

    for (attempt = 1; attempt <= BT_CONNECT_ATTEMPTS; attempt++)
    {
        /* `connect` is asynchronous: bluetoothctl prints "Attempting to
         * connect" and the outcome arrives later. Feeding `quit` on the next
         * line therefore tears the session down with the connect still in
         * flight, which is why a cold device reliably failed the first try
         * (linked=no) and succeeded on the second. Keep the session alive
         * while the link comes up, and background it so a successful connect
         * still returns as soon as the PCM appears rather than waiting out
         * the sleep. */
        snprintf(cmd, sizeof(cmd),
                 "(printf 'power on\\nconnect %s\\n'; sleep %d; printf 'quit\\n') "
                 "| bluetoothctl >/tmp/rb_bt_connect.log 2>&1 &",
                 mac, BT_CONNECT_HOLD_SECS);
        system(cmd);

        /* A2DP transport/PCM acquisition is asynchronous; wait for the sink. */
        if (bt_wait_for_bluealsa_pcm(mac, HZ * BT_CONNECT_WAIT_SECS))
            return true;

        /* Failing here means no route is attempted at all, which presents as
         * "connected (chime, device listed) but silent". Say so: it is
         * otherwise the one connect outcome that leaves no trace in the log.
         * linked=no means the ACL link never came up (bluetoothctl's connect
         * did not take); linked=yes means the link is up but BlueALSA never
         * registered an A2DP sink -- different faults, do not conflate. */
        bt_dbg("connect: no a2dpsrc/sink for %s within %ds (try %d/%d, linked=%s)",
               mac, BT_CONNECT_WAIT_SECS, attempt, BT_CONNECT_ATTEMPTS,
               bt_is_connected(mac) ? "yes" : "no");

        /* Drop the backgrounded session before retrying so a stale one cannot
         * quit out from under the next attempt. (`pidof` rather than `pkill`:
         * it is already proven present on this BusyBox. A no-op if none is
         * running.) */
        system("kill $(pidof bluetoothctl) >/dev/null 2>&1");
    }

    return false;
}

static void bt_show_devices(void)
{
    struct bt_device devices[BT_MAX_DEVICES];
    int count;
    int idx;

    if (!bt_prepare_stack())
        return;

    splash(0, "Loading devices...");
    count = bt_load_devices_via_sys_list(devices, BT_MAX_DEVICES);
    if (count <= 0)
    {
        splash(0, "Scanning...");
        count = bt_scan_and_merge_devices(devices, count, BT_MAX_DEVICES);
    }

    while (1)
    {
        idx = bt_choose_device("Devices", devices, count, true);
        if (idx == BT_DEVICE_PICK_SCAN)
        {
            splash(0, "Scanning...");
            count = bt_scan_and_merge_devices(devices, count, BT_MAX_DEVICES);
            if (count <= 0)
                splash(HZ, "No devices");
            continue;
        }

        if (idx >= 0 && idx < count)
            bt_connect_device(&devices[idx]);
        return;
    }
}

static void bt_connect_device(const struct bt_device *device)
{
    const char *mac;
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];
    int ctl_rc;
    bool routed;
    bool pair_reply_ok = true;

    if (!device || !device->mac[0])
        return;

    mac = device->mac;
    splash(0, "Connecting...");
    bt_log_sys_stats("connect-begin");
    bt_log_bluealsa_state("connect-begin");

    if (!bt_prepare_stack())
    {
        splash(HZ * 2, "BT unavailable");
        return;
    }

    if (!device->paired)
    {
        snprintf(cmd, sizeof(cmd), "BT:PAIR:%s", mac);
        ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
        pair_reply_ok = (ctl_rc == 0) && bt_sys_reply_ok(reply, "BT:PAIR");
        if (pair_reply_ok)
            sleep(HZ / 2);
    }

    if (!pair_reply_ok && !device->paired)
    {
        splash(HZ * 2, "BT pair failed");
        return;
    }

    bt_set_selected_mac(mac);

    /* Connect via bluetoothctl -- the single, reliable bring-up. On-device
     * testing showed a plain "bluetoothctl connect" cleanly negotiates the
     * link; bt_route_to_bluetooth() then forces the transport to a playable
     * codec (see bt_force_playable_codec). There is deliberately no sys_server
     * fallback (see note below). */
    routed = false;
    if (bt_connect_via_bluetoothctl(mac))
        routed = bt_route_to_bluetooth(mac);
    bt_log_bluealsa_state("after-route");

    /* No sys_server BT:CONNECT fallback. It re-engages the stock orchestration
     * (BT:A2DPPROFILE / bluealsa_profile) which kills and relaunches bluealsa,
     * tearing down the A2DP transport we just brought up -- the connect/
     * no-audio thrash and its CPU churn. bluetoothctl is the reliable bring-up;
     * if it fails we report it rather than fight the stock stack. */

    if (routed)
    {
        bt_save_last_device(mac, device->name);
        splash(HZ, "BT connected");
    }
    else
        splash(HZ * 2, "BT connected, no audio route");

    bt_log_sys_stats("connect-end");
}

static void bt_reconnect_last(void)
{
    struct bt_device device;

    if (!bt_load_last_device(&device))
    {
        splash(HZ * 2, "No last device");
        return;
    }

    bt_connect_device(&device);
}

static void bt_disconnect(void)
{
    char mac[18];
    char cmd[96];
    char reply[BT_SYS_REPLY_MAX];

    mac[0] = '\0';
    if (!bt_get_active_mac(mac, sizeof(mac)) && bt_selected_mac[0])
        snprintf(mac, sizeof(mac), "%s", bt_selected_mac);

    if (mac[0])
    {
        snprintf(cmd, sizeof(cmd), "BT:DISCONNECT:%s", mac);
        bt_sys_command(cmd, reply, sizeof(reply));
    }

    bt_set_selected_mac(NULL);
    bt_route_to_local(false);
    splash(HZ, "Disconnected");
}

static void bt_show_status(void)
{
    struct simplelist_info info;
    char active_mac[18];

    simplelist_info_init(&info, "Status", 0, NULL);
    simplelist_reset_lines();

    if (bt_get_active_mac(active_mac, sizeof(active_mac)))
    {
        char codec[24];

        simplelist_addline("Device: Bluetooth");
        simplelist_addline("MAC: %s", active_mac);
        simplelist_addline("Connected: %s",
                           bt_is_connected(active_mac) ? "Yes" : "No");
        simplelist_addline("A2DP PCM: %s",
                           bt_bluealsa_pcm_ready(active_mac) ? "Ready" : "Not ready");
        if (bt_read_active_codec(active_mac, codec, sizeof(codec)))
            simplelist_addline("Codec: %s", codec);
    }
    else if (bt_selected_mac[0])
    {
        simplelist_addline("Device: Last selected");
        simplelist_addline("MAC: %s", bt_selected_mac);
        simplelist_addline("Connected: No");
    }
    else
    {
        simplelist_setline("Device: Local");
    }

    simplelist_addline("Output: %s", bt_playback_dev);

    info.count = simplelist_get_line_count();
    simplelist_show_list(&info);
}

int hiby_bluetooth_menu(void)
{
    static const char *const action_items[] =
    {
        "Status",
        "Reconnect last",
        "Devices",
        "Disconnect",
    };

    int action = -1;

    while (true)
    {
        struct simplelist_info info;

        simplelist_info_init(&info, "Bluetooth",
            (int)(sizeof(action_items) / sizeof(action_items[0])),
            (void *)action_items);
        info.get_name = bt_action_name_cb;
        info.action_callback = bt_simplelist_ok_cancel;
        info.selection = -1;
        info.title_icon = Icon_Submenu;

        simplelist_show_list(&info);
        action = info.selection;
        if (action < 0)
            break;

        switch (action)
        {
            case 0:
                bt_show_status();
                break;
            case 1:
                bt_reconnect_last();
                break;
            case 2:
                bt_show_devices();
                break;
            case 3:
                bt_disconnect();
                break;
            default:
                break;
        }
    }
    return 0;
}

#endif /* HIBY_LINUX */
