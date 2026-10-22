# PixeLED - architecture

PixeLED (`esp32-pixeled`) is an LED controller for WS2812-type strips and matrices on the ESP32,
ESP32-S3, ESP32-C3 and ESP32-C6, written in C++ on native ESP-IDF. It is controlled over MQTT
(with Home Assistant auto discovery) and a web UI, and aims at up to 8192 LEDs at 30 frames per
second on 8 parallel outputs.

This file describes the firmware as it is built. Something that is not built says so where it
stands ("not built"). The LED engine, its drivers and the feature plan are in [leds.md](leds.md);
open work is in [TODO.md](TODO.md), the history in [CHANGELOG.md](CHANGELOG.md).

The infrastructure (build, network, web server, storage, update, recovery app) comes from
`esp32-hassctrl`, the LVGL Home Assistant panel by the same author; its screens and its Home
Assistant client were left out. The LED features follow the clock and LED matrix plugin of
`kfc_fw` (Arduino); its behavior was the reference, its code was not carried over.

---

## 1. Principles and non-goals

* Boundaries the build enforces: a module that must not know about a driver or the network
  cannot include it (section 3).
* Every piece of mutable state has exactly one owning task. No shared state, no `volatile` flags
  (section 4).
* Pure logic (pixel mapping, effects, formats, the backup, the network choice) runs on the PC as
  unit tests (`host/tests`, `tools/host_test.py`).
* Adding a board touches one board file, one directory in `boards/` and one Kconfig choice,
  nothing else (section 8).
* First setup and network recovery work from a phone through an access point with a captive
  portal (section 9).
* Strips and matrices are set up at runtime with a few simple answers; custom shapes (a
  7-segment clock, hexagons, rings) are compiled in from a generated table ([leds.md](leds.md)
  section 4).

Not planned: BLE provisioning, a runtime shape editor, zones (one animation per device),
schedules, Ethernet, IR remotes, the ESP32-P4, clocked LEDs (APA102), RGBW
([leds.md](leds.md) section 1 lists what may come later).

---

## 2. Toolchain and build

* **Native ESP-IDF with `idf.py`** (CMake + Ninja), not PlatformIO: PlatformIO lags behind
  ESP-IDF releases.
* ESP-IDF **6.1**, installed with the ESP-IDF Installation Manager (EIM): the source in
  `C:\esp\v6.1\esp-idf`, the tools in `C:\Espressif\tools`. A terminal is prepared with EIM's
  activation script `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`.
* Dependencies come from the Espressif component registry, declared in each component's
  `idf_component.yml`, locked in `dependencies.lock` (committed).
* One build directory per board and application (`build/<board>/<app>`). Board selection is a
  Kconfig choice set by the board's defaults file. `tools/build.py` passes the layers and the
  target; see the README for the commands. The layers, a later one wins: `sdkconfig.defaults`,
  `boards/<board>/sdkconfig.defaults`, `sdkconfig.<app>`, then the `sdkconfig.defaults` of a
  variant (section 8, "Variants") when the build is for one (`<board>.<variant>`, build directory
  `build/<board>.<variant>/<app>`). ESP-IDF also reads `<layer>.<target>` next to each layer.
* One ESP-IDF project builds both applications: `PX_APP=main` or `PX_APP=recovery` excludes the
  other application's top-level component (section 9.5).
* FreeRTOS tick 1 ms (`CONFIG_FREERTOS_HZ=1000`, `sdkconfig.main`): the render task paces frames
  to the millisecond. Durations are converted with `pdMS_TO_TICKS`, never with a fixed tick.
* `-Werror=double-promotion` on our own components: the ESP32 and S3 have a single precision FPU,
  the C3 and C6 none; double math is slow everywhere.
* Host builds (tests) are a plain CMake project in `host/` that compiles the pure components
  directly, without the ESP-IDF build system. Compiler: MSVC of Visual Studio 2022, with the cmake
  and ninja that ESP-IDF installs.

