#include <stdio.h>
#include <string.h>
#include <tchar.h>
#include <windows.h>
#include <synchapi.h>

#include <ViGEm/Client.h>

#include "tray.h"
#include "hid.h"
#include "mi.h"
#include "hid_hide.h"
#include "debug_win.h"
#include "autostart.h"
#include "utils.h"

#ifndef _DEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#define MAX_ACTIVE_DEVICE_COUNT 4
#define REATTACH_REMOVE_TIMEOUT_MS 5000
#ifdef UNICODE
#define ACTIVE_DEVICE_MENU_TEMPLATE TEXT("#%d Xiaomi Gamepad (batt. %ls)")
#else
#define ACTIVE_DEVICE_MENU_TEMPLATE TEXT("#%d Xiaomi Gamepad (batt. %s)")
#endif
#define BATTERY_NA_TEXT TEXT("N/A")

struct active_device
{
    int index;
    struct hid_device *src_device;
    int src_gamepad_id;
    int src_battery_level;
    PVIGEM_TARGET tgt_device; // NULL if ViGEm is not connected
    BOOLEAN tgt_added; // the target was added to the bus (needs unregister + remove)
    XUSB_REPORT tgt_report;
    LPTSTR tray_text;
    struct tray_menu *tray_menu;
    BOOLEAN hid_hidden;
    int xinput_slot; // 0..3 XInput user index of the virtual pad, -1 if unknown
};

// number shown in the tray: XInput slot + 1 when known, otherwise our own running index
static int _device_number(const struct active_device *device)
{
    return device->xinput_slot >= 0 ? device->xinput_slot + 1 : device->index;
}

static int last_active_device_index = 0;
static int active_device_count = 0;
// the "device count limit" notification is shown once until a pad is removed
static BOOLEAN limit_notified = FALSE;
static struct active_device *active_devices[MAX_ACTIVE_DEVICE_COUNT];
static SRWLOCK active_devices_lock = SRWLOCK_INIT;
// signaled whenever a device has been removed from active_devices
static CONDITION_VARIABLE active_devices_removed_cv = CONDITION_VARIABLE_INIT;
// set while a full refresh removes and re-adds all devices, to suppress the routine notifications
static volatile LONG reattaching = 0;
static PVIGEM_CLIENT vigem_client;
static BOOLEAN vigem_connected = FALSE;
// ViGEm is not thread safe: every vigem_* call after startup runs under this lock. Never call ViGEm while holding
// active_devices_lock or tray_ui_lock (unregistering a notification can wait for its callback, which takes both).
static SRWLOCK vigem_lock = SRWLOCK_INIT;

static void vigem_release_target(PVIGEM_TARGET target, BOOLEAN added)
{
    if (target == NULL)
    {
        return;
    }
    AcquireSRWLockExclusive(&vigem_lock);
    if (added)
    {
        vigem_target_x360_unregister_notification(target);
        vigem_target_remove(vigem_client, target);
    }
    vigem_target_free(target);
    ReleaseSRWLockExclusive(&vigem_lock);
}
static BOOLEAN hid_hide_connected = FALSE;

// future declarations
static void mi_gamepad_update_cb(int gamepad_id, struct mi_state *state);
static void mi_gamepad_stop_cb(int gamepad_id, BYTE break_reason);
static void CALLBACK x360_notification_cb(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR large_motor,
                                          UCHAR small_motor, UCHAR led_number, LPVOID user_data);
static void pad_menu_cb(struct tray_menu *item);
static void refresh_cb(struct tray_menu *item);
#ifdef MY_VIGEM_DEBUG
static void debug_cb(struct tray_menu *item);
#endif
static void autostart_cb(struct tray_menu *item);
static void github_cb(struct tray_menu *item);
static void quit_cb(struct tray_menu *item);

