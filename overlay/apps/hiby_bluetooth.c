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

/* Preferred A2DP codec. "Auto" negotiates the best codec the connected
 * sink advertises; the rest force a specific codec (falling back to SBC,
 * which is mandatory for A2DP, when the sink does not support it). */
enum bt_codec_pref
{
    BT_CODEC_AUTO = 0,
    BT_CODEC_SBC,
    BT_CODEC_AAC,
    BT_CODEC_APTX,
    BT_CODEC_APTX_HD,
    BT_CODEC_LDAC,
    BT_CODEC_COUNT
};

static const char *const bt_codec_pref_names[BT_CODEC_COUNT] = {
    "Auto (best available)",
    "SBC",
    "AAC",
    "aptX",
    "aptX-HD",
    "LDAC",
};

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
static int bt_codec_pref = BT_CODEC_AUTO;
static bool bt_codec_pref_loaded = false;

static bool bt_wait_for_bluealsa_pcm(const char *mac, int timeout_ticks);
static bool bt_apply_preferred_codec(const char *mac);
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

static int bt_load_codec_pref(void)
{
    char path[160];
    FILE *fp;
    int value = BT_CODEC_AUTO;

    snprintf(path, sizeof(path), "%s/bt_codec.txt", bt_state_dir());
    fp = fopen(path, "r");
    if (fp)
    {
        if (fscanf(fp, "%d", &value) != 1)
            value = BT_CODEC_AUTO;
        fclose(fp);
    }

    if (value < 0 || value >= BT_CODEC_COUNT)
        value = BT_CODEC_AUTO;
    return value;
}

static void bt_save_codec_pref(int pref)
{
    char path[160];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/bt_codec.txt", bt_state_dir());
    fp = fopen(path, "w");
    if (!fp)
        return;
    fprintf(fp, "%d\n", pref);
    fclose(fp);
}

static bool bt_is_codec_word_char(char c)
{
    return isalnum((unsigned char)c) || c == '-';
}

/* Whole-word, case-insensitive codec lookup so that "aptX" does not
 * spuriously match inside "aptX-HD" ('-' counts as part of a word). */
