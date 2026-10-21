# PixeLED - changelog

## 2026-10-19

* Project started from the infrastructure of `esp32-hassctrl`: build (ESP-IDF 6.1, layered
  sdkconfig, `tools/build.py`), network with hotspot and captive portal, web server with login,
  update and rollback, recovery app, settings and LittleFS, log buffer, backup, serial console,
  host tests. Renamed `hc` to `px`, `hassctrl` to `pixeled`.
* Left out: LVGL and the screens, the Home Assistant WebSocket client, the dashboard, camera,
  audio output, vibration, orientation sensor, the ESP32-P4 and its C6 coprocessor.
* Boards: ESP32-S3-DevKitC-1 N16R8, ESP32-DevKitC, ESP32-C3-DevKitM-1, ESP32-C6-DevKitC-1, with
  the default LED, relay, motion, I2C and LDR pins. `board::Info` holds pins only; the image tag
  is `PXBT`.
* The backup has two parts, settings and presets (`config::backup::Part`), in place of the
  dashboard part.
* Recovery app without a screen: holds the LED power switch off and logs its state.
* The hotspot of the first setup is open: the device has no screen to show the password.
* Design of the LED engine: [leds.md](leds.md).
* `relay` has an active-low mode; `board::Info::power_led_gpio` for the supply's indicator LED.
* The 4 MB partition table: factory 1.19 MB (the C6's recovery app is 1.07 MB), ota_0 2.25 MB.
* First flash on the WLED controller PCB (ESP32, variant `wled_controller`).
* `px::output` with the RMT backend (one channel per lane, WS2812 timing, color order) and the
  console command `leds` to test a lane: 32 LEDs on the controller PCB light.
* `px::layout`: canvas, panels from three answers (size, first corner, rows or columns with or
  without zigzag), lanes chained, the table canvas -> LED; host tests with the test matrix.
* Console: `leds setup|fill|px|bench` for single LEDs and speed, `leds canvas|panel|layout` for
  the layout; frames are scaled to a 10 W estimate.
* Render task (`components/leds`): state, fade, gamma 2.2, power limit and model as settings,
  the supply with a settle time, layout and state stored in NVS and restored at boot; console
  `leds on|off|bright|effect|color|speed|limit|resume`. Rainbow at 55 fps on the test matrix.
* Animations (`components/effects`): solid, rainbow, plasma, breathe, test: edges.
* Web: the LEDs page (controls, status, power and timing) and the Layout page (panels and a
  preview of the data path), `/api/leds` (`components/leds_api`), LEDs is the first page.
* Backup section `leds`: state, power and timing, canvas and panels.
* ESP32 parallel output (`i2s_output.cpp`): I2S0 in LCD mode, one DMA chain for all lanes, the
  default driver; setting `driver` (rmt, parallel) in the web UI, the API, the backup and the
  console (`leds driver`). 3 x 256 LEDs at 120 fps on the test matrix.
* Color depth: gamma on the color only, brightness linear, 16-bit compose and temporal dithering
  (setting `dither`, on); default 100 fps. Brightness 10 kept 3 or 4 steps per color before.
* Motion auto off (2026-10-20): the LEDs fade off N minutes after the sensor cleared and come
  back with the next motion, only after an auto off; LEDs page card "Motion", Home Assistant switch
  and number, console `leds autooff`, backup `leds.auto_off`, `leds.auto_off_minutes`.
* Motion auto off is one timer: the minutes are the sensor's hold, every motion restarts it, the
  LEDs go off when it runs out (the first build waited the sensor's fixed 5 min and then the
  minutes). The hold can be 10 s to 240 min; the status shows the seconds left.
* Console: an unknown `leds` word no longer suspends the LEDs (`leds autooff` without its value
  did); the auto off counts down also while the render task sleeps or is suspended.
* Home Assistant: the speed is sent only when it changes; no `object_id` in the discovery (HA
  ignores it).
* Home Assistant over MQTT (`components/hass`): MQTT page, discovery of a light (animations as
  effects), speed, power and fps sensors, motion, restart; state published on change, the
  motion sensor's state too; backup section `mqtt` without the password.
* Per-lane settings: color order, level, color balance, dithering (Layout page card "Lanes",
  `/api/leds/lanes`, console `leds lane`, backup `leds.lanes`), stored apart from the layout.
* Brightness on a perceptual curve from one output step up (2 to about 16 between one and two
  steps, shown by dithering); every channel of a dithering lane is dithered, weak ones too, so a
  dark color keeps its hue on average (250,0,60 stayed red up to brightness 12).
* Frame timings in the status (animation, compose, output); console `leds fps`, `leds dither`.
* The stored LED blobs carry a version; the first start with it drops the old ones (the layout
  has to be set again once).
* Three crashes in a row (counted in RTC memory) start the LEDs off with RMT, the status says why.
* RMT glitches while the LEDs page polled: the interrupt is now cache safe
  (`CONFIG_RMT_TX_ISR_CACHE_SAFE`, encoder in IRAM), each lane gets the pulse memory of the unused
  channels, interrupt level 3.
