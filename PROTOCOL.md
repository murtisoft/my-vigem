# Xiaomi Gamepad protocol (VID 0x2717, PID 0x3144)

Sources: the original author's reverse-engineering notes (trial and error, plus a decompiled `sensors.gxbaby.so` from a Xiaomi TV box) and this project's code.
Status tags: **[code]** = implemented and used in `mi.c` / `main.c`; **[notes]** = reported by the notes only, never exercised here; **[unknown]** = guessed or unexplained.

## 1. Hardware
- Same layout as an Xbox 360 pad: A, B, X, Y, L1, R1, both stick clicks, Start (Menu), Back (Return), D-pad, 2 sticks, 2 analog triggers.
- Extra: a Mi button on the front, a 3-axis accelerometer, 2 rumble motors (variable strength), a battery level.
- Windows HID instance ID contains `VID&00022717_PID&3144` (Win10+) or `VID_2717&PID_3144` (Win7).
- Only two report kinds are valid per the HID descriptor: the input report and the rumble feature report. More feature packets exist but are undocumented by Xiaomi.

## 2. Input report (device -> host), 21 bytes **[code]**
Header byte `0x04`; the app drops any report whose first byte differs.

| Byte | Content |
|---|---|
| 0 | Report ID `0x04` |
| 1 | Buttons 1, one bit each (table below) |
| 2 | Buttons 2, one bit each (table below) |
| 3 | Always 0 |
| 4 | D-pad value (table below) |
| 5 | Left stick X (0x00-0xFF, centre 0x80) |
| 6 | Left stick Y (0x00-0xFF, centre 0x80, **up = low value**) |
| 7 | Right stick X |
| 8 | Right stick Y (up = low value) |
| 9-10 | Always 0 **[notes]** |
| 11 | Left trigger (L2), 0x00-0xFF |
| 12 | Right trigger (R2), 0x00-0xFF |
| 13-14 | Accelerometer X, 16-bit little endian |
| 15-16 | Accelerometer Y |
| 17-18 | Accelerometer Z |
| 19 | Battery level (the app shows it as a percent) |
| 20 | Mi button, non-zero while pressed |

Stick Y inversion: the app inverts Y when mapping to XInput, so the raw "up" is a small byte.

### Button bits
| Byte 1 bit | Button | | Byte 2 bit | Button |
|---|---|---|---|---|
| 0 | A | | 2 | Back / Return |
| 1 | B | | 3 | Start / Menu |
| 2 | unused | | 5 | Left stick click |
| 3 | X | | 6 | Right stick click |
| 4 | Y | | 0, 1, 4, 7 | unused |
| 5 | unused | | | |
| 6 | L1 | | | |
| 7 | R1 | | | |

Unused bits are not read by the code; the notes list 11 buttons plus the Mi button, which matches the set above.

### D-pad (byte 4)
Value is a clockwise index from up; any value of 8 or more means released (the code ignores it).

| Value | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| Direction | Up | Up+Right | Right | Right+Down | Down | Down+Left | Left | Left+Up |

### Accelerometer
- Notes: signed little endian, 2 bytes per axis. The code reads each axis as an unsigned `WORD` and does not use it.
- Only reported after it is enabled with packet `0x31` (below) **[notes]**. The notes say it works nowhere on a PC except with the Xiaomi TV boxes or via this packet.

### Mi button
The report only holds it while pressed. The app turns each press into a 200 ms pulse on the virtual Xbox GUIDE button so short taps are not lost.

## 3. Output report: rumble (host -> device) **[notes]**
`write()` of 3 bytes: `[0x20][small][big]`, each strength 0x00-0xFF.
- `[0x20][0x00][0x00]` does not fully stop the motors; `[0x20][0x01][0x01]` nearly does (faint sound).
- Advantage over the feature version: it does not interrupt input reports.
- The app does **not** use this; it uses the feature variant (section 4.1).

## 4. Feature reports (host -> device)
### 4.1 Rumble, `0x20`, 3 bytes **[code]**
`[0x20][small motor 0-255][big motor 0-255]`
- The only packet Xiaomi documents. Sent at startup as `[0x20][0][0]` (also acts as an init check; failure aborts the gamepad start), on every change, and as an off command on stop (up to 20 tries, 50 ms apart).
- A failed send is retried every 50 ms until it succeeds.
- Xbox 360 mapping: large motor -> Xiaomi **big**, small motor -> Xiaomi **small** (the callback swaps the order into the packet).
- Side effect from the notes: sending a feature report can pause input from the controller.

### 4.2 Unknown, `0x21` **[unknown]**
`[0x21][...]`. Sending it once stopped controller input; the values used were not recorded.

### 4.3 Calibration, `0x22`, 24+ bytes **[notes]**
`[0x22][enable mask][LJoy 8][RJoy 8][LTrig 3][RTrig 3]`
- Enable mask: bit 0 = section 1 (left stick), bit 1 = section 2 (right stick), bit 2 = left trigger, bit 3 = right trigger (the notes describe one bit per section, bit 0 being section 1).
- Stick section (8 bytes) = X axis 4 bytes, then Y axis 4 bytes. Each axis: `[lower bound][lower middle][higher middle][higher bound]`.
  - Bounds set the min and max output values. Anything between the two middle values reports as centred (0x80).
  - Neutral starting point: `[0x00][0x7F][0x7F][0xFF]` on all four axes, then tweak the bounds until each axis spans 0x00-0xFF with 0x80 at rest.
  - On the second axis the bounds are inverted: if it only reaches 0xFF at the top, lower the higher bound, not the lower.
- Trigger section (3 bytes): `[lower bound][?][higher bound]`. The middle byte has no visible effect; the author used 0xFF. Start with `[0x00][0xFF][0xFF]` and tweak.
- **Persistent:** values are saved in the controller. A wrong packet (author sent all 0xFF) leaves garbage input until fixed by hand. Do not send this casually.

### 4.4 Input stop/start, `0x23`, 2 bytes **[notes]**
- `[0x23][0x01]` stops all input except the Mi button.
- `[0x23][0x00]` restarts input.

### 4.5 Unpair, `0x24`, 2 bytes **[notes]**
`[0x24][?]` unpairs and closes the controller. This is the closest thing to a power-off command; the app never sends it.

### 4.6 Accelerometer, `0x31`, 3 bytes **[notes]**
- Enable: `[0x31][0x01][sensitivity]`. Lower sensitivity value = less movement needed to trigger a report.
- Disable: `[0x31][0x00][0x00]`.
- On Linux `HIDIOCSFEATURE` reported failure for this packet even though it worked.

### 4.7 Unknown, `0x04`, 21 bytes **[unknown]**
A feature report that looks like the input report, seen in the decompiled library. Purpose unknown, untested.

## 5. Quirks summary
- The app talks to the pad through exclusive HID access; if opening fails it disables and re-enables the device node, then falls back to shared access.
- Input reads use a 1 s timeout and loop; any other header than `0x04` is ignored.
- Rumble through feature reports can stall input; output reports avoid that.
- Calibration writes are permanent, accelerometer enable is not (it was only observed active, not documented as saved).
