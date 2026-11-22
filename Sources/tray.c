#include <stdlib.h>
#include <tchar.h>
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>

#include "tray.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

#define WM_TRAY_CALLBACK_MESSAGE (WM_USER + 1)
// a freshly built menu handed over by tray_update (lparam = struct menu_bundle *)
#define WM_TRAY_SWAP (WM_USER + 2)
#define WC_TRAY_CLASS_NAME TEXT("MiViGEmClass")
#define WC_TRAY_MUTEX_NAME TEXT("MI-VIGEM")
#define ID_TRAY_FIRST 1000

// A menu item is copied when the menu is built, so the caller's item array may be freed right after tray_update.
struct menu_copy
{
    struct tray_menu item;
    struct menu_copy *next;
};

struct menu_bundle
{
    HMENU hmenu;
    struct menu_copy *copies;
};

static WNDCLASSEX wc;
static NOTIFYICONDATA nid;
static HWND hwnd = NULL;
// tray thread only: the menu in use, a newer one waiting for the open popup to close, and the popup state
static struct menu_bundle *cur_menu = NULL;
static struct menu_bundle *pending_menu = NULL;
static BOOLEAN popup_open = FALSE;
static HANDLE hmutex;
// guards nid and hwnd between tray_show_notification (any thread) and tray_exit
static SRWLOCK nid_lock = SRWLOCK_INIT;
static HDEVNOTIFY hdevntf;
static void (*devntf_cb)(UINT op, LPTSTR path) = NULL;

static void _tray_free_bundle(struct menu_bundle *bundle)
{
    if (bundle == NULL)
    {
        return;
    }
    if (bundle->hmenu != NULL)
    {
        DestroyMenu(bundle->hmenu);
    }
    while (bundle->copies != NULL)
    {
        struct menu_copy *next = bundle->copies->next;
        free(bundle->copies);
        bundle->copies = next;
    }
    free(bundle);
}

static LRESULT CALLBACK _tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CLOSE:
        if (hdevntf != NULL)
        {
            UnregisterDeviceNotification(hdevntf);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_TRAY_CALLBACK_MESSAGE:
        if ((lparam == WM_LBUTTONUP || lparam == WM_RBUTTONUP) && cur_menu != NULL && !popup_open)
        {
            POINT p;
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            struct menu_bundle *shown = cur_menu;
            popup_open = TRUE;
            UINT cmd = TrackPopupMenu(shown->hmenu, TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                      p.x, p.y, 0, hwnd, NULL);
            // resolve the click in the menu that was shown (a rebuilt menu has other command IDs)
            struct tray_menu picked = { 0 };
            BOOLEAN has_pick = FALSE;
            if (cmd >= ID_TRAY_FIRST)
            {
                MENUITEMINFO item = {
                    .cbSize = sizeof(MENUITEMINFO),
                    .fMask = MIIM_DATA,
                };
                if (GetMenuItemInfo(shown->hmenu, cmd, FALSE, &item) && item.dwItemData != 0)
                {
                    picked = *(struct tray_menu *)item.dwItemData;
                    has_pick = TRUE;
                }
            }
            popup_open = FALSE;
            // without this the menu may stay open after a click outside of it
            PostMessage(hwnd, WM_NULL, 0, 0);
            if (pending_menu != NULL)
            {
                _tray_free_bundle(cur_menu);
                cur_menu = pending_menu;
                pending_menu = NULL;
            }
            if (has_pick && picked.cb != NULL)
            {
                picked.cb(&picked);
            }
            return 0;
        }
        break;
    case WM_TRAY_SWAP:
        if (popup_open)
        {
            // the popup is still showing the old menu, it must not be destroyed under the user
            _tray_free_bundle(pending_menu);
            pending_menu = (struct menu_bundle *)lparam;
        }
        else
        {
            _tray_free_bundle(cur_menu);
            cur_menu = (struct menu_bundle *)lparam;
        }
        return 0;
    case WM_DEVICECHANGE:
        if (devntf_cb != NULL)
        {
            UINT op = DO_TRAY_UNKNOWN;
            switch (wparam)
            {
            case DBT_DEVICEARRIVAL:
                op = DO_TRAY_DEV_ATTACHED;
                break;
            case DBT_DEVICEREMOVECOMPLETE:
                op = DO_TRAY_DEV_REMOVED;
                break;
            }
            LPTSTR path = NULL;
            if (lparam != 0)
            {
                // other broadcasts (volumes, ports, ...) carry a different structure: ignore them
                if (((PDEV_BROADCAST_HDR)lparam)->dbch_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
                {
                    break;
                }
                PDEV_BROADCAST_DEVICEINTERFACE bdi = (PDEV_BROADCAST_DEVICEINTERFACE)lparam;
                path = bdi->dbcc_name;
            }
            devntf_cb(op, path);
        }
        break;
    }
    return DefWindowProc(hwnd, msg, wparam, lparam);
}

static HMENU _tray_menu(struct tray_menu *m, UINT *id, struct menu_copy **copies)
{
    HMENU menu = CreatePopupMenu();
    for (; m != NULL && m->text != NULL; m++, (*id)++)
    {
        if (_tcscmp(m->text, TEXT("-")) == 0)
        {
            InsertMenu(menu, *id, MF_SEPARATOR, TRUE, TEXT(""));
        }
        else
        {
            MENUITEMINFO item;
            item = (MENUITEMINFO){ 0 };
            item.cbSize = sizeof(MENUITEMINFO);
            item.fMask = MIIM_ID | MIIM_TYPE | MIIM_STATE | MIIM_DATA;
            item.fType = 0;
            item.fState = 0;
            if (m->submenu != NULL)
            {
                item.fMask = item.fMask | MIIM_SUBMENU;
                item.hSubMenu = _tray_menu(m->submenu, id, copies);
            }
            if (m->disabled)
            {
                item.fState |= MFS_DISABLED;
            }
            if (m->checked)
            {
                item.fState |= MFS_CHECKED;
            }
            item.wID = *id;
            item.dwTypeData = m->text;
            struct menu_copy *copy = (struct menu_copy *)malloc(sizeof(struct menu_copy));
            if (copy != NULL)
            {
                copy->item = *m;
                copy->item.text = NULL; // the text pointer would dangle, the menu keeps its own copy
                copy->next = *copies;
                *copies = copy;
                item.dwItemData = (ULONG_PTR)&copy->item;
            }

            InsertMenuItem(menu, *id, TRUE, &item);
        }
    }
    return menu;
}