static const struct tray_menu tray_menu_refresh = { .text = TEXT("Reset Gamepads"), .cb = refresh_cb };
#ifdef MY_VIGEM_DEBUG
static const struct tray_menu tray_menu_debug = { .text = TEXT("Debug"), .cb = debug_cb };
#endif
static const struct tray_menu tray_menu_github = { .text = TEXT("GitHub Page"), .cb = github_cb };
static struct tray_menu tray_menu_autostart = { .text = TEXT("Autostart at Logon"), .cb = autostart_cb };
#define _TEXT_EXPAND(x) TEXT(x)
static const struct tray_menu tray_menu_quit = { .text = TEXT("Quit  (ver. ") _TEXT_EXPAND(APP_VERSION_STR) TEXT(")"), .cb = quit_cb };
static const struct tray_menu tray_menu_separator = { .text = TEXT("-") };
static const struct tray_menu tray_menu_terminator = { .text = NULL };
static BOOLEAN autostart_checked = FALSE;
static struct tray tray =
{
    .icon = TEXT("APP_ICON"),
    .tip = TEXT("Mi-ViGEm"),
    .menu = NULL
};

SHORT FORCEINLINE _map_byte_to_short(BYTE value, BOOLEAN inverted)
{
    CHAR centered = value - 128;
    if (centered < -127)
    {
        centered = -127;
    }
    if (inverted)
    {
        centered = -centered;
    }
    return (SHORT)(32767 * centered / 127);
}

static SRWLOCK tray_text_lock = SRWLOCK_INIT;

// Rebuilds the menu text of a pad from its current number and battery level.
static void update_tray_text(struct active_device *active_device)
{
    TCHAR battery_buffer[8];
    if (active_device->src_battery_level < 0)
    {
        _tcscpy(battery_buffer, BATTERY_NA_TEXT);
    }
    else
    {
        _stprintf(battery_buffer, TEXT("%d%%"), active_device->src_battery_level);
    }
    int tray_text_length = _sctprintf(ACTIVE_DEVICE_MENU_TEMPLATE, _device_number(active_device), battery_buffer);
    LPTSTR text = (LPTSTR)malloc((tray_text_length + 1) * sizeof(TCHAR));
    if (text == NULL)
    {
        return;
    }
    _stprintf(text, ACTIVE_DEVICE_MENU_TEMPLATE, _device_number(active_device), battery_buffer);
    AcquireSRWLockExclusive(&tray_text_lock);
    LPTSTR prev_text = active_device->tray_text;
    active_device->tray_text = text;
    if (active_device->tray_menu != NULL)
    {
        active_device->tray_menu->text = text;
    }
    ReleaseSRWLockExclusive(&tray_text_lock);
    free(prev_text);
}

static void rebuild_tray_menu()
{
    struct tray_menu *prev_menu = tray.menu;

    AcquireSRWLockShared(&active_devices_lock);
    int device_count = active_device_count;
    int dyn_item_count = device_count > 0 ? device_count + 1 : 0;
    struct tray_menu *new_menu = (struct tray_menu *)malloc((dyn_item_count + 9) * sizeof(struct tray_menu));
    if (new_menu == NULL)
    {
        ReleaseSRWLockShared(&active_devices_lock);
        return;
    }
    int index = 0;
    for (; index < device_count; index++)
    {
        new_menu[index] = *active_devices[index]->tray_menu;
    }
    ReleaseSRWLockShared(&active_devices_lock);
    if (dyn_item_count > 0)
    {
        new_menu[index++] = tray_menu_separator;
    }
    new_menu[index++] = tray_menu_refresh;
#ifdef MY_VIGEM_DEBUG
    new_menu[index++] = tray_menu_debug;
#endif
    new_menu[index++] = tray_menu_separator;
    tray_menu_autostart.checked = autostart_checked;
    new_menu[index++] = tray_menu_autostart;
    new_menu[index++] = tray_menu_github;
    new_menu[index++] = tray_menu_separator;
    new_menu[index++] = tray_menu_quit;
    new_menu[index++] = tray_menu_terminator;

    tray.menu = new_menu;
    free(prev_menu);
}

