/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "nes.h"

enum { COMPOSITE_OFF, COMPOSITE, COMPOSITE_SVIDEO };

#define COMPOSITE_SCALE 3
#define COMPOSITE_WIDTH (NES_WIDTH * COMPOSITE_SCALE)

void compositeFrame(int mode, bool pal, unsigned frame, const uint16_t* in, uint16_t* out);
