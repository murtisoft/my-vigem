#include <ctype.h>
#include <stddef.h>
#include <tchar.h>

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
