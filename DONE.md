# DONE

## 1. Planning
- [x] 1.1. Reorganize notes into steps
- [x] 1.2. Read the whole project, deduce decisions made in code, fill `DECISIONS.md`
- [x] 1.3. Plan protocol documentation and debug window steps

## 2. Protocol documentation
- [x] 2.1. Write `PROTOCOL.md`: hardware, input report (byte/bit layout checked against `mi.c`), feature/output packets (0x20-0x24, 0x31, 0x04), calibration, quirks; source: user's `readme.txt` + code

## 3. Debug window
- [x] 3.1. Add `Debug` tray menu item between `Refresh` and `Quit` (callback stub)
- [x] 3.2.1. Create `debug_win.c/h`: plain Win32 window on its own thread, toggle/append/free API (not yet in the build)
- [x] 3.2.2. Wire `debug_win` into `CMakeLists.txt` and `main.c` (`debug_cb` calls `debug_win_toggle`, quit calls `debug_win_free`)
- [x] 3.3.1. `mi.c/h`: add `mi_set_raw_cb` debug hook, called with every raw input report before the header check
- [x] 3.3.2. `debug_win.c/h`: add `debug_win_set_report(id, bytes, size)` showing a live hex dump per device (throttled by a timer, not one repaint per report)
- [x] 3.3.3. `main.c`: register `mi_set_raw_cb` to feed `debug_win_set_report`; clear it on exit
- [x] 3.4. Show parsed live state per device: buttons, dpad, sticks, triggers, accel, battery, Mi button (plus unknown bits/bytes)
- [x] 3.5. Show event log: `debug_log` now also feeds the window; added logs for remove, refresh, device change, stop and rumble
- [x] 3.6. Update `DECISIONS.md` for the debug window
