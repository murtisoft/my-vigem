#include <tchar.h>
#include <windows.h>

#include "hid_hide.h"
#include "utils.h"

#pragma comment(lib, "kernel32.lib")

// The Hid Hide I/O control custom device type (range 32768 .. 65535)
#define IO_CONTROL_DEVICE_TYPE 32769u

// The Hid Hide I/O control codes
#define IOCTL_GET_WHITELIST CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2048, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_SET_WHITELIST CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2049, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_GET_BLACKLIST CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2050, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_SET_BLACKLIST CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2051, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_GET_ACTIVE    CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2052, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_SET_ACTIVE    CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2053, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_GET_WLINVERSE CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2054, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_SET_WLINVERSE CTL_CODE(IO_CONTROL_DEVICE_TYPE, 2055, METHOD_BUFFERED, FILE_READ_DATA)

struct hh_str_arr;

struct hh_str_arr
{
    LPTSTR value;

    struct hh_str_arr *next;
};

// A device we bound: its blacklist entries are removed again on unbind (the parent only when no other bound
// interface of the same parent is left)
struct hh_bound
{
    LPTSTR instance;
    LPTSTR parent;

    struct hh_bound *next;
};

struct hid_hide_ctx
{
    HANDLE handle;

    BOOLEAN initially_active;
    BOOLEAN active;
    struct hh_str_arr *black_list;
    struct hh_str_arr *white_list;
    BOOLEAN inverse;
    struct hh_bound *bound;
    // our own whitelist entry, set only when we added it (removed again on exit)
    LPTSTR added_whitelist_entry;
};

static struct hid_hide_ctx *hh_ctx = NULL;
// guards hh_ctx and its lists: bind runs on the worker, unbind on the pad input threads, free on exit
static SRWLOCK hh_lock = SRWLOCK_INIT;

static struct hh_str_arr* _hh_multi_string_to_arr(LPWSTR input_str, DWORD input_len)
{
    struct hh_str_arr *head = NULL, *tail = NULL;
    for (DWORD i = 0, start = 0; i < input_len; i++)
    {
        if (input_str[i] == 0)
        {
            if (i > start)
            {
                struct hh_str_arr *cur = (struct hh_str_arr *)malloc(sizeof(struct hh_str_arr));
#ifdef UNICODE
                cur->value = (LPWSTR)malloc((i - start + 1) * sizeof(WCHAR));
                wcscpy(cur->value, &input_str[start]);
#else
                int value_size = WideCharToMultiByte(CP_ACP, 0, &input_str[start], -1, NULL, 0, NULL, NULL);
                cur->value = (LPSTR)malloc(value_size);
                WideCharToMultiByte(CP_ACP, 0, &input_str[start], -1, cur->value, value_size, NULL, NULL);
#endif /* UNICODE */
                cur->next = NULL;

                if (head == NULL)
                {
                    head = cur;
                }
                else
                {
                    tail->next = cur;
                }
                tail = cur;
            }
            start = i + 1;
        }
    }
    return head;
}

static LPWSTR _hh_arr_to_multi_string(struct hh_str_arr* input_arr, LPDWORD output_len)
{
    struct hh_str_arr *cur = input_arr;
    LPWSTR buffer = NULL;
    DWORD allocated_len = 0;
    
    while (cur)
    {
        size_t len_to_alloc = _tcslen(cur->value) + 1;
        LPWSTR grown = (LPWSTR)realloc(buffer, (allocated_len + len_to_alloc) * sizeof(WCHAR));
        if (grown == NULL)
        {
            free(buffer);
            return NULL;
        }
        buffer = grown;
#ifdef UNICODE
        wcscpy(&buffer[allocated_len], cur->value);
#else
        MultiByteToWideChar(CP_ACP, 0, cur->value, -1, &buffer[allocated_len], len_to_alloc);
#endif /* UNICODE */
        allocated_len += len_to_alloc;
        cur = cur->next;
    }

    // add one extra trailing \0
    LPWSTR terminated = (LPWSTR)realloc(buffer, (allocated_len + 1) * sizeof(WCHAR));
    if (terminated == NULL)
    {
        free(buffer);
        return NULL;
    }
    buffer = terminated;
    buffer[allocated_len] = L'\0';
    allocated_len++;

    *output_len = allocated_len;
    return buffer;
}

