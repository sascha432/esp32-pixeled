# PixeLED - changelog

Every change to the firmware, the web UI, the tools and the docs, newest first, under the day it
was made; a release gets its version as the heading. Each entry says what a user or a developer
sees: a new feature under Added, a different behavior under Changed, a repaired one under Fixed,
and what is gone under Removed. Updated in the same commit as the change ([CLAUDE.md](../CLAUDE.md)).

## Unreleased (0.1.0)

### 2026-11-09

Added

* The example variant `esp32c6_devkit.supermini` ([boards.md](boards.md)): the ESP32-C6 SuperMini,
  a board with only the C6's own USB. Its `sdkconfig.defaults` moves the console to the USB port
  (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), lane 1 is the board's WS2812 on GPIO 8. The first C6
  the firmware ran on.

Fixed

* The C6 offers 2 LED lanes, not 4: its RMT sends 2 until the parallel output (PARLIO) is built.
  The layout page, the console and the backup counted the board's 4 pins; a layout with lane 3 or
  4 stopped with "the outputs did not start", now it says "lane 3: this chip sends 2 lanes".
  `available_lanes` in `/api/leds/layout` is the board's pins, at most what the chip sends.

### 2026-11-06

Added

* The Glow example "Fire 2012 (Glow)" (`web/glow/fire-2012-glow.glow`, under "Start from" on the
  Scripts page): the built-in Fire 2012 as a script, a fire in every column on its own, with
  sliders for the cooling and the sparking; with sound each column sparks with its band. It burns
  like the built-in one; 2.2 ms a frame at 768 LEDs on the ESP32 (the built-in 0.5 ms).
* The Glow example "Octopus (Glow)" (`web/glow/octopus-glow.glow`): the built-in Octopus as a
  script, with WLED's sliders the built-in one fixes (legs, offset x and y); each pixel's angle and
  distance are worked out once and kept in two grids. It looks like the built-in one; 2.1 to 2.7 ms
  a frame (the built-in 1.3 ms).
* Five more Glow examples from built-in animations, with the sliders WLED has: Julia (Glow) (a loop
  per pixel, 9.6 ms a frame at 768 LEDs), Fireworks 1D (Glow) (the sparks in arrays, 1.1 ms), DNA
  spiral (Glow) (1.1 ms), Sun radiation (Glow) (the relief from a grid, 3.3 ms) and Drift rose
  (Glow) (1.4 ms).
* Animations left out of the lists: the LED settings page has a box per animation (the scripts
  too); one not ticked is not in Control's list, Home Assistant's effects or the buttons' and the
  encoder's steps, and can still be shown (by its slug, a preset). `POST /api/leds` takes
  `hide=<slug>` and `show=<slug>`, `/api/leds` says `hidden` in the list; NVS key `leds/hidden`,
  in the backup (`hidden` per animation and per script).
* Console: `leds effect next` and `leds effect prev` step like the buttons.

Changed

* Scripts take RAM only while they run: the one shown (and the one fading out) has its program and
  the machine's 6.5 KB loaded by the render task from the file system when it comes on, given back
  when it goes and while the LEDs are dark; the others are files only (`/fs/scripts/index.bin`
  with a record per slot instead of `slots.txt`, the program `<slug>.bin`, compiled again at the
  start when it is from another firmware or another source). Their style and sliders have a key
  each (`leds/script0` to `script16`; the blob `leds/scripts` is moved at the first start). On the
  test board with twelve scripts: 96 KB free with a built-in animation shown, 88 KB with a script
  (76 KB with seven scripts before, whatever was shown). Up to 16 scripts by default (was 8): a
  slot costs no RAM now. For developers: a build directory keeps its `sdkconfig`, so it gets the
  16 after the sdkconfig is made again.
* `/api/leds`: the list of the animations (`effects`) only with GET; the answers to a change and
  the live socket's pushes have the rest and `shown` (the animation shown: slug, name, a script's
  sliders), so no file is read while the LEDs render (a file read stops the code in flash on both
  cores). The web UI's pages keep the list they loaded.
* Glow: a block of a per-pixel `if` is skipped where no pixel of the 32 is in it (Julia: 27 ms a
  frame before, 9.6 ms now).
