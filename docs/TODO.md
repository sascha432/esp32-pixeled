# PixeLED - TODO

State 2026-10-21: phases 1 (skeleton), 3 (render task, layout, web pages) and 5 (Home
Assistant) built and running on the test matrix; phase 2 has RMT and the ESP32 I2S and S3 LCD_CAM
outputs with their DMA ring; phase 6 has the microphone, WARLS and WLED audio sync (approved with music); phase
4 has 13 animations, the crossfade, palettes and presets; phase 7 the buttons and the encoder;
phase 8 the live preview and the overlay ([leds.md](leds.md) section 11).

State 2026-11-03: the LEDs page split into Control, Sound, LED settings and Buttons, the menu as an
icon rail and a phone tab bar, `features` in `/api/status`, the sound-only preview socket; flashed on the test board.

State 2026-11-02: the web server, the web UI's live socket and the modem sleep rule ported from
hassctrl, flashed on the test board; `tools/test_web.py` 63 of 63 there, the browser check is
open (phase 1).

**For the user's eyes (built 2026-10-21, not looked at on the LEDs):** the seven new animations
(gradient, color fade, twinkle, meteor, ripple, matrix rain, noise) with and without sound, the
crossfade, the overlay text and clock (3 x 5 digits on the 24 wide matrix), the live preview on
Control (fixed for ESP-IDF 6.1's handshake, with the sound bars at 20 fps; tried on the test
matrix 2026-10-21), attack and release, the I2S ring's picture (its timing is measured), the supply
pin in the panic handler (`crash yes` with the LEDs on, below 10 W).

**Buttons, palettes, presets, Home Assistant (built 2026-10-21):** tried on the test board over
the console and MQTT (under a separate discovery prefix, removed afterwards; MQTT is off on the
board as before). Working by the user 2026-10-22: two presets saved and toggled, darker and
brighter, on/off, the open hotspot with BTN3 and the reset (gone after another reset). The
hotspot after 5 minutes without Wi-Fi (`wifi pause 400`): with its password, 2026-10-22. Still to
try: the rest of the list below (steps 1, 3, 5 to 7, 9).

### To test on the WLED controller (COM7, rev 1.1, firmware of 2026-10-22)

The test board is rev 1.1 of the PCB. Its BTN0 is the hardware reset (EN), not a GPIO; BTN1 to
BTN3 are buttons 2 to 4 (button 1 stays empty, so they keep the default actions). The GPIOs come
from the rev 1.3 schematic and are to be checked (step 1); all three read "up" when not pressed,
2026-10-22. `input` on the console lists them:

| Button | GPIO | PCB | Click | Hold |
| --- | --- | --- | --- | --- |
| - | (EN) | BTN0 | hardware reset | |
| 2 | 35 | BTN1 | darker | darker, every 0.1 s |
| 3 | 34 | BTN2 | brighter | brighter, every 0.1 s |
| 4 | 19 | BTN3 | on / off | next preset (needs a preset) |

Button 4 is the hotspot button (`board::Info::hotspot_button`): held while the device starts, it
opens the hotspot. No button has "next animation" now; give it one on the Buttons card (a double
click of button 4, for example) if it is wanted.

1. **Pins.** Hold one button and type `input` in the serial console (COM7, 115200): that button
   says "pressed", the others "up". Each of BTN1 to BTN3. A wrong one means rev 1.1 has other
   GPIOs than rev 1.3: tell me which.
2. **Clicks.** Each click logs `I (...) input: <action>` on the console. BTN1 and BTN2: one step
   darker or brighter (`leds` shows the brightness). BTN3: off, then on again (with the fade).
3. **Holds.** BTN1 and BTN2 held: the brightness moves every 0.1 s until let go; BTN2 while the
   LEDs are off switches them on. The power limit (10 W) stays in force at full brightness. BTN3
   held 0.6 s: the next preset (save one or two on Control first).
4. **Hotspot.** Hold BTN3, press and let go of BTN0 (reset), keep BTN3 down for 2 s more: the boot
   log says "button 4 held at boot: the hotspot opens", and a phone sees `pixeled-4271` without a
   password. Let go: no click comes from that press (the LEDs stay as they are). The Hotspot page
   shows "turns off in" about 15 minutes. Also: power the board up with BTN3 held.