// All tray menu changes go through here: texts, menu array and tray_update are touched by the input threads,
// the worker, the ViGEm notification thread and the tray thread, so they are serialized by one lock.
// dev (optional): pad whose menu text is rebuilt first. Never call ViGEm or take active_devices_lock exclusively while holding it.
static SRWLOCK tray_ui_lock = SRWLOCK_INIT;

static void refresh_tray(struct active_device *dev)
{
    AcquireSRWLockExclusive(&tray_ui_lock);
    if (dev != NULL)
    {
        update_tray_text(dev);
    }
    rebuild_tray_menu();
    tray_update(&tray);
    ReleaseSRWLockExclusive(&tray_ui_lock);
}

// Symlink of the device whose add failed last (worker thread only). A switched-off pad can keep its devnode and
// fail on every refresh: its failure is shown once, until the device is added or gone.
static LPTSTR add_failed_symlink = NULL;

static void _add_failed(struct hid_device_info *device_info, LPTSTR text)
{
    if (add_failed_symlink != NULL && _tcscmp(add_failed_symlink, device_info->symlink) == 0)
    {
        debug_log("add_device: failed again, notification already shown");
        return;
    }
    free(add_failed_symlink);
    add_failed_symlink = _tcsdup(device_info->symlink);
    tray_show_notification(NT_TRAY_WARNING, TEXT("Add new device"), text);
}

// Forgets the failed device when it was added or is not listed any more.
static void _add_failed_clear_unless(struct hid_device_info *list)
{
    if (add_failed_symlink == NULL)
    {
        return;
    }
    for (struct hid_device_info *cur = list; cur != NULL; cur = cur->next)
    {
        if (_tcscmp(add_failed_symlink, cur->symlink) == 0)
        {
            return;
        }
    }
    free(add_failed_symlink);
    add_failed_symlink = NULL;
}

