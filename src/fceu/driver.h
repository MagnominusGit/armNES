#ifndef _FCEU_DRIVER_H
#define _FCEU_DRIVER_H

#include <stdbool.h>
#include "fceu-types.h"
#include "git.h"

void FCEU_printf(const char *format, ...);
#define FCEUI_printf FCEU_printf

#endif