* Home Assistant gets the effect list again once its changes stopped for 2 s (scripts saved or
  deleted, animations left out), not for each one: every announce queues some 30 retained
  messages, a backup's scripts took tens of KB at once.
* The backup's file is sent in pieces as it is written (it was made in RAM at once).
* For developers: `tools/test_web.py` counts a download the device cuts off (two `files.zip` at
  once without the memory for both) as a failed check instead of stopping.

Fixed

* A backup export with many scripts stopped the device (the file in RAM at once: no memory).
* A backup import that the free memory cannot read (its tree of strings, about 3 times the file,
  and a compile) is refused with 503 "not enough memory ..." instead of stopping the device; on
  the test board a file of 19 KB with twelve scripts (the settings part alone imports).
* A script found on the file system without a slot (an image, an unpacked backup) started with its
  sliders at 0: now at their defaults.
* Saving a script could hang the web server (the task watchdog): it waited for the compile task by
  yielding, which gives no time to a task of lower priority.

### 2026-11-05

Added

* Seventeen more animations after WLED's 2D ones (its numbers 182, 164, 123, 66, 149, 42, 90, 120,
  180, 126, 168, 176, 174, 122, 181, 125, 166): DNA spiral, Drift, Drift rose, Fire 2012,
  Firenoise, Fireworks, Fireworks 1D, Ghost rider, Hiphotic, Octopus, Julia, Lissajous, Polar
  lights, Scrolling text, Sindots, Soap, Sun radiation. Each has the sound switch and takes a
  palette; WLED's extra sliders are fixed values, the speed 0 stands still on a picture. Scrolling
  text runs the overlay's text while the overlay is set to text (the overlay then steps back for
  it), else the time and the date, in the light's color. WLED's Matrix (153) is the Matrix rain
  that was there. 0.4 to 1.5 ms a frame at 768 LEDs on the ESP32, 94 to 100 fps.
* Scripts: animations in Glow, a small language of their own ([glow.md](glow.md)), written on
  the web UI's new Scripts page (an editor with line numbers, five examples, the live preview, Run
  without saving, Save), compiled on the device and run in the render task, 32 pixels per
  instruction. A saved script is an animation: in the list, on the buttons, in presets and Home
  Assistant's effects, with its rotation, palette and sound settings and its own sliders (`param`,
  on Control too). Up to 8 (`CONFIG_PX_SCRIPT_SLOTS`), as files in `/fs/scripts`, in the backup's
  presets part; console `script`. A compile error says its line; a script with an endless loop
  stops (the status says where), the firmware does not. Plasma as a script takes 1.3 ms a frame at
  768 LEDs on the ESP32 (the built-in one 0.7 ms).
* For developers: `CONFIG_PX_SCRIPTS` (on by default) takes the scripts out whole: off, the
  firmware has no code and no RAM of them and the web image no page (`tools/build_web.py`'s
  `OPTION_FILES`); the code 432 bytes smaller than before them, the static RAM the same (the test
  board). Glow is host tested (`glow_test.cpp`: the math, exact pixels, 1 against 32 pixels at
  once, the budget, the examples).
* Twelve animations after WLED's 2D ones, each with the sound switch, strength, attack and
  release: Colored bursts, Blobs, Distortion waves, Waverly, PS vortex, PS fuzzy noise, PS
  waterfall, PS GEQ 2D, PS GEQ nova, PS ghost rider, PS blobs, PS galaxy. The two GEQs need sound
  (listed with an audio source only, like Spectrum); all but Distortion waves take a palette.
  WLED's extra sliders are fixed values; the speed 0 stands still on a picture.

Changed

* The styles hold 64 animations (NVS key `styles` version 5, 514 bytes); those of version 4 are
  taken over with the update. The test pattern Edges is number 42 now: a light or a preset stored
  on it (number 25) shows DNA spiral.