static BOOLEAN add_device(struct hid_device_info *device_info)
{
    // the count and the notified flag are shared with the pad threads (remove_device)
    AcquireSRWLockExclusive(&active_devices_lock);
    BOOLEAN limit_reached = active_device_count >= MAX_ACTIVE_DEVICE_COUNT;
    BOOLEAN notify_limit = limit_reached && !limit_notified;
    if (notify_limit)
    {
        limit_notified = TRUE;
    }
    ReleaseSRWLockExclusive(&active_devices_lock);
    if (limit_reached)
    {
        if (notify_limit)
        {
            tray_show_notification(NT_TRAY_WARNING, TEXT("Add new device"),
                                   TEXT("Device count limit reached"));
        }
        return FALSE;
    }

    debug_log("add_device: " DBG_TSTR, device_info->symlink);
    struct hid_device *device = hid_open_device(device_info, TRUE, FALSE);
    if (device == NULL)
    {
        debug_log("add_device: exclusive open failed, re-enabling device");
        if (hid_reenable_device(device_info))
        {
            device = hid_open_device(device_info, TRUE, FALSE);
            if (device == NULL)
            {
                device = hid_open_device(device_info, TRUE, TRUE);
            }
        }
        else
        {
            device = hid_open_device(device_info, TRUE, TRUE);
        }
    }
    if (device == NULL)
    {
        debug_log("add_device: could not open device");
    }

    if (device == NULL)
    {
        _add_failed(device_info, TEXT("Error opening new device"));
        return FALSE;
    }

    // everything the pad needs is allocated before its threads start, so an allocation failure has nothing to undo
    struct active_device *active_device = (struct active_device *)calloc(1, sizeof(struct active_device));
    if (active_device != NULL)
    {
        active_device->src_device = device;
        active_device->index = last_active_device_index + 1; // taken over once the pad has started
        active_device->src_battery_level = -1;
        active_device->xinput_slot = -1; // learned from the LED notification, the slot is assigned later
        int tray_text_length = _sctprintf(ACTIVE_DEVICE_MENU_TEMPLATE, _device_number(active_device), BATTERY_NA_TEXT);
        if (tray_text_length > 0)
        {
            active_device->tray_text = (LPTSTR)malloc((tray_text_length + 1) * sizeof(TCHAR));
        }
        active_device->tray_menu = (struct tray_menu *)calloc(1, sizeof(struct tray_menu));
    }
    if (active_device == NULL || active_device->tray_text == NULL || active_device->tray_menu == NULL)
    {
        if (active_device != NULL)
        {
            free(active_device->tray_text);
            free(active_device->tray_menu);
            free(active_device);
        }
        debug_log("add_device: out of memory");
        _add_failed(device_info, TEXT("Error initializing new device"));
        hid_close_device(device);
        hid_free_device(device);
        return FALSE;
    }
    _stprintf(active_device->tray_text, ACTIVE_DEVICE_MENU_TEMPLATE, _device_number(active_device), BATTERY_NA_TEXT);
    active_device->tray_menu->text = active_device->tray_text;
    active_device->tray_menu->cb = pad_menu_cb;

    int mi_gamepad_id = mi_gamepad_start(device, mi_gamepad_update_cb, mi_gamepad_stop_cb);
    if (mi_gamepad_id < 0)
    {
        _add_failed(device_info, TEXT("Error initializing new device"));
        free(active_device->tray_text);
        free(active_device->tray_menu);
        free(active_device);
        hid_close_device(device);
        hid_free_device(device);
        return FALSE;
    }
    last_active_device_index = active_device->index;
    active_device->src_gamepad_id = mi_gamepad_id;
    active_device->tray_menu->context = (void *)(INT_PTR)mi_gamepad_id;
    if (vigem_connected)
    {
        AcquireSRWLockExclusive(&vigem_lock);
        active_device->tgt_device = vigem_target_x360_alloc();
        if (active_device->tgt_device != NULL)
        {
            VIGEM_ERROR add_res = vigem_target_add(vigem_client, active_device->tgt_device);
            if (add_res == VIGEM_ERROR_NONE)
            {
                active_device->tgt_added = TRUE;
                vigem_target_x360_register_notification(vigem_client, active_device->tgt_device, x360_notification_cb,
                                                        (LPVOID)active_device);
            }
            else
            {
                debug_log("add_device: vigem_target_add failed, error=0x%X", (unsigned)add_res);
            }
        }
        ReleaseSRWLockExclusive(&vigem_lock);
    }
    active_device->hid_hidden = hid_hide_bind(device->device_info) == HID_HIDE_RESULT_OK;

    AcquireSRWLockExclusive(&active_devices_lock);
    active_devices[active_device_count++] = active_device;
    ReleaseSRWLockExclusive(&active_devices_lock);

    if (add_failed_symlink != NULL && _tcscmp(add_failed_symlink, device_info->symlink) == 0)
    {
        free(add_failed_symlink);
        add_failed_symlink = NULL;
    }

    refresh_tray(NULL);
    if (active_device->tgt_added)
    {
        if (!reattaching)
        {
            tray_show_notification(NT_TRAY_INFO, TEXT("Add new device"),
                                   TEXT("Device added successfully"));
        }
    }
    else
    {
        tray_show_notification(NT_TRAY_WARNING, TEXT("Add new device"),
                               TEXT("Device added, but emulation doesn't work due to ViGEmBus problem"));
    }
    return TRUE;
}

