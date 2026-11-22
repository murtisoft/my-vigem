# DONE

## 14. Crashes and corruption
- [x] 14.1. `tray.c` WM_DEVICECHANGE ignores broadcasts that are not device-interface structures (no garbage path read)
- [x] 14.2. `remove_device` releases the ViGEm target (unregisters the notification) before the `active_device` is freed
- [x] 14.3. `hid_hide.c` `hh_lock` guards `hh_ctx` and its lists
- [x] 14.4. Tray menu races
  - [x] 14.4.1. `tray.c` copies the menu items, so freeing the caller's array no longer leaves dangling `dwItemData`
  - [x] 14.4.2. `tray_update` posts a prebuilt menu to the tray thread; an open popup is not destroyed, the click is resolved in the shown menu
- [x] 14.5. Delay thread resets its event before the pulse; the Mi pulse starts on a new press only
- [x] 14.6. `upd_cb` runs under `state_lock`, so the input and delay thread callbacks are serialized and read a consistent state
- [x] 14.7. Quit stops and awaits all pads (`stop_all_devices`) before ViGEm/HidHide teardown
- [x] 14.8. HidHide: failed list reads/writes abort init or roll back instead of overwriting the user's lists

## 15. Smaller bugs
- [x] 15.1. `tray.c` posts `WM_NULL` after `TrackPopupMenu`, so the menu dismisses on an outside click
- [x] 15.2. `mi.c` `mi_gamepad_start` frees events and terminates the never-started suspended threads on failure; allocations and events are checked; the delay thread waits on `stopping_event` for 200 ms instead of a waitable timer
- [x] 15.3. `mi.c` Mi release uses `&= ~`; reports shorter than 21 bytes are skipped
- [x] 15.4. Checked allocations in `main.c` `add_device` (all pad memory is allocated before the pad starts), `hid.c` `hid_open_device`, `hid_enumerate`, `hid_clone_device_info`
- [x] 15.5. `main.c` `add_device` checks the limit (and sets the notified flag) under `active_devices_lock`
- [x] 15.6. Verified in the code; an add failure of the same device is shown once until it is added or gone
- [x] 15.7. `tray.c` `tray_show_notification` copies `nid` under `nid_lock` and bounds the title/text copies
- [x] 15.8. `hid_hide.c` `_hh_get_app_image_name` uses heap buffers, checks `GetModuleFileName` and compares paths case-insensitively
- [x] 15.9. HidHide leftovers decided and recorded in `DECISIONS.md` (crash leftovers accepted, shared parent kept, our whitelist entry removed on exit)
- [x] 15.10. The debug window shows and asks for the internal pad id (`Gamepad id N`, `Pad id:` box); the tray keeps the XInput slot
