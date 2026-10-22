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

One interface (`px::output`; RMT and the parallel backends of the ESP32 and the S3 are built): configure the lanes (pin, pixel count, color order),
hand it a frame, get a callback when it is out. Behind it one backend per chip:

| Chip | Parallel backend | Lanes | Fallback |
| --- | --- | --- | --- |
| ESP32-S3 | LCD_CAM in i80 mode: 16 data lines, GDMA from a ring of small buffers in internal RAM, encoded while sending | 16 | RMT, 4 channels |
| ESP32 | I2S1 in LCD mode: 8 data lines, DMA from a ring of small buffers in internal RAM, encoded while sending | 8 | RMT, 8 channels |
| ESP32-C6 | PARLIO TX (lane count to verify) | up to 8 | RMT, 2 channels |
| ESP32-C3 | none | | RMT, 2 channels |

* **Parallel encoding**: each WS2812 bit becomes 3 slots of the bus clock (2.4 MHz: high, data,
  low). For 8 lanes one slot is a byte with one bit per lane, so a pixel row of all lanes is
  24 x 3 = 72 bytes; 1024 pixels per lane are 74 KB per frame (148 KB for 16 lanes). The bits are
  transposed with a lookup table.
* **S3** (built 2026-10-22, `components/output/src/lcd_output.cpp`): LCD_CAM in i80 mode, 16-bit
  samples, bit n on lane n (data lines 0 to 15), no WR, DC or CS pin (ESP-IDF's i80 driver needs
  them, so the backend sets the registers itself). 2.42 MHz: the 160 MHz PLL / 2 / 33, integer
  dividers (a slot 0.41 us, a bit 1.24 us). The same ring as on the ESP32: 4 buffers of 24
  positions in internal DMA memory, refilled from the GDMA EOF interrupt (cache safe, level 3),
  then the latch, after which the LCD stops by itself; a frame in the PSRAM is copied to internal
  RAM first. Measured on the AI-S3 board: 4 x 1024 LEDs 31.2 ms a frame (29 fps with the render
  task, the theory 30.7 ms), the encoder 116 us per buffer for 4 lanes (`leds bench encode`), as
  on the ESP32. 8 lanes of 32 LEDs: 1.4 ms a frame, 100 fps, smooth.
* **ESP32** (built, `components/output/src/i2s_output.cpp`): I2S0 in LCD mode, 16-bit samples,
  bit n on lane n (data lines 8 to 23 of the GPIO matrix), 2.4 MHz from the 160 MHz PLL. The frame
  is encoded while it is sent (built 2026-10-21): a ring of 4 buffers in internal DMA memory, 24
  LED positions each (144 bytes per position, 3456 bytes and 0.72 ms per buffer, the always-high
  and always-low slots written once at the start). The DMA reads them in a circle; the interrupt
  at the end of each buffer (from IRAM, also while the flash is busy) encodes the next positions
  into it, three buffers ahead of the DMA; after the last positions the chain goes on to 800 zero
  slots (the latch) and ends. 15 KB for any LED count (one buffer for the frame took 147 KB for
  1024 per lane). A frame in the PSRAM is copied to internal RAM first. The I2S hardware sends the
  two 16-bit samples of a 32-bit word in reverse order; the encoder swaps them. Measured on the
  test matrix: 3 lanes of 256 LEDs 8.35 ms a frame (94 fps with the render task, which aims at
  100; the encoding no longer adds to the frame: 8.9 ms before), 3 x 1024 31.2 ms (32.0 fps, the
  theory 31.0), 3 x 2048 61.9 ms (16.2 fps). 8 lanes (2026-10-21, lanes 5 to 8 on free pins of
  the test board without LEDs): 8 x 256 8.21 ms (122 fps), 8 x 1024 31.3 ms (32.0 fps), 8 x 2048
  62.1 ms (16.1 fps), the same as 3 lanes: the wire sets the time. The encoder (`leds bench
  encode`: every bit of every lane set, the slowest data) takes 110 us per buffer for 3 lanes and
  248 us for 8, for any count, of the 720 us the buffer is sent in; about 28 us per lane, so 16
  lanes would need about 470 us. Not tried: how the LEDs look with 1024 per lane (the matrix has
  256). 8-bit samples were planned to halve the
  memory for up to 8 lanes; they would not: at 8 bits the ESP32's I2S in LCD mode still takes one
  32-bit FIFO word per two samples (ESP-IDF's own i80 LCD driver for the ESP32 says so), and the
  ring made the memory small anyway. The default driver. I2S1 stays for the microphone.
* **RMT**: one channel per lane, the IDF's encoder; the C3 and C6 have 2 TX channels. Without
  DMA (ESP32, C3) an interrupt refills the pulse memory every few pixels; a late refill ends the
  frame early and the LEDs show garbage. Seen on the ESP32 while the web UI polled: the interrupt
  waited for the flash. Built: the interrupt runs while the flash is busy
  (`CONFIG_RMT_TX_ISR_CACHE_SAFE`), each lane gets the memory of the unused channels, level 3.
  Measured on the AI-C3 board (2026-10-22, strips on both lanes): 2 x 512 LEDs at 60 fps (the
  wire 15.7 ms a frame), so about 2 x 600 for 50 fps.
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
  brightness, animation, color, speed) and the animations' styles 10 s after the last change, at
  most 60 s after the first (the write policy, [architecture.md](architecture.md) section 9.3);
  restored at boot (built). Each
  blob starts with a version (`kStoredVersion`); a blob of another version is not read and the
  defaults apply. A field added into a struct's padding does not change its size, so the size
  alone could not tell (found 2026-10-19: the new dither field read the old padding).
