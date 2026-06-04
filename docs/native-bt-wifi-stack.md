# HiBy R1 Native BT & WiFi Stack — Reverse-Engineering Notes

> Analysis only. No code changes. Captures how the **stock** HiBy R1 firmware
> brings up and drives Bluetooth and WiFi, so we can decide what (if anything)
> to adopt in the Rockbox port. Derived from on-device `adb` exploration plus
> `strings`/`objdump` of `/usr/bin/sys_server` (76 KB stripped `elf32-mipsel`,
> build dated `Sep 13 2025`, git `94f5d17`).

---

## 0. TL;DR

- **`sys_server` is a thin orchestrator, not a stack.** It is a single-threaded
  unix-socket daemon that parses short text commands (`BT:CONNECT:AA:BB:...`,
  `WIFI:ON`, `MOUNT:...`) and, for almost every one of them, just builds a shell
  string and calls libc **`system()`**. The real work is done by the stock
  BlueZ CLI tools (`bt-device`, `bt-adapter`, `bluealsa-cli`), `bluealsa_profile`,
  `wpa_cli`, `udhcpc`, and a handful of `/usr/bin/*.sh` scripts.
- **The Bluetooth data path is identical to ours:** BlueZ `bluetoothd` +
  `bluealsa` (a2dp-source). The only thing the stock app does that we don't is
  drive it through `sys_server` text commands instead of calling the same tools
  directly. There is **no secret IPC, no proprietary audio routing** — our
  direct `bluetoothctl` + `bluealsa` approach reaches the exact same endpoint.
- **The "cleaner codec mechanism" the stock app uses** is literally
  `bluealsa-cli codec <path> <codec>` (and `bluealsa_profile` to restart the
  daemon). This is post-hoc codec switching on a live sink — the same teardown
  race we already rejected. Our approach (start `bluealsa -c SBC -c AAC` so the
  *negotiation* never picks LDAC) is strictly more robust for this hardware.
- **WiFi is wholly conventional:** `wpa_supplicant -Dnl80211` + `wpa_cli` +
  `udhcpc`, with `sys_server` shelling out to `wpa_cli` for every operation and
  using `libwpa_client.so` (`wpa_ctrl_*`) to monitor async events. Implementing
  WiFi in Rockbox is very feasible and needs **no HiBy-specific daemon** — just
  the same `wpa_supplicant`/`wpa_cli`/`udhcpc` already on the rootfs.

---

## 1. Hardware & kernel substrate

| Item | Value |
|------|-------|
| SoC | Ingenic X1600 (XBurst, MIPS32r2, single core) |
| RAM | ~57 MB usable |
| Kernel | 4.4.94 |
| Combo chip | Broadcom **BCM4343A1** (BT + WiFi, SDIO for WiFi, UART for BT) |
| BT transport | `/dev/ttyS0` @ 3 Mbaud, firmware `BCM4343A1_*.hcd` |
| WiFi driver | `bcmdhd` (kernel module; nvram/fw patch paths via sysfs) |
| WiFi iface | `wlan0` |

Both radios sit behind `rfkill`. BT firmware is patched into the chip at boot by
`brcm_patchram_plus`; WiFi firmware/nvram is selected via
`/sys/module/bcmdhd/parameters/{firmware_path,nvram_path}`.

---

## 2. Boot / init order (busybox `init`, `/etc/init.d`)

```
S10mdev                     device nodes (mdev -df)
S11jpeg_display_shell       splash
S11module_driver_default    load kernel modules
S20urandom
S21mount_ubifs
S30dbus                     dbus-daemon --system   (BlueZ + BlueALSA bus)
S39_recovery.recovery
S40network                  ifup -a (lo, etc.)
S43wifi_bcm_init_config     set wlan0 MAC, bcmdhd params           ← WiFi HW init
S50sys_server               launch /usr/bin/sys_server             ← the daemon
S80_bt_init                 run /usr/bin/bt_init                    ← BT stack up
S92_03_start_music_player   hiby_player.sh → bootloader.rb → app   ← UI
```

