#include <string.h>
#include <tchar.h>
#include <windows.h>

#include "autostart.h"

#define RUN_KEY TEXT("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
#define RUN_VALUE TEXT("My-ViGEm")
#define COMMAND_CAPACITY (MAX_PATH + 3)

/* Quoted path of the running exe, as stored in the Run key. */
static BOOLEAN _get_command(TCHAR *buffer)
{
    TCHAR path[MAX_PATH];
    DWORD length = GetModuleFileName(NULL, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
    {
        return FALSE;
    }
    buffer[0] = TEXT('"');
    memcpy(buffer + 1, path, length * sizeof(TCHAR));
    buffer[length + 1] = TEXT('"');
    buffer[length + 2] = TEXT('\0');
    return TRUE;
}

BOOLEAN autostart_is_enabled()
{
    TCHAR expected[COMMAND_CAPACITY];
    TCHAR stored[COMMAND_CAPACITY];
    if (!_get_command(expected))
    {
        return FALSE;
    }
    HKEY key;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    {
        return FALSE;
    }
    DWORD type = 0;
    DWORD size = (COMMAND_CAPACITY - 1) * sizeof(TCHAR);
    LONG result = RegQueryValueEx(key, RUN_VALUE, NULL, &type, (LPBYTE)stored, &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS || type != REG_SZ)
    {
        return FALSE;
    }
    stored[size / sizeof(TCHAR)] = TEXT('\0');
    return _tcsicmp(stored, expected) == 0;
}

BOOLEAN autostart_set(BOOLEAN enabled)
{
    HKEY key;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
    {
        return FALSE;
    }
    LONG result;
    if (enabled)
    {
        TCHAR command[COMMAND_CAPACITY];
        if (!_get_command(command))
        {
            RegCloseKey(key);
            return FALSE;
        }
        result = RegSetValueEx(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)command,
                               (DWORD)((_tcslen(command) + 1) * sizeof(TCHAR)));
    }
    else
    {
        result = RegDeleteValue(key, RUN_VALUE);
        if (result == ERROR_FILE_NOT_FOUND)
        {
            result = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