static BOOLEAN remove_device(int mi_gamepad_id)
{
    debug_log("remove_device: gamepad %d", mi_gamepad_id);
    struct active_device *dev = NULL;
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
    {
        if (active_devices[i]->src_gamepad_id == mi_gamepad_id)
        {
            dev = active_devices[i];
            break;
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);
    if (dev == NULL)
    {
        return FALSE;
    }

    // The ViGEm target goes first: unregistering waits for the notification callback, which uses dev and takes
    // tray_ui_lock, so no lock may be held here.
    PVIGEM_TARGET tgt_device = dev->tgt_device;
    BOOLEAN tgt_added = dev->tgt_added;
    dev->tgt_device = NULL;
    dev->tgt_added = FALSE;
    vigem_release_target(tgt_device, tgt_added);

    // lock order: tray_ui_lock, then active_devices_lock (like refresh_tray); a menu rebuild may still read the texts
    BOOLEAN removed = FALSE;
    AcquireSRWLockExclusive(&tray_ui_lock);
    AcquireSRWLockExclusive(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
    {
        if (active_devices[i] == dev)
        {
            if (dev->hid_hidden)
            {
                hid_hide_unbind(dev->src_device->device_info);
            }
            hid_close_device(dev->src_device);
            hid_free_device(dev->src_device);
            free(dev->tray_menu);
            free(dev->tray_text);
            free(dev);
            for (int j = i; j < active_device_count - 1; j++)
            {
                active_devices[j] = active_devices[j + 1];
            }
            active_device_count--;
            limit_notified = FALSE;
            WakeAllConditionVariable(&active_devices_removed_cv);
            removed = TRUE;
            break;
        }
    }
    ReleaseSRWLockExclusive(&active_devices_lock);
    ReleaseSRWLockExclusive(&tray_ui_lock);
    return removed;
}

static void refresh_devices()
{
    LPTSTR mi_hw_path_filters[3] = { MI_HW_FILTER_WIN10, MI_HW_FILTER_WIN7, NULL };
    hid_enable_disabled_devices(mi_hw_path_filters);
    struct hid_device_info *device_info = hid_enumerate(mi_hw_path_filters);
    struct hid_device_info *cur;
    BOOLEAN found = FALSE;
    _add_failed_clear_unless(device_info);

    // remove missing devices
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
    {
        found = FALSE;
        cur = device_info;
        while (cur != NULL)
        {
            if (_tcscmp(active_devices[i]->src_device->device_info->symlink, cur->symlink) == 0)
            {
                found = TRUE;
                break;
            }
            cur = cur->next;
        }
        if (!found)
        {
            mi_gamepad_stop(active_devices[i]->src_gamepad_id);
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);

    // add new devices
    cur = device_info;
    while (cur != NULL)
    {
        found = FALSE;
        AcquireSRWLockShared(&active_devices_lock);
        for (int i = 0; i < active_device_count; i++)
        {
            if (_tcscmp(cur->symlink, active_devices[i]->src_device->device_info->symlink) == 0)
            {
                found = TRUE;
                break;
            }
        }
        ReleaseSRWLockShared(&active_devices_lock);
        if (!found)
        {
            add_device(cur);
        }
        cur = cur->next;
    }

    // free hid_device_info list
    while (device_info)
    {
        cur = device_info->next;
        hid_free_device_info(device_info);
        device_info = cur;
    }
}

// Asks every pad to stop and waits (bounded) until all of them are removed.
static void stop_all_devices()
{
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
    {
        mi_gamepad_stop(active_devices[i]->src_gamepad_id);
    }
    ReleaseSRWLockShared(&active_devices_lock);

    ULONGLONG deadline = GetTickCount64() + REATTACH_REMOVE_TIMEOUT_MS;
    AcquireSRWLockShared(&active_devices_lock);
    while (active_device_count > 0)
    {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline ||
            !SleepConditionVariableSRW(&active_devices_removed_cv, &active_devices_lock, (DWORD)(deadline - now),
                                       CONDITION_VARIABLE_LOCKMODE_SHARED))
        {
            break;
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);
}

// Removes all virtual pads and turns the real pads off (disables and re-enables their parent devnode).
// Nothing is added back: the pads come back through the normal refresh when they are switched on again.
static void reset_devices()
{
    InterlockedExchange(&reattaching, 1);

    // find the real pads first (also ones that are not active), they are gone from the list after stopping
    LPTSTR mi_hw_path_filters[3] = { MI_HW_FILTER_WIN10, MI_HW_FILTER_WIN7, NULL };
    struct hid_device_info *device_info = hid_enumerate(mi_hw_path_filters);

    stop_all_devices();
    last_active_device_index = 0;

    // turn the real pads off, once per parent devnode (a pad can expose several HID interfaces)
    for (struct hid_device_info *cur = device_info; cur != NULL; cur = cur->next)
    {
        BOOLEAN done = FALSE;
        for (struct hid_device_info *prev = device_info; prev != cur && !done; prev = prev->next)
        {
            done = cur->container_instance_path != NULL && prev->container_instance_path != NULL &&
                   _tcsicmp(cur->container_instance_path, prev->container_instance_path) == 0;
        }
        if (!done)
        {
            hid_power_off_device(cur);
        }
    }
    while (device_info)
    {
        struct hid_device_info *next = device_info->next;
        hid_free_device_info(device_info);
        device_info = next;
    }

    InterlockedExchange(&reattaching, 0);
    debug_log("reset done, waiting for new connections");
}

// Device refreshes (enumeration, re-enabling, opening) can block for seconds, so they run on a
// worker thread and the tray thread only requests them. Requests made while a refresh is running
// are coalesced into a single follow-up refresh.
static HANDLE refresh_event = NULL;
static HANDLE refresh_thread = NULL;
static volatile LONG refresh_quit = 0;
static volatile LONG refresh_full = 0;

static DWORD WINAPI refresh_thread_proc(LPVOID param)
{
    (void)param;
    while (WaitForSingleObject(refresh_event, INFINITE) == WAIT_OBJECT_0)
    {
        if (InterlockedCompareExchange(&refresh_quit, 0, 0))
        {
            break;
        }
        if (InterlockedExchange(&refresh_full, 0))
        {
            reset_devices();
        }
        else
        {
            refresh_devices();
        }
    }
    return 0;
}

static void request_refresh(void)
{
    SetEvent(refresh_event);
}

static void request_full_refresh(void)
{
    InterlockedExchange(&refresh_full, 1);
    SetEvent(refresh_event);
}

static void device_change_cb(UINT op, LPTSTR path)
{
    // refresh devices regardless operation type
    debug_log("device change: op=%u", op);
#ifdef UNICODE
    printf("Device operation %d with path %S\n", op, path);
#else
    printf("Device operation %d with path %s\n", op, path);
#endif
    fflush(stdout);
    request_refresh();
}

static void mi_gamepad_update_cb(int gamepad_id, struct mi_state *state)
{
    struct active_device *active_device = NULL;
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
    {
        if (active_devices[i]->src_gamepad_id == gamepad_id)
        {
            active_device = active_devices[i];
            break;
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);

    if (active_device == NULL)
    {
        return;
    }

    if (active_device->src_battery_level != state->battery)
    {
        active_device->src_battery_level = state->battery;
        refresh_tray(active_device);
    }

    if (active_device->tgt_added)
    {
        active_device->tgt_report.wButtons = 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_UP) != 0 ? XUSB_GAMEPAD_DPAD_UP : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_DOWN) != 0 ? XUSB_GAMEPAD_DPAD_DOWN : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_LEFT) != 0 ? XUSB_GAMEPAD_DPAD_LEFT : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_RIGHT) != 0 ? XUSB_GAMEPAD_DPAD_RIGHT : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_MENU) != 0 ? XUSB_GAMEPAD_START : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_RETURN) != 0 ? XUSB_GAMEPAD_BACK : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_LS) != 0 ? XUSB_GAMEPAD_LEFT_THUMB : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_RS) != 0 ? XUSB_GAMEPAD_RIGHT_THUMB : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_L1) != 0 ? XUSB_GAMEPAD_LEFT_SHOULDER : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_R1) != 0 ? XUSB_GAMEPAD_RIGHT_SHOULDER : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_A) != 0 ? XUSB_GAMEPAD_A : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_B) != 0 ? XUSB_GAMEPAD_B : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_X) != 0 ? XUSB_GAMEPAD_X : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_Y) != 0 ? XUSB_GAMEPAD_Y : 0;
        active_device->tgt_report.wButtons |= (state->buttons & MI_BUTTON_MI_BTN) != 0 ? XUSB_GAMEPAD_GUIDE : 0;
        active_device->tgt_report.bLeftTrigger = state->l2_trigger;
        active_device->tgt_report.bRightTrigger = state->r2_trigger;
        active_device->tgt_report.sThumbLX = _map_byte_to_short(state->left_stick_x, FALSE);
        active_device->tgt_report.sThumbLY = _map_byte_to_short(state->left_stick_y, TRUE);
        active_device->tgt_report.sThumbRX = _map_byte_to_short(state->right_stick_x, FALSE);
        active_device->tgt_report.sThumbRY = _map_byte_to_short(state->right_stick_y, TRUE);
        AcquireSRWLockExclusive(&vigem_lock);
        vigem_target_x360_update(vigem_client, active_device->tgt_device, active_device->tgt_report);
        ReleaseSRWLockExclusive(&vigem_lock);
    }
}

