# PixeLED - LED engine

The design of the LED part: outputs, render pipeline, layout, animations, reactivity, realtime
input, MQTT. Built so far: the RMT output, the layout, the render task with the first animations,
the web pages and the backup section; each section says what is built. [TODO.md](TODO.md) has the
phases and their state. The infrastructure is in [architecture.md](architecture.md).

The features follow the clock and LED matrix plugin of `kfc_fw`; its code is not reused.

---

## 1. Scope

| In | Later, maybe | Not planned |
| --- | --- | --- |
| WS2812 / WS2815 RGB, 800 kHz, any color order | SK6812 RGBW | APA102 / SK9822 (clocked) |
| ESP32-S3 (main target), ESP32, ESP32-C3, ESP32-C6 | WS2811 at 400 kHz | ESP32-P4 |
| strips and matrices set up at runtime | zones (one animation per part of the grid) | a runtime shape editor |
| custom shapes compiled in (7-segment clock, hexagons, rings) | schedules, night mode | IR remotes |
| MQTT with Home Assistant discovery | Ethernet | sync between controllers |
| web UI: control, layout, settings, live preview | | |
| presets and playlists | | |
| realtime input: DDP, E1.31 (sACN), Art-Net | | |
| text and clock overlay | | |
| audio: I2S microphone, UDP spectrum (WARLS, WLED audio sync) | | |
| optional hardware: power switch, motion, buttons, rotary encoder, light and temperature sensor | | |
| power limit, gamma, temperature protection | | |

---

## 2. Outputs

### 2.1 Throughput

A WS2812 bit takes 1.25 µs, a pixel 24 bits = 30 µs, and a frame ends with a latch of at least
280 µs (300 µs used). One data pin (a **lane**) therefore carries about 1100 pixels at 30 fps.

| Lanes | Pixels per lane for 8192 | Frame time | Max fps |
| --- | --- | --- | --- |
| 1 | 8192 | 246 ms | 4 |
| 4 | 2048 | 61.7 ms | 16 |
| 8 | 1024 | 31.0 ms | 32 |
| 16 | 512 | 15.7 ms | 63 |

**8192 LEDs at 30 fps need 8 lanes sent in parallel.** The ESP32-S3 can send 16 for headroom;
the C3 and C6 are for small installations (1000 to 2000 LEDs).

### 2.2 Drivers per chip

One interface (`px::output`; the RMT backend is built, the parallel ones not): configure the lanes (pin, pixel count, color order),
hand it a frame, get a callback when it is out. Behind it one backend per chip:

| Chip | Parallel backend | Lanes | Fallback |
| --- | --- | --- | --- |
| ESP32-S3 | LCD_CAM in i80 mode: 8 or 16 data lines, GDMA, frame buffers in PSRAM | 8 / 16 | RMT, 4 channels |
| ESP32 | I2S1 in LCD mode: 8 data lines, DMA from a ring of small buffers in internal RAM, encoded while sending | 8 | RMT, 8 channels |
| ESP32-C6 | PARLIO TX (lane count to verify) | up to 8 | RMT, 2 channels |
| ESP32-C3 | none | | RMT, 2 channels |

* **Parallel encoding**: each WS2812 bit becomes 3 slots of the bus clock (2.4 MHz: high, data,
  low). For 8 lanes one slot is a byte with one bit per lane, so a pixel row of all lanes is
  24 x 3 = 72 bytes; 1024 pixels per lane are 74 KB per frame (148 KB for 16 lanes). The bits are
  transposed with a lookup table.
