# PixeLED - TODO

State 2026-10-21: phases 1 (skeleton), 3 (render task, layout, web pages) and 5 (Home
Assistant) built and running on the test matrix; phase 2 has RMT and the ESP32 I2S output;
phase 6 has the microphone and the spectrum ([leds.md](leds.md) section 11).

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
  not inverted as the old notes said; its indicator LED is lit by the supply.
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
* ESP32 I2S parallel driver built and the default (2026-10-19): 3 x 256 LEDs at 120 fps. Open:
  8-bit samples and a ring of small DMA buffers for 1024 LEDs per lane (8192 in all); a test
  with 8 lanes; CPU time of the encoder (measured with the frame, not alone).
* A crash at boot with a stored setting repeated forever (the first I2S driver did): built 2026-10-19,
  three crashes in a row start the LEDs off with RMT; not yet tried with a real crash.

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
* Crossfade between two animations (a second canvas).
* The supply off in the panic handler.
* A test pattern per lane ("light the first LED of each lane") on the layout page.
* The power model and the panel correction per lane (above).
* The LED settings show up in the web UI only after a reload when changed from the console.

## Phase 5: Home Assistant (built 2026-10-19, works with the user's HA 2026.10)

* Seen in Home Assistant 2026.10 (container, Mosquitto at 192.168.0.4, MQTT 5): all six entities,
  light commands (color, effect, on/off, brightness), motion on/off, sensors every 30 s.
* The `transition` of HA's light commands is ignored (the fade time is a setting).
* Motion auto off: built and tried 2026-10-20 (1 min: off, on with the next motion); one timer
  since the afternoon. Open: the mark "switched off by motion" is lost with a restart; a blind
  time after the supply switches, if the sensor turns out to see the switch (3 s after an off
  it reported motion once).
* More entities: presets, palette, transition time, dithering.

## Phase 6: audio (microphone built 2026-10-21)

* Microphone on the test matrix (2026-10-21): found in the left slot, a quiet room reads -65 to
  -75 dB (silence), typing shows short bars. Open: tried with music by the user; the silence
  level (-62 dB) and the beat with real music; the bars' fall speed.
* Reactive switch for rainbow, plasma and breathe ([leds.md](leds.md) section 6.2); audio
  settings in the web UI and Home Assistant; the LEDs page hides "Spectrum" without a source.
* The microphone on the S3 and the C chips (I2S0 there, the LEDs use LCD_CAM, RMT or PARLIO).
* UDP spectrum: WARLS and WLED audio sync.

## Phase 4 and later

See [leds.md](leds.md) section 11: animations and palettes, presets and playlists, MQTT with
Home Assistant discovery, audio (microphone, WARLS, WLED audio sync), buttons and encoder,
light and temperature, realtime input (DDP, E1.31, Art-Net), live preview, text and clock overlay,
custom shapes.
