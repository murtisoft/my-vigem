#include <stdio.h>
#include <tchar.h>
#include <windows.h>
#include <synchapi.h>

#include "mi.h"
#include "hid.h"
#include "utils.h"

#pragma comment(lib, "kernel32.lib")

#define MI_READ_TIMEOUT 1000
// Win11 keeps the HID devnode (and a pending read) after the pad powers off, so an idle read never fails.
// The pad powers off after 300 s idle, so only after this many silent read timeouts (1 s each) in a row
// probe it with a harmless feature report (probing earlier could keep the pad awake).
#define MI_PROBE_AFTER_TIMEOUTS 300
// A manual power-off (Mi button held) sends one last blank (all neutral) report and then goes silent. So when
// the last report was blank, probe once after this many silent read timeouts (1 s each).
#define MI_PROBE_AFTER_BLANK_TIMEOUTS 2
#define MI_VIBRATION_RETRY_DELAY 50
#define MI_MI_BTN_PULSE_MS 200
#define MI_VIBRATION_OFF_RETRIES 20

// an input report must reach the Mi button byte (index 20), a shorter one would reuse stale bytes
#define MI_INPUT_REPORT_MIN_SIZE 21

#define MI_VIBRATION_HEADER         0
#define MI_VIBRATION_SMALL_MOTOR    1
#define MI_VIBRATION_BIG_MOTOR      2

static const BYTE init_vibration[3] = { 0x20, 0x00, 0x00 };
static const BYTE enable_accel[3] = { 0x31, 0x01, 0x08 };
static const BYTE disable_accel[3] = { 0x31, 0x00, 0x00 };

static const DWORD dpad_map[8] =
{
    MI_BUTTON_UP,
    MI_BUTTON_UP | MI_BUTTON_RIGHT,
    MI_BUTTON_RIGHT,
    MI_BUTTON_RIGHT | MI_BUTTON_DOWN,
    MI_BUTTON_DOWN,
    MI_BUTTON_DOWN | MI_BUTTON_LEFT,
    MI_BUTTON_LEFT,
    MI_BUTTON_LEFT | MI_BUTTON_UP
};

struct mi_gamepad;

struct mi_gamepad
{
    int id;
    struct hid_device *device;
    // guards state and hold_mi_btn and serializes the upd_cb calls of the input and the delay thread
    SRWLOCK state_lock;
    struct mi_state state;
    BOOLEAN hold_mi_btn;
    void (*upd_cb)(int, struct mi_state *);
    void (*stop_cb)(int, BYTE);

    BOOLEAN active;
    HANDLE stopping_event;

    HANDLE in_thread;
    HANDLE out_thread;
    HANDLE out_event;
    HANDLE delay_thread;
    HANDLE delay_event;

    SRWLOCK vibr_lock;
    BYTE small_motor;
    BYTE big_motor;

    struct mi_gamepad *prev;
    struct mi_gamepad *next;
};

static int last_gamepad_id = 0;
static struct mi_gamepad *root_gp = NULL;
static SRWLOCK gp_lock = SRWLOCK_INIT;

static volatile mi_raw_cb_t raw_cb = NULL;

void mi_set_raw_cb(mi_raw_cb_t cb)
{
    raw_cb = cb;
}

