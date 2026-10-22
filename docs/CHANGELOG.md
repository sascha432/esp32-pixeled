# PixeLED - changelog

Every change to the firmware, the web UI, the tools and the docs, newest first, under the day it
was made; a release gets its version as the heading. Each entry says what a user or a developer
sees: a new feature under Added, a different behavior under Changed, a repaired one under Fixed,
and what is gone under Removed. Updated in the same commit as the change ([CLAUDE.md](../CLAUDE.md)).

## Unreleased (0.1.0)

### 2026-10-22

Added

* Parallel LED output on the ESP32-S3 (LCD_CAM): up to 16 lanes from one DMA stream, encoded
  into a small ring while sending, as on the ESP32; no extra pins. 4 x 1024 LEDs at 29 fps on
  the AI-S3. It is the default driver on the S3; RMT stays a choice.
* docs/boards.md: every board and variant with its GPIOs (LED lanes, supply, sensors, buttons,
  microphone, taken pins), with the clones AI-S3 (ESP32-S3 N16R8: photo, pinout image, RGB LED on
  GPIO 48, the solder jumpers) and AI-C3 (ESP32-C3-MINI-1: photo, pin headers). Their variants
  `esp32s3_devkit.ai_s3_test` (8 lanes on GPIO 4 to 7 and 15 to 18) and
  `esp32c3_devkit.ai_c3_test` (2 lanes on GPIO 4 and 5), without relay or sensors, are in git as
  examples.
* Time page: "Custom rule (POSIX TZ)" in the zone list, a text field for a rule of its own when the
  firmware's IANA table is out of date. `/api/time` takes a rule as `zone` and reports `rule`; the
  backup keeps either.
* `board::Info::hotspot_button`: which button, held while the device starts, opens the hotspot
  (button 1 unless the board names another). The Buttons card, `GET /api/input` (`hotspot`) and
  the console's `input` name it.
* Console `wifi pause <s>`: the Wi-Fi off for a while, as if the networks were gone, to try the
  hotspot of an offline device without the router.

Changed

* The hotspot of a device without Wi-Fi always takes its password: an open time left from the
  button or a setup ends when it opens.

* The test board is the WLED controller rev 1.1: its BTN0 is the hardware reset, so its variant
  has buttons 2 to 4 only (BTN1 darker, BTN2 brighter, BTN3 on/off on GPIO 35, 34, 19) and BTN3 is
  the hotspot button (hold it and press the reset). A button a board does not have is -1 and the
  others keep their number and actions.
* The WLED controller variants are in git, one per PCB revision: `wled_controller` is now
  `esp32_devkit.wled_controller_rev1_1` (the test board), and `esp32_devkit.wled_controller_rev1_3`
  is new: the same pins with all 4 buttons, button 1 on GPIO 13 (BTN0) and the hotspot button, and
  its signal LED on GPIO 16 (not verified on a rev 1.3 board). Other variants stay private.

Fixed

* The render task keeps the set frame rate: the frames follow a fixed cadence, so a wake-up a
  tick late no longer adds to the period (100 fps set gave 90 on the S3, 94 on the ESP32).
* `leds` on the console names the driver that sends (it said "parallel" on chips that fall back to
  RMT). On the C3 and C6 a changed setting no longer restarts the output.
* A smaller layout (fewer LEDs or lanes) leaves no LEDs lit at the end of a strip: the old
  lanes get one dark frame before the new layout starts.
* WLED controller rev 1.1: no power LED on GPIO 16, its indicator LED hangs on the MOSFET.
* Docs: the controller's buttons for rev 1.1 and 1.3 as a table in leds.md section 9, the hotspot
  button per board in architecture.md section 9.1, and a list of what to test on the board, step
  by step, in TODO.md.

### 2026-10-21

Added

* Headless recovery: a device with saved networks and no Wi-Fi for 5 minutes opens its hotspot
  (with its password) and keeps trying; button 1 held while the device starts (2 s) opens it
  without a password. Both stay until 15 minutes after the Wi-Fi is back. Console `hotspot open`
  does what the button does; the Hotspot page says when it opens by itself.