Key ordering facts:
- `sys_server` starts **before** BT and the UI. When the app sends `BT:ON`
  before `bt_init` has finished, `sys_server` replies **`BT:ON:WAITINIT`**
  (it gates on the existence of `/tmp/bt_init_ok`).
- WiFi hardware is *configured* at S43 but **not associated** at boot. Actual
  association happens on demand via `WIFI:ON`/`WIFI:CONNECT`.

### `S92_03_start_music_player` → our entry point
`hiby_player.sh` runs `bootloader.rb`, which on the stock firmware launches the
HiBy player; in our port it launches `/tmp/rockbox.r1`. **Rockbox replaces only
the app layer.** Everything below (`dbus-daemon`, `bluetoothd`, `bt-agent`,
`bluealsa`, `sys_server`, `wpa_supplicant`) is the stock rootfs and is still
running underneath us. This is why our direct tool calls work.

---

## 3. Bluetooth bring-up (`/usr/bin/bt_init`, run by S80)

`bt_init` is the canonical BT cold-start sequence:

```
1. ensure dbus-daemon (system bus) is up
2. rfkill unblock bluetooth
3. brcm_patchram_plus --enable_hci --baudrate 3000000 --no2bytes \
       --patchram /lib/firmware/bt_bcm/BCM4343A1_*.hcd /dev/ttyS0 \
       --tosleep=50000 --use_baudrate_for_download --enable_lpm \
       --bd_addr <MAC>                                   # loads BT FW, brings up hci0
4. bluetoothd -E -C                                       # BlueZ, compat + experimental
5. bt-agent -c NoInputNoOutput                            # auto-accept pairing, no PIN
6. bluealsa -p a2dp-source --a2dp-volume                  # A2DP source + AVRCP absvol
7. write /usr/data/alsa.conf  (bt_alsa_sink: type plug, codec "ldac",
                               ldac_eqmid LDAC_ABR)        # stock ALSA convenience cfg
8. bt-adapter --set Powered {On|Off}
9. touch /tmp/bt_init_ok
```

Resulting live process tree (observed via `/proc`):

```
brcm_patchram_plus … /dev/ttyS0 --bd_addr 4C:BC:98:B0:2B:A3
/usr/libexec/bluetooth/bluetoothd -E -C
bt-agent -c NoInputNoOutput
/usr/bin/bluealsa -p a2dp-source --a2dp-volume
dbus-daemon --system
/usr/bin/sys_server
/tmp/rockbox.r1          ← us
```

**Layering (bottom → top):**

```
BCM4343A1 chip
   │  HCI over /dev/ttyS0 (3 Mbaud), FW patched by brcm_patchram_plus
kernel Bluetooth core + rfkill  →  hci0
   │  HCI
bluetoothd (BlueZ)   ──D-Bus(org.bluez)──  bt-agent (pairing), bt-* CLI tools
   │  A2DP/AVRCP via BlueZ MediaTransport
bluealsa (a2dp-source)  ──D-Bus(org.bluealsa)──  exposes PCM as ALSA "bluealsa" dev
   │  ALSA PCM (plughw / bluealsa pcm)
sys_server (text-cmd orchestrator over /var/run/sys_server)
   │  BT:* commands
HiBy player  /  (in our port: Rockbox, which bypasses sys_server)
```

### Helper scripts (exact contents)

```sh
# bt_enable           bt_disable                  bt_done
bt-adapter --set Powered On    bt-adapter --set Discoverable Off   killall -9 bt-agent
bt-adapter --set Discoverable On  bt-adapter --set Powered Off     killall brcm_patchram_plus

# bluealsa_profile  <a2dp-sink|a2dp-source>
killall -9 bluealsa; bluealsa -p <profile> --a2dp-volume &

# bt_suspend: hciconfig hci0 down; killall bluetoothd brcm_patchram_plus bluealsa dbus-daemon; rfkill block
# bt_resume:  re-runs the bt_init sequence

# bt_enable_bsa.sh  — ALTERNATE stack using Broadcom's proprietary bsa_server
#   (NOT used by default; default path is BlueZ. Present for a BSA-based variant.)
```

