#ifndef FDS_APU_H
#define FDS_APU_H

#include "fceu-types.h"

void FDSSoundReset(void);
void FDSSoundStateAdd(void);

void FDSSoundPower(void);
void FDSSoundWrite(uint32_t A, uint8_t V);

#endif