* **S3**: the frame is encoded into one of two DMA buffers in PSRAM while the other is sent.
* **ESP32** (built, `components/output/src/i2s_output.cpp`): I2S0 in LCD mode, 16-bit samples,
  bit n on lane n (data lines 8 to 23 of the GPIO matrix), 2.4 MHz from the 160 MHz PLL. The frame
  is encoded into one buffer in internal DMA memory (144 bytes per LED position, the always-high
  and always-low slots written once at the start), a chain of descriptors, 800 zero slots as the
  latch; one interrupt at the end of the frame. The I2S hardware sends the two 16-bit samples of
  a 32-bit word in reverse order; the encoder swaps them. Measured on the test matrix: 3 lanes of
  256 LEDs at 120 fps (8.33 ms a frame), no glitches while the web UI is busy. The default driver.
  Open: 1024 LEDs per lane need 147 KB in one buffer, too much next to Wi-Fi: 8-bit samples for up
  to 8 lanes (half) and a ring of small buffers encoded from the interrupt of the finished one
  (TODO). I2S1 stays for the microphone.
* **RMT**: one channel per lane, the IDF's encoder; the C3 and C6 have 2 TX channels. Without
  DMA (ESP32, C3) an interrupt refills the pulse memory every few pixels; a late refill ends the
  frame early and the LEDs show garbage. Seen on the ESP32 while the web UI polled: the interrupt
  waited for the flash. Built: the interrupt runs while the flash is busy
  (`CONFIG_RMT_TX_ISR_CACHE_SAFE`), each lane gets the memory of the unused channels, level 3.
* Lanes may have different pixel counts; the shorter ones send zeros after their last pixel.
* **First step is a spike per chip**: fps, CPU load and glitches with Wi-Fi, MQTT and the web UI
  busy, checked with a logic analyzer.

---

## 3. Render pipeline

```
 commands (web, MQTT, buttons, encoder, motion, playlist, realtime)
        |
        v
 [render task]  animation -> canvas (RGB888, W x H) -> overlay (text, clock)
                -> crossfade with the previous animation
                -> brightness, gamma, power limit
                -> map (lookup table: canvas -> lane, index)
                -> encode -> DMA
```

* **One render task** owns the LED state, pinned to core 1 on the dual-core chips, at a high
  priority. Every change arrives as a command in its queue and is applied between two frames
  (built: `components/leds`). It sleeps while the LEDs are off and dark.
* **The supply**: switched on when the LEDs turn on, the first frame after the settle time
  (50 ms); off after the fade-out and one dark frame (built).
* **Stored** (NVS namespace `leds`): the layout and the config at once, the state (on,
  brightness, animation, color, speed) 5 s after its last change; restored at boot (built). Each
  blob starts with a version (`kStoredVersion`); a blob of another version is not read and the
  defaults apply. A field added into a struct's padding does not change its size, so the size
  alone could not tell (found 2026-10-19: the new dither field read the old padding).
* **Memory**: 8192 pixels x 3 bytes = 24 KB per canvas; a second canvas for the crossfade
  between animations. On the ESP32 without PSRAM: the canvas sizes and the DMA ring decide the
  maximum LED count (to measure).
* **Frame pacing**: a frame is rendered while the previous one is sent; the target fps is a
  setting, the reached fps a status value.
* **Color depth** (built): gamma 2.2 on the color as a 16-bit table, times the brightness on a
  perceptual curve, in 16 bits. The curve: brightness 1 is one output step of a full channel (the
  lowest the LEDs show), 255 all 255, gamma 2.2 between them with 2 % linear (so the first slider
  steps differ); brightness 2 to about 16 lie between one and two output steps and only
  dithering shows them. Tried before on the test matrix: gamma on the brightness too (brightness
  10 was 0.08 % and left 3 or 4 steps), then a linear brightness (every slider step one output
  step, the dark end jumped by doubling and dithering had nothing to do). **Temporal dithering** spreads
  the 8 bits below the output's 8 over the frames: a value of 2.4 is 2 in three frames out of five
  and 3 in two. The threshold of a frame is its number with the bits reversed, so any run of
  frames meets them evenly; every LED has its own offset, so they do not step together. It needs
  frames: the default target is 100 fps (90 reached on the test matrix: 0.3 ms animation, 0.25 ms
  compose, 8.9 ms output). A setting (on by default). Every channel of a dithering lane is
  dithered, also below one output step: rounding a weak channel away changed the hue (250,0,60
  stayed red up to brightness 12, then turned pink at once). LEDs that show dithering as flicker
  get it off for their lane (per-lane settings).