static BOOLEAN _hh_in_str_arr(struct hh_str_arr* arr, LPTSTR value)
{
    while (arr)
    {
        // Windows paths and instance IDs are case-insensitive
        if (_tcsicmp(arr->value, value) == 0)
        {
            return TRUE;
        }
        arr = arr->next;
    }
    return FALSE;
}

static void _hh_free_str_arr(struct hh_str_arr* arr)
{
    struct hh_str_arr* cur;
    while (arr)
    {
        cur = arr->next;
        free(arr->value);
        free(arr);
        arr = cur;
    }
}

static void _hh_free_bound(struct hh_bound *bound)
{
    while (bound)
    {
        struct hh_bound *next = bound->next;
        free(bound->instance);
        free(bound->parent);
        free(bound);
        bound = next;
    }
}

static BOOLEAN _hh_get_flag(HANDLE dev_handle, DWORD ioctl, BOOLEAN *value)
{
    DWORD bytes_returned = 0;
    BOOLEAN output_buffer[1];
    if (!DeviceIoControl(dev_handle, ioctl, NULL, 0, output_buffer, sizeof(output_buffer), &bytes_returned, NULL)
        || bytes_returned != sizeof(output_buffer))
    {
        return FALSE;
    }
    *value = (output_buffer[0] != FALSE);
    return TRUE;
}

static BOOLEAN _hh_set_flag(HANDLE dev_handle, DWORD ioctl, BOOLEAN value)
{
    DWORD bytes_returned = 0;
    BOOLEAN input_buffer[1];
    input_buffer[0] = (value ? TRUE : FALSE);
    return DeviceIoControl(dev_handle, ioctl, input_buffer, sizeof(input_buffer), NULL, 0, &bytes_returned, NULL) ? TRUE : FALSE;
}

// Reads a list. Returns FALSE on failure; an empty list is a success with *list == NULL.
static BOOLEAN _hh_get_list(HANDLE dev_handle, DWORD ioctl, struct hh_str_arr **list)
{
    DWORD bytes_returned = 0;
    *list = NULL;
    if (!DeviceIoControl(dev_handle, ioctl, NULL, 0, NULL, 0, &bytes_returned, NULL))
    {
        return FALSE;
    }
    if (bytes_returned == 0)
    {
        return TRUE;
    }
    LPWSTR output_buffer = (LPWSTR)malloc(bytes_returned);
    if (output_buffer == NULL)
    {
        return FALSE;
    }
    if (!DeviceIoControl(dev_handle, ioctl, NULL, 0, output_buffer, bytes_returned, &bytes_returned, NULL))
    {
        free(output_buffer);
        return FALSE;
    }
    *list = _hh_multi_string_to_arr(output_buffer, bytes_returned / sizeof(WCHAR));
    free(output_buffer);
    return TRUE;
}

static BOOLEAN _hh_set_list(HANDLE dev_handle, DWORD ioctl, struct hh_str_arr *list)
{
    DWORD bytes_returned = 0, chars = 0;
    LPWSTR input_buffer = _hh_arr_to_multi_string(list, &chars);
    if (input_buffer == NULL)
    {
        return FALSE;
    }
    BOOLEAN ok = DeviceIoControl(dev_handle, ioctl, input_buffer, chars * sizeof(WCHAR), NULL, 0, &bytes_returned, NULL) ? TRUE : FALSE;
    free(input_buffer);
    return ok;
}

static struct hh_str_arr *_hh_new_el(LPCTSTR value)
{
    struct hh_str_arr *el = (struct hh_str_arr *)malloc(sizeof(struct hh_str_arr));
    if (el == NULL)
    {
        return NULL;
    }
    el->value = (LPTSTR)malloc((_tcslen(value) + 1) * sizeof(TCHAR));
    if (el->value == NULL)
    {
        free(el);
        return NULL;
    }
    _tcscpy(el->value, value);
    el->next = NULL;
    return el;
}