* **Crossfade** (built 2026-10-21): another animation fades in over the previous one
  (`Config::crossfade_ms`, default 1000, 0: at once). The previous one keeps drawing on a second
  canvas with its own memory (allocated with the first switch; without the memory no crossfade)
  and is blended in less with every frame. Only while the LEDs are lit.
* **Overlay** (built 2026-10-21): text or the clock drawn over the canvas after the crossfade
  (section 10).
* **Live preview** (built 2026-10-21): while someone asks (`leds::preview`, the web page's
  WebSocket), the task copies the canvas after the overlay, before the brightness. The socket
  (`components/leds_api`) sends 20 messages a second: the canvas in every second one (every fourth
  for more than 4096 pixels), with an audio input the sound at the end of each (flags, loudness,
  the 32 bands; main hands `leds_api::register_routes` a getter on `audio::status`). ESP-IDF 6.1
  answers the handshake without calling the route's handler: the login is checked in the
  pre-handshake callback and the viewer taken in the post-handshake one.
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
use it until the layout page exists). Before a new layout starts, the lanes of the old one get a
dark frame, so LEDs a smaller layout no longer reaches go dark.

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
* Forced off in the panic handler (built 2026-10-21: `relay::off_at_crash` for relays 1 and 2;
  the wrapped `esp_panic_handler` writes the pins' registers before it prints; the console's
  `crash yes` tries it; the panic and the restart tried on the test matrix with the LEDs off, the
  pin itself not yet measured), and by the recovery app.
* **At power on** (`Config::power_on`, built 2026-10-21): as before (default), off, or on. "As
  before" keeps the mark "switched off by motion" too (NVS key `boot`, written when it changes):
  the LEDs stay off and come on with the next motion.
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
  * 32 bands log-spaced from 50 Hz to 10 kHz, each the loudest bin of its range in dB, plus the
    gain (setting, -20 to 40 dB, default 0);
  * an automatic gain (setting, default on): the reference follows the loudest band up at once
    and falls 3 dB a second; the range below it (setting, 10 to 80 dB, default 40) is 0..255, so
    quiet and loud music both fill the bars. Off: the reference is full scale (0 dB), the gain
    decides how full the bars get;
  * silence below the silence level (setting, -90 to -20 dB, default -62; a quiet room reads
    -65 to -75 dB, the gain added): the bars fall to 0;
  * the bars rise at once and fall by 12 % of full height per block;
  * a beat when the bass (bands 0 to 3) is the beat threshold (setting, 1.2 to 3.0 times,
    default 1.6) of its average, at most one in 140 ms;
  * the loudness from the block's RMS on the same scale.

  The settings (`audio::Settings`, NVS namespace `audio`, key `settings`) are set on the LEDs
  page's card "Microphone" (`GET/POST /api/audio`, with a live view of the bands and levels),
  with the console `audio set gain|agc|silence|range|beat <value>` and in the backup (section
  `audio`); the task uses them from the next block.

  The render task takes the latest result with each frame; a beat stays until a frame took it.
  The analysis is host tested (`host/tests/audio_test.cpp`). Console `audio [watch [s]]` prints
  the bands as digits. Not built yet: loudness left and right, the peak, the input on the S3 and
  the C chips.
