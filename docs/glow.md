# PixeLED - Glow, the script language

Glow writes animations without building the firmware: a script is typed on the web UI's Scripts
page, compiled on the device in a few milliseconds and run by a small machine in the render task.
A saved script is an animation like the built-in ones: in the list of animations, in Home
Assistant's effects, in presets and on the buttons. Its sliders (`param`) sit on Control.

The code: `components/effects/src/glow` (the compiler and the machine, pure C++, host tested in
`host/tests/glow_test.cpp`), `components/scripts` (the files, compiling, the programs for the render
task), `components/leds_api/src/api_scripts.cpp` (the web API), `web/scripts.js` (the page),
`web/glow/*.glow` (the examples). The LED engine's side: [leds.md](leds.md) section 6.5.

All of it is an option: `CONFIG_PX_SCRIPTS` (menu "PixeLED scripts", on by default). Without it the
firmware has no code, no RAM and no web page of it (section 9).

---

## 1. A first script

```
// Plasma waves: three waves across the canvas, summed; the sum picks the color from the palette.
anim "Plasma waves"
param scale = 2 [0.5 .. 6] "Size"

pixel {
  s = wave(u * scale + t / 5) + wave(v * scale - t / 7) + wave((u + v) * scale / 2 + t / 3)
  pal(s / 3 + t / 20)
}
```

`anim` names it (the name in the lists, up to 31 characters). `param` makes a slider named "Size"
from 0.5 to 6, starting at 2. The `pixel` block runs for every pixel: `u` and `v` are its place
across the canvas (0 to 1), `t` the time in seconds, `wave` a sine from 0 to 1 and `pal` the light's
palette at a place (0 to 1, once around).

More in `web/glow`: `plasma-waves`, `fire-grid` (a grid of heat), `bouncing-balls` (arrays and the
trail), `beat-rings` (sound), `lava-lamp` (blobs, params), `fire-2012-glow` (the built-in Fire 2012
as a script: a fire per column, sparks by the bands), `octopus-glow` (the built-in Octopus with
WLED's legs and offset sliders; each pixel's angle and distance kept in two grids), `julia-glow`
(a loop per pixel), `fireworks-1d-glow` (particles in arrays, drawn in `frame`), `dna-spiral-glow`,
`sun-radiation-glow` (a relief from a grid) and `drift-rose-glow`. The editor offers them under
"Start from". A script's file name is its slug: not one of a built-in animation (`fire`, `plasma`).

## 2. Numbers

One kind of number: fixed point with 16 bits before and after the point, from -32768 to
32767.99998 in steps of 1/65536. They behave the same on every chip; the C3 and C6 have no FPU.

* An overflow wraps around; `x / 0` is 0; `sqrt` of a negative number is 0; `x % y` has the sign of
  `y` (so `x % 1` is 0..1 for a negative `x` too).
* A division by a number written in the script is a multiplication by its reciprocal (a division
  takes many times as long), within 2^-15 of it; one by a value is a division in 32 bits.
* A comparison gives 1 or 0; a condition is true when it is not 0.
* **Angles are turns**: 1 is a full circle. `sin(0.25)` is 1; `atan2` gives 0 to 1.
* Colors take 0 to 1 for each part (`rgb(1, 0.5, 0)`); hues are turns as well (`hsv(0.33, 1, 1)` is
  green).

## 3. The parts of a script

Declarations first (any order), then the blocks. A line ends a statement; `;` separates several on
one line. Comments: `//` to the end of the line, `/* ... */`.

| Declaration | Meaning |
|---|---|
| `anim "Name"` | the name, required, unique among the animations |
| `sound required` / `optional` / `never` | whether it reacts to sound; without it: optional when it reads a sound value, else never |
| `param size = 0.5 [0 .. 1] "Size"` | a slider (up to 8): its value, the range, `step 1` for whole steps, the label (else the name) |
| `var a, b = 2` | variables, kept between frames, 0 or their value at the start |
| `var p[16]` | an array of 16 numbers, kept; at most 16 arrays of 1024 numbers together |
| `grid g` | a number from 0 to 1 per pixel (a byte each), kept; up to 8 |
| `fn f(a, b) { ...; return a * b }` | a function (up to 6 values); `return` is its last statement |

| Block | Runs |
|---|---|
| `init { }` | once, when the script starts (fresh memory: another animation, rotation or program) |
| `frame { }` | once a frame, before the pixels |
| `pixel { }` | once per pixel |