static DWORD WINAPI _mi_input_thread_proc(LPVOID lparam)
{
    struct mi_gamepad *gp = (struct mi_gamepad *)lparam;
    INT bytes_read = 0;
    BYTE break_reason = MI_BREAK_REASON_UNKNOWN;
    BOOLEAN last_blank = FALSE;
    BOOLEAN mi_was_down = FALSE;

    while (TRUE)
    {
        int silent_timeouts = 0;
        BOOLEAN probe_failed = FALSE;
        while (gp->active && (bytes_read = hid_get_input_report(gp->device, MI_READ_TIMEOUT)) == 0)
        {
            ++silent_timeouts;
            if (silent_timeouts >= MI_PROBE_AFTER_TIMEOUTS
                || (last_blank && silent_timeouts == MI_PROBE_AFTER_BLANK_TIMEOUTS))
            {
                BYTE probe[3];
                probe[MI_VIBRATION_HEADER] = init_vibration[MI_VIBRATION_HEADER];
                AcquireSRWLockShared(&gp->vibr_lock);
                probe[MI_VIBRATION_SMALL_MOTOR] = gp->small_motor;
                probe[MI_VIBRATION_BIG_MOTOR] = gp->big_motor;
                ReleaseSRWLockShared(&gp->vibr_lock);
                if (!hid_probe_feature(gp->device, probe, sizeof(probe)))
                {
                    probe_failed = TRUE;
                    debug_log("gamepad %d probe failed, pad is gone", gp->id);
                    break;
                }
            }
        }

        if (probe_failed)
        {
            break_reason = MI_BREAK_REASON_WRITE_ERROR;
            break;
        }

        if (!gp->active || bytes_read < 0)
        {
            break_reason = !gp->active ? MI_BREAK_REASON_REQUESTED : MI_BREAK_REASON_READ_ERROR;
            break;
        }

        mi_raw_cb_t raw = raw_cb;
        if (raw != NULL)
        {
            raw(gp->id, gp->device->input_buffer, (DWORD)bytes_read);
        }

        // check packet header
        if (gp->device->input_buffer[0] != 0x04)
        {
            continue;
        }

        // check packet length
        if (bytes_read < MI_INPUT_REPORT_MIN_SIZE)
        {
            continue;
        }

        // blank report: no buttons, centered sticks and d-pad, no triggers, no Mi button (accel and battery ignored)
        {
            const BYTE *b = gp->device->input_buffer;
            last_blank = b[1] == 0 && b[2] == 0 && b[4] == 0x0F
                && b[5] == 0x80 && b[6] == 0x80 && b[7] == 0x80 && b[8] == 0x80
                && b[11] == 0 && b[12] == 0 && b[20] == 0;
        }

        AcquireSRWLockExclusive(&gp->state_lock);

        gp->state.buttons = MI_BUTTON_NONE;

        gp->state.buttons |= (gp->device->input_buffer[1] & (1 << 0)) != 0 ? MI_BUTTON_A : 0;
        gp->state.buttons |= (gp->device->input_buffer[1] & (1 << 1)) != 0 ? MI_BUTTON_B : 0;
        gp->state.buttons |= (gp->device->input_buffer[1] & (1 << 3)) != 0 ? MI_BUTTON_X : 0;
        gp->state.buttons |= (gp->device->input_buffer[1] & (1 << 4)) != 0 ? MI_BUTTON_Y : 0;
        gp->state.buttons |= (gp->device->input_buffer[1] & (1 << 6)) != 0 ? MI_BUTTON_L1 : 0;
        gp->state.buttons |= (gp->device->input_buffer[1] & (1 << 7)) != 0 ? MI_BUTTON_R1 : 0;

        gp->state.buttons |= (gp->device->input_buffer[2] & (1 << 2)) != 0 ? MI_BUTTON_RETURN : 0;
        gp->state.buttons |= (gp->device->input_buffer[2] & (1 << 3)) != 0 ? MI_BUTTON_MENU : 0;
        gp->state.buttons |= (gp->device->input_buffer[2] & (1 << 5)) != 0 ? MI_BUTTON_LS : 0;
        gp->state.buttons |= (gp->device->input_buffer[2] & (1 << 6)) != 0 ? MI_BUTTON_RS : 0;

        gp->state.buttons |= gp->device->input_buffer[4] < 8 ? dpad_map[gp->device->input_buffer[4]] : 0;

        gp->state.left_stick_x = gp->device->input_buffer[5];
        gp->state.left_stick_y = gp->device->input_buffer[6];
        gp->state.right_stick_x = gp->device->input_buffer[7];
        gp->state.right_stick_y = gp->device->input_buffer[8];

        gp->state.l2_trigger = gp->device->input_buffer[11];
        gp->state.r2_trigger = gp->device->input_buffer[12];

        gp->state.accel_x = *(WORD *)(gp->device->input_buffer + 13);
        gp->state.accel_y = *(WORD *)(gp->device->input_buffer + 15);
        gp->state.accel_z = *(WORD *)(gp->device->input_buffer + 17);

        gp->state.battery = gp->device->input_buffer[19];

        // pulse on a new press only, a held Mi button must not start the pulse again and again
        BOOLEAN mi_down = gp->device->input_buffer[20] > 0;
        if (!gp->hold_mi_btn && mi_down && !mi_was_down)
        {
            gp->hold_mi_btn = TRUE;
            SetEvent(gp->delay_event);
        }
        mi_was_down = mi_down;
        gp->state.buttons |= gp->hold_mi_btn ? MI_BUTTON_MI_BTN : 0;

        // the callback runs under the lock: the state cannot change meanwhile and the callbacks of the
        // input and the delay thread reach the virtual pad in order
        gp->upd_cb(gp->id, &gp->state);

        ReleaseSRWLockExclusive(&gp->state_lock);
    }

    gp->active = FALSE;
    SetEvent(gp->stopping_event);
    HANDLE wait_threads[2] = { gp->out_thread, gp->delay_thread };
    WaitForMultipleObjects(2, wait_threads, TRUE, INFINITE);

    AcquireSRWLockExclusive(&gp_lock);
    if (gp->prev == NULL)
    {
        root_gp = gp->next;
    }
    else
    {
        gp->prev->next = gp->next;
        if (gp->next != NULL)
        {
            gp->next->prev = gp->prev;
        }
    }
    ReleaseSRWLockExclusive(&gp_lock);

    CloseHandle(gp->stopping_event);
    CloseHandle(gp->out_event);
    CloseHandle(gp->delay_event);
    CloseHandle(gp->in_thread);
    CloseHandle(gp->out_thread);
    CloseHandle(gp->delay_thread);
    gp->stop_cb(gp->id, break_reason);
    free(gp);

    return 0;
}

