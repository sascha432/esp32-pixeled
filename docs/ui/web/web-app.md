# Web app

The setup, login, LEDs, Layout, Status, Wi-Fi, Hotspot, Logs and System pages in `web/`; the LED
pages are in `web/leds.js`, loaded on their first visit ([leds.md](../../leds.md)). The look, the page frame and how a page is built:
[web-design.md](web-design.md). The built-in recovery page and the recovery app:
[web-recovery.md](web-recovery.md).

## 1. Site map

```
Setup                     only while the device is not set up, no login
  Wi-Fi  ->  Web UI login  ->  Done
Login
Main pages                after login (the first one: LEDs)
  LEDs                    on, brightness, animation, color, speed, rotation, sound, status; motion auto off
                          (with a sensor); power and timing
  Layout                  canvas size, panels (lane, place, size, first LED, runs, zigzag),
                          preview of the data path, "Show the edges"
  Status                  device, Wi-Fi, hotspot, memory, uptime
  Wi-Fi                   saved networks (up to 5), add, forget, reconnect
  Hotspot                 on or off, name, password (none during the setup), new password
  MQTT                    broker, login, discovery prefix, topics; the connection's state
  Logs                    view and filter, debug mode
  System                  backup (export and import), firmware and file system update,
                          restart, factory reset
Recovery                  compiled into the firmware: login, update, format
```

There is no file manager. Settings and presets are exported and imported on the System page as
YAML, without secrets; an import may carry passwords added by hand.

## 2. Setup pages

The captive portal opens the first page. Three steps, shown as `1 / 3` in the header.

```
+--------------------------------------------+
| Set up pixeled-1a2b                 1 / 3 |
+--------------------------------------------+
| Choose your Wi-Fi                 [rescan] |
| +--------------------------------------+   |
| | Home-Net             [lock]  ||||    |   |
| | Neighbor             [lock]  |||     |   |
| | Guest                        ||      |   |
| | Other network ...                    |   |
| +--------------------------------------+   |
| Password                                   |
| [************************]          [show] |
|                              [  Connect  ] |
+--------------------------------------------+
```

The web UI login page follows. The last page shows where the device is now reachable:

```
+--------------------------------------------+
| Set up pixeled-1a2b                 3 / 3 |
+--------------------------------------------+
| Connected to Home-Net                      |
|                                            |
| Join Home-Net with this phone and open     |
|   http://pixeled-1a2b.local               |
|   http://192.168.1.57                      |
|                                            |
| [ ] Keep the hotspot on                    |
|                                 [  Done  ] |
+--------------------------------------------+
```

When the device joins the home network, the phone may lose the hotspot for a moment
(accepted). The page keeps polling and shows the result as soon as the phone is back. The hotspot
stays on, open, for 15 minutes after the first connection, so there is no hurry
([architecture.md](../../architecture.md) section 9.1).

## 3. Login

```
+--------------------------------------------+
| pixeled-1a2b                              |
+--------------------------------------------+
|                                            |
|   Username  [admin                  ]      |
|   Password  [************           ]      |
|   [x] Remember me                          |
|                             [  Log in  ]   |
+--------------------------------------------+
```

* **Remember me** stores a persistent cookie that lasts 90 days. Without it the
  cookie ends with the browser session.
* The cookie is a signed token holding the username and the expiry, so no session table is
  needed.
* **Anything that changes ends all tokens**: the username, the password, or a counter
  stored in NVS. The counter is increased by "log out everywhere" in the menu.