* **UDP "WARLS"** spectrum packets (built 2026-10-21), port 21324 (a setting), the `kfc_fw`
  format (its `scripts/audio_analyzer` sends them from a PC, as does `tools/audio_send.py`): 2 to
  32 bands spread over the 32, the loudness the louder of left and right, the beat found from the
  bass like the microphone's; the source holds for the packet's timeout.
* **WLED audio sync v2** (built 2026-10-21), multicast 239.0.0.1 port 11988, so a WLED
  sound-reactive device or LedFx can be the source: 16 bands spread over the 32, the loudness
  from `sampleSmth`, the beat from `samplePeak`; the source holds for 1 s. The group is joined once
  there is a network and again after 30 s without packets.

  Both are switched on in the card "Sound input" (`POST /api/audio` `warls`, `warls_port`,
  `wled_sync`), the console (`audio set warls|port|wled ...`) and the backup (section `audio`);
  NVS key `inputs` in namespace `audio`. Their task (`components/audio/src/udp.cpp`, the packets
  in `packets.cpp`, host tested) starts after the network (a socket before lwIP is up asserts:
  found as a boot loop 2026-10-21). While a UDP source holds it is used instead of the
  microphone. Tried on the test matrix with `tools/audio_send.py`: WARLS to the device and WLED
  sync over multicast, about 40 packets a second, the bands and the beat arrive.

### 6.2 Reactive is a switch, not an animation

There are no separate reactive animations. Each animation declares whether it reacts: never,
optional, or only with audio. An optional one has a "reactive" switch ("Reacts to sound"), shown
only on a device with an audio source. Built 2026-10-21:

* **Per animation:** each animation keeps its own switch and its own rotation (`leds::Style`,
  NVS key `styles`, indexed like the animations: a new animation goes before the test patterns,
  whose index may change). The web UI, Home Assistant and the console set them for the current
  animation; the backup lists the animations that are not at the default.
* **Rotation:** 0, 90, 180 or 270 degrees clockwise, done by `effects::render` for every
  animation. A quarter turn draws on a second canvas of height x width (allocated by the render
  task when first used) and copies it turned; half a turn reverses the pixels in place. So a
  strip turned by a quarter gets the fire along its length.
* **Memory between frames:** the render task gives each animation a block
  (`effects::memory_size`: a small header and a byte per pixel), cleared when the animation, its
  rotation or the canvas changes. Fire keeps its heat there, the reactive animations their phase,
  their smoothed level and the beat's flash.
* **What an animation hears:** the loudness smoothed with the attack and release (20 and 250 ms
  as built); in silence it falls with the release instead of starting again from 0. The
  brightness comes from it (never below a quarter) while there is sound and is full in silence,
  gliding between the two (100 ms in, 1 s out), so sound at the silence level does not make the
  LEDs flicker. Breathe adds the flash of a beat (fading in 300 ms); the others move their colors
  on a beat and glide there in 250 ms instead of jumping. Without a source an animation with the
  switch on runs as without it.
* Rainbow and plasma: faster and brighter with the sound; a beat moves the colors on (by the
  strength). The speed follows only the smoothed loudness, not the beat's flash, so attack
  and release decide how it changes; the strength sets how much: at most 1.5 times as fast at
  128, twice at 255 (also gradient, noise, meteor). Until 2026-10-21 it was up to 5 times, and
  with the automatic gain the loudness is high through most music: the animations raced. Breathe: the brightness follows the sound instead of the breathing. Fire: each column
  sparks with its band (bass left, treble right), loud means taller flames, a beat sparks every
  column. The brightness of a reactive animation never goes below about a tenth.
* **Strength** per animation (0 to 255, default 128 as above): the level an animation hears and
  the bands (fire, matrix rain, spectrum) are multiplied by strength / 128 (at most full); 255
  reacts twice as much, 0 is the switch off. A slider under the switch on the LEDs page, Home
  Assistant number "Sound strength", console `leds strength`, backup `strength` in
  `leds.animations`.