// Removes the whitelist entry that init added for our exe and writes the list back (no-op if we did not add one).
static void _hh_remove_added_whitelist(struct hid_hide_ctx *ctx)
{
    if (ctx->added_whitelist_entry == NULL)
    {
        return;
    }
    struct hh_str_arr *cur = ctx->white_list, *prev = NULL;
    while (cur)
    {
        if (_tcsicmp(cur->value, ctx->added_whitelist_entry) == 0)
        {
            if (prev != NULL)
            {
                prev->next = cur->next;
            }
            else
            {
                ctx->white_list = cur->next;
            }
            free(cur->value);
            free(cur);
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    free(ctx->added_whitelist_entry);
    ctx->added_whitelist_entry = NULL;
    if (!_hh_set_list(ctx->handle, IOCTL_SET_WHITELIST, ctx->white_list))
    {
        debug_log("hid_hide: removing our whitelist entry failed");
    }
}

static void _hh_ctx_free(struct hid_hide_ctx *ctx)
{
    CloseHandle(ctx->handle);
    _hh_free_str_arr(ctx->black_list);
    _hh_free_str_arr(ctx->white_list);
    _hh_free_bound(ctx->bound);
    free(ctx->added_whitelist_entry);
    free(ctx);
}

static LPTSTR _hh_get_app_image_name()
{
    LPTSTR result = NULL;
    // UNICODE_STRING_MAX_CHARS wide chars are 64 KB each: the long buffers live on the heap, not on the stack
    LPWSTR module_filename = (LPWSTR)malloc(UNICODE_STRING_MAX_CHARS * sizeof(WCHAR));
    LPWSTR volume_path_names = (LPWSTR)malloc(UNICODE_STRING_MAX_CHARS * sizeof(WCHAR));
    LPWSTR dos_device_name = (LPWSTR)malloc(UNICODE_STRING_MAX_CHARS * sizeof(WCHAR));
    LPWSTR mount_point = NULL;
    WCHAR volume_name[MAX_PATH];
    DWORD module_len = 0;
    if (module_filename != NULL && volume_path_names != NULL && dos_device_name != NULL)
    {
        module_len = GetModuleFileNameW(NULL, module_filename, UNICODE_STRING_MAX_CHARS);
    }
    // 0 = failed, a full buffer = truncated path
    if (module_len > 0 && module_len < UNICODE_STRING_MAX_CHARS)
    {
        HANDLE fv_handle = FindFirstVolumeW(volume_name, MAX_PATH);
        if (fv_handle != INVALID_HANDLE_VALUE)
        {
            while (TRUE)
            {
                DWORD chars_returned;
                if (GetVolumePathNamesForVolumeNameW(volume_name, volume_path_names, UNICODE_STRING_MAX_CHARS, &chars_returned))
                {
                    for (DWORD i = 0, start = 0; i < chars_returned; i++)
                    {
                        if (volume_path_names[i] == 0)
                        {
                            if (i > start)
                            {
                                // the longest mount point that is a prefix of our path (paths are case-insensitive)
                                if ((_wcsnicmp(module_filename, &volume_path_names[start], i - start) == 0)
                                    && (mount_point == NULL || i - start > wcslen(mount_point)))
                                {
                                    LPWSTR grown = (LPWSTR)realloc(mount_point, (i - start + 1) * sizeof(WCHAR));
                                    if (grown != NULL)
                                    {
                                        mount_point = grown;
                                        wcscpy(mount_point, &volume_path_names[start]);
                                    }
                                }
                            }
                            start = i + 1;
                        }
                    }
                }
                if (!FindNextVolumeW(fv_handle, volume_name, MAX_PATH))
                {
                    break;
                }
            }
            FindVolumeClose(fv_handle);
        }
        if (mount_point != NULL && GetVolumeNameForVolumeMountPointW(mount_point, volume_name, MAX_PATH)
            && wcslen(volume_name) > 5)
        {
            WCHAR stripped_volume_name[MAX_PATH];
            // Strip the leading '\\?\' and trailing '\' and isolate the Volume{} part in the volume name
            size_t stripped_len = wcslen(volume_name) - 5;
            wcsncpy(stripped_volume_name, volume_name + 4, stripped_len);
            stripped_volume_name[stripped_len] = L'\0';
            if (QueryDosDeviceW(stripped_volume_name, dos_device_name, UNICODE_STRING_MAX_CHARS))
            {
                // the path of our exe below the mount point
                const WCHAR *filename_wo_mount_point = module_filename + wcslen(mount_point);
                LPWSTR result_w = (LPWSTR)malloc((wcslen(dos_device_name) + 1 + wcslen(filename_wo_mount_point) + 1) * sizeof(WCHAR));
                if (result_w != NULL)
                {
                    wcscat(wcscat(wcscpy(result_w, dos_device_name), L"\\"), filename_wo_mount_point);
#ifdef UNICODE
                    result = result_w;
#else
                    int result_size = WideCharToMultiByte(CP_ACP, 0, result_w, -1, NULL, 0, NULL, NULL);
                    result = (LPSTR)malloc(result_size);
                    if (result != NULL)
                    {
                        WideCharToMultiByte(CP_ACP, 0, result_w, -1, result, result_size, NULL, NULL);
                    }
                    free(result_w);
#endif /* UNICODE */
                }
            }
        }
    }
    free(mount_point);
    free(module_filename);
    free(volume_path_names);
    free(dos_device_name);
    return result;
}

int hid_hide_init()
{
    HANDLE handle = CreateFile(HID_HIDE_HW_PATH, GENERIC_READ, (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE),
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle == INVALID_HANDLE_VALUE)
    {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
        {
            return HID_HIDE_NOT_INSTALLED; // HidHide driver not installed
        }
        else
        {
            return HID_HIDE_INIT_ERROR; // other error
        }
    }

    struct hid_hide_ctx *ctx = (struct hid_hide_ctx *)calloc(1, sizeof(struct hid_hide_ctx));
    if (ctx == NULL)
    {
        CloseHandle(handle);
        return HID_HIDE_INIT_ERROR;
    }
    ctx->handle = handle;

    // flags that cannot be read stay FALSE; a list that cannot be read must not pass for an empty one, because
    // writing it back would wipe the user's entries
    _hh_get_flag(handle, IOCTL_GET_ACTIVE, &ctx->active);
    _hh_get_flag(handle, IOCTL_GET_WLINVERSE, &ctx->inverse);
    ctx->initially_active = ctx->active;
    if (!_hh_get_list(handle, IOCTL_GET_BLACKLIST, &ctx->black_list)
        || !_hh_get_list(handle, IOCTL_GET_WHITELIST, &ctx->white_list))
    {
        debug_log("hid_hide_init: reading the HidHide lists failed, hiding disabled");
        _hh_ctx_free(ctx);
        return HID_HIDE_INIT_ERROR;
    }

    // check if our app whitelisted
    LPTSTR app_image_name = _hh_get_app_image_name();
    if (app_image_name == NULL)
    {
        // without our own image name we cannot whitelist ourselves, and hiding would lock us out of the pads
        _hh_ctx_free(ctx);
        return HID_HIDE_INIT_ERROR;
    }
    BOOLEAN app_whitelisted = _hh_in_str_arr(ctx->white_list, app_image_name);
    BOOLEAN lists_ok = TRUE;
    if (!ctx->inverse && !app_whitelisted)
    {
        // add app to the white list
        struct hh_str_arr *app_el = (struct hh_str_arr *)malloc(sizeof(struct hh_str_arr));
        if (app_el != NULL)
        {
            // remember our entry so exit can remove it again (stays NULL if the copy fails: the entry is then kept)
            ctx->added_whitelist_entry = (LPTSTR)malloc((_tcslen(app_image_name) + 1) * sizeof(TCHAR));
            if (ctx->added_whitelist_entry != NULL)
            {
                _tcscpy(ctx->added_whitelist_entry, app_image_name);
            }
            app_el->value = app_image_name;
            app_el->next = ctx->white_list;
            ctx->white_list = app_el;
            app_image_name = NULL; // owned by the list now
            lists_ok = _hh_set_list(ctx->handle, IOCTL_SET_WHITELIST, ctx->white_list);
        }
        else
        {
            lists_ok = FALSE;
        }
    }
    else if (ctx->inverse && app_whitelisted)
    {
        // remove app from the white list
        struct hh_str_arr *cur = ctx->white_list, *prev = NULL;
        while (cur)
        {
            if (_tcsicmp(cur->value, app_image_name) == 0)
            {
                if (prev != NULL)
                {
                    prev->next = cur->next;
                }
                else
                {
                    ctx->white_list = cur->next;
                }
                free(cur->value);
                free(cur);
                break;
            }
            prev = cur;
            cur = cur->next;
        }
        lists_ok = _hh_set_list(ctx->handle, IOCTL_SET_WHITELIST, ctx->white_list);
    }
    free(app_image_name);

    // activate HidHide, but only if we are really whitelisted, otherwise hiding would lock us out of the pads
    if (!lists_ok || (!ctx->active && !_hh_set_flag(ctx->handle, IOCTL_SET_ACTIVE, TRUE)))
    {
        debug_log("hid_hide_init: writing the HidHide settings failed, hiding disabled");
        _hh_remove_added_whitelist(ctx);
        _hh_ctx_free(ctx);
        return HID_HIDE_INIT_ERROR;
    }
    ctx->active = TRUE;

    AcquireSRWLockExclusive(&hh_lock);
    hh_ctx = ctx;
    ReleaseSRWLockExclusive(&hh_lock);
    return HID_HIDE_RESULT_OK;
}

// hh_lock held
static int _hh_bind_locked(struct hid_device_info *dev_info)
{
    // bound already (the same device added twice)
    for (struct hh_bound *b = hh_ctx->bound; b != NULL; b = b->next)
    {
        if (_tcsicmp(b->instance, dev_info->instance_path) == 0)
        {
            return HID_HIDE_RESULT_OK;
        }
    }

    // the bound record is made first, so a failed allocation changes nothing
    struct hh_bound *bound = (struct hh_bound *)calloc(1, sizeof(struct hh_bound));
    if (bound == NULL)
    {
        return HID_HIDE_INIT_ERROR;
    }
    bound->instance = (LPTSTR)malloc((_tcslen(dev_info->instance_path) + 1) * sizeof(TCHAR));
    if (bound->instance != NULL)
    {
        _tcscpy(bound->instance, dev_info->instance_path);
    }
    if (dev_info->container_instance_path != NULL)
    {
        bound->parent = (LPTSTR)malloc((_tcslen(dev_info->container_instance_path) + 1) * sizeof(TCHAR));
        if (bound->parent != NULL)
        {
            _tcscpy(bound->parent, dev_info->container_instance_path);
        }
    }
    if (bound->instance == NULL || (dev_info->container_instance_path != NULL && bound->parent == NULL))
    {
        _hh_free_bound(bound);
        return HID_HIDE_INIT_ERROR;
    }

    struct hh_str_arr *cur = hh_ctx->black_list, *prev = NULL;
    while (cur)
    {
        if (_tcsicmp(cur->value, dev_info->instance_path) == 0)
        {
            // already in the black list (e.g. left by a crash): it is ours to remove on unbind
            bound->next = hh_ctx->bound;
            hh_ctx->bound = bound;
            return HID_HIDE_RESULT_OK;
        }
        prev = cur;
        cur = cur->next;
    }

    struct hh_str_arr *dev_el = _hh_new_el(dev_info->instance_path);
    if (dev_el == NULL)
    {
        _hh_free_bound(bound);
        return HID_HIDE_INIT_ERROR;
    }

    // Since Xiaomi Gamepad doesn't have other sibling devices within BTENUM container
    // in Windows configuration manager, we can add parent container device to black list too.
    // However, this is not generally true for other gamepad vendors.
    // (This logic based on official HidHideClient behavior)
    // A parent that is listed already (another interface of the same pad) is not added twice.
    if (dev_info->container_instance_path != NULL && !_hh_in_str_arr(hh_ctx->black_list, dev_info->container_instance_path))
    {
        dev_el->next = _hh_new_el(dev_info->container_instance_path);
        if (dev_el->next == NULL)
        {
            _hh_free_str_arr(dev_el);
            _hh_free_bound(bound);
            return HID_HIDE_INIT_ERROR;
        }
    }

    if (prev != NULL)
    {
        prev->next = dev_el;
    }
    else
    {
        hh_ctx->black_list = dev_el;
    }

    if (!_hh_set_list(hh_ctx->handle, IOCTL_SET_BLACKLIST, hh_ctx->black_list))
    {
        // the driver kept its old list: undo the change in ours (prev is the old tail)
        debug_log("hid_hide_bind: writing the blacklist failed");
        if (prev != NULL)
        {
            prev->next = NULL;
        }
        else
        {
            hh_ctx->black_list = NULL;
        }
        _hh_free_str_arr(dev_el);
        _hh_free_bound(bound);
        return HID_HIDE_INIT_ERROR;
    }
    bound->next = hh_ctx->bound;
    hh_ctx->bound = bound;
    return HID_HIDE_RESULT_OK;
}

int hid_hide_bind(struct hid_device_info *dev_info)
{
    int result = HID_HIDE_NOT_INITIALIZED; // HidHide wasn't properly initialized
    AcquireSRWLockExclusive(&hh_lock);
    if (hh_ctx != NULL)
    {
        result = _hh_bind_locked(dev_info);
    }
    ReleaseSRWLockExclusive(&hh_lock);
    return result;
}

void hid_hide_unbind(struct hid_device_info *dev_info)
{
    AcquireSRWLockExclusive(&hh_lock);
    if (hh_ctx != NULL)
    {
        // drop the device from the bound list and see whether another bound interface shares its parent
        BOOLEAN parent_shared = FALSE;
        struct hh_bound *cur_b = hh_ctx->bound, *prev_b = NULL;
        while (cur_b)
        {
            struct hh_bound *next_b = cur_b->next;
            if (_tcsicmp(cur_b->instance, dev_info->instance_path) == 0)
            {
                if (prev_b != NULL)
                {
                    prev_b->next = next_b;
                }
                else
                {
                    hh_ctx->bound = next_b;
                }
                free(cur_b->instance);
                free(cur_b->parent);
                free(cur_b);
            }
            else
            {
                if (dev_info->container_instance_path != NULL && cur_b->parent != NULL
                    && _tcsicmp(cur_b->parent, dev_info->container_instance_path) == 0)
                {
                    parent_shared = TRUE;
                }
                prev_b = cur_b;
            }
            cur_b = next_b;
        }

        struct hh_str_arr *cur = hh_ctx->black_list, *prev = NULL;
        while (cur)
        {
            if (_tcsicmp(cur->value, dev_info->instance_path) == 0
                || (!parent_shared && dev_info->container_instance_path != NULL
                    && _tcsicmp(cur->value, dev_info->container_instance_path) == 0))
            {
                if (prev != NULL)
                {
                    prev->next = cur->next;
                }
                else
                {
                    hh_ctx->black_list = cur->next;
                }
                struct hh_str_arr *tmp = cur->next;
                free(cur->value);
                free(cur);
                cur = tmp;
            }
            else
            {
                prev = cur;
                cur = cur->next;
            }
        }

        if (!_hh_set_list(hh_ctx->handle, IOCTL_SET_BLACKLIST, hh_ctx->black_list))
        {
            debug_log("hid_hide_unbind: writing the blacklist failed");
        }
    }
    ReleaseSRWLockExclusive(&hh_lock);
}

void hid_hide_free()
{
    AcquireSRWLockExclusive(&hh_lock);
    struct hid_hide_ctx *ctx = hh_ctx;
    hh_ctx = NULL;
    ReleaseSRWLockExclusive(&hh_lock);

    if (ctx != NULL)
    {
        // restore initial state of HidHide: our whitelist entry goes, the active flag returns to its old value
        _hh_remove_added_whitelist(ctx);
        if (!ctx->initially_active)
        {
            _hh_set_flag(ctx->handle, IOCTL_SET_ACTIVE, FALSE);
        }
        _hh_ctx_free(ctx);
    }
}