* Buttons and a rotary encoder (`components/input`): up to 4 buttons, each with an action for a
  click, a double click and a hold (on / off, next or previous animation, next preset, brighter,
  darker, restart after 3 s, open the hotspot, the encoder's knob on the animations). The
  encoder dims with acceleration (PCNT; GPIO interrupts on the C3). LEDs page card "Buttons",
  `GET/POST /api/input`, backup section `input`, console `input`; host tests of the buttons. The
  WLED controller's 4 buttons are in its variant (13 animation and restart, 35 darker, 34
  brighter, 19 on / off); an encoder is not wired yet.
* Palettes: rainbow, party, ocean, forest, lava, heat, cloud, sunset, pastel and shades of the
  color, for rainbow, plasma, fire, color fade, ripple, spectrum and noise; kept per animation
  (none: its own colors). LEDs page select, Home Assistant select "Palette", console
  `leds palette`, backup `palette`; host tests.
* Presets: up to 16 named lights (animation, color, speed, brightness, rotation, sound settings,
  palette) on LittleFS. LEDs page card "Presets" (show, delete, save the light),
  `GET /api/presets`, `POST /api/presets/save|apply|delete`, Home Assistant select "Preset" (the
  state is the preset the light matches), the button action "next preset", console
  `leds preset`, the backup's presets part (section `presets`).
* Home Assistant: the light takes `transition` (the brightness and on/off of that command take
  that long); numbers "Transition" (the fade time) and "Crossfade", switch "Dithering".
* Console `mqtt [set ...]`: the MQTT state and settings, without the password.

* Web UI page "Time": the clock (local time, time zone, last answer of a server), the time zone
  from the firmware's IANA list ("This browser's" picks the browser's) and two SNTP servers (empty:
  pool.ntp.org and time.cloudflare.com). `GET/POST /api/time`, `GET /api/time/zones`, backup
  section `time`. New servers take effect at once, without a restart.

* I2S microphone (INMP441) on the ESP32's I2S1, from the board's pins `mic_sck_gpio`,
  `mic_ws_gpio`, `mic_sd_gpio`: 32 log bands from 50 Hz to 10 kHz, loudness and beat, with an
  automatic gain and silence detection. It finds the slot the microphone sends in by itself.
* Animation "Spectrum": a rainbow bar per band from the bottom; a dim line without sound.
* Console `audio [status|watch [s]]`: the analysis, the bands as digits.
* Host tests of the analysis (FFT, bands, gain, fall, silence, beat).
* Animation "Fire": flames rising from the bottom, the speed sets how fast they move.
* Rotation per animation: 0, 90, 180 or 270 degrees. LEDs page, Home Assistant select
  "Rotation", console `leds rotate`, backup `leds.animations`.
* "Reacts to sound" per animation for rainbow, plasma, breathe and fire: faster and brighter with
  the music, beats move the colors or spark the fire. LEDs page (with an audio source), Home
  Assistant switch, console `leds reactive on|off`; the status shows whether it hears sound.
* Animations keep memory between frames (the render task gives it); host tests for the rotation,
  fire and the reactions.
* "Strength" per animation for the sound (0 to 255, 128 as before): a slider under "Reacts to
  sound", Home Assistant number "Sound strength", console `leds strength`, backup. Faster with
  the sound means at most 1.5 times the speed at strength 128 and twice at 255 (rainbow, plasma,
  gradient, noise, meteor; it was up to 5 times, so they raced with the music), from the smoothed
  loudness alone, so attack and release change it. Sound and silence glide into each other
  (100 ms in, 1 s out; noise at the silence level made the LEDs flicker between full and almost
  dark), a reactive animation is never darker than a quarter, a beat's new colors are reached in
  250 ms instead of a jump, and only breathe flashes on a beat.
* Microphone settings: gain, automatic gain on or off, silence level, range and beat threshold.
  LEDs page card "Microphone" with the 32 bands and the levels live, `GET/POST /api/audio`,
  console `audio set ...`, backup section `audio`.
* Attack and release per animation (how fast it follows the sound up and down, 20 and 250 ms as
  before): sliders next to the strength, Home Assistant numbers, console `leds attack|release`,
  backup `attack_ms`, `release_ms`. The bands of fire and spectrum follow them too.