* For developers: the delayed settings writes (`platform::set_blob_later`) share one buffer of
  1 KB (a value up to 640 bytes) instead of 288 bytes for each of 8 keys; the static RAM of the
  test board is 464 bytes less, the styles' growth included. `effects::Params::text` is the text
  of the animations that show one; `leds::style_of` is there without the scripts too (no copy of
  all the styles on the caller's stack).
* The animations' memory is the larger of a byte per pixel and a trail of 3 bytes per pixel plus
  up to 384 particles (6 KB): 6.5 KB for 768 LEDs, about 30 KB for 8192, twice during a
  crossfade. An ESP32 without PSRAM fits fewer LEDs than before.
* For developers: `memory::allocate_large` returns null when the heap is full instead of
  stopping the firmware; `memory::Vector` and `memory::String` still stop it (no exceptions).
  The layout table is a `memory::Array`, whose `assign` says false then. `read_large_body` says
  why it failed (`std::expected`, `ESP_ERR_NO_MEM`).

Fixed

* A layout too large for the memory (an ESP32 without PSRAM) no longer restarts the device: the
  LEDs stay dark, the status says "no memory for the frame (79 KB)" (or "... for the layout
  table") and the web UI keeps working to change the layout. The buffers that did fit are given
  back. Measured on the WLED controller rev 1.1 with no client connected: 5120 LEDs fit (25 KB
  left), 8192 do not.
* Without the memory for the quarter turn, the crossfade, the preview or the live view these are
  skipped and tried again a second later, not every frame. The backup import and the log download
  answer 503 "no memory" where they stopped the device.

### 2026-11-04

Changed

* `GET /api/leds/live`: the preview's socket sends the canvas alone, 10 a second (5 for one over
  4096 pixels); the sound at the end of each message and the 38-byte sound messages between
  them are gone (nothing read them since the LEDs page was split). `?audio` sends the sound
  alone as 34 bytes, without the empty 0 x 0 header.

Fixed

* ESP32-S3: the recovery app starts again when the PSRAM fails. mbedTLS allocated from PSRAM there
  too (the board's layer) and its first allocation aborted the boot, in a loop; it allocates from
  PSRAM in the main firmware only now (`sdkconfig.main.esp32s3`).

### 2026-11-03

Changed

* Web UI: the LEDs page is split in four. Control keeps the preview, the light with its state
  (now with the motion sensor's), the presets and the overlay; Sound has the sound input; LED
  settings the power and timing form and the motion auto off; Buttons the buttons' actions.
  `#/leds` is still the first page.
* Web UI: a new menu. From 800 px a rail of icons with a short name, the groups (Light,
  Hardware, Network, Device) apart by a line, the appearance and log out at its bottom; on a
  phone a tab bar at the bottom (Control, Sound, Layout, Wi-Fi, More), More opens every page as
  a sheet. The Wi-Fi entry carries the connection's state as a dot. Sound and Buttons only show
  on a board that has them; the recovery app leaves out the LED pages, MQTT and Time and starts
  on Status. "Log out everywhere"
  moved to the System page (card "Password").

Added

* `/api/status` lists the device's `features` (`leds`, `mqtt`, `time`, `audio`, `buttons`) for
  the menu.
* `GET /api/leds/live?audio`: the preview's socket with the sound alone (38 bytes, 20 a second),
  for the bars of the Sound page; the canvas is not copied for it.

Fixed

* Web UI: a page left while it still loaded no longer takes the live socket from the next page
  (that page was drawn once and got no pushes); `render()` returns the page's `live()`.
* The live socket of a logout or an ended login is closed also when the server's queue was full
  at that moment (it stayed open until TCP keep-alive ended it).
* The web server no longer hangs when no task can be made for a long response (`files.zip`): the
  free memory for its stack is checked before the request is handed over.

Added

* Console: `mem tasks` lists every task's stack, its peak use and where it lives (needs
  `CONFIG_FREERTOS_USE_TRACE_FACILITY`, off in the builds), from hassctrl.

Changed

* ESP32-S3: 21 KB of static data that only tasks touch (the network task's state, import
  tables, pending settings writes, web and MQTT buffers, the live socket's clients) and mDNS in
  PSRAM instead of internal RAM, as hassctrl; the recovery app keeps them internal, so it still
  starts when the PSRAM fails.
* Task stacks sized from a measurement with `mem tasks` (docs/TODO.md, "Task stacks"): web server
  8 to 6 KB (also in the recovery app), console 6 to 4 KB, mDNS 4 to 3 KB, sound analysis 4 to
  3 KB, UDP sound inputs 3 to 2.5 KB; 6.5 KB more internal RAM. While they run: `files.zip` 5 to
  3.5 KB, the URL download 8 to 6 KB.

### 2026-11-02

Added

* Web UI: the pages follow their state on one WebSocket per tab (`/api/live`) instead of asking
  every 1 to 5 s; the LEDs page follows the light, the sound input and the presets on it, a preset
  saved in another tab shows up. A hidden tab gets only a heartbeat every 10 s. Not in the recovery
  app, whose pages ask every 3 s (`"live": false` in `/api/status`).
* Build time next to the firmware version on the Status and System pages, in `/api/status`
  (`device.built`) and in the boot log; every build stamps it, not only a full build.
* Console: `sockets` lists lwIP's sockets with both ends; `wifi status` shows modem sleep
  (`modem:` sleeps, awake and held by the LEDs or a web page, or the seconds until it sleeps).
* `tools/build_fs.py <board> --device <address>`: the web image over the network, keeping the
  device's own files (the presets, from `/api/fs/files.zip`).

Changed

* Wi-Fi modem sleep 5 minutes after the LEDs went off and the last web request, the first 5
  minutes from the start; a web page that is shown keeps the Wi-Fi awake as the LEDs do (was: at
  once with the LEDs off). `/api/status` has `wifi.modem_sleep`.
* Web server: a session silent for 30 s is closed, TCP keep-alive finds a dead WebSocket; 24 lwIP
  sockets and TCP connections (was 16), the budget checked at build time; `files.zip` is sent on a
  task of its own, the other pages answer meanwhile, and its list of files grows with them (two
  at once fit a board without PSRAM).
* The captive portal's DNS listens only while the hotspot is on.
* The LEDs page's preview closes while the tab is hidden and opens again when it shows.
* `tools/update.py ... filesystem` needs `--fresh` (the image replaces the presets).
  `tools/test_web.py` checks idle sessions, the live socket and a download next to the API.

Fixed

* A restart that hangs in a shutdown step ends anyway: every restart (console, web, button,
  Home Assistant, update) arms the RTC watchdog for 5 s first; the next start then reports a
  watchdog reset.
* `tools/build_fs.py -p` kept nothing of a file system the device had formatted itself (its
  superblock's name limit is LittleFS's own, 255, the tool mounted it with 64); it reads it now.

### 2026-10-23

Changed

* Wi-Fi modem sleep while the LEDs are off: on the ESP32 test board the idle current at 5 V drops
  from 70 to 80 mA to 15 to 25 mA, and the device answers up to about 100 ms later (ping 80 ms
  instead of 13 ms on average). With the LEDs on power save stays off as before.
* Web image: `tools/build_web.py` keeps only the Bootstrap rules in use and minifies JS and CSS
  (about 30 KB less to load per page); `// #if CONFIG_*` blocks in the JS can leave out the pages
  of options a firmware lacks.
* Web files carry the image's ETag: a browser that has them gets 304 instead of the files.

Fixed

* Web UI, Time: the time zone and its rule ran together ("America/VancouverMST7"), the space
  between them is back. The JS minifier squeezed the text of a template literal
  inside `${...}`; `tools/build_web.py` now passes template literals through unchanged.

### 2026-10-22

Added

* Parallel LED output on the ESP32-S3 (LCD_CAM): up to 16 lanes from one DMA stream, encoded
  into a small ring while sending, as on the ESP32; no extra pins. 4 x 1024 LEDs at 29 fps on
  the AI-S3. It is the default driver on the S3; RMT stays a choice.
* docs/boards.md: every board and variant with its GPIOs (LED lanes, supply, sensors, buttons,
  microphone, taken pins), with the clones AI-S3 (ESP32-S3 N16R8: photo, pinout image, RGB LED on
  GPIO 48, the solder jumpers) and AI-C3 (ESP32-C3-MINI-1: photo, pin headers). Their variants
  `esp32s3_devkit.ai_s3_test` (8 lanes on GPIO 4 to 7 and 15 to 18) and
  `esp32c3_devkit.ai_c3_test` (2 lanes on GPIO 10 and 4; 2 x 512 LEDs at 60 fps), without relay
  or sensors, are in git as examples.
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