---

## 3. Layers and components

Every module is an ESP-IDF **component** with its own `CMakeLists.txt`. `REQUIRES` in that file
is the dependency rule: a component that does not list another cannot include its headers.

```
                 +-------------------------------------------+
                 |  main/   composition root (app_main)       |
                 |  creates everything, wires the components |
                 +-------------------------------------------+
                    |          |           |           |          |
                    v          v           v           v          v
              +---------+ +---------+ +---------+ +----------+ +----------+
              |  leds   | |  mqtt   | |  audio  | |webserver | | platform |
              | render, | | HA dis- | | mic,UDP | | API, UI  | | network, |
              | effects | | covery  | |  mic    | |  files   | | fs, time |
              +---------+ +---------+ +---------+ +----------+ +----------+
                   |
                   v
              +-----------+     +-----------------------+
              |  output   |     | board: pins of a new  |
              |  drivers  |     | device, shared buses  |
              +-----------+     +-----------------------+
```

Built: `leds` (the render task), `output` (RMT), `layout`, `effects`. Not built yet: the parallel
outputs, `mqtt` and `audio` ([leds.md](leds.md), [TODO.md](TODO.md)).

| Component | Depends on | Pure (host build) | Responsibility |
| --- | --- | --- | --- |
| `memory` | nothing | yes | The heap for large data: `main` points it at PSRAM at start where there is one, the PC build uses malloc. `memory::Vector` and `memory::String` allocate from it |
| `formats` | `memory` | yes | `JsonWriter` (the web server's answers), the YAML subset reader of the backup, the multipart reader of uploads |
| `config` | `formats`, `memory` | yes | The backup file: sections with write, check and apply (section 9.3) |
| `platform` | `littlefs`, `nvs_flash` | no | Boot log, both NVS partitions, LittleFS at `/fs`, the settings class, the device name, SNTP and the time zones |
| `network` | `esp_wifi`, `esp_netif`, `mdns` | no | Station and access point state machine, scans, stored networks (section 9) |
| `webserver` | `esp_http_server`, `network`, `ota` | no | Captive portal, login, setup, status, Wi-Fi, update, files, log and backup API; the web UI from LittleFS (section 9.4) |
| `ota` | `app_update`, `board` | no | Firmware and file system updates, rollback, the recovery app switch (section 9.5) |
| `board` | `esp_driver_i2c` | no | The constants of the board (pins of a new device), the image tag, the shared I2C bus (section 8) |
| `output` | `esp_driver_rmt` | no | The LED lanes: the RMT backend ([leds.md](leds.md) section 2.2) |
| `layout` | `memory` | yes | Canvas and panels, the table canvas -> LED ([leds.md](leds.md) section 4.1) |
| `effects` | nothing | yes | The animations: canvas, parameters, time, the memory between frames, the rotation, the audio for the reactive ones ([leds.md](leds.md) section 6) |
| `leds` | `effects`, `layout`, `output`, `platform` | `frame.cpp` | The render task: state, fade, gamma, power limit, the supply, NVS ([leds.md](leds.md) section 3) |
| `audio` | `effects`, `platform`, `webserver` | `analysis.cpp` | The I2S microphone: its task, the FFT, bands, gain and beat, its settings (NVS) and `/api/audio`; the render task takes the result with each frame ([leds.md](leds.md) section 6.1) |
| `presets` | `leds`, `effects`, `platform` | no | The presets: the light under a name, on LittleFS (`/fs/presets.bin`), applied through the render task's commands ([leds.md](leds.md) section 10) |
| `leds_api` | `leds`, `presets`, `webserver` | no | `/api/leds` and `/api/presets`, registered by main through `webserver::Config::add_routes`; the live socket gets the sound through a getter from main, not from `audio` |
| `hass` | `espressif/mqtt`, `cjson`, `leds`, `presets`, `webserver` | no | Home Assistant over MQTT: connection, discovery, commands, state; `/api/mqtt` ([leds.md](leds.md) section 8) |
| `input` | GPIO, PCNT, `platform`, `webserver` | `button.cpp` | Buttons and a rotary encoder: events, their actions (settings), `/api/input`; the actions go to a handler in main (section 9.6) |
| `relay`, `motion`, `light_sensor` | GPIO, I2C, ADC drivers | no | Optional hardware (section 9.6) |
| `logbuf`, `devconsole` | `log`, `console` | no | The web UI's log, the serial console |
| `main` | everything | no | `app_main`: start order, the function tables between the components, error policy at boot |

Rules:

1. `memory`, `formats` and `config` include no ESP-IDF or FreeRTOS header. They are compiled by
   the host build, which proves it. The pure parts of the LED engine (mapping, effects) follow
   the same rule.
2. The web server and MQTT never touch the LED state. They post commands to the render task and
   read snapshots (section 4).
3. `main` is the only place that knows every component. It contains wiring, not logic.

---

## 4. Tasks, ownership and data flow

| Task | Created by | Owns |
| --- | --- | --- |
| **network task** | `network` | the Wi-Fi driver, the saved networks, the hotspot |
| **web server task** | `esp_http_server` | one request at a time; static buffers of its handlers |
| **render task** | `leds` | the LED state: animation, parameters, brightness, the layout, the frame buffers, the output, the supply ([leds.md](leds.md) section 3); core 1 on the dual-core chips, priority 5 |
| **mqtt task** | `espressif/mqtt` | the connection; its event handler announces the device and posts commands to the render task; the web server's task replaces the client when the settings change (the handle under a lock) |
| console task | `devconsole` | the serial command line |

* **Commands in, snapshots out.** Other tasks post a command to the owner's FreeRTOS queue (16
  entries) and read a copy of its state under a mutex (`status()`). A blocking call that needs
  the answer (an import that checks first) waits on the owner with a timeout.
