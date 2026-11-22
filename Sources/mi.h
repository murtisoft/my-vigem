#ifndef MI_H
#define MI_H

#include <wtypes.h>

struct hid_device;

#define MI_HW_VENDOR_ID 0x2717
#define MI_HW_PRODUCT_ID 0x3144

#define MI_HW_FILTER_WIN10  TEXT("VID&00022717_PID&3144")
#define MI_HW_FILTER_WIN7   TEXT("VID_2717&PID_3144")

#define MI_BUTTON_NONE      0x00000000
#define MI_BUTTON_A         0x00000001
#define MI_BUTTON_B         0x00000002
#define MI_BUTTON_X         0x00000004
#define MI_BUTTON_Y         0x00000008
#define MI_BUTTON_L1        0x00000010
#define MI_BUTTON_R1        0x00000020
#define MI_BUTTON_LS        0x00000040
#define MI_BUTTON_RS        0x00000080
#define MI_BUTTON_UP        0x00000100
#define MI_BUTTON_DOWN      0x00000200
#define MI_BUTTON_LEFT      0x00000400
#define MI_BUTTON_RIGHT     0x00000800
#define MI_BUTTON_RETURN    0x00001000
#define MI_BUTTON_MENU      0x00002000
#define MI_BUTTON_MI_BTN    0x00004000

#define MI_BREAK_REASON_UNKNOWN        0x0000
#define MI_BREAK_REASON_REQUESTED      0x0001
#define MI_BREAK_REASON_INIT_ERROR     0x0002
#define MI_BREAK_REASON_READ_ERROR     0x0004
#define MI_BREAK_REASON_WRITE_ERROR    0x0008

struct mi_state
{
    DWORD buttons;
    
    BYTE left_stick_x;
    BYTE left_stick_y;
    BYTE right_stick_x;
    BYTE right_stick_y;
    
    BYTE l2_trigger;
    BYTE r2_trigger;
    
    WORD accel_x;
    WORD accel_y;
    WORD accel_z;

    BYTE battery;
};

typedef void (*mi_raw_cb_t)(int gamepad_id, const BYTE *report, DWORD size);

// Debug hook: receives every raw input report (any header) on the gamepad's input thread. NULL disables it.
void mi_set_raw_cb(mi_raw_cb_t cb);

// upd_cb runs under the pad's state lock (serialized, on the input or the delay thread): keep it short, do not keep the state pointer.
int mi_gamepad_start(struct hid_device *device, void (*upd_cb)(int, struct mi_state *), void (*stop_cb)(int, BYTE));
void mi_gamepad_set_vibration(int gamepad_id, BYTE small_motor, BYTE big_motor);
void mi_gamepad_stop(int gamepad_id);
// Briefly vibrates the pad (both motors, non-blocking) so the user can tell which one it is.
void mi_gamepad_identify(int gamepad_id);

#define MI_RAW_SEND_FEATURE 0
#define MI_RAW_SEND_OUTPUT  1
#define MI_RAW_GET_FEATURE  2

// Debug/probing: send arbitrary bytes (feature or output report) or read a feature report (ID = in[0], reply in out)
// on a live gamepad. Returns bytes transferred, -1 on failure, -2 if the gamepad does not exist.
// *error (optional) receives the Win32 error, 0 on success. The call is logged with debug_log.
// Debug: log the HID descriptor summary of a live gamepad. Returns FALSE if the gamepad does not exist.
BOOLEAN mi_gamepad_describe(int gamepad_id);

INT mi_gamepad_raw(int gamepad_id, BYTE kind, const BYTE *in, size_t in_len, BYTE *out, size_t out_size, DWORD *error);

#endif /* MI_H */