static bool bt_output_has_codec(const char *text, const char *token)
{
    size_t tlen = strlen(token);
    const char *p = text;

    while (*p)
    {
        if (bt_is_codec_word_char(*p))
        {
            const char *start = p;
            size_t len;

            while (bt_is_codec_word_char(*p))
                p++;

            len = (size_t)(p - start);
            if (len == tlen && strncasecmp(start, token, tlen) == 0)
                return true;
        }
        else
        {
            p++;
        }
    }

    return false;
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

/* Select the playback codec for the connected sink according to the
 * user's preference. Replaces the previous unconditional SBC force.
 * Returns true if the codec was actually changed (which tears down and
 * re-acquires the A2DP transport), false if no change was made. */
static bool bt_apply_preferred_codec(const char *mac)
{
    static const char *const codec_tokens[BT_CODEC_COUNT] = {
        NULL, "SBC", "AAC", "aptX", "aptX-HD", "LDAC"
    };
    /* Best-to-worst order used when the preference is "Auto".
     *
     * NOTE: LDAC is intentionally excluded. On the R1, the BlueALSA LDAC
     * encoder negotiates a 96 kHz transport that cannot be opened for
     * playback at all (verified on-device: snd_pcm_open/set_params fails
     * even when fed LDAC's own native 96 kHz/stereo params, while SBC@48k
     * and AAC@48k both play). Picking LDAC here yields a connected sink
     * with no audio and a frozen player. AAC is the best codec that
     * actually works; SBC is the mandatory fallback. LDAC remains
     * explicitly selectable from the menu for users whose sink/stack
     * combination can open it. */
    static const int auto_order[] = {
        BT_CODEC_APTX_HD, BT_CODEC_APTX, BT_CODEC_AAC, BT_CODEC_SBC
    };

    char mac_u[18];
    char pcm_path[96];
    char cmd[256];
    char avail[512];
    const char *chosen = NULL;
    FILE *fp;

    if (!mac || !*mac)
        return false;

    bt_mac_to_underscore(mac, mac_u, sizeof(mac_u));
    snprintf(pcm_path, sizeof(pcm_path),
             "/org/bluealsa/hci0/dev_%s/a2dpsrc/sink", mac_u);

    /* Ask bluealsa which codecs this sink actually advertises. */
    avail[0] = '\0';
    snprintf(cmd, sizeof(cmd),
             "bluealsa-cli codec '%s' 2>/dev/null", pcm_path);
    fp = popen(cmd, "r");
    if (fp)
    {
        char line[256];
        size_t used = 0;

        while (fgets(line, sizeof(line), fp))
        {
            size_t l = strlen(line);
            if (used + l < sizeof(avail))
            {
                memcpy(avail + used, line, l);
                used += l;
                avail[used] = '\0';
            }
        }
        pclose(fp);
    }

    if (bt_codec_pref == BT_CODEC_AUTO)
    {
        size_t i;
        for (i = 0; i < sizeof(auto_order) / sizeof(auto_order[0]); i++)
        {
            const char *tok = codec_tokens[auto_order[i]];
            if (tok && bt_output_has_codec(avail, tok))
            {
                chosen = tok;
                break;
            }
        }
    }
    else
    {
        const char *tok = codec_tokens[bt_codec_pref];
        /* Honour the request if the sink advertises it, or if we could
         * not read the list at all (let bluealsa decide/validate). */
        if (tok && (avail[0] == '\0' || bt_output_has_codec(avail, tok)))
            chosen = tok;
    }

    if (!chosen)
        chosen = "SBC"; /* mandatory A2DP codec: always a safe fallback */

    /* If the sink is already on the codec we want, do NOT issue
     * "bluealsa-cli codec" again: changing the codec tears down and
     * re-acquires the A2DP transport (the PCM disappears for a moment),
     * which races our route setup and was causing "PCM gone after codec
     * select" / "no route to audio". Parse the current "Selected codec:"
     * from the query output and skip the switch when it already matches. */
    {
        const char *sel = strstr(avail, "elected codec:"); /* "Selected codec:" */
        if (sel)
        {
            char cur[24];
            const char *p = strchr(sel, ':');
            int n = 0;
            if (p)
            {
                p++;
                while (*p == ' ' || *p == '\t')
                    p++;
                while (p[n] && bt_is_codec_word_char(p[n]) && n < (int)sizeof(cur) - 1)
                {
                    cur[n] = p[n];
                    n++;
                }
                cur[n] = '\0';
                if (n > 0 && strcasecmp(cur, chosen) == 0)
                {
                    bt_dbg("codec: already %s, skipping switch (pref=%d)",
                           cur, bt_codec_pref);
                    return false;
                }
            }
        }
    }

    snprintf(cmd, sizeof(cmd),
             "bluealsa-cli codec '%s' %s >/tmp/rb_bt_codec.log 2>&1",
             pcm_path, chosen);
    system(cmd);

    bt_dbg("codec: pref=%d available=[%s] chosen=%s (switched)",
           bt_codec_pref, avail[0] ? avail : "?", chosen);
    return true;
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

    /* bt_apply_preferred_codec() returns true if it actually changed the
     * codec (which tears down + re-acquires the A2DP transport), false if
     * the sink was already on the wanted codec (no teardown). */
    if (bt_apply_preferred_codec(mac))
    {
        /* The codec switch makes the PCM briefly disappear and reappear.
         * Do NOT trust an immediate "PCM present" check: the OLD transport
         * may still be listed for a moment before teardown, so we would
         * race onto a dying PCM. Give bluealsa time to drop the old
         * transport first, then wait for the new one to settle. */
        sleep(HZ);
        if (!bt_wait_for_bluealsa_pcm(mac, HZ * 6))
        {
            bt_dbg("route: PCM gone after codec select for %s", mac);
            bt_route_to_local(false);
            return false;
        }
        /* Small extra settle so the re-acquired transport is fully ready
         * for snd_pcm_open (avoids the transient open failure downstream). */
        sleep(HZ / 2);
    }

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
    int ticks = 0;

    while (ticks < timeout_ticks)
    {
        if (bt_bluealsa_pcm_ready(mac))
            return true;

        sleep(HZ / 5);
        ticks += HZ / 5;
    }

    return false;
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
    int deadline;
    int elapsed = 0;
    bool ready = false;

    if (timeout_ticks < HZ / 2)
        timeout_ticks = HZ / 2;
    deadline = timeout_ticks;

    /* Fast path: it may already be up. */
    if (bt_bluealsa_pcm_ready(mac))
        return true;

    if (pipe(pipefd) != 0)
        return bt_wait_for_bluealsa_pcm_poll(mac, deadline);

    pid = fork();
    if (pid < 0)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        return bt_wait_for_bluealsa_pcm_poll(mac, deadline);
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

    while (elapsed < deadline)
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

        elapsed += HZ; /* one ~1s quantum */
    }

    /* Tear down the monitor child. */
    close(pipefd[0]);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    if (ready)
        return true;

    /* If the monitor died early, finish out the remaining budget polling. */
    if (elapsed < deadline)
        return bt_wait_for_bluealsa_pcm_poll(mac, deadline - elapsed);

    return bt_bluealsa_pcm_ready(mac);
}

