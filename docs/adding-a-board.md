# Adding a board

A board is a dev kit or a controller PCB with one of the supported chips (ESP32, ESP32-S3,
ESP32-C3, ESP32-C6). Adding one touches one board file, one directory in `boards/` and one Kconfig
choice ([architecture.md](architecture.md) section 8). One unit with other wiring is a variant,
not a new board (section "Variants" below).

## 1. What to find out

| What | Where it goes |
| --- | --- |
| chip, flash size, PSRAM (and its mode: quad or octal) | `boards/<name>/sdkconfig.defaults` |
| pins that are taken: flash, PSRAM, USB, UART0, strapping pins, the board's own LED and buttons | the comment at the top of the board file |
| LED data pins: as many lanes as the chip's parallel output has ([leds.md](leds.md) section 2.2), on pins that are free at boot | `Info::led_gpios` (`lanes({...})`) |
| a level shifter on the data pins (3.3 V to 5 V), and whether it inverts | the comment; an inverting one is not supported yet |
| the LED power switch (relay or MOSFET) and the level that turns it on | `Info::power_gpio`, `power_active_high` |
| a motion sensor input and its active level | `Info::motion_gpio`, `motion_active_high` |
| the I2C pins for sensor modules | `Info::i2c_sda_gpio`, `i2c_scl_gpio` |
| an LDR on an ADC1 pin | `Info::light_ldr_gpio` |
| the board's button and up to 3 more, to GND (-1 for one that is not there: the others keep their number and actions) | `Info::button_gpios` (`buttons({...})`, button 1 the board's own) |
| the button that, held at boot, opens the hotspot (button 1 unless another suits better, e.g. when button 1 is the reset) | `Info::hotspot_button` (0 is button 1) |
| a rotary encoder: A, B and its push switch | `Info::encoder_a_gpio`, `encoder_b_gpio`, `encoder_button_gpio` |
| an I2S microphone (INMP441): clock, word select, data | `Info::mic_sck_gpio`, `mic_ws_gpio`, `mic_sd_gpio` |

## 2. Files

| File | What |
| --- | --- |
| `boards/<name>/sdkconfig.defaults` | `CONFIG_IDF_TARGET`, `CONFIG_PX_BOARD_<NAME>=y`, flash size and mode, partition table (`partitions/4mb.csv` or `16mb.csv`), PSRAM, options for the flash chip if the boot log asks for them |
| `components/board/Kconfig` | a `config PX_BOARD_<NAME>` in the `PX_BOARD` choice, with `depends on IDF_TARGET_...` |
| `components/board/CMakeLists.txt` | `if(CONFIG_PX_BOARD_<NAME>)` adds `boards/<name>.cpp` |
| `components/board/boards/<name>.cpp` | the image tag (`px_image_tag` with the board's name), `constexpr Info kInfo = variant::apply(Info{...})`, `info()` |

Copy the board file of the closest board and change the name in both places (the image tag and
`Info::name`; a `static_assert` checks that they agree). The name is at most 31 characters: an
update for another board is refused by it.

## 3. Bring-up

1. `python tools/build.py <name> --app recovery -p COMx flash`, then
   `python tools/build.py <name> -p COMx flash monitor` and `python tools/build_fs.py <name> -p COMx`.
2. The boot log names the board, the flash and the PSRAM (`log_system_info`); check them against
   the hardware.
3. First setup over the open hotspot (README, "First setup"), then `python tools/test_web.py
   <address>`.
4. Console: `relay` switches the LED supply, `motion` shows the sensor, `light` the light sensor.
5. The LED lanes: test pattern per lane (with the LED engine, phase 3 in [TODO.md](TODO.md)).
6. README ("Supported hardware") and [architecture.md](architecture.md) section 8 list the board.

## Variants

One unit's add-ons or other wiring: a directory `boards/<board>/variants/<variant>/` (lower
case, digits and `_`; not in git) with

* `px_board_variant.hpp`: a copy of `components/board/src/variant/px_board_variant.hpp` whose
  `apply()` changes the fields of `Info` that differ (other LED pins, a relay, a motion sensor);
  set `info.variant` to the variant's name, it is logged at boot.
* `sdkconfig.defaults` (optional): Kconfig options of this unit only, the last layer.

`python tools/build.py <board>.<variant>` builds it in `build/<board>.<variant>/<app>`. The image
tag stays the board's: a variant takes the board's updates and the other way round, so update
with the variant's own build.