A name assigned in a block is a local of that block (and the blocks inside it): `a = 1` declares
it. To use a value after an `if`, assign it before:

```
c = 0
if u < 0.5 { c = 1 }
hsv(c, 1, 1)
```

## 4. Values

| Value | Where | Meaning |
|---|---|---|
| `t`, `dt` | anywhere | seconds since the start and since the last frame, at the light's speed (128: the clock; 0 stands; 255 about four times as fast) |
| `W`, `H`, `N` | anywhere | the canvas' width, height and pixels (turned by the rotation) |
| `hue`, `sat` | anywhere | the light's color as a hue (turns) and a saturation |
| `level`, `beat` | anywhere | the sound's loudness (0..1, with strength, attack and release) and 1 in the frame of a beat |
| `bass`, `mid`, `high`, `band(k)` | anywhere | the bands: 0..5, 6..17, 18..31 averaged; band k of 32 |
| `speed` | anywhere | the light's speed, 0..1 |
| `x`, `y` | pixel | the pixel's column and row from 0 |
| `u`, `v` | pixel | the middle of the pixel across the canvas, 0..1 |
| `i` | pixel | `y * W + x` (exact up to 32767 pixels) |

The sound values are 0 when the script does not react (the "Reacts to sound" switch is off, or there
is no source).

## 5. Statements and operators

* Assignment: `=`, `+=`, `-=`, `*=`, `/=`; `p[k] = 1` (not per pixel), `g[x, y] = 1` (section 7).
* `if c { } else if d { } else { }`
* `for k in a .. b { }`: k from a up to b - 1. Per pixel the ends must be the same for every pixel.
* `repeat n { }`, `while c { }` (not per pixel), `break`, `continue`.
* Operators, the strongest first: `-` and `not`/`!` before a value; `* / %`; `+ -`;
  `< <= > >= == !=`; `and`/`&&`; `or`/`||`; `c ? a : b`.

## 6. Functions

| Function | Gives |
|---|---|
| `abs floor ceil frac round sqrt` | as named |
| `min(a, b ...)`, `max(a, b ...)` | up to 4 values |
| `clamp(x, a, b)`, `mix(a, b, t)`, `step(e, x)`, `smooth(e0, e1, x)` | as in shaders |
| `hypot(dx, dy)`, `atan2(y, x)` | a length, an angle in turns 0..1 |
| `sin(x)`, `cos(x)` | -1..1, a turn a period |
| `wave(x)`, `tri(x)`, `saw(x)` | 0..1 a turn a period: a sine (0.5 at 0), a triangle (0 at 0, 1 at 0.5), the fraction |
| `square(x, duty)` | 1 for the first `duty` of every turn (0.5 without it), else 0 |
| `time(p)` | 0..1 every `p` seconds (at the speed) |
| `noise(x, y, z)` | smooth value noise 0..1, a cell per 1 |
| `rand()`, `rand(n)` | 0..1, a whole number 0..n-1; different for every pixel |
| `hash(a, b)` | 0..1 from two whole numbers, always the same (a fixed pattern) |

