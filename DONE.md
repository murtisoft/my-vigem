# DONE

## 7. Reset Gamepads
- [x] 7.1. Renamed `Refresh` to `Reset Gamepads` (menu text, logs, `DECISIONS.md`)
- [x] 7.2.1. `hid_power_off_device`: turns a pad off via disable + re-enable of the parent devnode of its HID device (shares `_hid_toggle_devnode` with `hid_reenable_device`)
- [x] 7.2.2. `reset_devices` (replaces `reattach_devices`): stop and clear all pads, turn each real pad off via `hid_power_off_device` (once per parent), do not re-add
- [x] 7.3. `DECISIONS.md` updated (Reset behavior, Autostart method, version define); `MAP.md` was not in the uploaded zip, so it was not touched

## 8. Idle disconnect
- [x] 8.1. Win11 does not remove the virtual pad when the real one powers off (read never fails); input thread now probes the pad (`hid_probe_feature`) after 300 silent read timeouts and stops the pad if the probe fails