* **Attack and release** per animation (built 2026-10-21): how fast the level and the bands rise
  (attack, 0 to 2000 ms, default 20) and fall (release, 0 to 5000 ms, default 250); the bands are
  smoothed in the animation's memory. Sliders next to the strength (also for Spectrum, whose bars
  follow them), Home Assistant numbers "Sound attack" and "Sound release", console `leds attack`
  and `leds release`, backup `attack_ms` and `release_ms`. The stored styles of the versions
  before are kept (NVS key `styles`, version 4 with the palette, 32 animations; version 3 is read
  with the palette at none).

### 6.3 The list (a proposal, to be decided)

| Animation | Reactive | Notes |
| --- | --- | --- |
| Solid | no | |
| Flash / strobe | no | also the alarm and the reboot warning |
| Game of life | no | 2D |
| Gradient | optional: pulse | Built 2026-10-21: from the color to the color a third of the hue circle on, there and back along the diagonal, moving; with sound faster and brighter |
| Rainbow | optional: speed, brightness | along the strip or at an angle |
| Color fade | optional: next color on the beat | Built 2026-10-21: one color around the hue circle (8 s at speed 128); with sound brighter, a beat jumps on |
| Fire | optional: heat, sparks, a band per column | Built 2026-10-21: a heat map, a byte per pixel; each step cools, lets the heat rise and sparks the bottom rows; black, red, yellow, white; the speed sets 15 to 90 steps a second |
| Plasma | optional: speed, hue, zoom, bands | |
| Noise / lava | optional | Built 2026-10-21 as "Noise": smooth value noise (8 pixel cells) drifting through the hues; with sound faster and brighter |
| Twinkle / stars | optional: sparks on the beat | Built 2026-10-21 as "Twinkle": stars in the color rise and fade (3 to 0.4 s), about one LED in seven; more with the sound, a burst on the beat |
| Fairy lights | optional: sparkle | the `kfc_fw` Xmas animation |
| Meteor / scanner | optional | Built 2026-10-21 as "Meteor": one per row, left to right, a tail fading unevenly, 4 to 64 pixels a second; with sound faster and brighter |
| Ripple | optional: a ring on the beat | Built 2026-10-21: rings in rainbow colors from random places, fading as they grow (a new one every 3 to 0.4 s); with sound a ring on every beat |
| Matrix rain | optional | Built 2026-10-21: drops in the color with a light head and a tail of half the height; with sound each column drops with its band, all on a beat |
| Spectrum bars | only with audio | color: rainbow, gradient or one color; peaks; VU rows (the three bar animations of `kfc_fw` in one). Built 2026-10-21 as "Spectrum": a rainbow bar per band from the bottom, a narrower canvas shows the loudest band of each group; a dim line while there are no bars |

The animations with colors of their own take a palette instead (section 6.4). The new
animations of 2026-10-21 are host tested (they draw, move, follow the sound) and timed on the
host only; how they look on the matrix is open.

### 6.4 Palettes

Built 2026-10-21 (`px/effects/effects.hpp`, host tested). A palette is 16 colors once around a
circle; the colors between two of them are blended, the last into the first. The animations that
draw from the hue circle (rainbow, plasma, color fade, noise, ripple, spectrum) take the
palette's color at the same place instead; fire takes it for its heat, from dark at no heat.
Solid, breathe, gradient, twinkle, meteor and matrix rain show the color and take none.

| Palette | Colors |
| --- | --- |
| none ("Its own colors", the default) | the animation as built: the hue circle, fire's black-red-yellow-white |
| rainbow | the hue circle (the same as none for most) |
| party, ocean, forest, lava, heat, cloud | the colors of FastLED's palettes of these names |
| sunset | dark red, orange, yellow, pink, purple, dark blue and back |
| pastel | the hue circle at little saturation |
| color ("Shades of the color") | the color, down to a quarter of it in the middle and back |