5. **Web cards** (Control, needs the login): the palette select (shown for rainbow, plasma, fire,
   color fade, ripple, spectrum, noise; "Shades of the color" shows the color picker), the card
   Presets (save, show, delete; "shown" on the matching one), the card Buttons (buttons 2 to 4,
   its hint names button 4 for the hotspot; change an action, it holds after a restart; "restart"
   as a click is refused with a message). A double click action on a button makes its click come
   0.3 s later.
6. **Backup:** an export has the section `input` with buttons 2 to 4 and, under the presets part,
   `presets`.
7. **Home Assistant** (MQTT is off on the board: switch it on on the MQTT page): the entities
   Palette, Preset (only with a preset), Transition, Crossfade, Dithering; a light change with a
   transition (Developer tools, `light.turn_on` with `transition: 5`) takes 5 s.
8. **5 minutes without Wi-Fi:** on the console `wifi pause 400`: the device drops its Wi-Fi and
   tries no network for 400 s (nothing stored; a reset ends it). After 5 minutes the log says "no
   Wi-Fi for 5 minutes: the hotspot opens with its password", and a phone sees `pixeled-4271`
   asking for the password (the Hotspot page shows it; `wifi` on the console too). After the 400 s
   the device connects again; the hotspot stays 15 minutes, then goes off. Without the console:
   switch the router's 2.4 GHz off for more than 5 minutes.
9. **Encoder** (once wired, e.g. A 17, B 18, push 23): set `encoder_a_gpio`, `encoder_b_gpio`,
   `encoder_button_gpio` in the variant, build and flash; `input` shows the position. Turning
   changes the brightness (faster when turned fast; the wrong way round: swap A and B), the push
   switches on and off, held 0.6 s the knob changes the animation until it rests for 10 s.
10. **Modem sleep** (firmware of 2026-11-02, architecture.md section 9.2; keep the matrix below
    10 W: low brightness, no full white). With the serial log on (`log on`):
    * LEDs on (`leds bright 10`, `leds on`): the log says `modem sleep off` at once if it slept;
      `wifi status` shows `modem: awake, held by leds`, also after 5 minutes.
    * LEDs off (`leds off`): after the fade `wifi status` shows `modem: awake, sleeps in` about
      300 s, counting down; after 5 minutes the log says `modem sleep on`, `wifi status`
      `modem: sleeps`. Then `leds on`: `modem sleep off` within a second.
    * A web page (on the Wi-Fi, LEDs off): the Status page open shows `held by web`; the tab hidden
      or closed starts the 5 minutes again. While it sleeps, one request (`/api/status`) wakes it
      at once (`modem sleep off`) and the 5 minutes start again.
    * Ping the device with the LEDs off and the modem asleep (about 80 ms), then awake (about
      13 ms).

