/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMNES_BOARD_H
#define ARMNES_BOARD_H

// What every board file shares: its registers live in nes->boardState, it maps in banks of the
// sizes the hardware uses, and the registry (list.c) finds it by iNES/NES 2.0 mapper number.

#include "nes.h"

#include <string.h>

#define STATE(type) ((struct type*) nes->boardState)

static inline void prg8(struct NES* nes, uint16_t address, int bank) {
	nesMapPrg(nes, address, 0x2000, bank);
}

static inline void prg16(struct NES* nes, uint16_t address, int bank) {
	nesMapPrg(nes, address, 0x4000, bank);
}

static inline void prg32(struct NES* nes, int bank) {
	nesMapPrg(nes, 0x8000, 0x8000, bank);
}

static inline void chr1(struct NES* nes, uint16_t address, int bank) {
	nesMapChr(nes, address, 0x400, bank);
}

static inline void chr2(struct NES* nes, uint16_t address, int bank) {
	nesMapChr(nes, address, 0x800, bank);
}

static inline void chr4(struct NES* nes, uint16_t address, int bank) {
	nesMapChr(nes, address, 0x1000, bank);
}

static inline void chr8(struct NES* nes, int bank) {
	nesMapChr(nes, 0, 0x2000, bank);
}

static inline void wram8(struct NES* nes) {
	nesMapWram(nes, 0x6000, 0x2000, 0, true);
}

// Each name table from CIRAM's two pages
static inline void mirrorPages(struct NES* nes, int a, int b, int c, int d) {
	nesSetNametable(nes, 0, nes->ppu.ntRam + a * 0x400);
	nesSetNametable(nes, 1, nes->ppu.ntRam + b * 0x400);
	nesSetNametable(nes, 2, nes->ppu.ntRam + c * 0x400);
	nesSetNametable(nes, 3, nes->ppu.ntRam + d * 0x400);
}

// Discrete boards write through the ROM: the value is what both drive on the bus
static inline uint8_t busConflict(struct NES* nes, uint16_t address, uint8_t value) {
	uint8_t* page = nes->prgPages[address >> 12];
	return page ? value & page[address & 0xFFF] : value;
}

#define BOARD(var) extern const struct Board var

// basic.c
BOARD(boardNrom);
BOARD(boardUxrom);
BOARD(boardCnrom);
BOARD(boardAxrom);
BOARD(boardMmc1);

// mmc3.c
BOARD(boardMmc3);
BOARD(boardTxsrom);
BOARD(boardTqrom);
BOARD(boardNamco108);
BOARD(boardNamco3446);
BOARD(boardNamco3433);
BOARD(boardNamco3425);
BOARD(boardNamco3453);

// asic.c
BOARD(boardMmc2);
BOARD(boardFme7);
BOARD(boardVrc24);
BOARD(boardVrc2);
BOARD(boardTc0190);
BOARD(boardSs88006);
BOARD(boardNamco163);
BOARD(boardNamco175);

// multicart.c
BOARD(boardFk23c);
BOARD(boardCoolboy);

// discrete.c
const struct Board* discreteFind(int mapper, int submapper);

#endif
