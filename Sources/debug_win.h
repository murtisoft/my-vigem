#ifndef DEBUG_WIN_H
#define DEBUG_WIN_H

#include <wtypes.h>

// Show the debug window if hidden, hide it if shown. Creates it (and its thread) on first use.
void debug_win_toggle(void);

// Append a line of text to the debug window log. Thread-safe; ignored while the window does not exist.
void debug_win_append(LPCTSTR text);

// Store the latest raw input report of a gamepad for the live hex dump. Thread-safe and cheap; same signature as `mi_raw_cb_t`.
void debug_win_set_report(int gamepad_id, const BYTE *report, DWORD size);

// Destroy the window and stop its thread.
void debug_win_free(void);

#endif /* DEBUG_WIN_H */
