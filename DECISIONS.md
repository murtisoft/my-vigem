# DECISIONS

Durable project choices, deduced from the code. Follow them; fix a line when a step changes it.

## Platform / build
- Windows-only, plain C (C99) with the vendored ViGEmClient C++ file; CMake project `my-vigem`, MinGW-style static link (`-static`).
- UNICODE build (`-DUNICODE -D_UNICODE`), but code stays TCHAR-clean (`LPTSTR`, `TEXT()`, `_tcs*`, `DBG_TSTR` for log formats).
- Version lives in `CMakeLists.txt` (`project(... VERSION x.y.z)`); the `.rc` (icon `APP_ICON` + version info) is generated into the build dir.
- Tray app, GUI subsystem (no console). `--debug` flag attaches/allocs a console for `debug_log`.
- Files use CRLF line endings; 4-space indent, Allman braces, `snake_case`, `_` prefix for internal helpers.
- No third-party libs besides ViGEmClient; own helpers in `utils.c` (`_mem_zero`, `_mem_copy`, `_tcsistr`, `_guid_to_str`).

## Device support
- Only the Xiaomi Gamepad (VID 0x2717, PID 0x3144) is supported; matched by substring on the HID instance ID (Win10+ `VID&00022717_PID&3144`, Win7 `VID_2717&PID_3144`).
- Max 4 simultaneous devices (`MAX_ACTIVE_DEVICE_COUNT`); extra ones are rejected with a tray warning.
- Each device is emulated as a ViGEm Xbox 360 pad; Xiaomi button -> `GUIDE`, Return -> `BACK`, Menu -> `START`.
- Input packets are accepted only with header `0x04`; sticks are bytes centered at 128, Y axes are inverted.
- The Mi button is reported as a 200 ms pulse (held/released by a delay thread).
- Accelerometer and battery are parsed; only battery is used (tray menu text). Accel is not forwarded.
- Rumble: X360 large/small motor map to Xiaomi big/small (swapped in the callback), sent as feature report `0x20`.

## Concurrency
- Per gamepad (`mi.c`): one input thread, one output (vibration) thread, one delay thread, stopped via `stopping_event`; input thread cleans everything up and calls `stop_cb` last.
- `active_devices` is guarded by an SRW lock; removals signal a condition variable.
- Device refreshes run on a single worker thread; the tray thread only sets an event. Requests are coalesced; a full refresh wins over a normal one.
- Tray UI calls come from the worker/input threads directly (`tray_update`, notifications).

## Behavior (fork features)
- Single instance via named mutex `MI-VIGEM`; a second instance exits silently.
- Device arrival/removal (any HID interface change) triggers a normal refresh, whatever the operation type.
- Refresh menu item = full refresh: stop all pads, wait up to 5 s for removal, re-add in the same order, restart numbering at #1, then a normal refresh. Routine notifications are suppressed while `reattaching`.
- Opening a device: exclusive RW first; on failure disable+enable the devnode and retry exclusive, then shared; if re-enable fails, shared open directly.
- Disabled devnodes of the gamepad are re-enabled at every refresh (`hid_enable_disabled_devices`), so a failed re-enable never leaves it disabled.
- Re-enable: disable, wait for state, then enable with up to 3 attempts and a `CM_Enable_DevNode` fallback; each state change waits up to 10 s (20 ms poll).
- Rumble retries a failed command every 50 ms until it works (no timed safety stop); on stop, vibration-off is retried up to 20 times.
- If ViGEmBus is missing/incompatible the app still runs and lists devices, with an error/warning notification.

## HidHide
- Optional: if the driver is missing, the app works without hiding.
- On init: whitelist our exe (NT device path) unless HidHide is in inverse mode (then remove it), and activate HidHide.
- Per device: add instance path and its parent (container) path to the blacklist; remove both on unbind.
- On exit: restore HidHide's initial active state; the blacklist/whitelist entries we added are not otherwise restored besides per-device unbind.

## UI
- Tray icon only: menu lists `#N Xiaomi Gamepad (batt. X%)` items, a separator, `Refresh`, `Quit`.
- Menu is rebuilt from `active_devices` (`rebuild_tray_menu`) then applied with `tray_update`.
- Notifications are balloon tips (10 s), levels info/warning/error.
- Tray menu uses `-` text as a separator and a `NULL` text terminator.

## Debug / docs
- Protocol knowledge lives in `PROTOCOL.md` (from the author's readme + code); keep it the single reference. Mark each packet confirmed/unknown.
- Debug window is a plain Win32 window (no extra libs) on its own thread, opened from the tray `Debug` item; closing it only hides it (log is kept).
- It shows raw report bytes, the report decoded per `PROTOCOL.md` (unknown bits/bytes are listed), and the event log. The decoder in `debug_win.c` is independent of `mi.c` on purpose, so it shows what the hardware sends, not what the app assumes.
- All `debug_log` output goes to the console (`--debug`) and to the window; use it for anything worth seeing there.
- The raw report hook (`mi_set_raw_cb`) runs on the input thread: keep it cheap, no blocking.
