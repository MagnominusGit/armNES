/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "genie.h"
#include "nes.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

enum { GENIE_PRG = 0x1000, GENIE_CHR = 0x2000, CODES = 3 };

static struct {
	bool present;
	// Its code screen has the console; its pages are laid over the game's
	bool menu, mapped;
	// $8000-$800C as written: master control, then address high, address low, compare, replace for each
	uint8_t regs[13];
} genie;
static uint8_t prg[GENIE_PRG], chr[GENIE_CHR];
static struct Cheat imported[CODES];
static int importedCount;
bool genieMenuWrites;

// The game's pages under the Game Genie's in its code screen: $8000-$FFFF and the pattern tables
static struct {
	uint8_t* prgPages[8];
	bool prgWritable[8];
	uint8_t* chrPages[8];
	bool chrWritable[8];
	uint16_t readHooks;
	bool ppuReadHook;
} game;

bool genieIdentify(const uint8_t* data, size_t size) {
	if (size < 16 + GENIE_PRG + GENIE_CHR || memcmp(data, "NES\x1a", 4)) {
		return false;
	}
	// Its 4 KB program (the image FCEUX and Mesen take repeats it to 16 KB)
	return (uint32_t) crc32(0, data + 16, GENIE_PRG) == 0x1A3A22A1;
}

uint8_t* genieImageFrom(const uint8_t* data, size_t size, size_t* imageSize) {
	if (size != GENIE_PRG || (uint32_t) crc32(0, data, GENIE_PRG) != 0x1A3A22A1) {
		return NULL;
	}
	enum { SIZE = 16 + 4 * GENIE_PRG + GENIE_CHR };
	uint8_t* image = malloc(SIZE);
	if (!image) {
		return NULL;
	}
	static const uint8_t header[16] = { 'N', 'E', 'S', 0x1A, 1, 1 };
	memcpy(image, header, sizeof(header));
	for (int i = 0; i < 4; i++) {
		memcpy(image + 16 + i * GENIE_PRG, data, GENIE_PRG);
	}
	// The pattern tables: with PPU A2 high, A4 sets D4-D7 and A5 D0-D3; with it low, A6 and A7
	uint8_t* chr = image + 16 + 4 * GENIE_PRG;
	for (int a = 0; a < GENIE_CHR; a++) {
		int const high = (a & 4) ? (a >> 4) & 1 : (a >> 6) & 1;
		int const low = (a & 4) ? (a >> 5) & 1 : (a >> 7) & 1;
		chr[a] = (uint8_t) ((high ? 0xF0 : 0) | (low ? 0x0F : 0));
	}
	*imageSize = SIZE;
	return image;
}

void genieAttach(struct NES* nes, const uint8_t* image, size_t size) {
	if (genie.present) {
		genieUnmap(nes);
	}
	memset(&genie, 0, sizeof(genie));
	genie.present = image && genieIdentify(image, size);
	genieMenuWrites = genie.present;
	if (genie.present) {
		size_t const prgSize = (size_t) image[4] * 0x4000;
		memcpy(prg, image + 16, GENIE_PRG);
		memcpy(chr, image + 16 + (prgSize >= GENIE_PRG ? prgSize : GENIE_PRG), GENIE_CHR);
	}
}

bool geniePresent(void) {
	return genie.present;
}

bool genieInMenu(void) {
	return genie.present && genie.menu;
}

void genieMap(struct NES* nes) {
	if (!genie.present || genie.mapped || !genie.menu) {
		return;
	}
	memcpy(game.prgPages, nes->prgPages + 8, sizeof(game.prgPages));
	memcpy(game.prgWritable, nes->prgWritable + 8, sizeof(game.prgWritable));
	memcpy(game.chrPages, nes->chrPages, sizeof(game.chrPages));
	memcpy(game.chrWritable, nes->chrWritable, sizeof(game.chrWritable));
	game.readHooks = nes->readHooks;
	game.ppuReadHook = nes->ppuReadHook;
	for (int i = 0; i < 8; i++) {
		nes->prgPages[8 + i] = i < 4 ? NULL : prg;
		nes->prgWritable[8 + i] = false;
		nes->chrPages[i] = chr + i * 0x400;
		nes->chrWritable[i] = false;
	}
	nes->readHooks &= 0x00FF;
	nes->ppuReadHook = false;
	genie.mapped = true;
}

void genieUnmap(struct NES* nes) {
	if (!genie.present || !genie.mapped) {
		return;
	}
	// In the code screen the game's board can't have moved its pages: none of its writes reach it
	memcpy(nes->prgPages + 8, game.prgPages, sizeof(game.prgPages));
	memcpy(nes->prgWritable + 8, game.prgWritable, sizeof(game.prgWritable));
	memcpy(nes->chrPages, game.chrPages, sizeof(game.chrPages));
	memcpy(nes->chrWritable, game.chrWritable, sizeof(game.chrWritable));
	nes->readHooks = (uint16_t) ((nes->readHooks & 0x00FF) | (game.readHooks & 0xFF00));
	nes->ppuReadHook = game.ppuReadHook;
	genie.mapped = false;
}

