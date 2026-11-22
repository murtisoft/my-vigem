# DONE

## 5. Tray menu basics
- [x] 5.1. Pass CMake version into code (`APP_VERSION_STR` from `PROJECT_VERSION`); Quit text is `Quit  (ver. x.y.z)`
- [x] 5.2. Debug item only in debug builds (`MY_VIGEM_DEBUG`, defined for the CMake Debug config)
- [x] 5.3. `GitHub Page` item above `Quit` (`ShellExecute`)
- [x] 5.4.1. `autostart.c/h`: HKCU `Run` key helpers
- [x] 5.4.2. `Autostart at Logon` item, checkmark cached in `autostart_checked`, click toggles
- [x] 5.5. Separator before `Quit`; menu order finalized

## 6. Gamepad entries
- [x] 6.1.1. Read the XInput slot of each ViGEm pad after it is added (`vigem_target_x360_get_user_index`), stored in `active_device.xinput_slot` (-1 if unknown)
- [x] 6.1.2. Menu entry number = XInput slot + 1 (falls back to the running index)
- [x] 6.2.1. `mi_gamepad_identify`: 500 ms full rumble pulse on its own short thread
- [x] 6.2.2. Pad menu entries call `mi_gamepad_identify` (context = gamepad id)
