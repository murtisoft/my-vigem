#ifndef UTILS_H
#define UTILS_H

#include <wtypes.h>

PTCHAR _tcsistr(PTCHAR haystack, const PTCHAR needle);
LPTSTR _guid_to_str(const GUID *guid);
void _mem_zero(void *dst, size_t size);
void _mem_copy(void *dst, const void *src, size_t size);

#endif /* UTILS_H */
