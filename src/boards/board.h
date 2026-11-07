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

// A cart's solder pads or DIP switches: the frontend's setting, else the one known for the dump,
// else 0. They stay put through Reset, as on the cart.
static inline uint8_t jumperSetting(const struct NES* nes) {
	return nes->jumper >= 0 ? nes->jumper : nes->cart.jumper >= 0 ? nes->cart.jumper : 0;
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
BOARD(boardRambo);

// asic.c
BOARD(boardMmc2);
BOARD(boardFme7);
BOARD(boardVrc24);
BOARD(boardVrc2);
BOARD(boardTc0190);
BOARD(boardSs88006);
BOARD(boardNamco163);
BOARD(boardNamco175);
BOARD(boardVrc1);
BOARD(boardVrc3);
BOARD(boardVrc6);
BOARD(boardVrc7);

// multicart.c
BOARD(boardFk23c);
BOARD(boardCoolboy);
BOARD(board8237);

// sachen.c
BOARD(boardS8259);
BOARD(boardSachenLatch);
BOARD(boardTca01);
BOARD(boardS74ls374n);

// jy.c
BOARD(boardJy);

// bandai.c
BOARD(boardBandai);

// mmc5.c
BOARD(boardMmc5);

// rare.c
BOARD(board127);
BOARD(boardJy043);
BOARD(boardKaraoke);
BOARD(boardQta);
BOARD(boardYuxing);

// fds.c
BOARD(boardFds);
bool fdsCartLoad(struct NES*, const uint8_t* data, size_t size);
int fdsSide(struct NES*);
void fdsInsert(struct NES*, int side);
bool fdsBusy(struct NES*);

// discrete.c
const struct Board* discreteFind(int mapper, int submapper);

// fceu/bridge.c: FCEUmm's boards, for the rest
BOARD(boardFceu);
BOARD(boardFceuCycle);
const struct Board* fceuFind(int mapper);

#endif
