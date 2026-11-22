# TODO

## Rules
- **Start of every session: read `DECISIONS.md` first** (durable project choices; follow them). Add new durable choices there, and fix a line there when a step changes it.
- Do ONE step per session, then stop. Move its line to `DONE.md` as `[x]` under the same heading (create the heading there if missing; delete headings left empty here). Read `DONE.md` only if a step needs it.
- Don't refactor finished steps unless the current step requires it.
- No questionnaires; make a sensible choice and record durable ones in `DECISIONS.md`.
- **Step size cap:** touch at most 3 files and about 200 changed lines. If a step needs more, split it here in this file first (new sub-steps), then do only the first one.
- **After each step, reply in under 10 words** (what changed, what is next). No lists, no recaps.
- Update `files.zip` once, at the end.
- Group relevant steps and enumerate them like 12. , substeps get a second or third enumeration: 12.1. 12.2.1. etc.

## Todo

## 14. Crashes and corruption (fix first)
- [ ] 14.1. `tray.c` WM_DEVICECHANGE casts `lparam` to a device-interface struct without checking `dbcc_devicetype`; volume/other broadcasts make `device_change_cb` read a garbage path
- [ ] 14.2. `main.c` `remove_device` frees the `active_device` before `vigem_release_target` unregisters the notification, so `x360_notification_cb` can use freed memory (unregister first)
- [ ] 14.3. `hid_hide.c` `hh_ctx` lists are changed with no lock by the worker (`bind`), input threads (`unbind`) and exit; add an SRWLOCK
- [ ] 14.4. Tray menu races
  - [ ] 14.4.1. `rebuild_tray_menu` frees the old item array before `tray_update` swaps the HMENU, and the old `dwItemData` pointers dangle in between
  - [ ] 14.4.2. `tray_update` runs on worker threads and destroys the menu while the popup is open; a rebuild shifts command IDs, so the click can hit another item (even Quit). Post the update to the tray thread and resolve the click from the menu that was shown
- [ ] 14.5. `mi.c` stuck Mi (GUIDE) press: the delay thread calls `upd_cb`, then `ResetEvent(delay_event)`, which can swallow a new `SetEvent` from a still-held Mi button (`hold_mi_btn` stays TRUE forever); reset the event before clearing the hold
- [ ] 14.6. `mi.c`/`main.c` `upd_cb` runs on the input and the delay thread at once, reads `gp->state` outside `state_lock`, and both fill the shared `tgt_report`; pass a locked copy of the state and serialize the report build
- [ ] 14.7. `main.c` quit frees `active_devices` and disconnects ViGEm while pad threads still run; stop all pads and wait (as `reset_devices` does) first
- [ ] 14.8. `hid_hide.c` a failed list read returns NULL like an empty list, then `init`/`bind` write back a list without the user's entries; skip the write on read failure. Also `set_*` ignore IOCTL errors and `bind` still reports OK

## 15. Smaller bugs
- [ ] 15.1. `tray.c` missing `PostMessage(WM_NULL)` after `TrackPopupMenu` (menu may not dismiss on outside click)
- [ ] 15.2. `mi.c` `mi_gamepad_start` failure path leaks the events and closes still-suspended threads (they never end); malloc/CreateWaitableTimer unchecked
- [ ] 15.3. `mi.c` Mi release uses `buttons ^=` (use `&= ~`); packets are parsed without checking `bytes_read >= 21`, so a short report reuses stale bytes
- [ ] 15.4. Unchecked allocations: `main.c` `add_device` (active_device, tray_text, tray_menu), `hid.c` `hid_open_device`, `hid_enumerate` (instance_path), `hid_clone_device_info`
- [ ] 15.5. `main.c` `add_device` reads `active_device_count` for the limit check without the lock
- [ ] 15.6. Verify: a switched-off pad whose devnode lingers fails the init feature report on every refresh and shows "Error initializing new device" each time; show it once
- [ ] 15.7. `tray.c` `tray_show_notification` copies `nid` unlocked and uses unbounded `_tcscpy` into `szInfoTitle`/`szInfo`
- [ ] 15.8. `hid_hide.c` `_hh_get_app_image_name` uses about 450 KB of stack (7 x 64 KB buffers), skips the `GetModuleFileName` result check and compares paths case-sensitively
- [ ] 15.9. HidHide leftovers: a crash leaves pads in the blacklist; `unbind` also removes a parent entry shared with another interface; our whitelist entry is never removed. Decide and record in `DECISIONS.md`
- [ ] 15.10. The debug Pad box takes the internal gamepad id, the tray shows the XInput slot; show the id in the debug window or accept the slot

## 16. Bloat and cleanup
- [ ] 16.1. `mi.c` unused `enable_accel`/`disable_accel`, redundant `struct mi_gamepad;`; `mi.h` unused `MI_HW_VENDOR_ID`/`MI_HW_PRODUCT_ID`, `mi_gamepad_raw` doc comment sits above `mi_gamepad_describe`; `hid.h` `hid_get_feature_report` comment sits above `hid_describe_device`
- [ ] 16.2. `hid.c` unused `check_vendor_and_product`; `hid_enumerate` and `hid_enable_disabled_devices` duplicate the ID-list loop; `timeout >= 0` is always true for a DWORD; `_hid_toggle_devnode` logs "hid_reenable_device" for power-off too
- [ ] 16.3. `container_instance_path` is really the parent devnode id; rename to `parent_instance_path` (hid.c, hid.h, main.c, hid_hide.c: split per file)
- [ ] 16.4. `utils.c` `_mem_zero`/`_mem_copy` reimplement `memset`/`memmove`; make them macros
- [ ] 16.5. Dead build bits: `#pragma comment(lib/linker)` in 5 files (ignored by MinGW), empty `target_compile_options` with a stale comment, `dbghelp` and `ole32` linked but unused
- [ ] 16.6. `tray.c` dead code: non-blocking `tray_loop` branch, unreachable `WM_QUIT` check, `WM_CLOSE` handler, `DO_TRAY_UNKNOWN`
- [ ] 16.7. `debug_log` always formats, converts to UTF-16 and prints, and `debug_win.c` is compiled into release builds (only the menu item is hidden); compile out or skip the work without `MY_VIGEM_DEBUG`
- [ ] 16.8. `main.c` `device_change_cb` has a `printf` that duplicates `debug_log` and refreshes (two full HID enumerations) for every unrelated HID event; filter by the Mi path. `add_device` repeats the `update_tray_text` code and a double NULL check
- [ ] 16.9. `hid_hide.c` unused IOCTL defines (`WLINVERSE` setters etc.), redundant NULL checks in `hid_hide_free`
