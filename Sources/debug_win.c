#include "debug_win.h"

#include <stdio.h>
#include <stdlib.h>
#include <tchar.h>

#include "mi.h"
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
// layout (pixels)
#define DBG_MARGIN 8
#define DBG_ROW 26
#define DBG_GAP 6
#define DBG_CAPTION 18
#define DBG_LABEL_WIDTH 64
#define DBG_RAW_BUTTON_WIDTH 104
#define DBG_PRESET_BUTTON_WIDTH 118
#define DBG_MIN_WIDTH 700
#define DBG_MIN_HEIGHT 500

#define IDC_HEX 101
#define IDC_PAD 102
#define IDC_SEND_FEATURE 103
#define IDC_SEND_OUTPUT 104
#define IDC_GET_FEATURE 105
#define IDC_INPUT_OFF 106
#define IDC_INPUT_ON 107
#define IDC_ACCEL_ON 108
#define IDC_ACCEL_OFF 109
#define IDC_SENS 110
#define IDC_HID_INFO 111
#define IDC_IDENTIFY 112
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
static HWND debug_hex_edit = NULL;
static HWND debug_pad_combo = NULL;
static HWND debug_pad_label = NULL;
static HWND debug_hex_label = NULL;
static HWND debug_identify_button = NULL;
static HWND debug_raw_buttons[3];
static HWND debug_preset_buttons[4];
static HWND debug_info_button = NULL;
static HWND debug_sens_label = NULL;
static HWND debug_sens_edit = NULL;
static HWND debug_separator = NULL;
static HWND debug_live_caption = NULL;
static HWND debug_log_caption = NULL;
// fonts made by the window thread (NULL: the stock font is used)
static HFONT debug_ui_font = NULL;
static HFONT debug_mono_font = NULL;

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

    length += _stprintf(out + length, TEXT("Gamepad id %d, %lu bytes:\r\n"), slot->gamepad_id, slot->size);
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

// Id of the gamepad selected in the Pad dropdown (internal id; the tray shows the XInput slot). -1 if none.
static int _debug_win_pick_gamepad(void)
{
    LRESULT selected = SendMessage(debug_pad_combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR)
    {
        return -1;
    }
    return (int)SendMessage(debug_pad_combo, CB_GETITEMDATA, (WPARAM)selected, 0);
}

// Keeps the Pad dropdown equal to the live gamepads, so only existing pads can be picked. The selection stays on
// the same pad when it is still there. The pad dependent controls are disabled while there is no pad.
static void _debug_win_refresh_pads(void)
{
    if (debug_pad_combo == NULL || SendMessage(debug_pad_combo, CB_GETDROPPEDSTATE, 0, 0))
    {
        return;
    }

    int ids[DEBUG_WIN_MAX_SLOTS];
    int count = mi_gamepad_list(ids, DEBUG_WIN_MAX_SLOTS);

    BOOLEAN same = (int)SendMessage(debug_pad_combo, CB_GETCOUNT, 0, 0) == count;
    for (int i = 0; i < count && same; i++)
    {
        same = (int)SendMessage(debug_pad_combo, CB_GETITEMDATA, (WPARAM)i, 0) == ids[i];
    }
    if (same)
    {
        return;
    }

    int selected_id = _debug_win_pick_gamepad();
    int select = 0;
    SendMessage(debug_pad_combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < count; i++)
    {
        TCHAR label[32];
        _stprintf(label, TEXT("Gamepad id %d"), ids[i]);
        LRESULT index = SendMessage(debug_pad_combo, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessage(debug_pad_combo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)ids[i]);
        if (ids[i] == selected_id)
        {
            select = (int)index;
        }
    }
    if (count > 0)
    {
        SendMessage(debug_pad_combo, CB_SETCURSEL, (WPARAM)select, 0);
    }

    BOOL enable = count > 0;
    EnableWindow(debug_pad_combo, enable);
    EnableWindow(debug_identify_button, enable);
    EnableWindow(debug_info_button, enable);
    for (int i = 0; i < 3; i++)
    {
        EnableWindow(debug_raw_buttons[i], enable);
    }
    for (int i = 0; i < 4; i++)
    {
        EnableWindow(debug_preset_buttons[i], enable);
    }
}