static void mi_gamepad_stop_cb(int gamepad_id, BYTE break_reason)
{
    debug_log("gamepad %d stopped, reason=0x%X", gamepad_id, break_reason);
    UINT ntf_type = break_reason == MI_BREAK_REASON_REQUESTED ? NT_TRAY_INFO : NT_TRAY_WARNING;
    LPTSTR ntf_text;
    switch (break_reason)
    {
    case MI_BREAK_REASON_REQUESTED:
        ntf_text = TEXT("Device removed successfully");
        break;
    case MI_BREAK_REASON_INIT_ERROR:
        ntf_text = TEXT("Error initializing device");
        break;
    case MI_BREAK_REASON_READ_ERROR:
        ntf_text = TEXT("Error reading data");
        break;
    case MI_BREAK_REASON_WRITE_ERROR:
        ntf_text = TEXT("Error writing data");
        break;
    default:
        ntf_text = TEXT("Unknown error");
        break;
    }
    if (remove_device(gamepad_id))
    {
        refresh_tray(NULL);
        if (!(reattaching && break_reason == MI_BREAK_REASON_REQUESTED))
        {
            tray_show_notification(ntf_type, TEXT("Remove device"), ntf_text);
        }
    }
}

static void CALLBACK x360_notification_cb(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR large_motor,
                                          UCHAR small_motor, UCHAR led_number, LPVOID user_data)
{
    struct active_device *active_device = (struct active_device *)user_data;
    // the LED number is the XInput slot (0..3); it arrives once Windows has assigned the slot
    if (led_number <= 3 && active_device->xinput_slot != (int)led_number)
    {
        active_device->xinput_slot = (int)led_number;
        debug_log("gamepad %d: xinput slot %d", active_device->src_gamepad_id, (int)led_number);
        if (active_device->tray_menu != NULL)
        {
            refresh_tray(active_device);
        }
    }
    mi_gamepad_set_vibration(active_device->src_gamepad_id, small_motor, large_motor);
}

