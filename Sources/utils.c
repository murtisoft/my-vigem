#include <ctype.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <tchar.h>
#include <windows.h>

#include "utils.h"

PTCHAR _tcsistr(PTCHAR haystack, const PTCHAR needle)
{
    do
    {
        PTCHAR h = haystack;
        PTCHAR n = needle;
        while (tolower((TBYTE)*h) == tolower((TBYTE)*n) && *n)
        {
            h++;
            n++;
        }
        if (*n == 0)
        {
            return (PTCHAR)haystack;
        }
    } while (*haystack++);
    return NULL;
}

LPTSTR _guid_to_str(const GUID *guid)
{
    LPTSTR result = malloc(39 * sizeof(TCHAR));
    if (result == NULL)
    {
        return NULL;
    }
    PTCHAR cur = result;
    cur += _stprintf(cur, TEXT("{%.8lX-%.4hX-%.4hX-"), guid->Data1, guid->Data2, guid->Data3);
    for (int i = 0; i < sizeof(guid->Data4); i++)
    {
        cur += _stprintf(cur, TEXT("%.2hhX"), guid->Data4[i]);
        if (i == 1)
        {
            *(cur++) = TEXT('-');
        }
    }
    *(cur++) = TEXT('}');
    *(cur++) = TEXT('\0');
    return result;
}

void _mem_zero(void *dst, size_t size)
{
    volatile unsigned char *d = (volatile unsigned char *)dst;
    while (size-- > 0)
    {
        *d++ = 0;
    }
}

void _mem_copy(void *dst, const void *src, size_t size)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || size == 0)
    {
        return;
    }
    if (d < s || d >= s + size)
    {
        for (size_t i = 0; i < size; i++)
        {
            d[i] = s[i];
        }
    }
    else
    {
        for (size_t i = size; i > 0; i--)
        {
            d[i - 1] = s[i - 1];
        }
    }
}

void debug_console_enable(void)
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole())
    {
        return;
    }
    FILE *f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    setvbuf(stdout, NULL, _IONBF, 0);
}

void debug_log(const char *fmt, ...)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}
