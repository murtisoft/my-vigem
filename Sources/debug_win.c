#include "debug_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>

#include "utils.h"

#define DEBUG_WIN_CLASS TEXT("MiVigemDebugWindow")
#define DEBUG_WIN_TITLE TEXT("Xiaomi Gamepad - Debug")
#define DEBUG_WIN_MAX_CHARS 60000

#define WM_DEBUG_APPEND (WM_APP + 1)
#define WM_DEBUG_TOGGLE (WM_APP + 2)
#define WM_DEBUG_QUIT   (WM_APP + 3)

#define DEBUG_WIN_MAX_SLOTS 8
#define DEBUG_WIN_MAX_REPORT 64
#define DEBUG_WIN_LIVE_HEIGHT 260
#define DEBUG_WIN_SLOT_TEXT 1200
#define DEBUG_WIN_TIMER_ID 1
#define DEBUG_WIN_TIMER_MS 100
#define DEBUG_WIN_STALE_MS 2000

struct report_slot
{
    BOOLEAN used;
    int gamepad_id;
    DWORD size;
    BYTE data[DEBUG_WIN_MAX_REPORT];
    DWORD tick;
};

static HWND debug_hwnd = NULL;
static HWND debug_edit = NULL;
static HANDLE debug_thread = NULL;
static HANDLE debug_ready_event = NULL;
static HWND debug_live_edit = NULL;

static struct report_slot report_slots[DEBUG_WIN_MAX_SLOTS];
static SRWLOCK report_lock = SRWLOCK_INIT;
static BOOLEAN report_dirty = FALSE;

static void _debug_win_append_text(LPTSTR text)
{
    int length = GetWindowTextLength(debug_edit);
    if (length > DEBUG_WIN_MAX_CHARS)
    {
        // drop the older half of the log
        SendMessage(debug_edit, EM_SETSEL, 0, length / 2);
        SendMessage(debug_edit, EM_REPLACESEL, FALSE, (LPARAM)TEXT(""));
        length = GetWindowTextLength(debug_edit);
    }
    SendMessage(debug_edit, EM_SETSEL, length, length);
    SendMessage(debug_edit, EM_REPLACESEL, FALSE, (LPARAM)text);
}

static size_t _debug_win_append_str(TCHAR *out, LPCTSTR text)
{
    size_t length = _tcslen(text);
    _mem_copy(out, text, (length + 1) * sizeof(TCHAR));
    return length;
}