static DWORD WINAPI _mi_output_thread_proc(LPVOID lparam)
{
    struct mi_gamepad *gp = (struct mi_gamepad *)lparam;

    HANDLE wait_events[2] = { gp->stopping_event, gp->out_event };
    DWORD timeout = INFINITE;
    BOOLEAN fail_logged = FALSE;
    // last successfully sent vibration state
    BYTE vibration[3];
    vibration[MI_VIBRATION_HEADER] = init_vibration[MI_VIBRATION_HEADER];
    vibration[MI_VIBRATION_SMALL_MOTOR] = gp->small_motor;
    vibration[MI_VIBRATION_BIG_MOTOR] = gp->big_motor;

    while (gp->active)
    {
        WaitForMultipleObjects(2, wait_events, FALSE, timeout);
        if (!gp->active)
        {
            break;
        }
        ResetEvent(gp->out_event);

        BYTE desired[3];
        desired[MI_VIBRATION_HEADER] = init_vibration[MI_VIBRATION_HEADER];
        AcquireSRWLockShared(&gp->vibr_lock);
        desired[MI_VIBRATION_SMALL_MOTOR] = gp->small_motor;
        desired[MI_VIBRATION_BIG_MOTOR] = gp->big_motor;
        ReleaseSRWLockShared(&gp->vibr_lock);

        if (desired[MI_VIBRATION_SMALL_MOTOR] != vibration[MI_VIBRATION_SMALL_MOTOR]
            || desired[MI_VIBRATION_BIG_MOTOR] != vibration[MI_VIBRATION_BIG_MOTOR])
        {
            if (hid_send_feature_report(gp->device, desired, sizeof(desired)) > 0)
            {
                vibration[MI_VIBRATION_SMALL_MOTOR] = desired[MI_VIBRATION_SMALL_MOTOR];
                vibration[MI_VIBRATION_BIG_MOTOR] = desired[MI_VIBRATION_BIG_MOTOR];
                timeout = INFINITE;
                fail_logged = FALSE;
                debug_log("gamepad %d rumble: small=%u big=%u", gp->id, desired[MI_VIBRATION_SMALL_MOTOR], desired[MI_VIBRATION_BIG_MOTOR]);
            }
            else
            {
                // command failed, retry until it succeeds
                timeout = MI_VIBRATION_RETRY_DELAY;
                if (!fail_logged)
                {
                    fail_logged = TRUE;
                    debug_log("gamepad %d rumble: send failed, retrying", gp->id);
                }
            }
        }
        else
        {
            timeout = INFINITE;
        }
    }

    // turn off vibration (bounded retries, the device may be gone already)
    for (int i = 0; i < MI_VIBRATION_OFF_RETRIES; i++)
    {
        if (hid_send_feature_report(gp->device, init_vibration, sizeof(init_vibration)) > 0)
        {
            break;
        }
        Sleep(MI_VIBRATION_RETRY_DELAY);
    }

    return 0;
}