The palette is part of each animation's style (`leds::Style::palette`, section 6.2), kept per
animation like the rotation: the LEDs page's select (shown for an animation that takes one), Home
Assistant select "Palette", console `leds palette <slug>`, backup `palette` in
`leds.animations` and in the presets. The index is stored: a new palette goes at the end.

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
| select | `palette/set`, `palette/state` | the current animation's palette, by name (section 6.4) | yes |
| select | `preset/set`, `preset/state` | the presets by name; the state is the one the light matches, `None` (unknown) when none does; announced again when the list changes, removed while there is none | yes |
| select | | playlist | no |
| number | `transition/set`, `transition/state` | "Transition": the fade time of on, off and the brightness, 0 to 10000 ms (config) | yes |
| number | `crossfade/set`, `crossfade/state` | "Crossfade" between animations, 0 to 10000 ms (config) | yes |
| switch | `dither/set`, `dither/state` | "Dithering" (config) | yes |
| number | | intensity | no |
| select | `rotation/set`, `rotation/state` | the current animation's rotation: 0, 90, 180, 270 | yes |
| switch | `reactive/set`, `reactive/state` | "Reacts to sound", the current animation's switch, with an audio source (announced again, or removed, when a UDP input is switched on or off; Spectrum is in the effect list only with a source) | yes |
| number | `strength/set`, `strength/state` | "Sound strength", the current animation's, 0 to 255, with an audio source | yes |
| switch | | playlist running | no |
| sensor | | temperature, light | no |
| select | `overlay/set`, `overlay/state` | the overlay: off, text, clock | yes |
| text | `overlay_text/set`, `overlay_text/state` | the overlay text, 63 bytes | yes |
| number | `attack/set`, `release/set` and `/state` | "Sound attack" and "Sound release", the current animation's, ms, with an audio source | yes |
| select | `power_on/set`, `power_on/state` | at power on: restore, off, on (config) | yes |

The light takes Home Assistant's `transition` (seconds): the brightness and on/off of that command
go from where the light is to the new value in that time (at most 5 minutes) instead of the fade
time (`leds::set_transition`). Tried on the test matrix 2026-10-21 under a separate discovery prefix:
every entity announced, palette, preset, transition, crossfade, dithering and a 4 s transition
work; not yet seen in Home Assistant itself.

---

## 9. Optional hardware

Pins come from the board ([architecture.md](architecture.md) section 8) and become runtime
settings (section 8.1 there).

* **Motion sensor** (built): auto off, a setting (off by default, 1 to 240 minutes, NVS key
  `motion` in namespace `leds`). One timer, the sensor's hold (`px::motion`): the sensor gives a
  short pulse per motion, every pulse sets the hold back to the minutes, and when it runs out
  without another pulse the detected state clears, the LEDs fade off and the mark "off by
  motion" is set; the next motion switches them on again, but only after an auto off. Every on
  or off by a person (web, MQTT, console) clears the mark. The mark is stored (key `boot`): with
  "at power on: as before" (section 5) the LEDs stay off after a restart and come on with the
  next motion. The first build had a second countdown after the
  sensor's fixed 5 min hold (from the hassctrl base), so the LEDs went off 5 + N minutes after
  the last motion; dropped 2026-10-20.
* **Buttons** (built 2026-10-21, `components/input`): up to 4 push buttons to GND
  (`board::Info::button_gpios`, the internal pull-up where the pin has one), sampled every 10 ms
  by an esp_timer, debounced (30 ms). Each has an action for a click, a double click and a hold:
  nothing, on / off, next or previous animation (without the test patterns and, without a
  source, Spectrum), next preset, brighter, darker (about a sixteenth of the brightness a step;
  brighter switches on), restart, open the hotspot, "Knob: animations". A hold acts after 0.6 s,
  brighter and darker repeat every 0.1 s while held, a restart needs 3 s (the `kfc_fw` behavior;
  the controller PCB has no reset button); without a hold action any press is a click; with a
  double click action a click waits 0.3 s for the second press. The defaults, made for the WLED
  controller (a button the board does not have stays empty, the others keep their number):

  | Button | Controller rev 1.3 (`wled_controller_rev1_3`) | Rev 1.1, the test board (`wled_controller_rev1_1`) | Click | Double click | Hold |
  | --- | --- | --- | --- | --- | --- |
  | 1 | GPIO 13, BTN0 | none: BTN0 is the reset (EN) | next animation | - | restart (3 s) |
  | 2 | GPIO 35, BTN1 | GPIO 35, BTN1 | darker | - | darker, repeating |
  | 3 | GPIO 34, BTN2 | GPIO 34, BTN2 | brighter | - | brighter, repeating |
  | 4 | GPIO 19, BTN3 | GPIO 19, BTN3 | on / off | - | next preset |
  | encoder push | not fitted | not fitted | on / off | next preset | knob: animations |

  Rev 1.3 has a 10k pull-up and 100 nF on each button; rev 1.1's GPIOs are taken from rev 1.3 and
  to be checked.

  Settings `input/settings` (written at once), the LEDs page card "Buttons",
  `GET/POST /api/input`, backup section `input`, console `input`. The state machine is host tested
  (`host/tests/input_test.cpp`). The board's hotspot button (`board::Info::hotspot_button`,
  button 1 unless the board says otherwise; button 4, BTN3, on the rev 1.1 test board) held while
  the device starts (2 s, checked in `main` before the network starts) opens the hotspot without
  a password ([architecture.md](architecture.md) section 9.1); on a board whose button 1 restarts
  after 3 s, holding it on does the same. On the test board (rev 1.1) 2026-10-22 by the user:
  darker, brighter, on/off and the hotspot with BTN3 and the reset work; the rest of the test
  steps are in [TODO.md](TODO.md).