// Text for one gamepad: hex dump plus the report decoded as in PROTOCOL.md. Unknown bits/bytes are listed to spot new behavior.
static size_t _debug_win_format_slot(const struct report_slot *slot, TCHAR *out)
{
    static const LPCTSTR dpad_names[8] =
    {
        TEXT("Up"), TEXT("Up+Right"), TEXT("Right"), TEXT("Right+Down"),
        TEXT("Down"), TEXT("Down+Left"), TEXT("Left"), TEXT("Left+Up")
    };
    static const struct { int byte; int bit; LPCTSTR name; } buttons[] =
    {
        { 1, 0, TEXT("A") }, { 1, 1, TEXT("B") }, { 1, 3, TEXT("X") }, { 1, 4, TEXT("Y") },
        { 1, 6, TEXT("L1") }, { 1, 7, TEXT("R1") },
        { 2, 2, TEXT("Back") }, { 2, 3, TEXT("Start") }, { 2, 5, TEXT("LS") }, { 2, 6, TEXT("RS") }
    };
    static const struct { int byte; int bit; } unknown_bits[] =
    {
        { 1, 2 }, { 1, 5 }, { 2, 0 }, { 2, 1 }, { 2, 4 }, { 2, 7 }
    };
    static const int unknown_bytes[] = { 3, 9, 10 };

    const BYTE *d = slot->data;
    size_t length = 0;

    length += _stprintf(out + length, TEXT("Gamepad %d, %lu bytes:\r\n"), slot->gamepad_id, slot->size);
    for (DWORD b = 0; b < slot->size; b++)
    {
        length += _stprintf(out + length, TEXT("%02X "), d[b]);
    }
    length += _stprintf(out + length, TEXT("\r\n"));

    if (slot->size < 21 || d[0] != 0x04)
    {
        length += _stprintf(out + length, TEXT("  Not an input report (header 0x%02X)\r\n\r\n"), d[0]);
        return length;
    }

    length += _debug_win_append_str(out + length, TEXT("  Buttons:"));
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
    {
        if ((d[buttons[i].byte] & (1 << buttons[i].bit)) != 0)
        {
            length += _debug_win_append_str(out + length, TEXT(" "));
            length += _debug_win_append_str(out + length, buttons[i].name);
        }
    }
    if (d[20] != 0)
    {
        length += _debug_win_append_str(out + length, TEXT(" Mi"));
    }
    length += _debug_win_append_str(out + length, TEXT("\r\n  Dpad: "));
    if (d[4] < 8)
    {
        length += _debug_win_append_str(out + length, dpad_names[d[4]]);
    }
    else
    {
        length += _stprintf(out + length, TEXT("none (0x%02X)"), d[4]);
    }

    length += _stprintf(out + length, TEXT("\r\n  Sticks L %3d,%3d  R %3d,%3d   Triggers L %3d  R %3d\r\n"),
                        d[5], d[6], d[7], d[8], d[11], d[12]);
    length += _stprintf(out + length, TEXT("  Accel %6d %6d %6d   Battery %d\r\n"),
                        (SHORT)(d[13] | (d[14] << 8)), (SHORT)(d[15] | (d[16] << 8)), (SHORT)(d[17] | (d[18] << 8)), d[19]);

    length += _debug_win_append_str(out + length, TEXT("  Unknown:"));
    BOOLEAN any_unknown = FALSE;
    for (size_t i = 0; i < sizeof(unknown_bits) / sizeof(unknown_bits[0]); i++)
    {
        if ((d[unknown_bits[i].byte] & (1 << unknown_bits[i].bit)) != 0)
        {
            length += _stprintf(out + length, TEXT(" bit%d.%d"), unknown_bits[i].byte, unknown_bits[i].bit);
            any_unknown = TRUE;
        }
    }
    for (size_t i = 0; i < sizeof(unknown_bytes) / sizeof(unknown_bytes[0]); i++)
    {
        if (d[unknown_bytes[i]] != 0)
        {
            length += _stprintf(out + length, TEXT(" b%d=%02X"), unknown_bytes[i], d[unknown_bytes[i]]);
            any_unknown = TRUE;
        }
    }
    for (DWORD b = 21; b < slot->size; b++)
    {
        if (d[b] != 0)
        {
            length += _stprintf(out + length, TEXT(" b%lu=%02X"), b, d[b]);
            any_unknown = TRUE;
        }
    }
    if (!any_unknown)
    {
        length += _debug_win_append_str(out + length, TEXT(" none"));
    }
    length += _debug_win_append_str(out + length, TEXT("\r\n\r\n"));
    return length;
}

static void _debug_win_refresh_live(void)
{
    TCHAR text[DEBUG_WIN_MAX_SLOTS * DEBUG_WIN_SLOT_TEXT];
    size_t length = 0;
    DWORD now = GetTickCount();

    text[0] = TEXT('\0');

    AcquireSRWLockExclusive(&report_lock);
    for (int i = 0; i < DEBUG_WIN_MAX_SLOTS; i++)
    {
        if (report_slots[i].used && now - report_slots[i].tick > DEBUG_WIN_STALE_MS)
        {
            report_slots[i].used = FALSE;
            report_dirty = TRUE;
        }
    }
    if (!report_dirty)
    {
        ReleaseSRWLockExclusive(&report_lock);
        return;
    }
    report_dirty = FALSE;

    for (int i = 0; i < DEBUG_WIN_MAX_SLOTS; i++)
    {
        if (!report_slots[i].used)
        {
            continue;
        }
        length += _debug_win_format_slot(&report_slots[i], text + length);
    }
    ReleaseSRWLockExclusive(&report_lock);

    if (length == 0)
    {
        _tcscpy(text, TEXT("No input reports (no device, or it is idle)."));
    }
    SetWindowText(debug_live_edit, text);
}