* **Listeners** are plain function pointers with a `void *context`, called on the owner's task.
* Cross-component wiring goes through structs of function pointers filled in `main`
  (`webserver::BackupAccess`), so the lower components do not depend on each other.
* esp_timer for periodic sampling (motion, delayed settings writes); a dedicated task only for
  blocking I/O (light sensor, network) or real-time work (rendering).

---

## 5. LED engine

The render task, the layout, the first animations and the RMT output are built; the design and
what is still open (parallel drivers, presets, audio, MQTT) are in [leds.md](leds.md).

## 6. MQTT and Home Assistant discovery

Built: [leds.md](leds.md) section 8.

## 7. Audio input

Built: an I2S microphone with an FFT on the device (`audio`, [leds.md](leds.md) section 6.1),
started by `main` when the board has `mic_sd_gpio`; `leds::Start::audio` hands each frame the
latest analysis. Not built yet: spectrum packets over UDP.

---

## 8. Boards (`board`)

A board is the hardware the firmware is built for: a dev kit or a controller PCB. Its file in
`components/board/boards/<name>.cpp` holds a `constexpr board::Info`: the name, the LED data pins
and the pins of the optional hardware of a new device. `-1` is a pin that is not there.

| Board | Target | Flash, PSRAM | LED lanes (default pins) | Partitions |
| --- | --- | --- | --- | --- |
| `esp32s3_devkit` | ESP32-S3 | 16 MB, 8 MB octal | 8: 4, 5, 6, 7, 15, 16, 17, 18 | 16mb.csv |
| `esp32_devkit` | ESP32 | 4 MB, none | 8: 13, 14, 16, 17, 18, 19, 23, 25 | 4mb.csv |
| `esp32c3_devkit` | ESP32-C3 | 4 MB, none | 2: 4, 5 | 4mb.csv |
| `esp32c6_devkit` | ESP32-C6 | 8 MB (4 used), none | 4: 18, 19, 20, 21 | 4mb.csv |