**2026-10-21, a mistake:** `leds fill 255 255 255` with `leds bench` sent full white to the test
matrix (bench sent the frame before the console's power limit), far above 10 W; the board browned
out and was power cycled. Bench now sends the limited frame. Check the matrix and its supply.

**Time page (2026-10-21, working on the test matrix):** the time zone and the SNTP servers,
`/api/time`, backup section `time`. Not tried: the backup section's import. A custom POSIX TZ
rule in place of a name (2026-10-22): the web UI works on the test matrix.

**NVS write policy (2026-10-21, [architecture.md](architecture.md) section 9.3):** the delayed
write ported from hassctrl (10 s, at most 60 s, 8 keys, discarded by a factory reset); the LED
styles take it. Not tried: the factory reset path (it erases the test board's settings).

## Decide

* The animation list ([leds.md](leds.md) section 6.3): what to add, what to drop, the palettes.
* The open hotspot of the first setup ([architecture.md](architecture.md) section 9.1): keep it
  open, or a fixed default password printed on the device.

## Phase 1: skeleton (done, to be tried on hardware)

* Flash recovery and main on the S3, the ESP32, the C3 and the C6; first setup over the open
  hotspot; `tools/test_web.py` against each.
* Headless recovery: built 2026-10-21 (architecture.md section 9.1): 5 minutes without Wi-Fi open
  the hotspot (password), the board's hotspot button held at boot opens it without one (button 4
  on the test board). Both tried 2026-10-22 (the button by the user, the 5 minutes with
  `wifi pause`). Open: the same with the router really off.
* Modem sleep while the LEDs are off: built 2026-10-23 (architecture.md section 9.2), switched
  with the supply; ping 13 ms on, 80 ms off on the test board; idle current at 5 V from 70 to 80 mA
  down to 15 to 25 mA (measured by the user). Changed 2026-11-02 to hassctrl's rule: 5 minutes
  after the last activity (a web request's bytes, the end of a hold), held awake by the LED supply
  and by a web page shown on the live socket, so a firmware upload or the web UI with the LEDs off
  wake it with their first request. Test board (COM7, no Wi-Fi saved, hotspot on): `modem sleep
  on` 301 s after the start, `wifi status` counts down. Open: the same on the Wi-Fi (a request
  wakes it at once, a shown page holds it, the LEDs hold it), and realtime input (when built)
  waking it before its first frame (its packets are no activity yet).
* Web server and web UI from hassctrl (2026-11-02, architecture.md section 9.4, ui/web/web-app.md
  section 4.1): the live socket with up to 3 paths per tab (no page polls any more), idle close
  after 30 s, TCP keep-alive, 24 lwIP sockets with the budget checked at build time, `files.zip`
  on a task of its own, the captive DNS socket only with the hotspot, console `sockets`, the build
  time, `platform::restart()` with the RTC watchdog, `build_fs.py --device`. Built for the ESP32,
  S3, C3 and C6 (main and recovery); flashed on the test board (COM7): boots, `sockets` 4 of 24
  with the hotspot on, console `restart` restarts. On the Wi-Fi (192.168.0.196): `tools/test_web.py`
  63 of 63 (idle sessions closed after 30 s, 20 idle connections do not lock out a new client, the
  live socket's pushes, two paths in one frame, heartbeat, logout closes it, two `files.zip` at
  once next to the API), internal RAM 138 KB free; the firmware update by `tools/update.py` through
  the recovery app. Found and fixed on the way: a second `files.zip` got 503 "no memory" (its
  44 KB entry list at once; it grows with the files now). Open: the pages in a browser (Status,
  Wi-Fi, Hotspot, MQTT, Time, LEDs: pushes, a hidden tab, the preview closing and opening),
  `build_fs.py --device` on a device with presets, `test_web.py` against the recovery app.
* **Task stacks** (2026-11-03, `mem tasks` on the test board, ESP32 without PSRAM, trace facility
  on for the measurement): `tools/test_web.py` (63 of 63) and every web API, the live socket with
  the LEDs page's three paths (now Control's two) and the preview, two `files.zip` at once, a Wi-Fi scan, a backup
  export, its check and an import that applies it, a preset saved, shown and deleted, the clock and
  text overlay, all 14 animations with the microphone on, MQTT connected with every entity set and
  put back (39 of 39), the web image uploaded with `build_fs.py --device`, a firmware upload into
  the recovery app, 40 DNS queries with the hotspot on, WARLS and WLED audio sync packets, the
  console's status commands and `audio watch`. Peak use before (bytes): httpd 3820 of 8192, wifi
  4336 of 6656, console 2136 of 6144, mqtt 2464 of 6144, network 2372 of 4096, leds 1964 of 4096,
  mdns 2000 of 4096, audio 1332 of 4096, esp_timer 2404 of 4096 (MQTT's state timer), tcpip 1736
  of 3584, captive DNS 1776 of 3072, audio_udp 1340 of 3072, sys_evt 1492 of 2816. Sized from it
  (the rule of architecture.md section 4; the web server, console and sound analysis above it for
  the paths not measured): httpd 6144 (both apps), console 4096, mDNS 3072
  (`CONFIG_MDNS_TASK_STACK_SIZE`), audio 3072, audio_udp 2560; 6.5 KB of internal RAM. The same
  load again with them: httpd 3816 of 6144, console 2140 of 4096, mdns 1560 of 3072, audio 1160
  of 3072, audio_udp 1384 of 2560 (the least left, 1176), the others as before; the recovery app
  took a firmware upload with its 6 KB. Kept: mqtt 6144 (an `mqtts://` broker's TLS handshake runs
  on it, not measured), esp_timer 4096 (a restart's shutdown handlers run on it), captive DNS 3072,
  network and leds 4096. The two tasks that end, with a log line of their unused stack for the
  measurement (removed again): `files.zip` 2.5 KB used of 5 KB (three at once) -> 3.5 KB; the URL
  download 1.9 KB over HTTP (the file system in the main firmware, the firmware in the recovery
  app), 3.7 KB over HTTPS (the handshake, the answer refused as no image) of 8 KB -> 6 KB. Not
  measured: TLS for MQTT, a firmware upload into the
  main firmware of a 16 MB board (the same handler as the file system upload, plus the image
  check), the S3, C3 and C6, the console's `leds bench` and `light calibrate`.
  Open, your decision: the stacks kept at their old size, by the margin rule of architecture.md
  section 4: network 4096 -> 3584 (2524 used; hassctrl took its network task to 3840 the same
  day), leds 4096 -> 3328 (2064), tcpip 3584 -> 2816 (1740, `CONFIG_LWIP_TCPIP_TASK_STACK_SIZE`),
  sys_evt 2816 -> 2560 (1496, `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE`); 2.5 KB together.
* **Static data in PSRAM on the S3** (2026-11-03, as hassctrl): the objects of architecture.md
  section 11 (21 KB) and mDNS; the linker map has them at 0x3c16xxxx, 37 KB of `.bss` in external
  RAM in all. Built for the S3 (main and recovery: the recovery app without the option), the
  ESP32, C3 and C6 (the attribute does nothing there). Not run on the AI-S3 yet (not connected):
  the boot, Wi-Fi with a saved network (the network task's defaults come from `start()` now), the
  web test and the backup import, `mem` before and after; hassctrl's check that the recovery app
  boots with a failing PSRAM applies here too (the option is not in its layers).
* The pins of the board are compile-time only (architecture.md section 8.1): a hardware page with
  runtime pins (NVS) for LED lanes, power switch, motion, I2C, LDR, buttons.
* Test board: the WLED controller PCB on COM4 (variant `esp32_devkit.wled_controller_rev1_1`),
  one strip of 32 LEDs on lane 1 (GPIO 27). Flashed 2026-10-19: boots, Wi-Fi, console; the RMT
  output lights the strip (`leds 32 0 0 40`, GRB). The supply MOSFET (GPIO 21) is on while high,
  not inverted as the old notes said; its indicator LED is lit by the supply (on the MOSFET, no GPIO: `power_led_gpio` -1). Netlist of the PCB
  (2026-10-21): GPIO 4 goes to the IR receiver footprint (not in `Info`), 2, 5, 12, 17, 18, 23 are
  free; only lanes 1 to 4 have a level shifter.
* Buttons of the test board (rev 1.1, phase 7, in the variant since 2026-10-21): BTN0 is the
  reset; BTN1 to BTN3 are buttons 2 to 4 on GPIO 35, 34, 19 (rev 1.3's pins, to check); the
  actions and the test steps are at the top. Microphone (INMP441: SD 32, WS 15, SCK 14) in the variant since
  2026-10-21. A rotary encoder: free pins 17, 18, 23 (not wired).
* Rev 1.3 of the controller (variant `esp32_devkit.wled_controller_rev1_3`, 2026-10-22): the same
  pins, plus button 1 on GPIO 13 (BTN0, the hotspot button) and a signal LED of its own on GPIO 16
  (rev 1.1's indicator LED hangs on the MOSFET, no GPIO). Built, nothing of it verified on a
  rev 1.3 board.
* S3 test board: the AI-S3 (ESP32-S3 N16R8) on COM8, variant `esp32s3_devkit.ai_s3_test` (in git,
  boards.md), 2026-10-22. Strips on lanes 1 to 4 (GPIO 4 to 7, 4 x 1024 in the
  layout) light with RMT and with the S3 parallel output (LCD_CAM, 2026-10-22: 29 fps); all 8 lanes with
  LCD_CAM (8 x 32 at 100 fps, smooth, checked by the user; RMT has 4 TX channels on the S3). Its 5Vin pin has no USB 5 V while the jumper IN-OUT is open.
* C3 test board: the AI-C3 (ESP32-C3-MINI-1, 4 MB) on COM14 (CH340K; its port USB: COM12),
  variant `esp32c3_devkit.ai_c3_test` (in git, boards.md), 2026-10-22. Strips on lanes 1 and 2
  (GPIO 10, 4) light with RMT: 2 x 512 at 60 fps, about 2 x 600 for 50 fps. Open: the GPIO of its
  RGB LED (8?), the 5V pins with power from USB.
* Recovery app sizes (2026-10-19): S3 0.93 MB, C3 0.99 MB, C6 1.07 MB. The 4 MB table's factory
  grew to 1.19 MB for the C6 (ota_0 2.25 MB). Shrink the recovery app (no TLS client? log level)
  before the main firmware with the LED engine needs the room.

* Test matrix on COM7 (FTDI, resets on open; ESP32, variant `esp32_devkit.wled_controller_rev1_1`): lane 1 and 2
  16x16, lane 3 8x32, 24 x 32 in all; INMP441 fitted. **Keep it below 10 W.** A camera watches
  it. Wiring found with the camera (2026-10-19), every panel first LED top right, zigzag rows:
  `leds canvas 24 32`, `leds panel 1 0 0 16 16 tr rows`, `leds panel 2 0 16 16 16 tr rows`,
  `leds panel 3 16 0 8 32 tr rows`, `leds layout`, `leds pattern edges` shows a closed frame.

* Power: the test matrix drew 7.54 W where the estimate said 10.0 W (2026-10-19). Calibrate per
  lane with the power meter the user provides, then per-lane brightness and color balance for the
  panel with other LEDs (leds.md section 3).

## Phase 2: outputs

* RMT on the ESP32 glitched while the web UI polled (refill interrupt late during flash access);
  fixed with a cache-safe interrupt and larger pulse memory (confirmed 2026-10-19).
* ESP32 I2S parallel driver built and the default (2026-10-19): 3 x 256 LEDs at 120 fps. The
  ring of small DMA buffers built 2026-10-21 (15 KB for any count): 3 x 1024 at 32 fps, 3 x 2048
  at 16 fps, measured. 8-bit samples dropped: the ESP32's I2S in LCD mode takes a 32-bit word per
  two samples at 8 bits too (leds.md section 2.2). 8 lanes tested 2026-10-21 (lanes 5 to 8 on
  the free pins 17, 18, 23, 5 of the test board, no LEDs): 8 x 1024 at 32 fps, 8 x 2048 at 16 fps;
  the encoder takes 248 us of a buffer's 720 us with 8 lanes (`leds bench encode`). Open: the
  picture with 1024 per lane; the render task with 8192 LEDs on an ESP32 without PSRAM (its
  buffers come from `allocate_large`, which aborts when the heap is full; the console's test
  frame of 8 x 2048 fits only alone).
* A crash at boot with a stored setting repeated forever (the first I2S driver did): built 2026-10-19,
  three crashes in a row start the LEDs off with RMT. A real crash (console `crash yes`) restarts
  with reason panic (2026-10-21); the count of three not tried yet.

* S3 LCD_CAM parallel driver built (2026-10-22, the ESP32's ring on GDMA): 4 x 1024 LEDs at
  29 fps on the AI-S3, the encoder 116 us per buffer for 4 lanes; 8 lanes with LEDs smooth by
  eye (2026-10-22). Open: lanes 9 to 16 with LEDs, glitches under Wi-Fi load and during flash
  writes (logic analyzer).
* Spike for the C6 (PARLIO). Measure fps, CPU load and glitches under Wi-Fi load with a logic
  analyzer.
* `px::output` interface, lanes with their own pixel counts and color order.

## Phase 3: render task and layout (done 2026-10-19, open points)

* Dithering on the matrix (2026-10-19): brightness on a perceptual curve, weak channels
  dithered too. The user judges it with the final LEDs (probably another kind again): the low
  end (brightness 1 to 20) with solid orange, yellow and 250,0,60; per lane dithering, level and
  balance; the power calibration with the meter. Two replacement LEDs on one panel (other chips) still flicker in
  changing colors at low brightness, the 254 original ones of that panel do not; one whole
  panel flickers in dark yellow and orange (red and green step together). Per-lane dithering
  built 2026-10-19: that lane gets it off. 2026-10-20, solid white at low brightness: two of the
  three panels sparkle with dithering, one is clean; without dithering all are clean, so the
  timing suits every chip and the sparkle is the chips' slow PWM showing each 1-step change.
  Rule for new LEDs: this test per lane, dithering off where it sparkles. Maybe later: a
  minimum number of steps per lane before it dithers.
* Crossfade between two animations: built 2026-10-21 (1 s, a setting).
* The supply off in the panic handler: built 2026-10-21; the pin not measured yet.
* A test pattern per lane ("light the first LED of each lane") on the layout page.
* The power model and the panel correction per lane (above).
* The LED settings show up in the web UI only after a reload when changed from the console or
  Home Assistant: since 2026-11-02 the LED pages' status, the sound input and the presets follow
  on the live socket, the controls (switch, sliders, animation) still show what the page sent.

## Phase 5: Home Assistant (built 2026-10-19, works with the user's HA 2026.10)

* Seen in Home Assistant 2026.10 (container, Mosquitto at 192.168.0.4, MQTT 5): all six entities,
  light commands (color, effect, on/off, brightness), motion on/off, sensors every 30 s.
* The `transition` of HA's light commands: built 2026-10-21 (tried over MQTT: a 4 s fade).
* Motion auto off: built and tried 2026-10-20 (1 min: off, on with the next motion); one timer
  since the afternoon. The mark "switched off by motion" is kept over a restart with "at power on:
  as before" (2026-10-21, not tried). Open: a blind time after the supply switches, if the sensor
  turns out to see the switch (3 s after an off it reported motion once).
* More entities: built 2026-10-21: attack, release, overlay, overlay text, power on; palette,
  preset, transition, crossfade, dithering (announced and tried over MQTT); not yet seen in Home
  Assistant. Open: playlist, intensity.
* MQTT test 2026-11-03 (test board, Mosquitto at 192.168.0.4, anonymous): 20 entities announced;
  speed, rotation, reactive, strength, attack, release, palette, transition, crossfade, dithering,
  overlay, overlay text, power on, auto off and its minutes, the light (effect, color, brightness,
  off and on with a transition) and a preset each set over MQTT, the state followed, every value
  put back (39 of 39). The preset select is announced with the first preset. Not tried: the
  restart button, `mqtts://`.

## Phase 6: audio (microphone built 2026-10-21)

* Microphone on the test matrix (2026-10-21): found in the left slot; with music the silence
  level, the beat and the bars' fall speed work (approved by the user 2026-10-21).
* Reactive switch, rotation per animation and fire built 2026-10-21 ([leds.md](leds.md) section
  6.2): fire, the rotations and the reactions with music approved by the user 2026-10-21.
* Microphone settings (gain, automatic gain, silence level, range, beat threshold) and the
  strength per animation built 2026-10-21: the Microphone card and the strength slider work in
  the browser (approved 2026-10-21). Attack and release per animation built 2026-10-21; the LEDs
  page and Home Assistant list "Spectrum" only with a source.
* The microphone on the S3 and the C chips (I2S0 there, the LEDs use LCD_CAM, RMT or PARLIO).
* UDP spectrum built 2026-10-21: WARLS and WLED audio sync, tried with `tools/audio_send.py`
  (the bands arrive). Open: LedFx or a WLED device as the sender; the reactive animations on the
  LEDs fed by UDP.

## Phase 4 and later

See [leds.md](leds.md) section 11: animations and palettes, presets and playlists, MQTT with
Home Assistant discovery, audio (microphone, WARLS, WLED audio sync), buttons and encoder,
light and temperature, realtime input (DDP, E1.31, Art-Net), live preview, text and clock overlay,
custom shapes.