* **Brightness** 0 to 255 with a fade time; **power limit**: the frame's
  current estimated from the sum of its channels (mA per channel at full, plus idle), the
  brightness scaled down to the limit. 8192 LEDs at full white draw about 490 A at 5 V, so the
  limit is on by default. The model (the `kfc_fw` defaults): 80 mW per channel at full (16 mA at
  5 V), 5 mW per dark LED. The idle power alone is not small: the 768 LEDs of the test matrix
  draw about 3.8 W while dark, so a 10 W limit leaves about 6 W for the colors (about 3% of full
  white). Built so far in the console's `leds` command (10 W fixed); the render task takes it over
  as a setting.
* **Calibration** (not built): the model is per lane, because the LEDs of one installation differ
  (the test matrix has a panel with other LEDs). Measured on the test matrix 2026-10-19: 7.54 W at
  the 5 V supply where the model said 10.0 W (the gradient pattern, capped), so the defaults are
  about a third too high: safe, but they cost brightness. A calibration with a power meter: all
  LEDs dark, then each lane in full red, green and blue at a low level, the readings typed into
  the web UI; the firmware fits idle mW per LED and mW per channel for each lane.
* **Per-lane settings** (built, NVS key `lanes`, apart from the layout): color order, a level
  against the other lanes, a color balance (R, G, B) and dithering on or off, so a lane with LEDs
  of another kind looks like the others. Applied in the compose pass: the layout table knows the
  lane of every canvas pixel. Found on the test matrix: one panel's LEDs (and two replacement
  LEDs) flicker with dithering in dark colors, yellow most (red and green step together), the
  others not; that lane gets dithering off. Not built: gamma per lane, the power model per lane.
* **Realtime input** (section 7) bypasses the animation while packets arrive and falls back after
  a timeout.

---

## 4. Layout

### 4.1 Strip and matrix (runtime)

The canvas is W x H; a strip is W x 1. A matrix is described per panel with three answers:

1. Size: width x height in LEDs.
2. Where is the first LED: top left, top right, bottom left, bottom right.
3. How do the rows run: horizontal or vertical, zigzag (serpentine) or all the same way.

A larger matrix is a grid of identical panels (for example 4 x 2 panels of 32 x 32), chained in
order, a number of panels per lane. A panel may be rotated when it is mounted differently. From
these answers the firmware builds the lookup table once, at every change (`px::layout`, built
and host tested; the console's `leds canvas`, `leds panel`, `leds layout` and `leds pattern`
use it until the layout page exists).

