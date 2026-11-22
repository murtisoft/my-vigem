#ifndef AUTOSTART_H
#define AUTOSTART_H

#include <windows.h>

/* Autostart at logon via the HKCU Run key (no admin rights needed). */
BOOLEAN autostart_is_enabled();
BOOLEAN autostart_set(BOOLEAN enabled);

#endif /* AUTOSTART_H */