static void _debug_win_do_raw(BYTE kind)
{
    TCHAR text[512];
    BYTE bytes[DEBUG_WIN_MAX_REPORT];
    size_t count = 0;
    int pending = -1;
    BOOLEAN valid = TRUE;

    GetWindowText(debug_hex_edit, text, sizeof(text) / sizeof(text[0]));
    for (const TCHAR *p = text; *p != TEXT('\0') && valid; p++)
    {
        int digit = -1;
        if (*p >= TEXT('0') && *p <= TEXT('9'))
        {
            digit = *p - TEXT('0');
        }
        else if (*p >= TEXT('a') && *p <= TEXT('f'))
        {
            digit = *p - TEXT('a') + 10;
        }
        else if (*p >= TEXT('A') && *p <= TEXT('F'))
        {
            digit = *p - TEXT('A') + 10;
        }
        else if (*p != TEXT(' ') && *p != TEXT(',') && *p != TEXT('\t'))
        {
            valid = FALSE;
            break;
        }

        int byte_value = -1;
        if (digit < 0)
        {
            // separator: a lone digit counts as a whole byte
            byte_value = pending;
            pending = -1;
        }
        else if (pending < 0)
        {
            pending = digit;
        }
        else
        {
            byte_value = (pending << 4) | digit;
            pending = -1;
        }
        if (byte_value >= 0)
        {
            if (count >= sizeof(bytes))
            {
                valid = FALSE;
                break;
            }
            bytes[count++] = (BYTE)byte_value;
        }
    }
    if (valid && pending >= 0)
    {
        if (count >= sizeof(bytes))
        {
            valid = FALSE;
        }
        else
        {
            bytes[count++] = (BYTE)pending;
        }
    }
    if (!valid || count == 0)
    {
        debug_log("raw: invalid hex input (use pairs like \"20 00 00\", max %d bytes)", DEBUG_WIN_MAX_REPORT);
        return;
    }

    if (bytes[0] == 0x22 && kind != MI_RAW_GET_FEATURE
        && MessageBox(debug_hwnd, TEXT("Report 0x22 writes calibration that the controller keeps permanently.\nA wrong packet can leave it reporting garbage.\n\nSend it anyway?"),
                      TEXT("Calibration packet"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    {
        debug_log("raw: calibration packet cancelled");
        return;
    }

    int gamepad_id = _debug_win_pick_gamepad();
    if (gamepad_id < 0)
    {
        debug_log("raw: no gamepad connected");
        return;
    }

    BYTE reply[256];
    DWORD error = 0;
    mi_gamepad_raw(gamepad_id, kind, bytes, count, reply, sizeof(reply), &error);
}

static void _debug_win_send_preset(const BYTE *bytes, size_t length)
{
    int gamepad_id = _debug_win_pick_gamepad();
    if (gamepad_id < 0)
    {
        debug_log("raw: no gamepad connected");
        return;
    }
    DWORD error = 0;
    mi_gamepad_raw(gamepad_id, MI_RAW_SEND_FEATURE, bytes, length, NULL, 0, &error);
}

static BYTE _debug_win_sensitivity(void)
{
    TCHAR text[16];
    GetWindowText(debug_sens_edit, text, sizeof(text) / sizeof(text[0]));
    if (text[0] == TEXT('\0'))
    {
        return 0x08;
    }
    unsigned long value = _tcstoul(text, NULL, 16);
    return (BYTE)(value > 0xFF ? 0xFF : value);
}

static HWND _debug_win_make(HWND parent, LPCTSTR class_name, LPCTSTR text, DWORD style, DWORD ex_style, int id, HFONT font)
{
    HWND child = CreateWindowEx(ex_style, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                                (HMENU)(INT_PTR)id, GetModuleHandle(NULL), NULL);
    if (child != NULL && font != NULL)
    {
        SendMessage(child, WM_SETFONT, (WPARAM)font, TRUE);
    }
    return child;
}

static LRESULT CALLBACK _debug_win_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // Segoe UI (the system message font) for the controls, Consolas for the dumps and the log
        NONCLIENTMETRICS metrics;
        _mem_zero(&metrics, sizeof(metrics));
        metrics.cbSize = sizeof(metrics);
        if (SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            debug_ui_font = CreateFontIndirect(&metrics.lfMessageFont);
        }
        debug_mono_font = CreateFont(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, TEXT("Consolas"));
        HFONT ui = debug_ui_font != NULL ? debug_ui_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HFONT mono = debug_mono_font != NULL ? debug_mono_font : (HFONT)GetStockObject(ANSI_FIXED_FONT);

        // row 1: which pad
        debug_pad_label = _debug_win_make(hwnd, TEXT("STATIC"), TEXT("Pad"), SS_LEFT | SS_CENTERIMAGE, 0, 0, ui);
        debug_pad_combo = _debug_win_make(hwnd, TEXT("COMBOBOX"), TEXT(""),
                                          CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, IDC_PAD, ui);
        debug_identify_button = _debug_win_make(hwnd, TEXT("BUTTON"), TEXT("Identify (vibrate)"),
                                                BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_IDENTIFY, ui);
        debug_info_button = _debug_win_make(hwnd, TEXT("BUTTON"), TEXT("HID info"),
                                            BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_HID_INFO, ui);

        // row 2: raw bytes
        debug_hex_label = _debug_win_make(hwnd, TEXT("STATIC"), TEXT("Raw hex"), SS_LEFT | SS_CENTERIMAGE, 0, 0, ui);
        debug_hex_edit = _debug_win_make(hwnd, TEXT("EDIT"), TEXT("20 00 00"), ES_AUTOHSCROLL | WS_TABSTOP,
                                         WS_EX_CLIENTEDGE, IDC_HEX, mono);
        static const struct { int id; LPCTSTR text; } raw_buttons[3] =
        {
            { IDC_SEND_FEATURE, TEXT("Send feature") },
            { IDC_SEND_OUTPUT, TEXT("Send output") },
            { IDC_GET_FEATURE, TEXT("Get feature") }
        };
        for (int i = 0; i < 3; i++)
        {
            debug_raw_buttons[i] = _debug_win_make(hwnd, TEXT("BUTTON"), raw_buttons[i].text,
                                                   BS_PUSHBUTTON | WS_TABSTOP, 0, raw_buttons[i].id, ui);
        }

        // row 3: presets
        static const struct { int id; LPCTSTR text; } preset_buttons[4] =
        {
            { IDC_INPUT_OFF, TEXT("Input off (0x23)") },
            { IDC_INPUT_ON, TEXT("Input on (0x23)") },
            { IDC_ACCEL_ON, TEXT("Accel on (0x31)") },
            { IDC_ACCEL_OFF, TEXT("Accel off (0x31)") }
        };
        for (int i = 0; i < 4; i++)
        {
            debug_preset_buttons[i] = _debug_win_make(hwnd, TEXT("BUTTON"), preset_buttons[i].text,
                                                      BS_PUSHBUTTON | WS_TABSTOP, 0, preset_buttons[i].id, ui);
        }
        debug_sens_label = _debug_win_make(hwnd, TEXT("STATIC"), TEXT("Accel sens (hex)"),
                                           SS_RIGHT | SS_CENTERIMAGE, 0, 0, ui);
        debug_sens_edit = _debug_win_make(hwnd, TEXT("EDIT"), TEXT("08"), ES_AUTOHSCROLL | WS_TABSTOP,
                                          WS_EX_CLIENTEDGE, IDC_SENS, mono);

        // output
        debug_separator = _debug_win_make(hwnd, TEXT("STATIC"), TEXT(""), SS_ETCHEDHORZ, 0, 0, NULL);
        debug_live_caption = _debug_win_make(hwnd, TEXT("STATIC"), TEXT("Live input"), SS_LEFT | SS_CENTERIMAGE, 0, 0, ui);
        debug_live_edit = _debug_win_make(hwnd, TEXT("EDIT"), TEXT(""), ES_MULTILINE | ES_READONLY,
                                          WS_EX_CLIENTEDGE, 0, mono);
        debug_log_caption = _debug_win_make(hwnd, TEXT("STATIC"), TEXT("Log"), SS_LEFT | SS_CENTERIMAGE, 0, 0, ui);
        debug_edit = _debug_win_make(hwnd, TEXT("EDIT"), TEXT(""),
                                     WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, WS_EX_CLIENTEDGE, 0, mono);
        SendMessage(debug_edit, EM_SETLIMITTEXT, 0, 0);

        // no pad yet: everything that needs one starts disabled (the refresh enables it)
        EnableWindow(debug_pad_combo, FALSE);
        EnableWindow(debug_identify_button, FALSE);
        EnableWindow(debug_info_button, FALSE);
        for (int i = 0; i < 3; i++)
        {
            EnableWindow(debug_raw_buttons[i], FALSE);
        }
        for (int i = 0; i < 4; i++)
        {
            EnableWindow(debug_preset_buttons[i], FALSE);
        }
        _debug_win_refresh_pads();
        SetTimer(hwnd, DEBUG_WIN_TIMER_ID, DEBUG_WIN_TIMER_MS, NULL);
        return 0;
    }

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *info = (MINMAXINFO *)lparam;
        info->ptMinTrackSize.x = DBG_MIN_WIDTH;
        info->ptMinTrackSize.y = DBG_MIN_HEIGHT;
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        // read-only edits are painted like static text (grey): keep the output areas white
        if ((HWND)lparam == debug_live_edit || (HWND)lparam == debug_edit)
        {
            SetTextColor((HDC)wparam, GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor((HDC)wparam, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;

    case WM_SIZE:
    {
        int width = LOWORD(lparam);
        int height = HIWORD(lparam);
        int inner = width - 2 * DBG_MARGIN;
        if (inner < 0)
        {
            inner = 0;
        }
        int row1 = DBG_MARGIN;
        int row2 = row1 + DBG_ROW + DBG_GAP;
        int row3 = row2 + DBG_ROW + DBG_GAP;
        int top = row3 + DBG_ROW + DBG_MARGIN + 4;

        // row 1: Pad [dropdown] [Identify] [HID info]
        int x = DBG_MARGIN;
        MoveWindow(debug_pad_label, x, row1, DBG_LABEL_WIDTH, DBG_ROW, TRUE);
        x += DBG_LABEL_WIDTH;
        MoveWindow(debug_pad_combo, x, row1, 190, 220, TRUE);
        x += 190 + DBG_GAP;
        MoveWindow(debug_identify_button, x, row1, 140, DBG_ROW, TRUE);
        x += 140 + 4;
        MoveWindow(debug_info_button, x, row1, 90, DBG_ROW, TRUE);

        // row 2: Raw hex [edit] [3 buttons]
        int buttons_x = width - DBG_MARGIN - 3 * DBG_RAW_BUTTON_WIDTH - 2 * 4;
        int hex_x = DBG_MARGIN + DBG_LABEL_WIDTH;
        int hex_width = buttons_x - DBG_GAP - hex_x;
        MoveWindow(debug_hex_label, DBG_MARGIN, row2, DBG_LABEL_WIDTH, DBG_ROW, TRUE);
        MoveWindow(debug_hex_edit, hex_x, row2, hex_width > 40 ? hex_width : 40, DBG_ROW, TRUE);
        for (int i = 0; i < 3; i++)
        {
            MoveWindow(debug_raw_buttons[i], buttons_x + i * (DBG_RAW_BUTTON_WIDTH + 4), row2, DBG_RAW_BUTTON_WIDTH, DBG_ROW, TRUE);
        }

        // row 3: presets and the accel sensitivity
        for (int i = 0; i < 4; i++)
        {
            MoveWindow(debug_preset_buttons[i], DBG_MARGIN + i * (DBG_PRESET_BUTTON_WIDTH + 4), row3, DBG_PRESET_BUTTON_WIDTH, DBG_ROW, TRUE);
        }
        int sens_x = DBG_MARGIN + 4 * (DBG_PRESET_BUTTON_WIDTH + 4) + 8;
        MoveWindow(debug_sens_label, sens_x, row3, 110, DBG_ROW, TRUE);
        MoveWindow(debug_sens_edit, sens_x + 116, row3, 40, DBG_ROW, TRUE);

        // output: live decode on top, log below
        int available = height - top - 2 * DBG_CAPTION - DBG_GAP - DBG_MARGIN;
        if (available < 0)
        {
            available = 0;
        }
        int live_height = available / 2 < DEBUG_WIN_LIVE_HEIGHT ? available / 2 : DEBUG_WIN_LIVE_HEIGHT;
        int log_y = top + DBG_CAPTION + live_height + DBG_GAP;
        int log_height = height - log_y - DBG_CAPTION - DBG_MARGIN;
        MoveWindow(debug_separator, DBG_MARGIN, top - 4, inner, 2, TRUE);
        MoveWindow(debug_live_caption, DBG_MARGIN, top, inner, DBG_CAPTION, TRUE);
        MoveWindow(debug_live_edit, DBG_MARGIN, top + DBG_CAPTION, inner, live_height, TRUE);
        MoveWindow(debug_log_caption, DBG_MARGIN, log_y, inner, DBG_CAPTION, TRUE);
        MoveWindow(debug_edit, DBG_MARGIN, log_y + DBG_CAPTION, inner, log_height > 0 ? log_height : 0, TRUE);
        return 0;
    }

    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED)
        {
            switch (LOWORD(wparam))
            {
            case IDCANCEL:
                // Esc (from the dialog-style keyboard handling) hides the window like the close button
                ShowWindow(hwnd, SW_HIDE);
                break;
            case IDC_IDENTIFY:
            {
                int gamepad_id = _debug_win_pick_gamepad();
                if (gamepad_id < 0)
                {
                    debug_log("identify: no gamepad connected");
                }
                else
                {
                    mi_gamepad_identify(gamepad_id);
                }
                break;
            }
            case IDC_SEND_FEATURE:
                _debug_win_do_raw(MI_RAW_SEND_FEATURE);
                break;
            case IDC_SEND_OUTPUT:
                _debug_win_do_raw(MI_RAW_SEND_OUTPUT);
                break;
            case IDC_GET_FEATURE:
                _debug_win_do_raw(MI_RAW_GET_FEATURE);
                break;
            case IDC_INPUT_OFF:
            {
                static const BYTE input_off[2] = { 0x23, 0x01 };
                if (MessageBox(hwnd, TEXT("This stops all input from the controller (except the Mi button) until \"Input on\" is sent.\n\nContinue?"),
                               TEXT("Stop input"), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                {
                    _debug_win_send_preset(input_off, sizeof(input_off));
                }
                break;
            }
            case IDC_INPUT_ON:
            {
                static const BYTE input_on[2] = { 0x23, 0x00 };
                _debug_win_send_preset(input_on, sizeof(input_on));
                break;
            }
            case IDC_ACCEL_ON:
            {
                BYTE accel_on[3] = { 0x31, 0x01, _debug_win_sensitivity() };
                _debug_win_send_preset(accel_on, sizeof(accel_on));
                break;
            }
            case IDC_HID_INFO:
            {
                int gamepad_id = _debug_win_pick_gamepad();
                if (gamepad_id < 0)
                {
                    debug_log("describe: no gamepad connected");
                }
                else if (!mi_gamepad_describe(gamepad_id))
                {
                    debug_log("describe: gamepad %d not found", gamepad_id);
                }
                break;
            }
            case IDC_ACCEL_OFF:
            {
                static const BYTE accel_off[3] = { 0x31, 0x00, 0x00 };
                _debug_win_send_preset(accel_off, sizeof(accel_off));
                break;
            }
            }
        }
        return 0;

    case WM_TIMER:
        if (wparam == DEBUG_WIN_TIMER_ID && IsWindowVisible(hwnd))
        {
            _debug_win_refresh_pads();
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
            _debug_win_refresh_pads();
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
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = DEBUG_WIN_CLASS;
    RegisterClassEx(&wc);

    debug_hwnd = CreateWindowEx(0, DEBUG_WIN_CLASS, DEBUG_WIN_TITLE, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 760, 800, NULL, NULL, wc.hInstance, NULL);
    SetEvent(debug_ready_event);
    if (debug_hwnd == NULL)
    {
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0)
    {
        // Tab / Shift+Tab / Esc work between the controls
        if (!IsDialogMessage(debug_hwnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    debug_hwnd = NULL;
    debug_edit = NULL;
    debug_live_edit = NULL;
    debug_pad_combo = NULL;
    if (debug_ui_font != NULL)
    {
        DeleteObject(debug_ui_font);
        debug_ui_font = NULL;
    }
    if (debug_mono_font != NULL)
    {
        DeleteObject(debug_mono_font);
        debug_mono_font = NULL;
    }
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