static DWORD WINAPI _mi_delay_thread_proc(LPVOID lparam)
{
    struct mi_gamepad *gp = (struct mi_gamepad *)lparam;

    HANDLE wait_events[2] = { gp->stopping_event, gp->delay_event };

    while (gp->active)
    {
        WaitForMultipleObjects(2, wait_events, FALSE, INFINITE);
        // reset before the pulse: a press that arrives meanwhile sets the event again and must not be lost
        ResetEvent(gp->delay_event);
        if (gp->hold_mi_btn)
        {
            // the pulse length; a stop request ends the wait early
            WaitForSingleObject(gp->stopping_event, MI_MI_BTN_PULSE_MS);

            AcquireSRWLockExclusive(&gp->state_lock);
            gp->hold_mi_btn = FALSE;
            gp->state.buttons &= ~MI_BUTTON_MI_BTN;
            gp->upd_cb(gp->id, &gp->state);
            ReleaseSRWLockExclusive(&gp->state_lock);
        }
    }

    return 0;
}

// Frees a gamepad whose threads were never started. Threads that were created suspended never ran, so they
// are terminated (closing the handle alone would leave them suspended forever).
static void _mi_gamepad_discard(struct mi_gamepad *gp)
{
    HANDLE threads[3] = { gp->in_thread, gp->out_thread, gp->delay_thread };
    for (int i = 0; i < 3; i++)
    {
        if (threads[i] != NULL)
        {
            TerminateThread(threads[i], 0);
            CloseHandle(threads[i]);
        }
    }
    HANDLE events[3] = { gp->stopping_event, gp->out_event, gp->delay_event };
    for (int i = 0; i < 3; i++)
    {
        if (events[i] != NULL)
        {
            CloseHandle(events[i]);
        }
    }
    free(gp);
}