static void pad_menu_cb(struct tray_menu *item)
{
    mi_gamepad_identify((int)(INT_PTR)item->context);
}

static void refresh_cb(struct tray_menu *item)
{
    (void)item;
    printf("Reset Gamepads\n");
    debug_log("Reset Gamepads requested");
    fflush(stdout);
    request_full_refresh();
}

#ifdef MY_VIGEM_DEBUG
static void debug_cb(struct tray_menu *item)
{
    (void)item;
    debug_log("Debug");
    debug_win_toggle();
}
#endif

static void autostart_cb(struct tray_menu *item)
{
    (void)item;
    BOOLEAN enable = !autostart_checked;
    if (autostart_set(enable))
    {
        autostart_checked = enable;
        debug_log("Autostart %s", enable ? "enabled" : "disabled");
    }
    else
    {
        debug_log("Autostart change failed");
        tray_show_notification(NT_TRAY_ERROR, TEXT("Autostart at Logon"),
                               TEXT("Could not change the autostart setting."));
    }
    refresh_tray(NULL);
}

static void github_cb(struct tray_menu *item)
{
    (void)item;
    ShellExecute(NULL, TEXT("open"), TEXT("https://github.com/murtisoft/my-vigem"), NULL, NULL, SW_SHOWNORMAL);
}