static LRESULT CALLBACK _debug_win_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CREATE:
        debug_live_edit = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
                                         WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                                         0, 0, 0, 0, hwnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessage(debug_live_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
        debug_edit = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), TEXT(""),
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                    0, 0, 0, 0, hwnd, NULL, GetModuleHandle(NULL), NULL);
        SendMessage(debug_edit, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
        SendMessage(debug_edit, EM_SETLIMITTEXT, 0, 0);
        SetTimer(hwnd, DEBUG_WIN_TIMER_ID, DEBUG_WIN_TIMER_MS, NULL);
        return 0;

    case WM_SIZE:
    {
        int width = LOWORD(lparam);
        int height = HIWORD(lparam);
        int live_height = height < DEBUG_WIN_LIVE_HEIGHT ? height : DEBUG_WIN_LIVE_HEIGHT;
        MoveWindow(debug_live_edit, 0, 0, width, live_height, TRUE);
        MoveWindow(debug_edit, 0, live_height, width, height - live_height, TRUE);
        return 0;
    }

    case WM_TIMER:
        if (wparam == DEBUG_WIN_TIMER_ID && IsWindowVisible(hwnd))
        {
            _debug_win_refresh_live();
        }
        return 0;

    case WM_CLOSE:
        // closing only hides the window, the log is kept
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DEBUG_TOGGLE:
        if (IsWindowVisible(hwnd))
        {
            ShowWindow(hwnd, SW_HIDE);
        }
        else
        {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }
        return 0;

    case WM_DEBUG_APPEND:
        _debug_win_append_text((LPTSTR)lparam);
        free((LPTSTR)lparam);
        return 0;

    case WM_DEBUG_QUIT:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, DEBUG_WIN_TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static DWORD WINAPI _debug_win_thread_proc(LPVOID param)
{
    (void)param;

    WNDCLASSEX wc;
    _mem_zero(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = _debug_win_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = DEBUG_WIN_CLASS;
    RegisterClassEx(&wc);

    debug_hwnd = CreateWindowEx(0, DEBUG_WIN_CLASS, DEBUG_WIN_TITLE, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 700, 720, NULL, NULL, wc.hInstance, NULL);
    SetEvent(debug_ready_event);
    if (debug_hwnd == NULL)
    {
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    debug_hwnd = NULL;
    debug_edit = NULL;
    debug_live_edit = NULL;
    UnregisterClass(DEBUG_WIN_CLASS, wc.hInstance);
    return 0;
}

void debug_win_toggle(void)
{
    if (debug_thread == NULL)
    {
        debug_ready_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (debug_ready_event == NULL)
        {
            return;
        }
        debug_thread = CreateThread(NULL, 0, _debug_win_thread_proc, NULL, 0, NULL);
        if (debug_thread == NULL)
        {
            CloseHandle(debug_ready_event);
            debug_ready_event = NULL;
            return;
        }
        WaitForSingleObject(debug_ready_event, 5000);
    }

    if (debug_hwnd != NULL)
    {
        PostMessage(debug_hwnd, WM_DEBUG_TOGGLE, 0, 0);
    }
}

void debug_win_append(LPCTSTR text)
{
    HWND hwnd = debug_hwnd;
    if (hwnd == NULL || text == NULL)
    {
        return;
    }

    size_t length = _tcslen(text);
    LPTSTR copy = (LPTSTR)malloc((length + 3) * sizeof(TCHAR));
    if (copy == NULL)
    {
        return;
    }
    _mem_copy(copy, text, length * sizeof(TCHAR));
    copy[length] = TEXT('\r');
    copy[length + 1] = TEXT('\n');
    copy[length + 2] = TEXT('\0');

    if (!PostMessage(hwnd, WM_DEBUG_APPEND, 0, (LPARAM)copy))
    {
        free(copy);
    }
}

void debug_win_set_report(int gamepad_id, const BYTE *report, DWORD size)
{
    if (debug_hwnd == NULL || report == NULL)
    {
        return;
    }
    if (size > DEBUG_WIN_MAX_REPORT)
    {
        size = DEBUG_WIN_MAX_REPORT;
    }

    AcquireSRWLockExclusive(&report_lock);
    struct report_slot *slot = NULL;
    for (int i = 0; i < DEBUG_WIN_MAX_SLOTS; i++)
    {
        if (report_slots[i].used && report_slots[i].gamepad_id == gamepad_id)
        {
            slot = &report_slots[i];
            break;
        }
        if (slot == NULL && !report_slots[i].used)
        {
            slot = &report_slots[i];
        }
    }
    if (slot != NULL)
    {
        slot->used = TRUE;
        slot->gamepad_id = gamepad_id;
        slot->size = size;
        _mem_copy(slot->data, report, size);
        slot->tick = GetTickCount();
        report_dirty = TRUE;
    }
    ReleaseSRWLockExclusive(&report_lock);
}

void debug_win_free(void)
{
    if (debug_thread == NULL)
    {
        return;
    }

    if (debug_hwnd != NULL)
    {
        PostMessage(debug_hwnd, WM_DEBUG_QUIT, 0, 0);
    }
    WaitForSingleObject(debug_thread, 2000);
    CloseHandle(debug_thread);
    CloseHandle(debug_ready_event);
    debug_thread = NULL;
    debug_ready_event = NULL;
}