The web UI draws the panels with the path of the data line and numbers, lights the first LED of
each lane, and shows a test pattern (each panel's edges in their own colors). The `kfc_fw`
plugin needed about 37 fields with reverse, rotate and interleave flags whose meaning depended on
each other; these three answers replace them.

### 4.2 Custom shapes (compiled in)

A shape that is not a grid (a 7-segment clock, hexagon panels, rings) is a file
`shapes/<name>.yaml`: the position of every LED on a virtual canvas, and named groups (the
segments of a digit). `tools/gen_shape.py` turns it into a `constexpr` table; a board variant
selects it. Animations render to the virtual canvas, the shape samples it. No runtime editor:
the table is fixed and the mapping as fast as a matrix.

A 7-segment clock is a shape with segment groups: the digits are a mask over any animation.

---

## 5. Power switch and safety

* The supply relay or MOSFET (`board::Info::power_gpio`) is off while the board starts, switched
  on before the first frame with a settle time (a setting), and off after the LEDs faded out.
* Forced off in the panic handler, and by the recovery app.
* Three crashes in a row (panic, watchdog; counted in RTC memory, cleared after 30 s of running)
  start the LEDs off with RMT, so a setting that crashes the device cannot restart it forever
  (built).
* Temperature protection with a sensor (optional): the brightness scaled down between two
  temperatures, off above a third, until the user turns the LEDs on again.

---

## 6. Animations and reactivity

### 6.1 Audio sources

All sources fill the same **audio frame**: loudness left and right, 32 bands, beat, peak.

* **I2S microphone** (INMP441 or similar), built 2026-10-21 (component `audio`). On the ESP32 it
  takes I2S1; I2S0 drives the LEDs. 22050 Hz, 32-bit samples, both slots read: the one with the
  signal is used (the L/R pin of the microphone decides it). A task on core 0 analyzes blocks of
  512 samples (23 ms, 43 a second):
  * a Hann window and a 512-point FFT (own radix-2 code, single precision);
  * 32 bands log-spaced from 50 Hz to 10 kHz, each the loudest bin of its range in dB;
  * an automatic gain: the reference follows the loudest band up at once and falls 3 dB a
    second; the 40 dB below it are 0..255, so quiet and loud music both fill the bars;
  * silence below -62 dB of full scale (a quiet room reads -65 to -75 dB): the bars fall to 0;
  * the bars rise at once and fall by 12 % of full height per block;
  * a beat when the bass (bands 0 to 3) is 1.6 times its average, at most one in 140 ms;
  * the loudness from the block's RMS on the same scale.

  The render task takes the latest result with each frame; a beat stays until a frame took it.
  The analysis is host tested (`host/tests/audio_test.cpp`). Console `audio [watch [s]]` prints
  the bands as digits. Not built yet: loudness left and right, the peak, settings (gain range,
  silence level), the input on the S3 and the C chips.
* **UDP "WARLS"** spectrum packets, port 21324, the `kfc_fw` format (its `scripts/audio_analyzer`
  sends them from a PC).
* **WLED audio sync v2**, multicast port 11988, so a WLED sound-reactive device or LedFx can be
  the source.

### 6.2 Reactive is a switch, not an animation

There are no separate reactive animations. Each animation declares whether it reacts: never,
optional, or only with audio. An optional one has a "reactive" switch, shown only while an audio
source is active. Shared settings for all of them: sensitivity, attack, release.

### 6.3 The list (a proposal, to be decided)

| Animation | Reactive | Notes |
| --- | --- | --- |
| Solid | no | |
| Flash / strobe | no | also the alarm and the reboot warning |
| Game of life | no | 2D |
| Gradient | optional: pulse | linear or radial, angle, palette |
| Rainbow | optional: speed, brightness | along the strip or at an angle |
| Color fade | optional: next color on the beat | |
| Fire | optional: heat, sparks, a band per column | |
| Plasma | optional: speed, hue, zoom, bands | |
| Noise / lava | optional | Perlin noise with a palette |
| Twinkle / stars | optional: sparks on the beat | |
| Fairy lights | optional: sparkle | the `kfc_fw` Xmas animation |
| Meteor / scanner | optional | 1D |
| Ripple | optional: a ring on the beat | 2D |
| Matrix rain | optional | 2D |
| Spectrum bars | only with audio | color: rainbow, gradient or one color; peaks; VU rows (the three bar animations of `kfc_fw` in one). Built 2026-10-21 as "Spectrum": a rainbow bar per band from the bottom, a narrower canvas shows the loudest band of each group; a dim line while there are no bars |

A shared palette system serves every animation that takes colors.

---

## 7. Realtime input

DDP, E1.31 (sACN) and Art-Net over UDP, so LedFx, xLights or Hyperion drive the LEDs. The data is
mapped through the same layout; while packets arrive the animation pauses, after a timeout it
continues.

---

## 8. MQTT and Home Assistant

Built (`components/hass`; the component is not called `mqtt`, that is the name of the client,
`espressif/mqtt`, a managed component since ESP-IDF 6.0): one connection to the broker, settings
on the web UI's MQTT page (address, user, password write-only, discovery prefix, base topic,
default `pixeled/<device name>`). On every connect: availability `online` (the last will
`offline`, both retained), the discovery configs (retained) under one device "PixeLED" with the
board as the model, the subscriptions, the state. A timer compares the LED state every 250 ms and
publishes what changed; the sensors every 30 s. Commands go to the render task.