* **Selection**: a Kconfig choice (`components/board/Kconfig`, `PX_BOARD_*`) set by
  `boards/<name>/sdkconfig.defaults`, which also sets the target, flash, PSRAM and partition
  table. The board's CMake adds only its own file.
* **Image tag**: every board file defines `px_image_tag` (`board::ImageTag`, magic `PXBT` and the
  board name) in `.rodata_custom_desc`, right after ESP-IDF's app description. An update is
  checked against the running board before anything is written (`ota`).
* **Variants**: one unit's add-ons (a relay, a motion sensor, other LED pins) are a private copy
  of `components/board/src/variant/px_board_variant.hpp` in `boards/<board>/variants/<name>/`
  (not in git), whose `apply()` changes fields of the board's `Info` at compile time.
  `tools/build.py <board>.<name>` builds it, with the variant's own `sdkconfig.defaults` as the
  last layer when there is one. See [adding-a-board.md](adding-a-board.md).
* **Bring-up**: `board::init` starts the shared I2C bus when the board names its pins. Nothing
  else: the LED outputs belong to the LED engine, the relay, motion sensor and light sensor to
  their components, started by `main`.

### 8.1 Pins at runtime

The board's pins are the defaults of a new device. The LED settings (lanes, pins, counts) and a
hardware page for the optional parts will change them at runtime, stored in NVS, so one firmware
fits every wiring of a board (not built: `main` uses the board's pins as they are).

---

## 9. Setup, settings and platform services

### 9.1 Setup and network

* No BLE, no screen. A device is set up from a phone or a PC through the device's access point
  (**Hotspot**) with a captive portal: the network, its password, and the username and password
  of the web UI.
* **Open during the first setup**: there is no screen to show a password, so the hotspot of the
  first setup (no network saved, or none connected yet) is open, named like the device
  (`pixeled-xxxx`). The setup page sends the Wi-Fi password over it, as a known default password
  would (WLED's `wled1234`); the risk is limited to the minutes of the setup.
* **Hotspot password**: generated randomly on the first boot, 8 digits and 2 lowercase letters,
  stored in NVS. Every hotspot after the setup takes it; the web UI's Hotspot page shows it and
  generates a new one.
* **After setup**: the hotspot stays on, still open, for 15 minutes after the first connection,
  so the phone can see the result, then turns off unless the user chose to keep it on (from then
  on with the password).
* **No network**: while no network is saved the hotspot is on.
* **Headless recovery** (built 2026-10-21): a device with saved networks that has had no
  connection for 5 minutes (`network::kOfflineHotspotMs`: the networks out of reach, a changed
  password) opens the hotspot with its password, and keeps trying its networks. The board's
  hotspot button (`Info::hotspot_button`, button 1 unless the board names another) held while the
  device starts (2 s; `main`, `network::open_hotspot`) opens it without a password, as in the first
  setup: whoever holds the button is at the device.
  * WLED controller rev 1.1 (the test board, variant `esp32_devkit.wled_controller_rev1_1`):
    button 4 (BTN3, GPIO 19). Hold it, press the reset button (BTN0, the EN line), keep it down
    2 s more; or hold it while the power comes on.
  * WLED controller rev 1.3 (variant `esp32_devkit.wled_controller_rev1_3`): button 1 (BTN0,
    GPIO 13, no strapping pin; the board has no reset button): held 3 s it restarts the device,
    kept down through the restart it opens the hotspot.
  * Espressif dev kits: button 1 is BOOT (a strapping pin: held through a reset the chip waits for
    a download), so it is pressed right after the reset, within about 0.8 s (the check runs then
    on the ESP32).

  Both stay
  on until 15 minutes after the next connection, then the hotspot goes off (unless kept on).
  Console `hotspot open` does what the button does. The open hotspot was tried on the test board
  2026-10-21 with the console (on, open, off in 15 minutes); the button (BTN3 with the reset, rev
  1.1) by the user 2026-10-22; the 5 minutes offline with `wifi pause 400` 2026-10-22 (the
  hotspot came after 300 s with its password, stayed 15 minutes after the reconnect). The hotspot of a device without Wi-Fi
  always takes the password: an open time left from a button or a setup ends when it opens (only a
  first setup that never connected stays open). Console `wifi pause <s>` drops the Wi-Fi for a
  while, as if the networks were gone, to try it without the router.
* **Up to 5 networks**, added on the web UI's Wi-Fi page.
* **Hotspot and station run at the same time** whenever the hotspot is on. A phone on the hotspot
  may lose its connection for a moment when the station connects or changes channel.
* Naming: on the web UI the two Wi-Fi roles are **Wi-Fi** (station) and **Hotspot** (AP). Code
  and developer docs use the ESP-IDF terms AP and STA.
* Web layouts: [ui/web/web-app.md](ui/web/web-app.md), design rules in
  [ui/web/web-design.md](ui/web/web-design.md).

### 9.2 Network selection and reconnect

* **Up to 5 saved networks, two policies**: the entered order, or the strongest signal. With the
  strongest signal the networks are sorted by signal at runtime and that order is never saved.
  The choice is plain C++ (`network/src/selection.cpp`, host tested).
* **On connect** (boot, or after the connection is lost): one scan. With the entered order, the
  first saved network in range whose signal is usable (-75 dBm or better); if none is, the
  strongest. Of several access points with the network's name (mesh, repeaters) the strongest is
  taken, and the connect names its BSSID.
* **While connected**: no periodic switching. Only when the signal stays below -70 dBm for 30 s
  does a background scan run, and the device switches only to an access point at least 8 dB
  stronger (`px/network/network.hpp`), Apple's published roaming numbers.
* **Fast reconnect**: DHCP skips the ARP probe of the offered address and asks for the last
  address again.
* A router with WPA3 and protected management frames keeps the connection of a device that was
  reset for a few seconds and does not answer its new login in time (reason 2). The first retry
  comes after 1 s and goes to the same network. A network saved without a password against a
  secured one ends in reason 210, shown as "the network needs a password, add it again".
* Wi-Fi power save is off: the controllers run on mains power, and realtime input
  ([leds.md](leds.md) section 7) needs every packet at once.

### 9.3 Storage

* **LittleFS** for files (`/fs`): the web UI (`/fs/www`, gzipped) and later the presets. It is
  power-loss safe and does its own wear leveling. A LittleFS that fails to mount is damaged and
  never formatted automatically, so its readable files can be exported first; a partition with no
  LittleFS at all (a new device) is formatted.
* **NVS** in two partitions: `nvs` for the Wi-Fi driver and PHY, `settings` for the firmware,
  one namespace per component. Secrets (Wi-Fi passwords, the hotspot password, the web UI
  password hash, later the MQTT password) live only in NVS, never in a file the web UI can
  download.
* The Wi-Fi driver's own NVS copy of the station config is off
  (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`), so `network` is the only owner of the networks.
* **Settings** are one blob per owner (a POD struct with a range check on load and defaults for
  anything out of range). A struct that changes gets a new size; the loader converts an older
  size.
* **The write policy** (`px/platform/settings.hpp`):

  | Kind of value | Written | Examples |
  | --- | --- | --- |
  | changed by a live control or by using the device, in bursts | `platform::set_blob_later`: 10 s after the last change, at most 60 s after the first, and at a restart | `leds/state` (on, brightness, animation, color, speed), `leds/styles` (rotation, reactive, strength, attack, release; sliders and Home Assistant numbers) |
  | saved by an explicit action (Save, a form), or a secret | at once, with commit; a failure is reported | Wi-Fi networks, policy and hotspot, web UI login, MQTT settings and password, time zone and SNTP servers, LED layout, config, lanes, overlay, auto off, audio settings and inputs, the buttons' actions; the presets (a file) |
  | device state that changes rarely | at once, when it changed | `leds/boot` (switched off by motion), `network/last_seen` |
  | developer and calibration | at once | console log level, `logbuf/debug`, the light sensor |

  Nothing writes per frame: a value that can change more often than about once a minute takes
  the delayed write. Its owner reads it from its RAM copy, not back from NVS. NVS skips a value
  that did not change, so writing the same one again costs no flash. The delayed table has 8
  keys of up to 288 bytes; a ninth key writes the waiting ones at once. A factory reset drops the
  waiting values, so none of them comes back with the restart. A backup import sets the live
  values through the same calls, so they are stored within 10 s.
* **Backup**: one YAML file with two parts, the settings and the presets
  (`config::backup::Part`). The sections come from a table filled in `main/backup.cpp`; each
  writes its fields and reads them back. An import is checked completely first and applied only
  when no section found a problem. Never a password in an export; an import may carry them,
  added by hand. Built sections: `network`, `leds`, `audio`, `mqtt`, `time`, `input`, `webui`
  (import only); `presets` in the presets part.
* **Logs** are viewable in the web UI (`logbuf`): errors and warnings always, info messages as
  well in the debug mode (stored).

### 9.4 Components

| Component | Responsibility |
| --- | --- |
| `platform` | boot log, both NVS partitions, LittleFS at `/fs`, the settings class, the device name (`pixeled-` and the last two MAC bytes), SNTP with its two servers and the time zone, an IANA name or a POSIX TZ rule (`time_sync.cpp`, the IANA zone table in `timezone.cpp`, built by `tools/build_timezones.py`) |
| `network` | the station and hotspot state machine of sections 9.1 and 9.2, up to 5 stored networks, the hotspot password, scan results, stored in the settings NVS. One task owns the driver and the state; commands are queued, readers get copies (`status()`). Files: `network.cpp` (the state, the task, start), `station.cpp` (scan, connect, retry, roam), `commands.cpp` (the saved networks), `api.cpp` (the calls of other tasks). mDNS: the device answers as `<device name>.local` and announces `_http._tcp`, in both apps |
| `webserver` | captive portal DNS and redirect, the status API and the built-in page, the setup pages, login, Wi-Fi, hotspot, update, files, log and backup API, the time API (`register_time_routes`, registered by `main` only). Serves the web files from LittleFS, with a recovery page compiled in that works without the file system ([ui/web/web-recovery.md](ui/web/web-recovery.md)). `Config::add_routes` lets `main` register more routes (the LED, audio, MQTT and time API). Routes are made with `detail::route()` (the main firmware has WebSocket support, `CONFIG_HTTPD_WS_SUPPORT`, which adds fields to `httpd_uri_t`) |
| `ota` | firmware and file system images from an upload, a tool or a URL download, app rollback ([ui/web/web-app.md](ui/web/web-app.md) section 5), restart into the recovery app and back, partition layout of section 9.5 |
| `devconsole` | serial command line for development (`CONFIG_PX_DEV_CONSOLE`): Wi-Fi networks, hotspot (`hotspot open` as the button at boot), file system, memory, log level, light sensor, relay, motion sensor, audio, LEDs (palette, presets), buttons (`input`), MQTT (`mqtt`, its settings without the password), restart, factory reset, a crash on purpose (`crash yes`) (`help` lists them) |
| `logbuf` | the log in a ring buffer for the web UI's log page |
| `relay` | relays on GPIOs: the LED power supply (section 9.6) |
| `motion` | a motion sensor on a GPIO (section 9.6) |
| `light_sensor` | ambient light: a BH1750 on the board's I2C bus or an LDR on an ADC1 pin (section 9.6) |
| `input` | buttons and a rotary encoder (section 9.6) |

### 9.5 Partition layout and the recovery app

Two equal OTA partitions on a 4 MB board would leave about 1.9 MB per firmware image. Instead
every board carries a second, small firmware: the **recovery app**. It is the ESP-IDF factory
app, built from the same repository and the same components (`platform`, `network`, `webserver`,
`ota`), without the LED engine. It reads the same NVS, so Wi-Fi, hotspot and web UI login are the
same as in the main firmware.

| Partition | 4 MB boards | 16 MB boards |
| --- | --- | --- |
| nvs, otadata, phy | 28 KB | 28 KB |
| factory: recovery app | 1.19 MB | 1.25 MB |
| ota_0: main firmware | 2.25 MB | 5.75 MB |
| ota_1: main firmware | none | 6 MB |
| LittleFS | 448 KB | 2.9 MB |
| settings (the firmware's NVS) | 64 KB | 64 KB |

* **Update on a 4 MB board**: the main firmware cannot overwrite itself, so it reboots into the
  recovery app, which receives the new image, writes it to ota_0 and boots it. On a 16 MB board
  the main firmware writes the other OTA partition directly. `esp_ota_get_next_update_partition()`
  tells the two cases apart, so the update code is the same.
* **Recovery for free**: when the main firmware is broken, does not confirm itself after an
  update, or an update was interrupted by a power loss, the bootloader starts the recovery app.
  The main firmware confirms itself once the network and the web server run.
* **No LEDs in the recovery app**: it holds the LED power switch off at its first line and logs
  its state (Wi-Fi, hotspot, an update in progress) to the serial port and the web UI's log when
  it changes. The hotspot is always on in this app (`network::force_hotspot`, not stored).
* **The recovery app changes only over USB** (`tools/build.py <board> --app recovery -p COMx
  flash`, which also writes the bootloader and the partition table): no update writes `factory`,
  the main firmware's update only `ota_0`/`ota_1` and `storage`.
* **Web pages in the recovery app**: the built-in page (always at `/recovery`, at `/` without web
  files) with first setup, login, status, firmware and file system update by file or URL, the
  files as .zip, format, restart and "Start the firmware"; and the web UI from LittleFS when it
  is there, without the backup and the LED pages (their routes are not registered: 404).
* IDF's build warns that the main image does not fit `factory` on the 16 MB layout; it never goes
  there.

### 9.6 Optional hardware

All optional parts are named by the board (`board::Info`, -1: not fitted) and started by `main`;
one that is not fitted or does not answer is logged and left out.

* **LED power switch** (`relay`, `Info::power_gpio`): a relay or MOSFET in the supply of the LEDs.
  Low before the pin becomes an output, so the LEDs are off while the board starts; the recovery
  app drives it off too. The LED engine switches it on before the first frame (after a settle
  time) and off after the LEDs faded out. The panic handler switches it off first
  (`relay::off_at_crash`; `relay` wraps `esp_panic_handler` with `-Wl,--wrap`, as ESP-IDF's
  Bluetooth controller does, the wrapper in IRAM writes the pin's register), see
  [leds.md](leds.md) section 5. Relay 1 is the supply
  (active high or low, `Info::power_active_high`), relay 2 its indicator LED
  (`Info::power_led_gpio`); console `relay 1 on` switches the supply by hand.
* **Motion sensor** (`motion`, `Info::motion_gpio`, `motion_active_high`): sampled every 100 ms by
  an esp_timer; a level that holds for two samples is the sensor's state. The detected state
  turns on with the sensor and off only when it saw nothing for the hold time (10 s to 240 min,
  the LEDs' auto off minutes set it). It drives the LEDs' auto off and the motion binary sensor
  over MQTT ([leds.md](leds.md) section 9).
* **Light sensor** (`light_sensor`): a BH1750 on the board's I2C bus (0x23 or 0x5C), averaged
  over a second, else an LDR on an ADC1 pin with a calibration table. Will drive the automatic
  brightness (not built). Console `light` shows and calibrates it.
* **Microphone** (`audio`): an INMP441 on the board's `mic_sck_gpio`, `mic_ws_gpio` and
  `mic_sd_gpio` (I2S1 on the ESP32). Console `audio` shows its analysis.
* **Buttons and a rotary encoder** (`input`, `Info::button_gpios` (up to 4, button 1 the board's
  own; -1 for one that is not there), `hotspot_button`, `encoder_a_gpio`, `encoder_b_gpio`,
  `encoder_button_gpio`): an esp_timer samples them
  every 10 ms; clicks, double clicks and holds become actions, which `main` maps to the LEDs, the
  presets, the hotspot and the restart ([leds.md](leds.md) section 9). The encoder is counted by
  the PCNT, by GPIO interrupts on the C3. Console `input` shows them.
* A temperature sensor is planned ([leds.md](leds.md) section 9).

---

## 10. Repository layout

```
esp32-pixeled/
  CMakeLists.txt                 ESP-IDF project, both applications
  sdkconfig.defaults             shared Kconfig defaults
  sdkconfig.main, sdkconfig.recovery   per application
  partitions/                    4mb.csv, 16mb.csv
  boards/<name>/sdkconfig.defaults   target, flash, PSRAM, partition table, board choice
  boards/<name>/variants/<variant>/  one unit's add-ons: px_board_variant.hpp, sdkconfig.defaults
  main/                          app_main and the wiring: main.cpp, backup.cpp
  recovery/                      the recovery app (section 9.5): its own app_main, same components
  components/
    <name>/      include/px/<name>/*.hpp  src/   one per component of sections 3 and 9.4
    board/       include/px/board/board.hpp  boards/<name>.cpp  src/variant/ the variant that changes nothing
  host/
    CMakeLists.txt  the host build (MSVC), python tools/host_test.py builds and runs it
    tests/       unit tests for formats, the backup, the network choice, the time zones;
                 stubs/ of ESP-IDF headers
  web/                           the web UI (index.html, app.js, app.css, vendor/bootstrap.min.css)
  tools/
    build.py, build_fs.py, update.py   firmware, file system image, update over the network
    host_test.py, msvc.py              host tests (msvc.py finds MSVC, cmake, ninja)
    test_web.py                        the web API against a device
    build_timezones.py                 the zone table, generated and committed
  docs/
    architecture.md              this file
    leds.md                      the LED engine: drivers, layout, animations, MQTT, plan
    TODO.md, CHANGELOG.md
    adding-a-board.md            how to add a board or a variant
    ui/web/                      the web UI: pages, design rules, recovery page
```

---

## 11. Conventions

* C++23 as supported by the ESP-IDF toolchain. No exceptions, no RTTI. Errors cross ESP-IDF
  boundaries as `esp_err_t` (`ESP_RETURN_ON_ERROR`, `ESP_RETURN_ON_FALSE`); pure code returns
  `std::expected<T, Error>`. `ESP_ERROR_CHECK` only at boot for storage and the event loop; a
  part that can be missed is logged with `ESP_LOGW` and left out.
* One namespace per component (`px::network`, `px::board`, ...), private parts in `detail` or an
  anonymous namespace. Public headers in `include/px/<component>/`, everything else private to
  the component.
* Names: types and enums PascalCase (enums sized `: uint8_t`), functions and variables
  snake_case, constants `kCamelCase`, file-level state `g_`, private members `_`. One
  `constexpr const char *kTag` per file; log messages are lower-case English sentences.
* No singleton classes: a component is a set of free functions with file-static state and
  `start()` / `running()` / `status()`. RAII for resources (`Settings`).
* Files stay small. A file over about 600 lines is a design smell to discuss, not a target;
  accepted: `devconsole.cpp` (a development tool).
* Memory: large buffers in PSRAM where there is one (`memory::allocate_large`,
  `heap_caps_malloc_prefer`); DMA buffers and code that runs with the cache off in internal RAM,
  asked for with `heap_caps_malloc`. No allocation per frame in the render path. The console
  command `mem` shows what is free.
* Comments explain why, not what, often with a measurement and its date. Behavior is specified
  in `docs/`; headers point there.