* **Change the password**: System, card "Password": the current password, the new one
  twice (8 to 64 characters). `POST /api/password` (`current`, `password`, the login of the
  session): a wrong current password waits 1 s and answers 400 (not 401, which would send the web
  UI to its login page). Every other session ends; the browser that changed it gets a new session
  cookie, not remembered. The username stays (a new one: the console's `login-reset` or a `webui:` file).
* **From a file**: a `webui:` section with `user` and `password` in the backup file
  replaces the login (same rules as the setup page; every
  session ends, the browser that imported signs in again). Import only: the device stores a
  hash, the export writes a commented hint where the section goes.
* How: the signing key is derived from the stored login record and the counter,
  `key = HMAC(stored password hash, counter)`. The stored hash is `hash(salt, username,
  password)` with PBKDF2. A new password or username gives a new hash, a new counter gives a new
  key, and either way every old token fails the check. The counter is kept out of the password
  hash itself, because the device stores only the hash: increasing a counter inside it would
  need the plain password. So "log out everywhere" works without asking for the password.
* Failed logins are slowed down (1 s). Log out is in the menu.
* PBKDF2-HMAC-SHA256 with 1000 iterations: 10000 took 5.3 s on the ESP32 (PSA crypto) and
  blocked the web server for every login; 1000 take about 0.4 s.

## 4. Main pages

**LEDs.** At the top the card "Live": the canvas as drawn (before the brightness), a cell per
LED, about 10 pictures a second over a WebSocket while the page is open (closed 3 s after leaving
it; at most 4 viewers). The live controls send on change (a slider while it moves, at most every
150 ms): the switch, brightness 1 to 255, the animation (an animation that needs sound, Spectrum,
only with an audio source), the color (only for an animation that uses one) and the speed, then
the rotation (0, 90, 180, 270 degrees) and "Reacts to sound" (only with an audio source and for
an animation that can react) with the sliders "Strength" (0 to 255, 128 as built), "Attack"
(0 to 2000 ms, 20 as built) and "Release" (0 to 5000 ms, 250 as built) below it while it is on
(always for Spectrum); all of them belong to the current animation, each one keeps its own (a
hint says so). The card "Overlay": show nothing, text or the clock, the color, the text (63
bytes, scrolling when wider than the matrix) with its scroll speed, or 24 hours for the clock;
Save. Below them the state (on with the frames per second, or off, or why
nothing is shown), "Sound" (hears sound or silence, with an audio source),
the estimated power with "limited" when the limit dims the frames, and the LEDs and lanes.
Refreshed every 2 s. The card "Sound input": 32 bars of the bands (bass on the left, grey in
silence), where the sound comes from (microphone, WARLS, WLED audio sync, nothing), the input
(sound with a "beat" badge, or silence), the loudest band and the reference (microphone) or the
packets (UDP) and the loudness, refreshed every 500 ms; a form with Save for the UDP inputs:
WARLS (a switch and its port) and WLED audio sync (a switch); with a microphone a second form:
gain (dB), automatic gain, silence below (dB), range (dB), beat threshold (times the bass), with
a hint how they work together. With a motion sensor the card "Motion": "Off when nobody moves" (a switch),
the minutes without motion (1 to 240, the sensor's hold: every motion starts them again; the hint
says that only an auto off is undone by motion), and the state: the sensor (motion with the time
it still holds, or clear) and the auto off ("off in 3 min without motion", "switched off by
motion, on with the next motion"). The card "Power and timing" is a form with Save: the power limit in W (0:
none), frames per second, fade time, supply settle time, the crossfade between animations (ms,
0: none), what the LEDs do at power on (as before, off, on), the power model (one color of one LED
at full, one LED while dark, in mW), with the hint that the estimate can be compared with a meter
at the supply, the output (RMT or parallel; disabled on a chip without the parallel output) and
dithering. The status also has the times of the last frame (`render_us`, `compose_us`,
`send_us` in the API).

Code: `components/leds`, `components/leds_api`, `web/leds.js`. API: `GET /api/leds` (state,
status, the list of animations with `slug`, `name`, `color`, `reactive`, and `config`),
`POST /api/leds` (any of `on`, `brightness`, `effect` (a slug), `color` (`#rrggbb`), `speed`,
`auto_off` (`1`, `0`), `auto_off_minutes`, `rotation` (`0`, `90`, `180`, `270`) and `reactive`
(`1`, `0`), `strength` (0 to 255), `attack` (0 to 2000 ms) and `release` (0 to 5000 ms) for the
animation given in the same request, else the current one; answers like GET; GET has `rotation`,
`reactive`, `strength`, `attack`, `release`, `audio` with `source` (an audio source is set up)
and `hears`, `motion` with `sensor`, `detected`, `auto_off`, `minutes`, `off_in_s`,
`off_by_motion`, and `overlay` with `mode`, `text`, `color`, `speed`, `clock_24h`),
`POST /api/leds/config` (`power_limit_mw`, `channel_mw`, `idle_mw`, `fps`, `fade_ms`,
`settle_ms`, `crossfade_ms`, `power_on` (`restore`, `off`, `on`), `driver` (`rmt`, `parallel`),
`dither` (`1`, `0`)), `POST /api/leds/overlay` (any of `mode` (`off`, `text`, `clock`), `text`,
`color`, `speed`, `clock_24h`; answers like `GET /api/leds`), `GET /api/leds/live` (a WebSocket,
login checked after the handshake: binary messages of 4 bytes width and height, 16 bits little
endian each, then RGB row by row; 10 a second, 5 for a canvas over 4096 pixels).
`GET /api/audio` (`running` and `microphone`, `source` (`""`, `microphone`, `warls`, `wled`),
`packets`, the analysis, `settings`, `inputs` with `warls`, `warls_port`, `wled_sync`),
`POST /api/audio` (the settings, `warls`, `warls_port`, `wled_sync`). The recovery app has none of them (404); its LED pages say so.

**MQTT.** The card "Connection" (state, the problem in words, connected for, the topics in use;
refreshed every 2 s) and the form "Broker": Connect (a switch), the address (`mqtt://host:1883`
or `mqtts://`), username, password (write-only: "stored" when there is one, an empty field keeps
it, "Forget the password" removes it), the discovery prefix (`homeassistant`) and the base topic
(empty: `pixeled/<device name>`); Save and connect. Code: `components/hass` (`api.cpp`). API:
`GET /api/mqtt`, `POST /api/mqtt` (`enabled`, `uri`, `username`, `password`, `forget_password`,
`prefix`, `base`). Backup section `mqtt` without the password (a comment says where it may go).

**Layout.** Two columns on a wide screen: the form and the preview. The form has the canvas width
and height and one box per panel: lane (the board's lanes), X, Y, width, height, the first LED (a
corner), whether the LEDs run along rows or columns, zigzag. Add panel (next lane, to the right of
the last), Remove, Save (`POST /api/leds/layout`, a problem such as "panel 2 overlaps another one
at 16,0" comes back as a toast), and "Show the edges", which switches the LEDs on with the edges
animation to compare the real wiring with the preview. The preview draws the canvas, every panel
in the color of its lane and the data path through its LEDs from a dot on the first one; it
follows every change of the form before it is saved. The page uses the whole width (`app-wide`).
Above the preview the card "Lanes": one row per lane of the board with the color order, the level
(255: as the others), red, green and blue (the color balance) and dithering; every change is saved
at once (`GET /api/leds/lanes`, `POST /api/leds/lanes` with `lane` and any of `order`, `level`,
`red`, `green`, `blue`, `dither`).
API: `GET /api/leds/layout` (`width`, `height`, `available_lanes`, `max_panels`, `panels` with
`lane`, `x`, `y`, `width`, `height`, `first` (`tl`, `tr`, `bl`, `br`), `runs` (`rows`, `columns`),
`zigzag`), `POST /api/leds/layout` (`width`, `height`, `panels`: one panel per `;`, its fields in
that order separated by `,`, `zigzag` or `straight` last).

**Logs.** Viewable over the web with a filter and a debug mode. No live view: the page shows the
log as of loading, `Refresh` loads it again.

Code: `components/logbuf`, `api_log.cpp`. The last 16 KB of esp_log messages in
PSRAM, the oldest lines go first, empty after a restart. Errors and warnings are always kept,
Debug (stored) keeps info messages as well (ESP-IDF has no "notice"). The console's `log` level
only changes the serial output. Filter: Errors, Warnings and errors, Everything (the default).
Times are the device's clock once it is set, else `+h:mm:ss` since the start. Download saves the
whole log as text. The bar right of the box drags the page wider than its 800 px,
kept in the browser, a double-click goes back to the default. API: `GET /api/log` `{"debug", "capacity"}`, `POST /api/log` `debug=1|0`,
`GET /api/log.txt` one line per message, `<unix time or 0> <esp_log line>`.

```
+--------------------------------------------+
| pixeled-1a2b            [Wi-Fi]  [=] |
+--------------------------------------------+
| Logs                         [Errors   v]  |
| +--------------------------------------+   |
| | 12:41:07 E wifi  wrong password      |   |
| | 12:41:12 I wifi  retry in 10 s       |   |
| | 12:41:22 I wifi  connected           |   |
| +--------------------------------------+   |
| [ Refresh ]  [ Download ]   Debug  [  o]   |
+--------------------------------------------+
```

**System.** Firmware and file system updates are uploaded here, see section 5. The
backup card sits above the update card, so the backup is seen before an update.

Backup: one YAML file in two parts, the settings (the sections `network`, `leds` (state, power
and timing, canvas and panels) and `webui`, import only, section 3) and the presets (none yet); Export all,
Settings only, Presets only (`GET /api/backup.yaml[?parts=settings|presets]`). A section of
something the device does not have is not written, and in a file from another device it is
ignored ("... (not on this device)"). No password is written; a Wi-Fi `password:` added to the
file by hand is used. Import and Check upload the file (`POST /api/backup/import[?check=1]`): the
whole file is checked first, one problem and nothing changes; the answer lists the sections, the
problems, what to know ("Office needs its password") and what was ignored. A network without a
password is saved as "password needed" and not tried until it gets one. Only the main firmware:
the recovery app has no backup (404), and its System page shows no Backup card.

```
+--------------------------------------------+
| pixeled-1a2b                 [Wi-Fi]  [=] |
+--------------------------------------------+
| System                                     |
|  Firmware  0.1.0      Uptime  3 d 4 h      |
|  Free memory  internal 84 KB, PSRAM 7 MB   |
| +--------------------------------------+   |
| | Backup (no passwords)                |   |
| | [ Export all ] [Settings] [Presets]  |   |
| | [ file ...  ]    [ Check ] [Import]  |   |
| +--------------------------------------+   |
| | Update                               |   |
| | [ Firmware ]       [ File system ]   |   |
| +--------------------------------------+   |
| | Device                               |   |
| | [ Restart ]     [ Factory reset ]    |   |
| +--------------------------------------+   |
+--------------------------------------------+
```

## 5. Firmware update

* **Update form**: the System page and the recovery page take a firmware image and a
  file system image, either as a file upload or as a URL the device downloads from.
* **OTA over the network**: the same authenticated endpoints take the raw
  image as the request body (`POST /api/update/firmware`, `/api/update/filesystem`), so `curl
  --data-binary` or `tools/update.py` push a build without a browser. `POST /api/update/url`
  makes the device download the image (http or https, common CA bundle, redirects followed);
  `GET /api/update` reports the app, what it can write and the progress. On a 4 MB board the
  main firmware answers 409 with `"recovery": true`; the page and the script restart the device
  into the recovery app (`POST /api/restart` with `to=recovery`) and send the image there.
* **Checks before writing**: a firmware image must be an app image for this chip whose project
  is `pixeled` (the recovery app and other firmware are refused); a file system image must carry
  the LittleFS magic. Only then is anything erased. A firmware image must also be built for this
  board: every image carries the board name right after `esp_app_desc_t` (ESP-IDF's custom app
  description, `px::board::ImageTag`), so an ESP32-DevKitC image on another ESP32 board is refused
  with "the firmware is for the ESP32-DevKitC, this is the ...".
* The firmware image goes to the inactive OTA partition and is booted after a check. If the new
  firmware does not confirm itself after boot, the bootloader rolls back (ESP-IDF app rollback).
* **Flash budget**: two OTA partitions on the 4 MB boards would leave about 1.9 MB per firmware
  image and little room for LittleFS, so they carry one OTA partition and the recovery app, see
  [architecture.md](../../architecture.md) section 9.5.

## 6. HTTPS

* There is no HTTPS web server. The web UI is plain HTTP only.
* TLS stays in use as a client: the update download by URL, later MQTT over TLS. On the S3 mbedTLS
  allocates from PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` in the board's defaults); the boards
  without PSRAM and the recovery app keep it in internal RAM.

## 7. Open points

* Live preview of the LEDs over a WebSocket, the calibration of the power model, presets
  ([leds.md](../../leds.md)).