* **Rotary encoder** (built 2026-10-21, not tried: none is wired): A and B
  (`board::Info::encoder_a_gpio`, `encoder_b_gpio`, to GND at the detents, pull-ups) counted by
  the PCNT in full quadrature (4 counts a detent, a 1 us glitch filter), on the C3 (no PCNT) by
  GPIO interrupts. Turning changes the brightness, 4 steps a detent when turned fast (a detent
  within 40 ms), 2 within 100 ms; after the action "Knob: animations" it changes the animation, a
  detent each, until it rests for 10 s. Its push switch (`encoder_button_gpio`) is a button like
  the others: on / off, twice the next preset, held the knob for the animations.
* **Light sensor**: automatic brightness.
* **Temperature sensor**: section 5.
* **I2S microphone**: section 6.1.

---

## 10. Presets, playlists, overlay

* **Preset** (built 2026-10-21, `components/presets`): the light under a name (up to 31 bytes),
  up to 16: the animation, color, speed, brightness and the animation's style (rotation, sound,
  palette). Stored on LittleFS (`/fs/presets.bin`, written beside it and renamed, so a power cut
  keeps the old or the new list; `tools/build_fs.py -p` keeps it), at once with every change.
  Showing one sets all of that and switches the LEDs on. The preset the light matches is found by
  comparing (so a change of the brightness leaves it). LEDs page card "Presets",
  `GET /api/presets` and `POST /api/presets/save|apply|delete`, Home Assistant select "Preset",
  the button action "next preset", console `leds preset [<name>|next|save <name>|delete <name>]`,
  the backup's presets part (section `presets`).
* **Playlist**: presets with a time each, cycled.
* **Text and clock overlay** (built 2026-10-21, `px/effects/overlay.hpp`, host tested): a 5 x 7
  font, proportional, ASCII and the German letters (UTF-8), drawn over any animation in its own
  color, centered vertically: a text (63 bytes) centered when it fits, else scrolling (5 to 50
  pixels a second), or the clock "hh:mm" (24 or 12 hours, the colon blinking; 3 x 5 digits when
  the 5 x 7 ones do not fit, as on the 24 wide test matrix), shown once the time is known. LEDs
  page card "Overlay", `POST /api/leds/overlay`, Home Assistant select "Overlay" and text "Overlay
  text", console `leds overlay`, backup `leds.overlay`, NVS key `overlay`. Not tried on the
  matrix yet.

---

## 11. Phases

1. Skeleton from esp32-hassctrl: build for all four boards, web UI, update, recovery (done).
2. Driver spike and `px::output`: RMT (done: 768 LEDs on 3 lanes at 127 fps on the ESP32), ESP32
   I2S (done 2026-10-21), S3 LCD_CAM (done 2026-10-22), then PARLIO.
3. Render task, layout and its lookup table, brightness, gamma, power limit, test pattern, the
   layout page of the web UI (done, 2026-10-19).
4. Animations, palettes, crossfade, presets (palettes and presets built 2026-10-21) and
   playlists.
5. MQTT and Home Assistant discovery.
6. Audio: microphone (built 2026-10-21 with the spectrum), WARLS, WLED audio sync (built
   2026-10-21); reactivity.
7. Optional hardware: buttons and encoder (built 2026-10-21), motion, power switch, light,
   temperature.
8. Realtime input, live preview over a WebSocket and text and clock overlay (built
   2026-10-21), custom shapes.
