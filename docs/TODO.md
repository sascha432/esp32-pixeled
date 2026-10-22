# PixeLED - TODO

State 2026-10-21: phases 1 (skeleton), 3 (render task, layout, web pages) and 5 (Home
Assistant) built and running on the test matrix; phase 2 has RMT and the ESP32 I2S output with
its DMA ring; phase 6 has the microphone, WARLS and WLED audio sync (approved with music); phase
4 has 13 animations, the crossfade, palettes and presets; phase 7 the buttons and the encoder;
phase 8 the live preview and the overlay ([leds.md](leds.md) section 11).

**For the user's eyes (built 2026-10-21, not looked at on the LEDs):** the seven new animations
(gradient, color fade, twinkle, meteor, ripple, matrix rain, noise) with and without sound, the
crossfade, the overlay text and clock (3 x 5 digits on the 24 wide matrix), the live preview on
the LEDs page (fixed for ESP-IDF 6.1's handshake, with the sound bars at 20 fps; tried on the test
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
   held 0.6 s: the next preset (save one or two on the LEDs page first).
4. **Hotspot.** Hold BTN3, press and let go of BTN0 (reset), keep BTN3 down for 2 s more: the boot
   log says "button 4 held at boot: the hotspot opens", and a phone sees `pixeled-4271` without a
   password. Let go: no click comes from that press (the LEDs stay as they are). The Hotspot page
   shows "turns off in" about 15 minutes. Also: power the board up with BTN3 held.
5. **Web cards** (LEDs page, needs the login): the palette select (shown for rainbow, plasma, fire,
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
* The pins of the board are compile-time only (architecture.md section 8.1): a hardware page with
  runtime pins (NVS) for LED lanes, power switch, motion, I2C, LDR, buttons.
* Test board: the WLED controller PCB on COM4 (variant `esp32_devkit.wled_controller`, not in git),
  one strip of 32 LEDs on lane 1 (GPIO 27). Flashed 2026-10-19: boots, Wi-Fi, console; the RMT
  output lights the strip (`leds 32 0 0 40`, GRB). The supply MOSFET (GPIO 21) is on while high,
  not inverted as the old notes said; its indicator LED is lit by the supply. Netlist of the PCB
  (2026-10-21): GPIO 4 goes to the IR receiver footprint (not in `Info`), 2, 5, 12, 17, 18, 23 are
  free; only lanes 1 to 4 have a level shifter.
* Buttons of the test board (rev 1.1, phase 7, in the variant since 2026-10-21): BTN0 is the
  reset; BTN1 to BTN3 are buttons 2 to 4 on GPIO 35, 34, 19 (rev 1.3's pins, to check); the
  actions and the test steps are at the top. Microphone (INMP441: SD 32, WS 15, SCK 14) in the variant since
  2026-10-21. A rotary encoder: free pins 17, 18, 23 (not wired).
* Recovery app sizes (2026-10-19): S3 0.93 MB, C3 0.99 MB, C6 1.07 MB. The 4 MB table's factory
  grew to 1.19 MB for the C6 (ota_0 2.25 MB). Shrink the recovery app (no TLS client? log level)
  before the main firmware with the LED engine needs the room.

* Test matrix on COM7 (FTDI, resets on open; ESP32, variant `wled_controller`): lane 1 and 2
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

* Spike per chip: S3 LCD_CAM 8/16 lanes, ESP32 I2S1 LCD mode with a DMA ring, RMT, C6 PARLIO.
  Measure fps, CPU load and glitches under Wi-Fi load with a logic analyzer.
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
* The LED settings show up in the web UI only after a reload when changed from the console.

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