int tray_init(struct tray *tray)
{
    hmutex = CreateMutex(NULL, TRUE, WC_TRAY_MUTEX_NAME);
    if (hmutex == NULL)
    {
        return -1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // another instance is already running
        CloseHandle(hmutex);
        hmutex = NULL;
        return 1;
    }

    wc = (WNDCLASSEX){ 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = _tray_wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = WC_TRAY_CLASS_NAME;
    if (!RegisterClassEx(&wc))
    {
        return -1;
    }

    hwnd = CreateWindowEx(0, WC_TRAY_CLASS_NAME, NULL, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (hwnd == NULL)
    {
        return -1;
    }
    UpdateWindow(hwnd);

    nid = (NOTIFYICONDATA){ 0 };
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = hwnd;
    nid.uID = 0;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK_MESSAGE;
    nid.hIcon = LoadIcon(wc.hInstance, tray->icon);
    lstrcpyn(nid.szTip, tray->tip, ARRAYSIZE(nid.szTip));
    Shell_NotifyIcon(NIM_ADD, &nid);

    tray_update(tray);
    return 0;
}

int tray_loop(BOOLEAN blocking)
{
    MSG msg;
    if (blocking)
    {
        // 0 = WM_QUIT, -1 = error: both end the loop
        if (GetMessage(&msg, NULL, 0, 0) <= 0)
        {
            return -1;
        }
    }
    else if (!PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
    {
        return 0;
    }
    if (msg.message == WM_QUIT)
    {
        return -1;
    }
    TranslateMessage(&msg);
    DispatchMessage(&msg);
    return 0;
}

// Callable from any thread: the menu is built here from tray->menu (which the caller may free afterwards)
// and handed to the tray thread, which swaps it in. Never blocks on the tray thread.
void tray_update(struct tray *tray)
{
    HWND target = hwnd;
    if (target == NULL)
    {
        return;
    }
    struct menu_bundle *bundle = (struct menu_bundle *)calloc(1, sizeof(struct menu_bundle));
    if (bundle == NULL)
    {
        return;
    }
    UINT id = ID_TRAY_FIRST;
    bundle->hmenu = _tray_menu(tray->menu, &id, &bundle->copies);
    if (!PostMessage(target, WM_TRAY_SWAP, 0, (LPARAM)bundle))
    {
        _tray_free_bundle(bundle);
    }
}

void tray_exit()
{
    AcquireSRWLockExclusive(&nid_lock);
    Shell_NotifyIcon(NIM_DELETE, &nid);
    hwnd = NULL;
    ReleaseSRWLockExclusive(&nid_lock);
    if (nid.hIcon != 0)
    {
        DestroyIcon(nid.hIcon);
    }
    _tray_free_bundle(cur_menu);
    cur_menu = NULL;
    _tray_free_bundle(pending_menu);
    pending_menu = NULL;
    PostQuitMessage(0);
    UnregisterClass(WC_TRAY_CLASS_NAME, GetModuleHandle(NULL));
    ReleaseMutex(hmutex);
    CloseHandle(hmutex);
}

void tray_register_device_notification(GUID filter, void (*cb)(UINT, LPTSTR))
{
    if (hwnd == NULL)
    {
        return;
    }

    DEV_BROADCAST_DEVICEINTERFACE dbdi;
    dbdi = (DEV_BROADCAST_DEVICEINTERFACE){ 0 };
    dbdi.dbcc_size = sizeof(dbdi);
    dbdi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbdi.dbcc_classguid = filter;

    hdevntf = RegisterDeviceNotification(hwnd, &dbdi, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (hdevntf != NULL)
    {
        devntf_cb = cb;
    }
}

void tray_show_notification(UINT type, LPTSTR title, LPTSTR text)
{
    // copy nid under the lock (tray_exit changes it), bounded: title and text may be longer than the fields
    NOTIFYICONDATA nid_info;
    AcquireSRWLockShared(&nid_lock);
    BOOLEAN have_icon = hwnd != NULL;
    nid_info = nid;
    ReleaseSRWLockShared(&nid_lock);
    if (!have_icon)
    {
        return;
    }

    nid_info.uFlags |= NIF_INFO;
    lstrcpyn(nid_info.szInfoTitle, title != NULL ? title : TEXT(""), ARRAYSIZE(nid_info.szInfoTitle));
    lstrcpyn(nid_info.szInfo, text != NULL ? text : TEXT(""), ARRAYSIZE(nid_info.szInfo));
    switch (type)
    {
    case NT_TRAY_INFO:
        nid_info.dwInfoFlags = NIIF_INFO;
        break;
    case NT_TRAY_WARNING:
        nid_info.dwInfoFlags = NIIF_WARNING;
        break;
    case NT_TRAY_ERROR:
        nid_info.dwInfoFlags = NIIF_ERROR;
        break;
    }
    nid_info.uTimeout = 10000;
    Shell_NotifyIcon(NIM_MODIFY, &nid_info);
}