static bool bt_prepare_stack(void)
{
    char reply[BT_SYS_REPLY_MAX];
    int i;

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
static bool bt_connect_via_bluetoothctl(const char *mac)
{
    char cmd[160];

    if (!mac || !*mac)
        return false;

    /* power on + connect; ignore output, we verify via the PCM below */
    snprintf(cmd, sizeof(cmd),
             "printf 'power on\\nconnect %s\\nquit\\n' | bluetoothctl "
             ">/tmp/rb_bt_connect.log 2>&1", mac);
    system(cmd);

    /* A2DP transport/PCM acquisition is asynchronous; wait for the sink. */
    return bt_wait_for_bluealsa_pcm(mac, HZ * 8);
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
    bool connect_reply_ok;
    bool pair_reply_ok = true;

    if (!device || !device->mac[0])
        return;

    mac = device->mac;
    splash(0, "Connecting...");

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

    snprintf(cmd, sizeof(cmd), "BT:CONNECT:%s", mac);
    ctl_rc = bt_sys_command(cmd, reply, sizeof(reply));
    connect_reply_ok = (ctl_rc == 0) && bt_sys_reply_ok(reply, "BT:CONNECT");

    if (!pair_reply_ok && !device->paired)
    {
        splash(HZ * 2, "BT pair failed");
        return;
    }

    if (!connect_reply_ok)
    {
        splash(HZ * 2, "BT connect failed");
        return;
    }

    bt_set_selected_mac(mac);

    routed = bt_route_to_bluetooth(mac);
    if (!routed)
    {
        /* sys_server BT:CONNECT linked the device but A2DP did not come
         * up. Bring the audio profile up the reliable way and re-route. */
        bt_dbg("connect: sys_server route failed, trying bluetoothctl for %s",
               mac);
        if (bt_connect_via_bluetoothctl(mac))
            routed = bt_route_to_bluetooth(mac);
    }

    if (routed)
        splash(HZ, "BT connected");
    else
        splash(HZ * 2, "BT connected, no audio route");
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

static const char *bt_codec_name_cb(int selected_item, void *data,
    char *buffer, size_t buffer_len)
{
    (void)data;

    if (selected_item < 0 || selected_item >= BT_CODEC_COUNT)
    {
        buffer[0] = '\0';
        return buffer;
    }

    snprintf(buffer, buffer_len, "%s%s", bt_codec_pref_names[selected_item],
             selected_item == bt_codec_pref ? "  [current]" : "");
    return buffer;
}

static void bt_show_codec_menu(void)
{
    struct simplelist_info info;

    simplelist_info_init(&info, "Preferred codec", BT_CODEC_COUNT, NULL);
    info.get_name = bt_codec_name_cb;
    info.action_callback = bt_simplelist_ok_cancel;
    info.selection = bt_codec_pref;
    info.title_icon = Icon_Submenu;

    simplelist_show_list(&info);
    if (info.selection >= 0 && info.selection < BT_CODEC_COUNT)
    {
        char mac[18];

        bt_codec_pref = info.selection;
        bt_codec_pref_loaded = true;
        bt_save_codec_pref(bt_codec_pref);

        /* Apply live if a Bluetooth sink is currently routed. */
        if (bt_get_active_mac(mac, sizeof(mac)))
        {
            bt_apply_preferred_codec(mac);
            splash(HZ, "Codec applied");
        }
        else
        {
            splash(HZ, "Codec saved");
        }
    }
}

int hiby_bluetooth_menu(void)
{
    static const char *const action_items[] =
    {
        "Status",
        "Devices",
        "Codec",
        "Disconnect",
    };

    int action = -1;

    if (!bt_codec_pref_loaded)
    {
        bt_codec_pref = bt_load_codec_pref();
        bt_codec_pref_loaded = true;
    }

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
                bt_show_devices();
                break;
            case 2:
                bt_show_codec_menu();
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