// A code's address and whether it's on
static uint16_t codeAddress(int i) {
	return (uint16_t) (0x8000 | (genie.regs[1 + i * 4] & 0x7F) << 8 | genie.regs[2 + i * 4]);
}

static bool codeOn(int i) {
	if (genie.regs[0] & (0x10 << i)) {
		return false;
	}
	// The ASIC takes only the first of codes for the same address
	for (int j = 0; j < i; j++) {
		if (!(genie.regs[0] & (0x10 << j)) && codeAddress(j) == codeAddress(i)) {
			return false;
		}
	}
	return true;
}

void genieCheatPages(struct NES* nes) {
	if (!genie.present || genie.menu) {
		return;
	}
	for (int i = 0; i < CODES; i++) {
		if (codeOn(i)) {
			nes->cheatPages |= (uint16_t) (1 << (codeAddress(i) >> 12));
		}
	}
}

void genieReset(struct NES* nes, bool hard) {
	if (!genie.present) {
		return;
	}
	// The code screen comes back only with the power: a reset keeps the game and its codes
	if (hard) {
		memset(genie.regs, 0, sizeof(genie.regs));
		genie.menu = true;
		genie.mapped = false;
	}
	genieMenuWrites = genie.menu;
	genieMap(nes);
	genieCheatPages(nes);
}

bool genieWrite(struct NES* nes, uint16_t address, uint8_t value) {
	if (!genie.present || !genie.menu || address < 0x8000) {
		return false;
	}
	// Its code screen has $8000-$FFFF: the game's board doesn't see these
	if (address <= 0x800C) {
		genie.regs[address - 0x8000] = value;
	}
	if (address == 0x8000 && (value & 1)) {
		// The imported codes go in the slots left empty
		uint8_t* r = genie.regs;
		r[0] = value;
		for (int i = 0, next = 0; i < CODES && next < importedCount; i++) {
			if (!(r[0] & (0x10 << i))) {
				continue;
			}
			const struct Cheat* c = &imported[next++];
			r[1 + i * 4] = (c->address >> 8) & 0x7F;
			r[2 + i * 4] = c->address & 0xFF;
			r[3 + i * 4] = c->compare >= 0 ? (uint8_t) c->compare : 0;
			r[4 + i * 4] = c->value;
			r[0] = (uint8_t) ((r[0] & ~(0x10 << i) & ~(2 << i)) | (c->compare >= 0 ? 2 << i : 0));
		}
		genieUnmap(nes);
		genie.menu = false;
		genieMenuWrites = false;
		genieCheatPages(nes);
	}
	return true;
}

uint8_t genieRead(const struct NES* nes, uint16_t address, uint8_t value) {
	if (!genie.present || genie.menu || address < 0x8000) {
		return value;
	}
	for (int i = 0; i < CODES; i++) {
		if (codeAddress(i) != address || !codeOn(i)) {
			continue;
		}
		if ((genie.regs[0] & (2 << i)) && value != genie.regs[3 + i * 4]) {
			return value;
		}
		uint8_t replaced = genie.regs[4 + i * 4];
		// The game's cart, kept off its ROM, answers as if at the address less $8000
		if (address >= 0xC020) {
			const uint8_t* page = nes->prgPages[(address & 0x7FFF) >> 12];
			if (page) {
				replaced &= page[address & 0xFFF];
			}
		}
		return replaced;
	}
	return value;
}

void genieImport(const struct Cheat* codes, int count) {
	importedCount = 0;
	for (int i = 0; i < count && i < CODES; i++) {
		if (codes[i].address >= 0x8000) {
			imported[importedCount++] = codes[i];
		}
	}
}

// Whether it's plugged in and has the console (0 without it, 1 the game, 2 its code screen), then its
// registers; the same size either way
size_t genieStateSize(void) {
	return 1 + sizeof(genie.regs);
}

void genieSaveState(uint8_t* out) {
	memset(out, 0, genieStateSize());
	if (genie.present) {
		out[0] = (uint8_t) (genie.menu ? 2 : 1);
		memcpy(out + 1, genie.regs, sizeof(genie.regs));
	}
}

void genieLoadState(struct NES* nes, const uint8_t* in) {
	if (!genie.present) {
		return;
	}
	// A state made without it: the game, no codes
	genie.mapped = false;
	genie.menu = in[0] == 2;
	memcpy(genie.regs, in + 1, sizeof(genie.regs));
	genieMenuWrites = genie.menu;
	genieMap(nes);
	genieCheatPages(nes);
}
