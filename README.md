# PixeLED

An LED controller for WS2812-type strips and matrices on the ESP32-S3, ESP32, ESP32-C3 and
ESP32-C6, written in C++ on native ESP-IDF 6.1. Controlled over MQTT (Home Assistant auto
discovery) and a web UI; the target is 8192 LEDs at 30 fps on 8 parallel outputs.

The design is in [docs/architecture.md](docs/architecture.md) (infrastructure) and
[docs/leds.md](docs/leds.md) (the LED engine and its plan); open work in
[docs/TODO.md](docs/TODO.md).

## Features

Built:

* Wi-Fi with up to 5 networks, entered order or strongest signal, roaming on a weak signal.
* Hotspot with a captive portal for the first setup (open while setting up), mDNS
  `<device>.local`.
* Web UI: status, Wi-Fi, hotspot, log, system (update, backup, restart, files), login.
* Firmware and file system updates by upload or URL, rollback, a recovery app in the factory
  partition.
* Backup of the settings in one YAML file (settings and presets parts).
* Settings in NVS by one write policy: live changes 10 s after the last one (at most 60 s), saves
  and secrets at once.
* LED output: parallel by DMA on the ESP32 (I2S, up to 16 lanes, encoded into a small ring while
  sending; 8 x 1024 LEDs at 32 fps), RMT on the other chips or by choice.
* Layout from three answers per panel (size, first LED, rows or columns with zigzag), the web
  page draws the data path; test pattern "edges".
* Render task: animations solid, rainbow, plasma, breathe, fire, gradient, color fade, twinkle,
  meteor, ripple, matrix rain, noise, spectrum; crossfade between animations; brightness with
  fade, gamma, power limit with an adjustable model, the LED supply switched with a settle time;
  stored and restored.
* Each animation turns by 0, 90, 180 or 270 degrees; all but solid react to sound with their own
  switch, strength, attack and release.
* Text and clock overlay over every animation (5 x 7 font with umlauts, scrolling).
* At power on: as before, off or on.
* Web: LEDs page (live preview, on, brightness, animation, color, speed, rotation, sound
  settings, overlay, status, sound input with the bands live, power and timing) and Layout.
* MQTT with Home Assistant discovery: light with the animations as effects, speed, rotation,
  reacts to sound, sound strength, attack and release, overlay and its text, power on, power,
  motion, motion auto off, restart.
* Motion auto off: LEDs off N minutes after the sensor clears, back on with the next motion (also
  after a restart).
* I2S microphone (INMP441) on the ESP32: 32 bands, loudness and beat with an automatic gain;
  settings for gain, automatic gain, silence level, range and beat threshold with a live view
  of the bands; animation "Spectrum" (a rainbow bar per band, only listed with a sound source);
  console `audio`.
* UDP sound inputs: WARLS spectrum packets and WLED audio sync (multicast); `tools/audio_send.py`
  sends a demo or the PC's sound.
* The LED supply goes off in the panic handler.
* Optional hardware started from the board's pins: LED power switch (relay or MOSFET, off at
  boot), motion sensor, light sensor (BH1750 or LDR), microphone.
* Serial console for development.

Planned ([docs/leds.md](docs/leds.md)):

* Parallel LED output on the S3 (LCD_CAM) and the C6 (PARLIO).
* Custom shapes compiled in.
* Palettes, presets and playlists.
* Realtime input: DDP, E1.31, Art-Net.
* Buttons, rotary encoder, temperature protection, power limit, gamma.

## Supported hardware

| Board | Chip | LED lanes |
| --- | --- | --- |
| `esp32s3_devkit` | ESP32-S3-DevKitC-1 N16R8 | 8 (16 planned) |
| `esp32_devkit` | ESP32-DevKitC (WROOM-32, 4 MB) | 8 |
| `esp32c3_devkit` | ESP32-C3-DevKitM-1 | 2 |
| `esp32c6_devkit` | ESP32-C6-DevKitC-1 | 4 |

The default pins are in `components/board/boards/<board>.cpp`; a unit with other wiring is a
variant ([docs/adding-a-board.md](docs/adding-a-board.md)).

## Build

Open a PowerShell prepared with the ESP-IDF activation script of EIM, then use the build script.
It keeps one build directory per board and application (`build/<board>/<app>`). In a plain shell
every tool fails with "IDF_PATH is not set"; the activation line goes first in every PowerShell
call (also for `tools/build_fs.py` and `tools/update.py`).

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1

python tools/build.py esp32s3_devkit                      # build the main firmware
python tools/build.py esp32s3_devkit --app recovery       # build the recovery app
python tools/build.py esp32s3_devkit menuconfig           # configure one board and app
python tools/build.py esp32s3_devkit.kitchen              # the variant "kitchen" of the board
```

## Tests on the PC

```powershell
python tools/host_test.py              # build and run every test (MSVC of Visual Studio 2022)
python tools/host_test.py backup_      # only the tests whose name contains "backup_"
```

## Flash

The recovery app goes to the factory partition and brings the bootloader and the partition table
with it, so it is flashed first. The main firmware goes to the OTA partition; the build script
does that instead of a plain `idf.py flash`, which would overwrite the recovery app.

```powershell
python tools/build.py esp32s3_devkit --app recovery -p COM9 flash
python tools/build.py esp32s3_devkit -p COM9 flash monitor
python tools/build_fs.py esp32s3_devkit -p COM9          # the web UI
```

Flashing the recovery app also resets the boot selection: the device starts the recovery app
until a main firmware is flashed or uploaded. Firmware and web UI go together: a firmware with a
change to the API is flashed with the web image built from the same commit.

## First setup

A new device opens an open hotspot named `pixeled-xxxx`. Join it with a phone; the setup page
opens by itself (captive portal), else go to `http://192.168.4.1/`. Choose the Wi-Fi network,
enter its password and create the web UI login. The hotspot stays on for 15 minutes after the
first connection, then turns off; later it takes the password shown on the web UI's Hotspot page.

## Web UI

The web pages are in `web/` (plain HTML, CSS and JavaScript, Bootstrap 5.3 CSS in
`web/vendor/`, no build step). Every main firmware build also builds the file system image
`build/<board>/fs/storage.bin` from them (`tools/build_fs.py`: gzipped into `/www`, sized for
the board's "storage" partition). Upload it on the System page, with `tools/update.py`, or over
serial with `python tools/build_fs.py <board> -p COM9`. Without the image the device serves its
built-in page; `/recovery` always serves that page.

## Update over the network

Once a device runs, firmware and file system images go over the network: the System part of the
web page (file or URL), or the script, which logs in, goes through the recovery app on 4 MB
boards and waits for the new firmware:

```powershell
python tools/update.py 192.168.0.206 firmware build/esp32s3_devkit/main/pixeled.bin
python tools/update.py 192.168.0.206 filesystem build/esp32s3_devkit/fs/storage.bin
```

The password is asked for, or taken from `PX_PASSWORD`. `python tools/test_web.py 192.168.0.206`
checks the web server of a running device (login, refusals, load, a stalled client); it changes
nothing on the device.