| Entity | Topics | What | Built |
| --- | --- | --- | --- |
| light (JSON schema) | `light/set`, `light/state` | on/off, brightness, RGB color, effect (the animation names) | yes |
| number | `speed/set`, `speed/state` | speed 0 to 255 | yes |
| sensor | `sensors` (JSON) | power (W), frames per second (diagnostic) | yes |
| binary_sensor | `motion` | motion, with a sensor | yes |
| switch | `auto_off/set`, `auto_off/state` | motion auto off, with a sensor (config) | yes |
| number | `auto_off_minutes/set`, `auto_off_minutes/state` | minutes after the sensor clears, 1 to 240 (config) | yes |
| button | `restart` | restart | yes |
| select | | palette, preset, playlist | no |
| number | | intensity, transition time | no |
| switch | | reactive, playlist running | no |
| sensor | | temperature, light | no |
| text | | overlay text | no |

---

## 9. Optional hardware

Pins come from the board ([architecture.md](architecture.md) section 8) and become runtime
settings (section 8.1 there).

* **Motion sensor** (built): auto off, a setting (off by default, 1 to 240 minutes, NVS key
  `motion` in namespace `leds`). One timer, the sensor's hold (`px::motion`): the sensor gives a
  short pulse per motion, every pulse sets the hold back to the minutes, and when it runs out
  without another pulse the detected state clears, the LEDs fade off and the mark "off by
  motion" is set; the next motion switches them on again, but only after an auto off. Every on
  or off by a person (web, MQTT, console) clears the mark. The mark is not stored: after a power
  cut the LEDs stay as they were stored. The first build had a second countdown after the
  sensor's fixed 5 min hold (from the hassctrl base), so the LEDs went off 5 + N minutes after
  the last motion; dropped 2026-10-20.
* **Buttons**: click, long press and hold, each with an action (on/off, next animation, next
  preset, brightness, restart). Holding a button for 3 s restarts the device (the `kfc_fw`
  behavior; the controller PCB has no reset button). Holding the board's button at boot opens the
  hotspot (TODO).
* **Rotary encoder** (PCNT): brightness with acceleration; a push switches to the animation.
* **Light sensor**: automatic brightness.
* **Temperature sensor**: section 5.
* **I2S microphone**: section 6.1.

---

## 10. Presets, playlists, overlay

* **Preset**: an animation with its parameters and the brightness, stored on LittleFS, part of the
  backup's presets part.
* **Playlist**: presets with a time each, cycled.
* **Text and clock overlay**: a bitmap font drawn over any animation on a matrix: scrolling text
  (also set over MQTT) or the time.

---

## 11. Phases

1. Skeleton from esp32-hassctrl: build for all four boards, web UI, update, recovery (done).
2. Driver spike and `px::output`: RMT (done: 768 LEDs on 3 lanes at 127 fps on the ESP32), then
   ESP32 I2S, S3 LCD_CAM, PARLIO.
3. Render task, layout and its lookup table, brightness, gamma, power limit, test pattern, the
   layout page of the web UI (done, 2026-10-19).
4. Animations, palettes, crossfade, presets and playlists.
5. MQTT and Home Assistant discovery.
6. Audio: microphone (built 2026-10-21 with the spectrum), WARLS, WLED audio sync; reactivity.
7. Optional hardware: buttons, encoder, motion, power switch, light, temperature.
8. Realtime input, live preview over a WebSocket, text and clock overlay, custom shapes.