**Color**: `hsv(h, s, v)` (s and v 1 without them), `rgb(r, g, b)`, `pal(p, v)` (the light's
palette at p, brightness v; without a palette the rainbow), `col(v)` (the light's color at v),
`heat(v)` (black, red, yellow, white, or the palette). In `pixel` they set the pixel's color; a
pixel given none stays as it was (the trail, or dark). In `init` and `frame` they set the pen.

**Drawing** on the trail, in `init` and `frame`, with the pen; places in pixels, between pixels too:
`fade(a)` (dims by a: 0.1 a little, 1 to dark), `blur(a)`, `clear()`, `dot(x, y)`,
`line(x1, y1, x2, y2)`, `disc(x, y, r)` (firm), `blob(x, y, r)` (soft). Each keeps the lighter of
the trail and the pen. A script that draws keeps the trail between frames; every frame starts from
it, then the pixel block (if any) colors over it.

## 7. Grids

`grid g` is a value 0..1 per pixel, kept between frames. `g[x, y]` reads it anywhere (a place
outside the canvas is the nearest edge's). In `init` and `frame` any cell can be written. Per pixel
only its own cell, `g[x, y] = ...`, and it is written after the row: a pixel reads the rows below
as they were before this frame and the rows above as already written. So fire rises a row a frame
without a second buffer (`web/glow/fire-grid.glow`).

## 8. Limits and errors

A script has up to 8 KB of source, 8 params, 64 variables, 1024 array numbers, 8 grids, 16 functions
and 96 different numbers; blocks, parentheses and calls nest 16 deep; the program has up to 512
instructions; per pixel up to 32 values are alive at once. Beyond, the compiler says so.

A compile error comes with its line and column: "line 3, column 3: unknown function 'hsb'". The
editor marks the line. Saving a script that does not compile keeps the file, and the program saved
before runs on.

A script cannot stop the firmware: an array's index and a grid's place are clamped, and every frame
has a budget of steps (100000 for `init` and `frame`, 512 per pixel for `pixel`, counted at the
loops). A script over it stops: it draws dark and the status says "the script stopped at line 12:
too many steps in one frame" until it starts again (another animation, a new save). A script that
needs more memory than the canvas gives draws dark with "the script needs N bytes".

**Memory** of a script, in the animation's memory between frames (`effects::memory_size`, about 3
bytes per pixel plus up to 6 KB for the particles of the built-in animations): 4 bytes per variable
and array number, 3 bytes per pixel for the trail (when it draws), a byte per pixel for each grid,
and a row per grid written per pixel. 768 pixels give 6.4 KB: a trail and two grids fit. 8192 give
about 30 KB: a trail and a grid do not.

## 9. How it runs

**The compiler** reads the source and writes the program at once, without a syntax tree, so it
needs little memory: about 15 KB while it compiles. It runs on a task of its own with an 8 KB
stack (the nesting: 5.5 KB for the deepest scripts that compile); console `script` shows how much
of it a compile took. A function is compiled where it is called (inlined). The blocks are compiled
again when a local turned out to be per pixel after it was used as one value for all pixels.

**Per pixel** the code runs on up to 32 pixels at once: every value of the pixel block is either the
same for all of them (a scalar register) or one per pixel (a vector register of 32 lanes), and an
instruction on a vector is a tight loop over the lanes, so reading an instruction costs a 32nd per
pixel. A value that is the same for every pixel for the whole frame (`t / 5`, `scale * 2`) is
computed once a frame before the pixels. An `if` whose condition differs from pixel to pixel runs
both branches, each where its condition holds (a mask per lane), and skips a branch no pixel of the
32 is in (so a loop whose pixels are done costs little: Julia); the same condition for every pixel
is a jump. A grid written per pixel makes the runs rows (section 7), else they go straight through
the canvas.

**Speed**: an instruction on a vector is a small loop over its lanes, the 16.16 math inlined; the
switch over the instructions has a jump table (ESP-IDF builds without them). The palette and the
heat are tables of 256 colors made once a frame, the sine a table of a turn; `hsv` is inline. The
pixels' places step from one pixel to the next without a division. Measured on the ESP32 (section
11): about 12 cycles a pixel for an addition, 20 for a `wave`, 120 for `rgb(u, v, 0)` with filling
in `u` and `v`.

**The program** is one block of bytes (1 to 3 KB for the examples): the name and params, the
numbers, the instructions (8 bytes each) and the line of each. It is checked before it runs
(registers, jumps, arrays and grids in range). Its state lives in the animation's memory, so the
crossfade between two scripts works like between two animations. The machine's registers and its
two color tables (6.5 KB, internal RAM) belong to the render task and are there while a script
runs.

Measured on the WLED controller rev 1.1 (ESP32 at 240 MHz, 24 x 32): see section 11.

## 10. On the device

* **Files**: `/fs/scripts/<slug>.glow` (the source), `<slug>.bin` (its program, with the firmware
  it is from and its source's CRC; the run's is `run.bin`) and `index.bin` (a record per slot: the
  slug, the name, what it uses, the error, the sizes; it took over `slots.txt` 2026-11-06). A file
  put there without a slot (a file system image, an unpacked backup) takes a free one at the start,
  with its sliders at their defaults. A program from another firmware or another source is compiled
  again at the start. `tools/build_fs.py -p` and `--device` keep them like the presets.
* **RAM**: only the scripts that run, the one shown and the one fading out: the render task loads
  the program from its file when the script comes on and frees it when it goes (and while the LEDs
  are dark); the machine's 6.5 KB are there while one runs. The lists (the web UI, Home Assistant,
  the console) read the index; the API's answers while the LEDs render read nothing (a file read
  stops the code in flash on both cores).