> Note the dormant **`bt_enable_bsa.sh`**: HiBy ships an alternate Broadcom BSA
> stack (`bsa_server`) but the shipped firmware uses **BlueZ**. We can ignore BSA.

---

## 4. `sys_server` — the orchestrator

### 4.1 Transport
- Listens on a **`AF_UNIX`** stream socket at **`/var/run/sys_server`**.
- Clients connect, register command interest (`cmd:%s has register`), and read
  unsolicited push notifications via **`/var/run/sys_client`**.
- Single accept loop: `"client disconnected, need wait new connect."` — handles
  one client connection at a time, blocking. Requests are newline/colon-delimited
  ASCII; replies are `<CMD>:OK` / `<CMD>:FAIL` (some `:WAITINIT`, `:TIMEOUT`).
- Internally namespaced into "services": `bt_service`, `wpa_service`,
  `mount_service`, `usb_service`, `shairport_service`, `samba_service`,
  plus `FINDABLE` (HiByLink discovery) and `airkiss` (WiFi SmartConfig).

### 4.2 Implementation strategy
- Dynamic imports of interest: **`system`**, `socket`, `connect`, and
  `wpa_ctrl_{open,close,attach,pending,recv}` (from `libwpa_client.so`).
- **Almost everything is `system("<shell string>")`.** `sys_server` adds no
  real BT/WiFi logic — it formats a command line and shells out, then maps the
  exit code to `:OK`/`:FAIL`. The only genuinely event-driven part is the WiFi
  monitor, which uses the `wpa_ctrl` socket to receive async events.

### 4.3 Bluetooth command map (verbatim from the binary)

| Request | What `sys_server` runs |
|---------|------------------------|
| `BT:ON` | gate on `/tmp/bt_init_ok` (else `BT:ON:WAITINIT`); `bt_enable`; `bt-device -l` → `/data/bt_list.txt` |
| `BT:OFF` | `/usr/bin/bt_disable` |
| `BT:SCAN` | `bt-adapter -d` (discover); results → `/data/bt_scan.txt` |
| `BT:CANCEL_SCAN` | `bt-adapter -t` |
| `BT:DISCOVER` | `bt-adapter --set "Discoverable" "On"` |
| `BT:NONDISCOVER` | `bt-adapter --set "Discoverable" "Off"` |
| `BT:PAIR:<mac>` | `bt-device -c <mac>` |
| `BT:UNPAIR:<mac>` | `bt-device -d <mac>` (disconnect) + `bt-device -r <mac>` (remove); `bt-device -l` |
| `BT:CONNECT:<mac>` | `bt-device -z <mac>`; on success write `/data/bt_lastused.txt` |
| `BT:DISCONNECT:<mac>` | disconnect (with retry: `disconnect bt err, retry[%d]`) |
| `BT:STATUS:<mac>` | `bt-device -i <mac>` → `/data/bt_status.txt` |
| `BT:SETNAME:<name>` | `bt-adapter --set "Alias" "<name>"` |
| `BT:LIST` | `bt-device -l` |
| `BT:ABSVOL:<mac> <0-100>` | `bluealsa-cli volume /org/bluealsa/hci0/dev_<mac>/a2dpsrc/sink <vol>` |
| `BT:A2DPPROFILE:<profile>` | `bluealsa_profile <profile>`  (restarts bluealsa as sink/source) |
| `BT:CODEC:<mac> <codec>` | `bluealsa-cli codec /org/bluealsa/hci0/dev_<mac>/a2dpsrc/sink <codec>` |
| `BT:CONTROL:<mac> <action> <n>` | `dbus-send … org.bluez.MediaPlayer1.<action>` or `…MediaControl1.<action>` (AVRCP play/pause/next/prev) |

Observations:
- **`bt-device -z` = connect, `-c` = pair.** The stock "connect" verb is
  `bt-device -z <mac>`. Our port connects via `bluetoothctl connect` (BlueZ
  D-Bus `Device1.Connect`), which is functionally equivalent — both end at the
  same BlueZ call.