static void quit_cb(struct tray_menu *item)
{
    (void)item;
    printf("Quit\n");
    fflush(stdout);
    tray_exit();
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--debug") == 0)
        {
            debug_console_enable();
        }
    }
    autostart_checked = autostart_is_enabled();
    rebuild_tray_menu();
    int tray_res = tray_init(&tray);
    if (tray_res > 0)
    {
        // already running, quit silently
        return 0;
    }
    if (tray_res < 0)
    {
        printf("Failed to create tray\n");
        return 1;
    }
    mi_set_raw_cb(debug_win_set_report);
    vigem_client = vigem_alloc();
    VIGEM_ERROR vigem_res = vigem_connect(vigem_client);
    if (vigem_res == VIGEM_ERROR_BUS_NOT_FOUND)
    {
        tray_show_notification(NT_TRAY_ERROR, TEXT("ViGEmBus connection"),
                               TEXT("ViGEmBus not installed"));
    }
    else if (vigem_res == VIGEM_ERROR_BUS_VERSION_MISMATCH)
    {
        tray_show_notification(NT_TRAY_ERROR, TEXT("ViGEmBus connection"),
                               TEXT("ViGEmBus incompatible version"));
    }
    else if (vigem_res != VIGEM_ERROR_NONE)
    {
        tray_show_notification(NT_TRAY_ERROR, TEXT("ViGEmBus connection"),
                               TEXT("Error connecting to ViGEmBus"));
    }
    else
    {
        vigem_connected = TRUE;
    }
    int hid_hide_res = hid_hide_init();
    if (hid_hide_res == HID_HIDE_INIT_ERROR)
    {
        tray_show_notification(NT_TRAY_ERROR, TEXT("HidHide connection"),
                               TEXT("Error initializing HidHide"));
    }
    else if (hid_hide_res == HID_HIDE_RESULT_OK)
    {
        hid_hide_connected = TRUE;
    }
    refresh_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    refresh_thread = CreateThread(NULL, 0, refresh_thread_proc, NULL, 0, NULL);
    request_refresh();
    tray_register_device_notification(hid_get_interface_guid(), device_change_cb);

    while (tray_loop(TRUE) == 0)
    {
        ;
    }

    InterlockedExchange(&refresh_quit, 1);
    SetEvent(refresh_event);
    WaitForSingleObject(refresh_thread, 15000);
    CloseHandle(refresh_thread);
    CloseHandle(refresh_event);

    // stop the pads first: their threads call back into this file and into ViGEm
    stop_all_devices();
    BOOLEAN pads_left = FALSE;
    AcquireSRWLockShared(&active_devices_lock);
    pads_left = active_device_count > 0;
    for (int i = 0; i < active_device_count; i++)
    {
        if (active_devices[i]->hid_hidden)
        {
            hid_hide_unbind(active_devices[i]->src_device->device_info);
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);
    if (!pads_left)
    {
        // a pad that did not stop in time keeps using ViGEm: skip the teardown, the process ends anyway
        AcquireSRWLockExclusive(&vigem_lock);
        if (vigem_connected)
        {
            vigem_disconnect(vigem_client);
        }
        vigem_free(vigem_client);
        ReleaseSRWLockExclusive(&vigem_lock);
    }
    if (hid_hide_connected)
    {
        hid_hide_free();
    }
    mi_set_raw_cb(NULL);
    debug_win_free();
    if (!pads_left)
    {
        free(tray.menu);
    }
    return 0;
}
