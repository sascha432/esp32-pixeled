# Instructions for Claude Code

- **ESP-IDF first.** `tools/build.py`, `tools/build_fs.py` and the other tools fail with "IDF_PATH is
  not set" in a plain shell. Run them in PowerShell after the activation script, in the same command:

  ```powershell
  . C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1 | Out-Null; python tools/build.py esp32s3_devkit
  ```

  The shell state does not persist between tool calls, so every call starts with the `.` line.
- Boards and ports: see README.md ("Supported hardware", "Flash") and docs/TODO.md. Flash the board
  under test after a change.
- **Style**: follow docs/architecture.md section 11 and the code around the change (free-function
  components, one owner task per state, `esp_err_t` / `std::expected`, `kCamel`, `g_`, `_member`).
- **Feature list with every change.** `README.md` starts with the list of features. A feature that
  is new, changed or gone changes the list in the same commit; keep it short, one line per point.
- **Docs with every change.** Update the affected `docs/*.md` (architecture, leds, adding-a-board,
  the pages under `docs/ui/web`) in the same commit as the code. Check `docs/TODO.md`:
  mark items done or changed, add new open points, update the state lines.
- **Changelog with every change.** `docs/CHANGELOG.md`: an entry under today's date (newest
  first, inside the unreleased version), under Added, Changed, Fixed or Removed, saying what a
  user or developer sees. A change that reverses an entry of the same day replaces it instead of
  adding a second one.
- **NVS writes follow the policy** in docs/architecture.md section 9.3: values changed by live
  controls (sliders, Home Assistant numbers) go through `platform::set_blob_later` (10 s delay),
  explicit saves and secrets are written at once, nothing writes per frame.
- **No code from kfc_fw.** `../kfc_fw` (the clock and LED matrix plugin) is the reference for
  features and behavior only.
