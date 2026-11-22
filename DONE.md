# DONE

## 9. Tray threading
- [x] 9.1. `refresh_tray` (one lock) now does every menu rebuild and `tray_update`; removed the cross-thread `SendMessage(WM_INITMENUPOPUP)` from `tray_update`
- [x] 9.2. Menu text is rebuilt inside the same lock, so no freed text is read
- [x] 9.3. `tray_loop` no longer reads an uninitialized message

## 10. HidHide
- [x] 10.1. `_hh_get_app_image_name`: terminate `stripped_volume_name`; if the image name is NULL, `hid_hide_init` fails cleanly (`hh_ctx` NULL, no hiding)
- [x] 10.2. `_hh_get_black_list` / `_hh_get_white_list`: no leak when the second call fails, `malloc` checked

## 11. Devices
- [x] 11.1. `hid_send_output_report` uses its own event (no wait on the shared file handle)
- [x] 11.2. `vigem_target_add` result checked (`tgt_added`), `tgt_report` zeroed, remove/update only for added targets
- [x] 11.3. "Device count limit reached" shown once until a pad is removed
- [x] 11.4. `BusReportedDeviceDesc` size query fixed (`ERROR_INSUFFICIENT_BUFFER`)
- [x] 11.5. `_tcsistr` uses `towlower` under UNICODE

## 12. ViGEm thread safety
- [x] 12.1. All `vigem_*` calls after startup run under `vigem_lock`; target teardown (`vigem_release_target`) happens after `active_devices_lock` is dropped, in `remove_device` and on quit
