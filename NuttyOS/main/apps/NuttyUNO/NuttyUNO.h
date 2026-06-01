#ifndef _NUTTYUNO_H
#define _NUTTYUNO_H

#include "services/NuttyApps/NuttyApps.h"

/* Main NuttyUNO menu app (shows Host / Join submenu) */
extern NuttyAppDefinition NuttyUNO;

/* Direct host/client entry points (hidden from main menu, launched from submenu) */
extern NuttyAppDefinition NuttyUNOHost;
extern NuttyAppDefinition NuttyUNOClient;

/* Function pointers for launching host/client from menu */
extern void uno_host_main(void);
extern void uno_client_main(void);
extern void uno_set_requested_bots(uint8_t count);

#endif /* _NUTTYUNO_H */
