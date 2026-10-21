# PixeLED - changelog

Every change to the firmware, the web UI, the tools and the docs, newest first, under the day it
was made; a release gets its version as the heading. Each entry says what a user or a developer
sees: a new feature under Added, a different behavior under Changed, a repaired one under Fixed,
and what is gone under Removed. Updated in the same commit as the change ([CLAUDE.md](../CLAUDE.md)).

## Unreleased (0.1.0)

### 2026-10-20

Added

* Motion auto off: the LEDs fade off after N minutes without motion and come back with the next
  motion, but only after an auto off (off by hand stays off). LEDs page card "Motion", Home
  Assistant switch "Motion auto off" and number "Auto off without motion", console
  `leds autooff`, backup `leds.auto_off` and `leds.auto_off_minutes`.
* The motion sensor's hold time can be set (10 s to 240 min, `motion::set_hold_s`) and the
  seconds it still holds are read (`motion::hold_left_s`); the LEDs page and the console show them.
* The repository is on GitHub (`sascha432/esp32-pixeled`); board variants stay out of git.

Changed

* Motion auto off is one timer: the minutes are the sensor's hold, every motion restarts it, the
  LEDs go off when it runs out. The first build of the day waited the sensor's fixed 5 min hold
  and then the minutes.
* Home Assistant: the speed is sent only when it changes; no `object_id` in the discovery (HA
  ignores it).

Fixed

* Console: an unknown `leds` word no longer suspends the LEDs (`leds autooff` without its value
  did); `leds autooff` alone shows the setting.
* The LED status (the hold's countdown) stays current while the render task is idle.

### 2026-10-19

Added

* The project: the infrastructure of `esp32-hassctrl` (ESP-IDF 6.1, layered sdkconfig,
  `tools/build.py`, network with hotspot and captive portal, web server with login, update and
  rollback, recovery app, settings and LittleFS, log buffer, backup, serial console, host tests),
  renamed `hc` to `px` and `hassctrl` to `pixeled`. The design of the LED engine: [leds.md](leds.md).
* Boards: ESP32-S3-DevKitC-1 N16R8, ESP32-DevKitC, ESP32-C3-DevKitM-1, ESP32-C6-DevKitC-1, with
  the default LED, relay, motion, I2C and LDR pins. `board::Info` holds pins only; the image tag
  is `PXBT`. The WLED controller PCB (ESP32) as the variant `wled_controller`, flashed and tried.
* `px::output`: the RMT backend (one channel per lane, WS2812 timing, color order), and the ESP32
  parallel output (`i2s_output.cpp`: I2S0 in LCD mode, one DMA chain for all lanes), the default;
  setting `driver` (rmt, parallel). 3 x 256 LEDs at 120 fps on the test matrix.
* `px::layout`: canvas, panels from three answers (size, first corner, rows or columns with or
  without zigzag), lanes chained, the table canvas -> LED; host tests with the test matrix.
* Render task (`components/leds`): state, fade, gamma 2.2, power limit and model as settings,
  the supply with a settle time, layout and state stored in NVS and restored at boot; frame
  timings in the status.
* Color depth: gamma on the color as a 16-bit table, brightness on a perceptual curve from one
  output step up, temporal dithering (setting `dither`, on), default 100 fps.
* Per-lane settings: color order, level, color balance, dithering (Layout page card "Lanes",
  `/api/leds/lanes`, console `leds lane`, backup `leds.lanes`), stored apart from the layout.
* Animations (`components/effects`): solid, rainbow, plasma, breathe, test: edges.
* Web: the LEDs page (controls, status, power and timing) and the Layout page (panels and a
  preview of the data path), `/api/leds` (`components/leds_api`); LEDs is the first page.
* Home Assistant over MQTT (`components/hass`): MQTT page, discovery of a light (animations as
  effects), speed, power and fps sensors, motion, restart; state published on change; backup
  section `mqtt` without the password.
* Backup section `leds`: state, power and timing, canvas, panels and lanes. The backup has two
  parts, settings and presets (`config::backup::Part`), in place of the dashboard part.
* Console: `leds` with the state, the layout, the lane settings and the output tests
  (`setup|fill|px|bench`, which suspend the render task until `leds resume`).
* `relay` has an active-low mode; `board::Info::power_led_gpio` for the supply's indicator LED.
* Three crashes in a row (counted in RTC memory) start the LEDs off with RMT, the status says why.
* The stored LED blobs carry a version; a blob of another version is dropped.

Changed

* Recovery app without a screen: holds the LED power switch off and logs its state.
* The hotspot of the first setup is open: the device has no screen to show the password.
* The 4 MB partition table: factory 1.19 MB (the C6's recovery app is 1.07 MB), ota_0 2.25 MB.

Fixed

* RMT glitches while the LEDs page polled: the interrupt is cache safe
  (`CONFIG_RMT_TX_ISR_CACHE_SAFE`, encoder in IRAM), each lane gets the pulse memory of the unused
  channels, interrupt level 3.
* Dark colors: a linear brightness left 3 or 4 steps per color at brightness 10 and dithering
  nothing to do; a weak channel rounded away changed the hue (250,0,60 stayed red up to
  brightness 12). Both went with the brightness curve and dithering of every channel.

Removed

* From the base: LVGL and the screens, the Home Assistant WebSocket client, the dashboard,
  camera, audio output, vibration, orientation sensor, the ESP32-P4 and its C6 coprocessor.