int mi_gamepad_start(struct hid_device *device, void (*upd_cb)(int, struct mi_state *), void (*stop_cb)(int, BYTE))
{
    if (hid_send_feature_report(device, init_vibration, sizeof(init_vibration)) <= 0)
    {
        return -1;
    }

    SECURITY_ATTRIBUTES security =
    {
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = NULL,
        .bInheritHandle = TRUE
    };

    // zeroed, so a failed start can free whatever was created so far
    struct mi_gamepad *gp = (struct mi_gamepad *)calloc(1, sizeof(struct mi_gamepad));
    if (gp == NULL)
    {
        return -1;
    }
    gp->id = ++last_gamepad_id;
    gp->device = device;
    InitializeSRWLock(&gp->state_lock);
    gp->hold_mi_btn = FALSE;
    gp->upd_cb = upd_cb;
    gp->stop_cb = stop_cb;
    gp->active = TRUE;
    gp->stopping_event = CreateEvent(&security, TRUE, FALSE, NULL);
    gp->out_event = CreateEvent(&security, TRUE, FALSE, NULL);
    gp->delay_event = CreateEvent(&security, TRUE, FALSE, NULL);
    InitializeSRWLock(&gp->vibr_lock);
    gp->small_motor = init_vibration[MI_VIBRATION_SMALL_MOTOR];
    gp->big_motor = init_vibration[MI_VIBRATION_BIG_MOTOR];
    gp->prev = NULL;
    gp->next = NULL;
    if (gp->stopping_event == NULL || gp->out_event == NULL || gp->delay_event == NULL)
    {
        _mi_gamepad_discard(gp);
        return -1;
    }

    gp->in_thread = CreateThread(&security, 0, _mi_input_thread_proc, gp, CREATE_SUSPENDED, NULL);
    gp->out_thread = CreateThread(&security, 0, _mi_output_thread_proc, gp, CREATE_SUSPENDED, NULL);
    gp->delay_thread = CreateThread(&security, 0, _mi_delay_thread_proc, gp, CREATE_SUSPENDED, NULL);
    if (gp->in_thread == NULL || gp->out_thread == NULL || gp->delay_thread == NULL)
    {
        _mi_gamepad_discard(gp);
        return -1;
    }

    AcquireSRWLockExclusive(&gp_lock);
    if (root_gp == NULL)
    {
        root_gp = gp;
        gp->prev = NULL;
    }
    else
    {
        struct mi_gamepad *cur_gp = root_gp;
        while (cur_gp->next != NULL)
        {
            cur_gp = cur_gp->next;
        }
        cur_gp->next = gp;
        gp->prev = cur_gp;
    }
    ReleaseSRWLockExclusive(&gp_lock);

    ResumeThread(gp->in_thread);
    ResumeThread(gp->out_thread);
    ResumeThread(gp->delay_thread);
    return gp->id;
}

void mi_gamepad_set_vibration(int gamepad_id, BYTE small_motor, BYTE big_motor)
{
    AcquireSRWLockShared(&gp_lock);
    struct mi_gamepad *cur_gp = root_gp;
    while (cur_gp != NULL)
    {
        if (cur_gp->id == gamepad_id)
        {
            break;
        }
        cur_gp = cur_gp->next;
    }
    ReleaseSRWLockShared(&gp_lock);
    if (cur_gp != NULL)
    {
        AcquireSRWLockExclusive(&cur_gp->vibr_lock);
        cur_gp->small_motor = small_motor;
        cur_gp->big_motor = big_motor;
        ReleaseSRWLockExclusive(&cur_gp->vibr_lock);
        SetEvent(cur_gp->out_event);
    }
}

#define MI_IDENTIFY_DURATION_MS 500

static DWORD WINAPI _mi_identify_thread_proc(LPVOID lparam)
{
    int gamepad_id = (int)(INT_PTR)lparam;
    mi_gamepad_set_vibration(gamepad_id, 0xFF, 0xFF);
    Sleep(MI_IDENTIFY_DURATION_MS);
    mi_gamepad_set_vibration(gamepad_id, 0x00, 0x00);
    return 0;
}

void mi_gamepad_identify(int gamepad_id)
{
    HANDLE thread = CreateThread(NULL, 0, _mi_identify_thread_proc, (LPVOID)(INT_PTR)gamepad_id, 0, NULL);
    if (thread != NULL)
    {
        CloseHandle(thread);
    }
    debug_log("gamepad %d identify", gamepad_id);
}

static void _mi_hex(char *dst, size_t dst_size, const BYTE *data, size_t length)
{
    size_t pos = 0;
    dst[0] = '\0';
    for (size_t i = 0; i < length && pos + 4 < dst_size; i++)
    {
        pos += snprintf(dst + pos, dst_size - pos, "%02X ", data[i]);
    }
}

