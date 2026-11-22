# DONE

## 4. Hardware probing
- [x] 4.1.1. `hid.c/h`: add `hid_get_feature_report` (own buffer, `HidD_GetFeature`)
- [x] 4.1.2. `mi.c/h`: add `mi_gamepad_raw` (send feature/output bytes, get feature report on a live gamepad; logged)
- [x] 4.2. Debug window: hex input box + `Send feature` / `Send output` / `Get feature` buttons, result and error in the log
- [x] 4.3. Debug window: guarded buttons for `0x23` stop/start input and `0x31` accelerometer enable/disable with sensitivity
- [x] 4.4.1. `hid.c/h`: add `hid_describe_device` (attributes, report sizes, button/value caps logged)
- [x] 4.4.2. `mi.c/h`: add `mi_gamepad_describe(gamepad_id)` calling `hid_describe_device` under `gp_lock`
- [x] 4.4.3. Debug window: `HID info` button (row 2) calling `mi_gamepad_describe` for the picked pad