* UDP sound inputs: WARLS spectrum packets (port 21324, a setting) and WLED audio sync (multicast
  239.0.0.1:11988); while packets come they replace the microphone. Card "Sound input" on the
  LEDs page, `POST /api/audio` `warls`, `warls_port`, `wled_sync`, console `audio set
  warls|port|wled`, backup `audio`.
* `tools/audio_send.py`: sends WARLS or WLED sync packets from the PC, a demo signal (120 beats a
  minute) or the PC's sound input.
* Animations gradient, color fade, twinkle, meteor, ripple, matrix rain and noise, each with or
  without sound; host tests.
* Crossfade between animations (1 s, `crossfade_ms` on the LEDs page, console `leds crossfade`,
  backup).
* Text and clock overlay over every animation: card "Overlay", `POST /api/leds/overlay`, Home
  Assistant select "Overlay" and text "Overlay text", console `leds overlay`, backup
  `leds.overlay`; host tests of the font.
* Live preview on the LEDs page over a WebSocket (`GET /api/leds/live`); the same socket brings
  the sound, so the 32 bars of the card "Sound input" move 20 times a second (the other values
  every 2 s). Under ESP-IDF 6.1 the socket is set up in the handshake callbacks
  (`CONFIG_HTTPD_WS_PRE/POST_HANDSHAKE_CB_SUPPORT`): IDF 6.1 no longer calls the route's handler
  for the handshake, and the first build of the preview stayed empty.
* At power on: as before (default), off or on (`power_on`; LEDs page, Home Assistant select,
  console `leds poweron`, backup). "As before" keeps "switched off by motion" over a restart.
* The LED supply goes off in the panic handler, before the crash is printed.
* Console `crash yes`: a crash on purpose, to try the panic handler and the crash count.
* Console `leds bench` also prints the I2S encoder's longest interrupt of the last frame;
  `leds bench encode` times the encoder alone on the slowest data (every bit set), nothing sent.

Changed

* `board::Info::button_gpio` is `button_gpios` (up to 4 buttons, `buttons({...})`), with
  `encoder_a_gpio`, `encoder_b_gpio` and `encoder_button_gpio`; a variant that set the old field
  sets the new one.
* The stored animation styles are version 4 (with the palette); version 3 is taken over with the
  palette at none.
* The animation "Test: edges" moved behind "Fire" in the list, and behind the new animations.
* ESP32 I2S output: the frame is encoded into a ring of 4 small DMA buffers while it is sent
  (15 KB for any count instead of 147 KB for 1024 LEDs per lane): 3 or 8 x 1024 LEDs at 32 fps,
  8 x 2048 at 16 fps. The planned 8-bit samples were dropped, they save nothing on this I2S.
* "Spectrum" is listed (LEDs page, Home Assistant effects) only with a sound source; its bars
  follow the strength, attack and release.
* The audio input starts on every board (the UDP inputs need no microphone); the Home Assistant
  sound entities come and go with a UDP input.
* The stored config and animation styles get new fields: the values of the version before are
  kept.
* The web server's routes are made with `route()`: the main firmware has WebSocket support.
* One NVS write policy (architecture.md section 9.3): live changes (on, brightness, animation,
  color, speed, rotation, sound strength, attack, release) are stored 10 s after the last change
  (was 5 s), at most 60 s after the first, and at a restart.

Fixed

* The strength, attack and release sliders and the Home Assistant style numbers wrote the flash
  several times a second while moving; they take the delayed write.
* Delayed settings: changes that kept coming put the write off without limit; a fifth waiting
  value was dropped (8 can wait now, a ninth writes the others); a value changed less than 5 s
  before a factory reset was written back during its restart.

* Console `leds bench` sent the frame without the power limit (full white went to the test
  matrix); it sends the limited frame.
* Console `leds setup` with 8 x 2048 LEDs crashed the board: the power limit copied the black
  frame (48 KB) a second time. A black frame is sent as it is, and a frame that does not fit is
  reported instead of aborting.
* A UDP input switched on crashed every boot (its socket opened before the network stack): the
  inputs start after the network.
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