INT mi_gamepad_raw(int gamepad_id, BYTE kind, const BYTE *in, size_t in_len, BYTE *out, size_t out_size, DWORD *error)
{
    static const char *kind_names[] = { "send feature", "send output", "get feature" };

    if (kind > MI_RAW_GET_FEATURE || in == NULL || in_len == 0
        || (kind == MI_RAW_GET_FEATURE && (out == NULL || out_size == 0)))
    {
        if (error != NULL)
        {
            *error = ERROR_INVALID_PARAMETER;
        }
        return -1;
    }

    INT result = -2;
    DWORD err = ERROR_NOT_FOUND;

    // keep gp_lock shared for the whole call so the gamepad cannot be torn down meanwhile.
    // note: send feature shares the device feature buffer with the rumble thread, fine for manual probing.
    AcquireSRWLockShared(&gp_lock);
    struct mi_gamepad *cur_gp = root_gp;
    while (cur_gp != NULL && cur_gp->id != gamepad_id)
    {
        cur_gp = cur_gp->next;
    }
    if (cur_gp != NULL)
    {
        SetLastError(0);
        switch (kind)
        {
        case MI_RAW_SEND_FEATURE:
            result = hid_send_feature_report(cur_gp->device, in, in_len);
            break;
        case MI_RAW_SEND_OUTPUT:
            result = hid_send_output_report(cur_gp->device, in, in_len);
            break;
        default:
            result = hid_get_feature_report(cur_gp->device, in[0], out, out_size);
            break;
        }
        err = result < 0 ? GetLastError() : 0;
    }
    ReleaseSRWLockShared(&gp_lock);

    char hex[256];
    _mi_hex(hex, sizeof(hex), in, in_len);
    if (result >= 0)
    {
        debug_log("gamepad %d raw %s [%s] -> %d bytes", gamepad_id, kind_names[kind], hex, (int)result);
        if (kind == MI_RAW_GET_FEATURE)
        {
            _mi_hex(hex, sizeof(hex), out, (size_t)result);
            debug_log("  reply: %s", hex);
        }
    }
    else
    {
        debug_log("gamepad %d raw %s [%s] failed, result=%d error=%lu", gamepad_id, kind_names[kind], hex, (int)result, err);
    }

    if (error != NULL)
    {
        *error = err;
    }
    return result;
}

BOOLEAN mi_gamepad_describe(int gamepad_id)
{
    BOOLEAN found = FALSE;

    // gp_lock stays shared during the call so the gamepad cannot be torn down meanwhile
    AcquireSRWLockShared(&gp_lock);
    struct mi_gamepad *cur_gp = root_gp;
    while (cur_gp != NULL && cur_gp->id != gamepad_id)
    {
        cur_gp = cur_gp->next;
    }
    if (cur_gp != NULL)
    {
        found = TRUE;
        hid_describe_device(cur_gp->device);
    }
    ReleaseSRWLockShared(&gp_lock);
    return found;
}

int mi_gamepad_list(int *ids, int max)
{
    int count = 0;
    AcquireSRWLockShared(&gp_lock);
    for (struct mi_gamepad *cur_gp = root_gp; cur_gp != NULL && count < max; cur_gp = cur_gp->next)
    {
        ids[count++] = cur_gp->id;
    }
    ReleaseSRWLockShared(&gp_lock);
    return count;
}

void mi_gamepad_stop(int gamepad_id)
{
    AcquireSRWLockShared(&gp_lock);
    struct mi_gamepad *cur_gp = root_gp;
    while (cur_gp != NULL)
    {
        if (cur_gp->id == gamepad_id)
        {
            cur_gp->active = FALSE;
            CancelIoEx(cur_gp->device->handle, &cur_gp->device->input_ol);
            break;
        }
        cur_gp = cur_gp->next;
    }
    ReleaseSRWLockShared(&gp_lock);
}
