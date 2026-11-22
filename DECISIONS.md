# DECISIONS

Durable project choices, deduced from the code. Follow them; fix a line when a step changes it.

## Platform / build
- Windows-only, plain C (C99) with the vendored ViGEmClient C++ file; CMake project `my-vigem`, MinGW-style static link (`-static`).
- UNICODE build (`-DUNICODE -D_UNICODE`), but code stays TCHAR-clean (`LPTSTR`, `TEXT()`, `_tcs*`, `DBG_TSTR` for log formats).
- Version lives in `CMakeLists.txt` (`project(... VERSION x.y.z)`), passed to code as the `APP_VERSION_STR` string define; the `.rc` (icon `APP_ICON` + version info) is generated into the build dir.
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
- Device refreshes run on a single worker thread; the tray thread only sets an event. Requests are coalesced; a reset (`Reset Gamepads`) wins over a normal refresh.
- Tray UI calls come from the worker/input threads directly (notifications); menu text, menu array and `tray_update` changes go through `refresh_tray` (`tray_ui_lock`, serialized). `tray_update` does no cross-thread `SendMessage` (it would deadlock with that lock).
- ViGEm (`vigem_*`) is NOT thread safe: after startup every call runs under `vigem_lock` (`vigem_release_target` for teardown), and never under `tray_ui_lock` or `active_devices_lock` (unregistering a notification can wait for its callback, which takes both); `remove_device` releases the target after dropping its lock.

## Behavior (fork features)
- Single instance via named mutex `MI-VIGEM`; a second instance exits silently.
- Device arrival/removal (any HID interface change) triggers a normal refresh, whatever the operation type.
- `Reset Gamepads` menu item = `reset_devices`: stop all pads, wait up to 5 s for removal, restart numbering at #1, turn every real pad off by disabling and re-enabling its PARENT devnode (the "Bluetooth HID Device", not the HID-compliant game controller child). Nothing is re-added; pads come back via the normal refresh when switched on. Routine notifications are suppressed while `reattaching`.
- Opening a device: exclusive RW first; on failure disable+enable the devnode and retry exclusive, then shared; if re-enable fails, shared open directly.
- Disabled HID devnodes of the gamepad (`HID\...`, the virtual-side children that got stuck) are re-enabled at every refresh (`hid_enable_disabled_devices`); the parent (`BTHENUM\...`) is never touched there, so a reset or a manual disable of it stays.
- Re-enable: disable, wait for state, then enable with up to 3 attempts and a `CM_Enable_DevNode` fallback; each state change waits up to 10 s (20 ms poll).
- Win11 keeps the HID devnode and the pending read after the pad powers off (Win10 failed the read), so the input thread probes with a feature report `0x20` (current motor values, private buffer) after 300 silent 1 s read timeouts (the pad's idle shutdown is 300 s; earlier probing might keep it awake); a failed probe ends the pad (write error).
- Rumble retries a failed command every 50 ms until it works (no timed safety stop); on stop, vibration-off is retried up to 20 times.
- If ViGEmBus is missing/incompatible, or adding a pad's target fails, the app still runs and lists devices, with an error/warning notification; only pads with `tgt_added` are emulated or removed from the bus.
- The "device count limit" warning is shown once until a pad is removed.

## HidHide
- Optional: if the driver is missing, the app works without hiding. Same if our exe's NT path cannot be resolved: init fails (error notification) and nothing is hidden, since we could not whitelist ourselves.
- On init: whitelist our exe (NT device path) unless HidHide is in inverse mode (then remove it), and activate HidHide.
- Per device: add instance path and its parent (container) path to the blacklist; remove both on unbind.
- On exit: restore HidHide's initial active state; the blacklist/whitelist entries we added are not otherwise restored besides per-device unbind.

## UI
- Tray icon only: menu lists `#N Xiaomi Gamepad (batt. X%)` items (N = XInput slot + 1, taken from the X360 LED number in the ViGEm notification because the slot is assigned after the target is added; until then our own running index, clicking one vibrates that pad for 500 ms), a separator, `Reset Gamepads`, (`Debug`, debug builds only), a separator, `Autostart at Logon` (checked), `GitHub Page`, a separator, `Quit  (ver. x.y.z)`.
- Autostart = HKCU `...\Run` value `My-ViGEm` holding the quoted exe path (`autostart.c`); enabled only if the stored path equals the running exe. State is cached in `autostart_checked`.
- Menu is rebuilt from `active_devices` (`rebuild_tray_menu`) then applied with `tray_update`.
- Notifications are balloon tips (10 s), levels info/warning/error.
- Tray menu uses `-` text as a separator and a `NULL` text terminator.

## Debug / docs
- Protocol knowledge lives in `PROTOCOL.md` (from the author's readme + code); keep it the single reference. Mark each packet confirmed/unknown.
- Debug window is a plain Win32 window (no extra libs) on its own thread, opened from the tray `Debug` item (only built with `MY_VIGEM_DEBUG`, i.e. the CMake Debug config); closing it only hides it (log is kept).
- It shows raw report bytes, the report decoded per `PROTOCOL.md` (unknown bits/bytes are listed), and the event log. The decoder in `debug_win.c` is independent of `mi.c` on purpose, so it shows what the hardware sends, not what the app assumes.
- All `debug_log` output goes to the console (`--debug`) and to the window; use it for anything worth seeing there.
- The raw report hook (`mi_set_raw_cb`) runs on the input thread: keep it cheap, no blocking.
- Raw probing (hex box + `Send feature` / `Send output` / `Get feature`) targets the Pad id typed in the box, or the first gamepad seen reporting. Every raw call is logged with its result and Win32 error. Report `0x22` (calibration, permanent on the pad) asks for confirmation before sending.
- Preset buttons: `Input off/on` (`0x23`; off asks for confirmation) and `Accel on/off` (`0x31`, sensitivity from a hex box, default 08 like the old unused `enable_accel` constant).