- **AVRCP / media control** is just `dbus-send` to `org.bluez.MediaPlayer1` /
  `MediaControl1`. If we ever want phone-side transport buttons reflected, this
  is the exact interface (no `sys_server` needed).
- **Absolute volume** is `bluealsa-cli volume <sink-path> <0-100>` — matches the
  absolute-volume support already landed in `5a8f32d`.
- **Codec/profile switching** (`BT:CODEC` / `BT:A2DPPROFILE`) operates on a
  *live* sink or restarts the daemon. This is the post-connect codec switch we
  deliberately avoided: it forces a transport teardown/renegotiation while the
  sink may be open. Our pre-negotiation approach (`bluealsa -c SBC -c AAC`)
  sidesteps it entirely.

### 4.4 The "is sys_server cleaner?" verdict
**No — for our use case, going through `sys_server` would be strictly worse:**
1. It is `system()`-per-command shelling to the *same* tools we already call,
   adding a socket round-trip and a fragile single-client accept loop.
2. Its codec path is the teardown-race path we rejected.
3. It couples us to HiBy's text protocol and its `/data/*.txt` scratch files.
4. We'd still depend on `bluetoothd` + `bluealsa` underneath — i.e. no layer
   removed, one added.

The one thing `sys_server` has that's genuinely useful as *reference* is the
exact D-Bus object paths and CLI verbs (above). We can call those tools directly.

---

## 5. WiFi (future work — feasible, no HiBy daemon required)

### 5.1 Stack
```
BCM4343A1 (SDIO)  →  bcmdhd kernel module  →  wlan0
   │  nl80211
wpa_supplicant -Dnl80211 -i wlan0 -c <conf> -B   (association, WPA handshake)
   │  control socket /var/run/wpa_supplicant/wlan0
wpa_cli   (one-shot commands)   +   libwpa_client.so wpa_ctrl_* (event monitor)
   │
udhcpc -b -i wlan0 -q            (DHCP lease)
```

### 5.2 Bring-up scripts (verbatim)
```sh
# wifi_on.sh
killall udhcpc; killall wpa_supplicant
[ -f /data/wpa_supplicant.conf ] || cp /etc/wpa_supplicant_default.conf /data/wpa_supplicant.conf
# select bcmdhd nvram/fw patch via sa_config, write to /sys/module/bcmdhd/parameters/*
ifconfig wlan0 up
wpa_supplicant -Dnl80211 -iwlan0 -c/data/wpa_supplicant.conf -B
usleep 1300000
udhcpc -b -i wlan0 -q -x hostname:HiBy_Music &

# wifi_off.sh:  killall udhcpc wpa_supplicant; ifconfig wlan0 down
# wifi_up.sh / wifi_down.sh:  thinner variants (rfkill + wlan0 up/down)
```
`S43wifi_bcm_init_config` sets the MAC and bcmdhd params at boot; the radio is
left down until `wifi_on.sh`.

### 5.3 `sys_server` WiFi command map (verbatim)

| Request | What `sys_server` runs |
|---------|------------------------|
| `WIFI:ON` | `sh /usr/bin/wifi_on.sh` |
| `WIFI:OFF` | `sh /usr/bin/wifi_off.sh` |
| `WIFI:SCAN` | `wpa_cli -i wlan0 scan`; `wpa_cli -i wlan0 scan_result > /data/wifi_result.txt` |
| `WIFI:STATUS` | `wpa_cli -i wlan0 status > /data/wifi_status.txt` (parses `wpa_state=`) |
| `WIFI:LIST_NETWORK` | `wpa_cli -i wlan0 list_network > /data/wifi_network.txt` |
| `WIFI:SSID_CONNECT:<ssid>:<psk>:<...>` | `add_network` → `set_network N ssid/psk/key_mgmt/scan_ssid` → `enable_network` → `select_network` → `save_config` → `udhcpc` |
| `WIFI:CONNECT:<id> <...>` | `select_network`/`enable_network` + `udhcpc -b -i wlan0 -q` |
| `WIFI:DISCONNECT:<id> <...>` | `wpa_cli disable_network <id>` |
| `WIFI:REMOVE:<id> <...>` | `wpa_cli remove_network <id>` + `save_config` |
| `WIFI:GETMACADDR` | read wlan0 MAC |
| `WIFI:AIRKISS` | `/sbin/akiss -d -t 60 -p 7 > /data/akiss_result.txt` (SmartConfig provisioning) |

