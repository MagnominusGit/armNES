/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMNES_MMC3_H
#define ARMNES_MMC3_H

#include "board.h"

// MMC3's registers, with room after them for a board's own
struct Mmc3 {
	uint8_t select;
	uint8_t banks[8];
	uint8_t mirroring;
	uint8_t wramControl;
	uint8_t latch, counter;
	bool reload, irqEnabled;
	bool a12;
	uint64_t a12LowSince;
	uint8_t extra[32];
};

// How a board changes MMC3: each PRG bank (8 KB, 0xFE and 0xFF for the fixed ones) and CHR bank
// (1 KB) as the chip asks for it, and mirroring and work RAM; NULL keeps MMC3's own
struct Mmc3Variant {
	void (*prg)(struct NES*, uint16_t address, int bank);
	void (*chr)(struct NES*, uint16_t address, int bank);
	void (*mirror)(struct NES*);
	void (*wram)(struct NES*);
};

void mmc3Sync(struct NES*);
void mmc3Write(struct NES*, uint16_t address, uint8_t value);
void mmc3PpuBus(struct NES*, uint16_t address);
int mmc3IrqQuietDots(struct NES*);
void mmc3Reset(struct NES*, bool hard);

#endif