* **Slots**: `CONFIG_PX_SCRIPT_SLOTS` (1 to 16, 16 by default: a slot costs no RAM). Slot n is
  animation number 200 + n (stored in the state and the presets); the editor's Run is 216 (slug
  "run"), not kept over a restart.
* **Stored** (NVS keys `leds/script0` to `leds/script16`, 10 s after the last change): each slot's
  style (rotation, sound switch, strength, attack, release, palette) and sliders. A slot that gets
  another script, or a script with other params, starts with the defaults.
* **Web**: the Scripts page (docs/ui/web/web-app.md section 4) and `GET /api/scripts`,
  `/api/scripts/source`, `POST /api/scripts/save|run|delete`; the sliders with `POST /api/leds`
  `p0..p7`.
* **Console**: `script` (the list, each one's state, the compiler's stack), `script show <slug>`,
  `script dis <slug>` (the program, an instruction a line), `script run <slug>`;
  `leds effect <slug>` finds scripts too.
* **Backup**: the section `scripts` in the presets part (the source, style and sliders of each);
  an import compiles every script before anything is applied and replaces them all.
* **Home Assistant**: the saved scripts are in the light's effect list, announced again when one is
  saved or deleted.

**Off**: `# CONFIG_PX_SCRIPTS is not set` in a board's or variant's `sdkconfig.defaults` leaves out
the compiler, the machine, the scripts component, the web routes and the page: no code, no RAM.

## 11. Measured

2026-11-05, the WLED controller rev 1.1 (ESP32, 4 MB, no PSRAM,
`esp32_devkit.wled_controller_rev1_1`), against the firmware before the scripts:

| | Before | Off | On |
|---|---|---|---|
| Flash code (.text) | 1058312 bytes | 1057880 (-432) | 1115892 (+57580) |
| Flash data (.rodata) | 267448 bytes | 267432 (-16) | 277496 (+10048) |
| Static RAM (.bss + .data) | 90899 bytes | 90899 (the same) | 91859 (+960) |
| Image | 1441670 bytes | 1441222 | 1509682 (+68012) |

The speed of the examples, the animation's time per frame at 24 x 32 (768 LEDs), 240 MHz, overlay
off, five readings each:

| Script | Per frame |
|---|---|
| `plasma-waves` | 1.31 ms (the built-in Plasma: 0.69 ms) |
| `fire-grid` | 0.74 ms |
| `bouncing-balls` | 0.5 to 0.63 ms |
| `beat-rings` | 1.71 ms |
| `lava-lamp` (5 blobs: a `hypot` and a division each per pixel) | 6.6 ms (2026-11-06; the 3.3 ms of 2026-11-05 were 2 blobs: its sliders were at 0) |
| `fire-2012-glow` (2026-11-06, the median of ten) | 0.9 ms (the built-in Fire 2012: 0.5 ms) |
| `octopus-glow` | 1.7 ms (the built-in Octopus: 1.3 ms; with `atan2` and `hypot` every frame instead of the grids 3.2 ms) |
| `julia-glow` (20 steps a pixel) | 9.6 ms (27 ms before a block no pixel is in was skipped; the built-in Julia: 1.5 ms) |
| `sun-radiation-glow` | 3.3 ms (9.3 ms with four noises a pixel instead of the grid; the built-in: 1.9 ms) |
| `fireworks-1d-glow`, `dna-spiral-glow`, `drift-rose-glow` (drawing in `frame`) | 1.1, 1.1, 1.4 ms |
| a `frame` block alone | 0.11 ms |
| an empty `pixel` block | 0.18 ms |

The overlay (the clock) adds 0.2 ms to every animation. The frame takes 10 ms at 100 fps, 8.3 ms of
it the output: the plasma, the fire, the balls and the rings keep about 95 fps there, the lava about
80. The compiler took 2.5 KB of its stack for the examples and 5.4 KB for the deepest scripts that
compile.

Off, the map file has no symbol of glow or the scripts and the web image no `scripts.js` and no
`glow/`; the few hundred bytes less are shared helpers that compile tighter since. On, while a
script runs, the heap holds its program (1 to 3 KB) and the machine's registers and color tables
(6.5 KB); a script that does not run takes none (2026-11-06; before, every saved one held its
program and a list entry of 0.6 KB). A compile borrows 15 KB of heap and the 8 KB stack of its task.
Twelve scripts on the test board: 96 KB free with a built-in animation shown, 88 KB with a script. The five examples on the test board: 14 KB of internal RAM (63 KB free, 77 KB without
scripts).