Async events: `sys_server` attaches a `wpa_ctrl` monitor and forwards
`CTRL-EVENT-CONNECTED` / `CTRL-EVENT-DISCONNECTED` / `CTRL-EVENT-SCAN-RESULTS` /
`WPA: 4-Way Handshake failed` to clients as `WIFI:STATUS:<iface>:<state>[:reason]`.

### 5.4 Implication for a Rockbox WiFi feature
Everything needed is already on the rootfs and is **standard wpa_supplicant**:
- **Enable:** run `wifi_on.sh` (or the explicit sequence) — brings up wlan0 +
  wpa_supplicant + DHCP. No HiBy daemon required.
- **Scan / list / connect / forget:** drive `wpa_cli` directly (same verbs the
  table above lists). Parse `scan_result` / `status` output.
- **Persisted networks:** `/data/wpa_supplicant.conf` (writable; seeded from
  `/etc/wpa_supplicant_default.conf`). `save_config` persists.
- **Status polling:** either poll `wpa_cli status` or open the
  `/var/run/wpa_supplicant/wlan0` control socket and read `CTRL-EVENT-*` lines
  (the `wpa_ctrl` protocol — same as `sys_server`).
- **No kernel/driver work:** `bcmdhd` + nl80211 are already configured at S43.

Open questions for a real implementation (deferred):
- Rockbox UI for SSID list / password entry (text input on this device).
- Whether to talk to `wpa_cli` (shell, simplest) or link `wpa_ctrl` (cleaner,
  event-driven, mirrors `sys_server`).
- What we'd *use* WiFi for in Rockbox (NTP time sync? network shares via the
  stock samba mount commands? OTA? — none currently planned).

---

## 6. Other `sys_server` services (catalogued, not used by us)

- **`MOUNT:*`** — `mount -t vfat,exfat[,ntfs]` / `ntfs-3g` / `mkfs.vfat`,
  USB mass-storage LUN setup (`MOUNT:SETLUN`), unmount, format. Manages
  `/data/mnt/{sd_0,udisk_0..3}`. (Rockbox handles its own storage.)
- **`FINDABLE:*`** — HiByLink discovery beacons (UDP broadcast / client-server).
- **`SHAIRPORT:*`** — AirPlay receiver on/off via `shairport_{on,off}.sh`.
- **`samba:*`** — CIFS share mount/list (`smblist`, `mount -t cifs`).
- **`WIFI:AIRKISS`** — WeChat SmartConfig provisioning via `/sbin/akiss`.

These confirm `sys_server` is HiBy's general-purpose "system actions" RPC for
their app — broad but shallow (mostly `system()` wrappers).

---

## 7. Bottom line for the Rockbox port

1. **BT audio:** our architecture already matches the native data path
   (`bluetoothd` + `bluealsa` a2dp-source). We bypass `sys_server` and call the
   same underlying tools directly — correctly. **No reason to adopt `sys_server`.**
2. **Codec:** native uses live codec-switch (`bluealsa-cli codec`) which is the
   race we avoid; our pre-negotiation `-c SBC -c AAC` is more robust on this HW.
3. **Absolute volume / AVRCP:** native uses `bluealsa-cli volume` and
   `dbus-send … MediaPlayer1/MediaControl1`. We already do absvol; AVRCP
   transport-button reflection is a small, well-understood future add if wanted.
4. **WiFi:** entirely conventional `wpa_supplicant`/`wpa_cli`/`udhcpc`, all
   present on the rootfs, no HiBy daemon needed. A Rockbox WiFi feature is
   feasible; the main cost is UI (SSID list + password entry), not plumbing.
