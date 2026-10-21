# PixeLED - TODO

State 2026-10-21: phases 1 (skeleton), 3 (render task, layout, web pages) and 5 (Home
Assistant) built and running on the test matrix; phase 2 has RMT and the ESP32 I2S output with
its DMA ring; phase 6 has the microphone, WARLS and WLED audio sync; phase 4 has 13 animations
and the crossfade; phase 8 the live preview and the overlay ([leds.md](leds.md) section 11).

**For the user's eyes (built 2026-10-21, not looked at on the LEDs):** the seven new animations
(gradient, color fade, twinkle, meteor, ripple, matrix rain, noise) with and without sound, the
crossfade, the overlay text and clock (3 x 5 digits on the 24 wide matrix), the live preview on
the LEDs page, attack and release, the I2S ring's picture (its timing is measured), the supply
pin in the panic handler (`crash yes` with the LEDs on, below 10 W).

**2026-10-21, a mistake:** `leds fill 255 255 255` with `leds bench` sent full white to the test
matrix (bench sent the frame before the console's power limit), far above 10 W; the board browned
out and was power cycled. Bench now sends the limited frame. Check the matrix and its supply.

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
* Headless recovery: the hotspot never opens by itself when the saved networks fail. Planned: the
  board's button held at boot opens it, and after N minutes without a connection it opens too.
* The pins of the board are compile-time only (architecture.md section 8.1): a hardware page with
  runtime pins (NVS) for LED lanes, power switch, motion, I2C, LDR, buttons.
* Test board: the WLED controller PCB on COM4 (variant `esp32_devkit.wled_controller`, not in git),
  one strip of 32 LEDs on lane 1 (GPIO 27). Flashed 2026-10-19: boots, Wi-Fi, console; the RMT
  output lights the strip (`leds 32 0 0 40`, GRB). The supply MOSFET (GPIO 21) is on while high,
  not inverted as the old notes said; its indicator LED is lit by the supply. Netlist of the PCB
  (2026-10-21): GPIO 4 goes to the IR receiver footprint (not in `Info`), 2, 5, 12, 17, 18, 23 are
  free; only lanes 1 to 4 have a level shifter.
* Buttons of the test board (phase 7): 13 animation, held 3 s restart (software: the PCB has no
  reset button), 35 brightness down, 34 brightness up, 19 on/off. Microphone (INMP441: SD 32,
  WS 15, SCK 14) in the variant since 2026-10-21.
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
* The `transition` of HA's light commands is ignored (the fade time is a setting).
* Motion auto off: built and tried 2026-10-20 (1 min: off, on with the next motion); one timer
  since the afternoon. The mark "switched off by motion" is kept over a restart with "at power on:
  as before" (2026-10-21, not tried). Open: a blind time after the supply switches, if the sensor
  turns out to see the switch (3 s after an off it reported motion once).
* More entities: presets, palette, transition time, dithering. Built 2026-10-21: attack,
  release, overlay, overlay text, power on; not yet seen in Home Assistant.

## Phase 6: audio (microphone built 2026-10-21)

* Microphone on the test matrix (2026-10-21): found in the left slot, a quiet room reads -65 to
  -75 dB (silence), typing shows short bars. Open: tried with music by the user; the silence
  level (-62 dB) and the beat with real music; the bars' fall speed.
* Reactive switch, rotation per animation and fire built 2026-10-21 ([leds.md](leds.md) section
  6.2); tried on the matrix over the console (fire about 0.15 ms per frame for 768 LEDs). Open: the
  user's look at fire, the rotations and the reactions with music; the web page and Home
  Assistant on the device; the camera (192.168.0.185) did not answer.
* Microphone settings (gain, automatic gain, silence level, range, beat threshold) and the
  strength per animation built 2026-10-21; tried over the console. Open: the Microphone card
  and the strength slider in the browser, the Home Assistant number, defaults after tries with
  music (the room read -56 to -60 dB at times, above the -62 dB silence level). Attack and release
  per animation built 2026-10-21; the LEDs page and Home Assistant list "Spectrum" only with a
  source.
* The microphone on the S3 and the C chips (I2S0 there, the LEDs use LCD_CAM, RMT or PARLIO).
* UDP spectrum built 2026-10-21: WARLS and WLED audio sync, tried with `tools/audio_send.py`
  (the bands arrive). Open: LedFx or a WLED device as the sender; the reactive animations on the
  LEDs fed by UDP.

## Phase 4 and later

See [leds.md](leds.md) section 11: animations and palettes, presets and playlists, MQTT with
Home Assistant discovery, audio (microphone, WARLS, WLED audio sync), buttons and encoder,
light and temperature, realtime input (DDP, E1.31, Art-Net), live preview, text and clock overlay,
custom shapes.
