![Co-authored with Claude](https://img.shields.io/badge/Co--authored%20with-Claude-D97757?logo=anthropic&logoColor=white)

# (My) Mi Vigem

XBox360 controller emulation for Xiaomi gamepad. Supports multiple devices and vibration.

## Notable Features of this Fork

- Resolves disabled HID device getting stuck on windows 11
- Resolves turned off gamepads occupying virtual gamepad on windows 11
- Reset Gamepads clears everything
- Resolves infinite rumble with proper command retries, instead of a 2 second safety.
- Only allows a single instance running.

## Requirements

- ViGEm bus driver needs to be installed https://github.com/ViGEm/ViGEmBus/releases

## User interface
![UI](Resources/UIShowcase.webp)

## Xiaomi Gamepad

![Controller](Resources/Bitmaps/MiController.webp)
