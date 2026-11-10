# PixeLED - TODO

State 2026-10-21: phases 1 (skeleton), 3 (render task, layout, web pages) and 5 (Home
Assistant) built and running on the test matrix; phase 2 has RMT and the ESP32 I2S and S3 LCD_CAM
outputs with their DMA ring; phase 6 has the microphone, WARLS and WLED audio sync (approved with music); phase
4 has 13 animations, the crossfade, palettes and presets; phase 7 the buttons and the encoder;
phase 8 the live preview and the overlay ([leds.md](leds.md) section 11).

State 2026-11-06, scripts only in RAM while they run: the render task loads the program of the
script shown (and of the one fading out) from the file system and gives it back when it goes and
while the LEDs are dark; the lists read `/fs/scripts/index.bin`, the styles and sliders have an NVS
key per slot; up to 16 scripts. Animations can be left out of the lists (LED settings, `hide`,
`show`, Home Assistant, the buttons, the backup). Five more examples from built-in animations
(Julia, Fireworks 1D, DNA spiral, Sun radiation, Drift rose; twelve in all). Host tested (127),
the test board's firmware built and flashed many times on the way; there with twelve scripts
(`full_test.py` in the session's scratchpad, 37 of 37): each example saved, shown, sliders and
picture right (0.5 to 9.6 ms a frame, Julia the slowest at 52 fps); 96 KB free with a built-in
animation, 88 KB with a script, the RAM given back while the LEDs are off; a slider kept while
the script did not run and after a restart; saved while shown (the same params keep the sliders,
others start again), deleted while shown (the first animation), the editor's run; hidden ones out
of `/api/leds`' list flags, Home Assistant's effect list (53 then) and the buttons' step, still
chosen by slug; the backup streamed (19 KB) and its settings part checked. 36 saves and shows in
a row without a stop. Found and fixed on the way: a self-deadlock of the render task (the status
took the lock twice), the web server spinning in a save (the compile task got no time: the task
watchdog), the backup export (a file of 30 KB in RAM at once) and import (the tree of strings)
stopping the device for want of memory, Home Assistant's announce per script change queueing tens
of KB, the sliders of scripts found without a slot at 0, the API reading files with every push
(the render task stalled: Plasma 4.8 ms instead of 1.3). `tools/test_web.py` 71 of 71 there (one
run before cut off a download of two `files.zip` at once: see the open point below; the tool now
counts it as a failed check). The four other boards' main firmware, the recovery apps of the ESP32
and the S3 and the ESP32 without the scripts build. The tests ran at brightness 40 (Sun radiation
draws 10 W at 76); the light is back on Octopus (Glow), brightness 76 (5.6 W). Open: the pages in
a browser (LED settings' boxes, Control's list), a backup with many scripts on an ESP32 without
PSRAM (refused, see below).

State 2026-11-06, Octopus: the Glow example "Octopus (Glow)" (`web/glow/octopus-glow.glow`, with
legs and offset sliders), host tested; saved on the test board and shown: through the preview the
same picture as the built-in Octopus, 2.1 to 2.7 ms a frame (the built-in 1.3; the same script
working out `atan2` and `hypot` every frame 3.2), 83 to 89 fps. Seen on the way: with the user's
page open the board had 70 KB of internal RAM free (33 KB 8-bit), the Wi-Fi driver logged a failed
1.6 KB allocation and two requests timed out or were reset; the board ran on (see the open point
on `files.zip`).

State 2026-11-06: the Glow example "Fire 2012 (Glow)" (`web/glow/fire-2012-glow.glow`), host
tested; saved on the WLED controller rev 1.1 (COM7) and shown there: through the preview it burns
like the built-in Fire 2012 (a few white sparks fewer at the very bottom: a spark goes up with the
same step instead of staying a step in its cell), 2.2 ms a frame (the built-in 0.5), 95 fps, 5.7 W
at brightness 76. The board's web image updated with `build_fs.py --device` (its six scripts and
presets kept), so the Scripts page offers it under "Start from"; the first try's `files.zip`
download timed out (the user's page open), the second went through.

State 2026-11-05, WLED's second set: seventeen more animations after WLED's (DNA spiral, drift,
drift rose, Fire 2012, firenoise, fireworks, fireworks 1D, ghost rider, hiphotic, octopus, Julia,
Lissajous, polar lights, scrolling text, sindots, soap, sun radiation; [leds.md](leds.md) section
6.3; WLED's Matrix is the Matrix rain there was), host tested (127 tests) and looked at as frames
rendered on the PC; the styles hold 64 animations (version 5, version 4 taken over: fire kept its
strength on the test board), the delayed settings share 1 KB (a style written 10 s later is
there after a restart). Flashed on the WLED controller rev
1.1 (COM7, over the network): each runs at 94 to 100 fps (0.4 to 1.5 ms per frame, the median), at
most 5.2 W at brightness 30, turned by a quarter, with a palette, at speed 0; Scrolling text seen
through the preview (the time and date in the middle rows; the overlay's text with the overlay
stepping back, which draws again over plasma); Home Assistant's effect list has them (1063
bytes of its 1536), "Polar lights" chosen by name over MQTT. With the demo of
`tools/audio_send.py` over WARLS and "Reacts to sound" on, each heard it and ran at 90 to 100 fps
(0.6 to 3.3 ms). The light is back on fire, brightness 39, overlay off, WARLS off, their sound
switches off. `tools/test_web.py` 70 of 71 there: two `files.zip` at once run out of memory (an
open point under the web server below, not from this change: its static RAM is lower). Open: how
they look on the matrix (for the user's eyes).

State 2026-11-05: twelve animations after WLED's 2D ones (colored bursts, blobs, distortion waves,
waverly and the particle ones PS vortex, fuzzy noise, waterfall, GEQ 2D, GEQ nova, ghost rider,
blobs, galaxy; [leds.md](leds.md) section 6.3), host tested and looked at as frames rendered on
the PC; flashed on the WLED controller rev 1.1 (COM7): each runs at 95 to 100 fps (0.4 to 2.3 ms
per frame), in silence and with the demo of `tools/audio_send.py` over WARLS (sound heard, the
GEQs light only with it), turned by a quarter, with a palette, at speed 0; no reset. Their "Reacts
to sound" is on there; the board is back on fire, brightness 52, WARLS off. Open: how they look on
the matrix (for the user's eyes).

State 2026-11-05, scripts: Glow ([glow.md](glow.md)) built and host tested (124 tests, the
1-lane against the 32-lane machine among them); the four boards' main firmware and the recovery
apps of the ESP32 and S3 build; with `CONFIG_PX_SCRIPTS` off the test board's firmware has the
static RAM of before, 432 bytes less code and no symbol of it. Flashed on the WLED controller rev
1.1 (COM7), the scripts put on its file system: the five examples compile at boot and run (plasma
1.31 ms a frame at 768 LEDs, the built-in plasma 0.69; fire 0.74, balls 0.6, rings 1.7, lava 3.3),
one with an error lists its line, an endless loop stops with its line while the LEDs run on at 99
fps, the deepest scripts compile with 5.4 KB of the compiler's 8 KB stack. Over the web API
there (login admin): `tools/test_web.py` 71 of 71; run with and without an error, save, a save
with an error (the program before runs on), a taken name, a slider and the rotation of a script,
a preset of one, the backup's presets part with the scripts exported, checked and imported, one
that does not compile refused, an endless loop stopped, and after a restart the script, its slider
and its rotation as they were; a deleted script shown goes back to the first animation. Found and
fixed there: a backup's scripts ran out of memory (a compile task that deleted itself kept its
stack until the idle task ran; now the caller deletes it), the sliders of the backup were listed
as ignored, and two examples had the slugs of built-in animations (they have their own names now,
and no script takes a built-in's slug). The Scripts page looked at in headless Edge against a
stand-in of the API (600 and 1100 px, light and dark). The board keeps the five examples; the
light is back on fire, brightness 39, the clock overlay. Home Assistant over the user's broker
(192.168.0.4): the light's effect list has the five scripts after the animations, a saved one comes
into it and a deleted one leaves it (announced again), one chosen by its name over MQTT is shown and
the state names it. Open: the page in a browser against the board.

State 2026-11-05, memory: a full heap no longer stops the firmware (`allocate_large` returns
null); tried on the WLED controller rev 1.1 with a 128 x 64 layout ("no memory for the frame", the
device ran on) and 5120 LEDs (fit), with the web UI open and again with no client connected (the
same numbers), then its 24 x 32 layout restored.

State 2026-11-04: the preview socket split like the pages, the canvas alone for Control and the
sound alone for Sound (`/api/leds/live?audio`, 34 bytes); flashed on the WLED controller rev 1.1
(COM7). Open: `tools/test_web.py` there (the login), Control and Sound looked at in the browser.

State 2026-11-03: the LEDs page split into Control, Sound, LED settings and Buttons, the menu as an
icon rail and a phone tab bar, `features` in `/api/status`, the sound-only preview socket; main,
recovery app and web image flashed on the test board, the recovery app's menu (Wi-Fi, Hotspot,
Status, Logs, System; starts on Status) looked at by the user. Open: `tools/test_web.py` against
the new pages (needs the login), light mode.

State 2026-11-02: the web server, the web UI's live socket and the modem sleep rule ported from
hassctrl, flashed on the test board; `tools/test_web.py` 63 of 63 there, the browser check is
open (phase 1).

**For the user's eyes (built 2026-11-05):** the twelve animations after WLED's on the matrix, with
the music and without (Control, the animation list after Noise); WLED's extra sliders are fixed
values: say where one should be different. The same for the second set of seventeen (after PS
galaxy in the list); Scrolling text shows the overlay's text (Overlay, Show: Text) or the time and
the date. Whether WLED's Matrix should be a second one in WLED's greens, beside Matrix rain in the
light's color.

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
styles take it. Since 2026-11-05 the values share 1 KB (the styles of 64 animations are 514
bytes). Not tried: the factory reset path (it erases the test board's settings).

## Decide

* The animation list ([leds.md](leds.md) section 6.3): what to add, what to drop, the palettes.
* Scrolling text: a text of its own (now the overlay's, Show: Text) and a larger font on a tall
  canvas (WLED has five sizes; here the overlay's 5 x 7).
* The open hotspot of the first setup ([architecture.md](architecture.md) section 9.1): keep it
  open, or a fixed default password printed on the device.

## Phase 1: skeleton (done, to be tried on hardware)

* Flash recovery and main on the S3, the ESP32, the C3 and the C6; first setup over the open
  hotspot; `tools/test_web.py` against each. The C6 booted 2026-11-09 (C6 test board below).
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
  Open (found 2026-11-05 on the test board, 77 KB of internal RAM free with the five scripts
  loaded and MQTT connected; 138 KB on 2026-11-02): two `files.zip` at once run out of memory
  while they send. Their archives are valid but miss files ("cannot be opened", "no memory to
  list it" in `unreadable.txt`), and the API answers after 1 to 4 s instead of at once, so
  `test_web.py` gives 70 of 71 there (one alone: complete, the API at once). 2026-11-06, with
  twelve scripts on the file system: once a download was cut off, the next run 71 of 71. The cause is not
  measured; likely the two downloads' Wi-Fi and TCP buffers and their 12 KB each on a smaller
  heap. Possibly one `files.zip` at a time (the second 503).
  Open (2026-11-06): a backup import is read into a tree of strings (3.1 times the file, measured
  with a 32-bit build) that cannot fail softly; one the free memory cannot hold is refused with
  503 before it is parsed. On the test board (ESP32 without PSRAM) a backup with twelve scripts
  (19 KB) is refused, its settings part alone imports. A reader that keeps the scripts' sources in
  the request's buffer, or the scripts outside the YAML (a zip with the files), would lift it.
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
  ESP32, C3 and C6 (the attribute does nothing there). AI-S3 (COM8, 2026-11-04), `mem` after
  the Wi-Fi connected: old firmware 106 KB internal free (largest block 32 KB); this firmware
  without the PSRAM layer 119 KB (44 KB, the smaller stacks), with it 159 KB (84 KB), PSRAM 39 KB
  less. It joins its saved network with a scan (the network task's defaults from `start()`),
  `test_web.py` 66 of 66, `build_fs.py --device`, a firmware upload straight into the main
  firmware (16 MB) in 12 s. The recovery app with the PSRAM driven in quad mode (fails for real)
  aborted the boot in a loop: not the PSRAM `.bss` (off there) but mbedTLS's PSA init, whose
  allocation from PSRAM the S3's board layer set for both apps. Fixed: that setting is in
  `sdkconfig.main.esp32s3` now; the recovery app then boots without PSRAM ("continuing without
  it", 319 KB internal), Wi-Fi and its web page answer. Seen once, not again: right after a
  recovery app and a main firmware were flashed in a row the S3 ran the recovery app (no rollback
  recorded); the second flash booted the main firmware. Not checked: the backup import on the S3.
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
* C6 test board: an ESP32-C6 SuperMini (C6FH4, 4 MB in the chip, only the C6's USB) on COM21,
  variant `esp32c6_devkit.supermini` (in git, boards.md; console on USB-Serial/JTAG, lane 1 on
  the board's WS2812, GPIO 8), 2026-11-09: recovery, main and web UI boot, console, hotspot,
  Wi-Fi, web server, 209 KB internal RAM free; the WS2812 lights as lane 1 (checked by the user).
  Open: a strip on lane 2, the LED on GPIO 15 (?), which of the DevKitC-1 pins reach the board's
  edge, a photo for boards.md, `tools/test_web.py`.
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
  picture with 1024 per lane. The render task with 8192 LEDs does not fit on an ESP32 without
  PSRAM (phase 3, measured 2026-11-05); the console's test frame of 8 x 2048 fits only alone.
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
* Large layouts on an ESP32 without PSRAM fail softly (built 2026-11-05): `allocate_large` returns
  null, the layout table is a `memory::Array`, the render task shows "no memory for the frame
  (79 KB)" and gives back the buffers that did fit (the table stays, for the status). A layout
  costs about 13 bytes per LED (table 3, canvas 3, frame 3, the animations' memory 3.8) plus up
  to 6 KB, the crossfade 7 more per LED. Measured on the WLED controller rev 1.1 (79 KB free with
  768 LEDs): 8192 LEDs (128 x 64, 4 lanes) "no memory for the frame (79 KB)" and the device ran
  on; 5120 (4 x 1280) fit at 22 fps with 25 KB left (largest block 11 KB). The same with the web
  UI open and with no client connected (console `sockets`). From 5120 straight to 8192,
  `set_layout`'s check found no room for its 16 KB table and took the layout unchecked; the
  render task then said "no memory for the frame". Open: the same with Control open (its live
  preview holds two more copies of the canvas, 15 KB each at 5120); how much Wi-Fi, MQTT and the
  web UI need free, so a safe maximum per board; the layout page could say before saving that a
  layout will not fit.
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
  (the bands arrive). Open: LedFx or a WLED device as the sender. The animations after WLED's ran
  fed by the WARLS demo on the test board 2026-11-05 (sound heard); the reactions not looked at.

## Scripts (Glow, built 2026-11-05)

Animations as scripts ([glow.md](glow.md)), `CONFIG_PX_SCRIPTS`. Open:

* A preset keeps a script's number and style, not its sliders: the sliders stay as the script has
  them (presets.bin would need another version).
* The sliders as Home Assistant numbers (the light's effect list has the scripts already).
* The editor: no syntax colors, no completion (a textarea with line numbers).
* The speed at 8192 LEDs on the S3 (measured so far: the test board, 768 LEDs).
* Playlists of scripts come with the playlists.

## Phase 4 and later

See [leds.md](leds.md) section 11: animations and palettes, presets and playlists, MQTT with
Home Assistant discovery, audio (microphone, WARLS, WLED audio sync), buttons and encoder,
light and temperature, realtime input (DDP, E1.31, Art-Net), live preview, text and clock overlay,
custom shapes.
