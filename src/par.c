/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "par.h"
#include "nes.h"

#include <string.h>
#include <zlib.h>

// What the console sees of the Pro Action Replay (see par.h), worked out from its BIOS: its RAM at
// $5000-$5FFF (the trainer's tables at $5400 and $5800, the RAM it copies at $5104), the stub it
// starts the game with at $5000 and the handler at $500C, and the write to $FFF8.

enum { PAR_PRG = 0x8000, PAR_CHR = 0x2000, PAR_RAM = 0x1000, START = 0xFFF8 };

static struct {
	bool present;
	// Its menu has the console; its pages are laid over the game's (mapped), the menu's too
	bool menu, mapped, menuMapped;
	uint8_t ram[PAR_RAM];
} par;
static uint8_t prg[PAR_PRG], chr[PAR_CHR];
enum { TABLE = 0x037, SLOTS = 5 };
static uint8_t imported[SLOTS][4];
static int importedCount;
bool parMenuWrites, parHooksNmi;

// The game's pages under the Pro Action Replay's: $5000-$5FFF, and in its menu $8000-$FFFF and the
// whole PPU bus, with the read hooks and the board's CHR read hook
static struct {
	uint8_t* prgPages[16];
	bool prgWritable[16];
	uint8_t* chrPages[16];
	bool chrWritable[16];
	uint16_t readHooks;
	bool ppuReadHook;
} game;

bool parIdentify(const uint8_t* data, size_t size) {
	static const uint32_t dumps[] = {
		0x178DBA78, // Pro Action Replay (Europe)
		0x3B4F32AB, // v1.1, 29/10/92
		0x64AC21ED, // v1.2, 26/11/92
	};
	if (size < 16 + PAR_PRG + PAR_CHR || memcmp(data, "NES\x1a", 4)) {
		return false;
	}
	uint32_t const crc = (uint32_t) crc32(0, data + 16, PAR_PRG + PAR_CHR);
	for (size_t i = 0; i < sizeof(dumps) / sizeof(*dumps); i++) {
		if (crc == dumps[i]) {
			return true;
		}
	}
	return false;
}

void parAttach(struct NES* nes, const uint8_t* image, size_t size) {
	if (par.present) {
		parUnmap(nes);
	}
	par.present = image && size >= 16 + PAR_PRG + PAR_CHR;
	par.menu = par.present;
	par.mapped = par.menuMapped = false;
	parHooksNmi = false;
	parMenuWrites = par.present;
	if (par.present) {
		memcpy(prg, image + 16, PAR_PRG);
		memcpy(chr, image + 16 + PAR_PRG, PAR_CHR);
		memset(par.ram, 0, sizeof(par.ram));
	}
}

bool parPresent(void) {
	return par.present;
}

uint8_t* parMemory(void) {
	return par.present ? par.ram : NULL;
}

bool parInMenu(void) {
	return par.present && par.menu;
}

void parMap(struct NES* nes) {
	if (!par.present || par.mapped) {
		return;
	}
	memcpy(game.prgPages, nes->prgPages, sizeof(game.prgPages));
	memcpy(game.prgWritable, nes->prgWritable, sizeof(game.prgWritable));
	memcpy(game.chrPages, nes->chrPages, sizeof(game.chrPages));
	memcpy(game.chrWritable, nes->chrWritable, sizeof(game.chrWritable));
	game.readHooks = nes->readHooks;
	game.ppuReadHook = nes->ppuReadHook;
	// Its RAM: a board that answers at $5000 itself is beneath it
	nes->prgPages[5] = par.ram;
	nes->prgWritable[5] = true;
	nes->readHooks &= (uint16_t) ~(1 << 5);
	if (par.menu) {
		for (int i = 0; i < 8; i++) {
			nes->prgPages[8 + i] = prg + i * 0x1000;
			nes->prgWritable[8 + i] = false;
			nes->chrPages[i] = chr + i * 0x400;
			nes->chrWritable[i] = false;
		}
		nes->readHooks &= 0x00FF;
		nes->ppuReadHook = false;
		nesSetMirroring(nes, MIRROR_VERTICAL);
	}
	par.mapped = true;
	par.menuMapped = par.menu;
}

void parUnmap(struct NES* nes) {
	if (!par.present || !par.mapped) {
		return;
	}
	nes->prgPages[5] = game.prgPages[5];
	nes->prgWritable[5] = game.prgWritable[5];
	nes->readHooks = (uint16_t) ((nes->readHooks & ~(1 << 5)) | (game.readHooks & (1 << 5)));
	if (par.menuMapped) {
		// In the menu the game's board can't have moved its pages: none of its writes reach it
		memcpy(nes->prgPages + 8, game.prgPages + 8, 8 * sizeof(*nes->prgPages));
		memcpy(nes->prgWritable + 8, game.prgWritable + 8, 8 * sizeof(*nes->prgWritable));
		memcpy(nes->chrPages, game.chrPages, sizeof(game.chrPages));
		memcpy(nes->chrWritable, game.chrWritable, sizeof(game.chrWritable));
		nes->readHooks = (uint16_t) ((nes->readHooks & 0x00FF) | (game.readHooks & 0xFF00));
		nes->ppuReadHook = game.ppuReadHook;
	}
	par.mapped = par.menuMapped = false;
}

void parReset(struct NES* nes, bool hard) {
	if (!par.present) {
		return;
	}
	if (hard) {
		memset(par.ram, 0, sizeof(par.ram));
		if (importedCount) {
			memcpy(par.ram + 0x034, "WHB", 3);
			memcpy(par.ram + TABLE, imported, (size_t) importedCount * 4);
		}
	}
	par.mapped = par.menuMapped = false;
	par.menu = true;
	parHooksNmi = false;
	parMenuWrites = true;
	parMap(nes);
}

// The stub at $5000 writes $FFF8: its ROM and CHR step aside for the game's, its RAM stays, and the
// NMI vector is its own from now on
static void start(struct NES* nes) {
	parUnmap(nes);
	par.menu = false;
	parHooksNmi = true;
	parMenuWrites = false;
	parMap(nes);
}

bool parWrite(struct NES* nes, uint16_t address, uint8_t value) {
	(void) value;
	if (!par.present || !par.menu || address < 0x8000) {
		return false;
	}
	// Its menu has $8000-$FFFF: the game's board doesn't see these
	if (address == START) {
		start(nes);
	}
	return true;
}

uint16_t parNmiVector(void) {
	return par.ram[0x00A] | par.ram[0x00B] << 8;
}

void parImport(const uint8_t codes[][4], int count) {
	importedCount = count < SLOTS ? count : SLOTS;
	memcpy(imported, codes, (size_t) importedCount * 4);
}

size_t parStateSize(void) {
	return par.present ? 1 + PAR_RAM : 0;
}

void parSaveState(uint8_t* out) {
	if (par.present) {
		out[0] = par.menu;
		memcpy(out + 1, par.ram, PAR_RAM);
	}
}

void parLoadState(struct NES* nes, const uint8_t* in) {
	if (!par.present) {
		return;
	}
	// The state's pages are the game's: its own go over them as the state has the menu or the game
	par.mapped = par.menuMapped = false;
	par.menu = in[0] != 0;
	memcpy(par.ram, in + 1, PAR_RAM);
	parHooksNmi = !par.menu;
	parMenuWrites = par.menu;
	parMap(nes);
}
