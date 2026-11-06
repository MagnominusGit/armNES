/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// iNES and NES 2.0 images: the header names the board, the ROM sizes, RAM and the region.

static uint32_t nes2Size(uint8_t lo, uint8_t hiNibble, uint32_t unit) {
	if (hiNibble == 0xF) {
		// Exponent-multiplier form
		return (1u << (lo >> 2)) * ((lo & 3) * 2 + 1);
	}
	return ((hiNibble << 8) | lo) * unit;
}

static uint32_t shiftSize(uint8_t shift) {
	return shift ? 64u << shift : 0;
}

// Old headers known to be wrong, by the CRC-32 of PRG and CHR ROM: FCEUmm's list
struct HeaderFix {
	uint32_t crc;
	int mapper, submapper, mirroring, battery, prgRam, chrRam, region, extra;
};

#define DEFAULT (-1)
#define NOEXTRA (-1)
#define MI_H 0
#define MI_V 1
#define MI_4 2
#define DFAULT8 8
#define PAL 1
#define MULTI 2
#define DENDY 3
static const struct HeaderFix headerFixes[] = {
#include "fceu/ines-correct.h"
};
#undef PAL

static uint32_t ramSize(int shift) {
	return shift ? 64u << shift : 0;
}

static void fixHeader(struct Cart* cart, const uint8_t* rom, size_t size) {
	uint32_t prg = cart->prgSize < size ? cart->prgSize : size;
	uint32_t chr = cart->chrRam ? 0 : cart->chrSize;
	chr = prg + chr <= size ? chr : size - prg;
	uint32_t crc = crc32(crc32(0, rom, prg), rom + prg, chr);
	const struct HeaderFix* f = headerFixes;
	while (f->crc && f->crc != crc) {
		f++;
	}
	if (f->crc) {
		if (f->mapper >= 0) {
			cart->mapper = f->mapper & 0xFFF;
		}
		if (f->submapper >= 0) {
			cart->submapper = f->submapper;
			cart->nes2 = true;
		}
		if (f->mirroring == DFAULT8) {
			if (cart->mirroring == MIRROR_FOUR) {
				cart->mirroring = MIRROR_HORIZONTAL;
			}
		} else if (f->mirroring >= 0x10) {
			cart->mirroring = (f->mirroring & 1) ? MIRROR_SCREEN_B : MIRROR_SCREEN_A;
		} else if (f->mirroring >= 0) {
			static const enum Mirroring mirrorings[3] = { MIRROR_HORIZONTAL, MIRROR_VERTICAL, MIRROR_FOUR };
			cart->mirroring = mirrorings[f->mirroring & 3];
		}
		if (f->battery > 0) {
			cart->battery = true;
		}
		if (f->region == 1) {
			cart->region = REGION_PAL;
		} else if (f->region == DENDY) {
			cart->region = REGION_DENDY;
		}
		if (f->prgRam >= 0) {
			cart->nes2 = true;
			cart->wramSize = ramSize(f->prgRam & 0x0F) + ramSize(f->prgRam >> 4);
			cart->batterySize = ramSize(f->prgRam >> 4);
		}
		if (f->extra == 0x800 && !cart->chrRam) {
			cart->chrRam = true;
			cart->chrSize = 0x2000;
		}
		if (f->chrRam >= 0) {
			cart->nes2 = true;
			uint32_t chrRam = ramSize(f->chrRam & 0x0F) + ramSize(f->chrRam >> 4);
			if (cart->chrRam) {
				cart->chrSize = chrRam;
			} else {
				cart->vramSize = chrRam;
			}
		}
	}
	if ((cart->mapper == 118 || cart->mapper == 24 || cart->mapper == 26) && cart->mirroring == MIRROR_FOUR) {
		cart->mirroring = MIRROR_HORIZONTAL;
	}
	if (cart->battery && !cart->batterySize) {
		cart->batterySize = cart->wramSize ? cart->wramSize : 0x2000;
	}
}

// UNIF: chunks with a board name instead of a header, made into an NES 2.0 image by FCEUmm's list
// of boards
struct UnifBoard {
	const char* name;
	int mapper;
	uint8_t chrRamShift;
	bool fourScreen;
};

static const struct UnifBoard unifBoards[] = {
#include "fceu/unif-boards.inc"
};

static int hexDigit(uint8_t c) {
	return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// The chunks in order, repeated to fill size
static void gather(uint8_t* out, uint32_t size, const uint8_t* const* chunks, const uint32_t* sizes) {
	uint32_t total = 0;
	for (int i = 0; i < 16; i++) {
		if (chunks[i]) {
			memcpy(out + total, chunks[i], sizes[i]);
			total += sizes[i];
		}
	}
	for (uint32_t i = total; i < size; i++) {
		out[i] = out[i % total];
	}
}

static uint8_t* unifToNes(const uint8_t* data, size_t size, size_t* nesSize) {
	const uint8_t* prg[16] = { 0 };
	const uint8_t* chr[16] = { 0 };
	uint32_t prgSizes[16] = { 0 }, chrSizes[16] = { 0 };
	uint32_t prgTotal = 0, chrTotal = 0;
	char name[64] = "";
	int mirroring = -1, battery = 0, tv = 0;
	for (size_t i = 32; i + 8 <= size;) {
		const uint8_t* id = data + i;
		uint32_t length = id[4] | id[5] << 8 | id[6] << 16 | (uint32_t) id[7] << 24;
		const uint8_t* body = id + 8;
		if (length > size - i - 8) {
			break;
		}
		int n = hexDigit(id[3]);
		if (!memcmp(id, "MAPR", 4)) {
			size_t copy = length < sizeof(name) - 1 ? length : sizeof(name) - 1;
			memcpy(name, body, copy);
			name[copy] = 0;
		} else if (!memcmp(id, "PRG", 3) && n >= 0) {
			prg[n] = body;
			prgSizes[n] = length;
			prgTotal += length;
		} else if (!memcmp(id, "CHR", 3) && n >= 0) {
			chr[n] = body;
			chrSizes[n] = length;
			chrTotal += length;
		} else if (!memcmp(id, "MIRR", 4) && length) {
			mirroring = body[0];
		} else if (!memcmp(id, "BATR", 4) && length) {
			battery = body[0] != 0;
		} else if (!memcmp(id, "TVCI", 4) && length) {
			tv = body[0];
		}
		i += 8 + length;
	}
	const char* board = name;
	static const char* prefixes[] = { "NES-", "UNL-", "HVC-", "BTL-", "BMC-" };
	for (int i = 0; i < 5; i++) {
		if (!strncmp(board, prefixes[i], 4)) {
			board += 4;
			break;
		}
	}
	const struct UnifBoard* u = NULL;
	for (size_t i = 0; i < sizeof(unifBoards) / sizeof(unifBoards[0]); i++) {
		if (!strcmp(board, unifBoards[i].name)) {
			u = &unifBoards[i];
			break;
		}
	}
	if (!u || !prgTotal) {
		return NULL;
	}
	uint32_t prgBanks = (prgTotal + 0x3FFF) / 0x4000, chrBanks = (chrTotal + 0x1FFF) / 0x2000;
	*nesSize = 16 + prgBanks * 0x4000 + chrBanks * 0x2000;
	uint8_t* out = calloc(1, *nesSize);
	uint8_t* h = out;
	memcpy(h, "NES\x1A", 4);
	h[4] = prgBanks & 0xFF;
	h[5] = chrBanks & 0xFF;
	h[6] = (u->mapper & 0x0F) << 4 | battery << 1 | (mirroring == 1) | (mirroring == 4 || u->fourScreen) << 3;
	h[7] = (u->mapper & 0xF0) | 0x08;
	h[8] = u->mapper >> 8;
	h[9] = (prgBanks >> 8) | (chrBanks >> 8) << 4;
	h[10] = battery ? 0x70 : 0x07;
	h[11] = chrBanks ? 0 : u->chrRamShift;
	h[12] = tv == 1;
	gather(out + 16, prgBanks * 0x4000, prg, prgSizes);
	if (chrBanks) {
		gather(out + 16 + prgBanks * 0x4000, chrBanks * 0x2000, chr, chrSizes);
	}
	return out;
}

bool cartLoad(struct NES* nes, const uint8_t* data, size_t size) {
	struct Cart* cart = &nes->cart;
	if (size >= 32 && !memcmp(data, "UNIF", 4)) {
		size_t nesSize;
		uint8_t* image = unifToNes(data, size, &nesSize);
		bool loaded = image && cartLoad(nes, image, nesSize);
		free(image);
		return loaded;
	}
	if (size < 16 || memcmp(data, "NES\x1A", 4)) {
		return false;
	}
	const uint8_t* h = data;
	memcpy(cart->header, h, 16);
	cart->battery = h[6] & 2;
	bool nes2 = (h[7] & 0x0C) == 0x08;
	cart->nes2 = nes2;
	cart->mapper = (h[6] >> 4) | (h[7] & 0xF0);
	cart->submapper = 0;
	if (nes2) {
		cart->mapper |= (h[8] & 0x0F) << 8;
		cart->submapper = h[8] >> 4;
		cart->prgSize = nes2Size(h[4], h[9] & 0x0F, 0x4000);
		cart->chrSize = nes2Size(h[5], h[9] >> 4, 0x2000);
		uint32_t prgRam = shiftSize(h[10] & 0x0F);
		uint32_t prgNvram = shiftSize(h[10] >> 4);
		uint32_t chrRam = shiftSize(h[11] & 0x0F);
		uint32_t chrNvram = shiftSize(h[11] >> 4);
		cart->wramSize = prgRam + prgNvram;
		cart->batterySize = prgNvram;
		if (!cart->chrSize) {
			cart->chrSize = chrRam + chrNvram;
			cart->chrRam = true;
		} else if (chrRam + chrNvram) {
			cart->vramSize = chrRam + chrNvram;
		}
		if (!cart->wramSize && cart->battery) {
			cart->wramSize = 0x2000;
		}
		switch (h[12] & 3) {
		case 1:
			cart->region = REGION_PAL;
			break;
		case 3:
			cart->region = REGION_DENDY;
			break;
		default:
			cart->region = REGION_NTSC;
			break;
		}
	} else {
		// Archaic headers: byte 7 on is junk, such as a dumper's name
		if (h[7] & 0x0C) {
			cart->mapper &= 0x0F;
		}
		cart->prgSize = h[4] * 0x4000;
		cart->chrSize = h[5] * 0x2000;
		cart->wramSize = 0x2000;
		cart->region = (h[9] & 1) ? REGION_PAL : REGION_NTSC;
		if (!cart->chrSize) {
			cart->chrSize = 0x2000;
			cart->chrRam = true;
		}
	}
	if (cart->battery && !cart->batterySize) {
		cart->batterySize = cart->wramSize ? cart->wramSize : 0x2000;
	}
	if (h[6] & 8) {
		cart->mirroring = MIRROR_FOUR;
	} else {
		cart->mirroring = (h[6] & 1) ? MIRROR_VERTICAL : MIRROR_HORIZONTAL;
	}

	size_t offset = 16;
	cart->hasTrainer = h[6] & 4;
	if (cart->hasTrainer) {
		if (size < offset + 512) {
			return false;
		}
		memcpy(cart->trainer, data + offset, 512);
		offset += 512;
	}
	if (!cart->prgSize) {
		return false;
	}
	if (!nes2) {
		fixHeader(cart, data + offset, size > offset ? size - offset : 0);
	}
	// Short images are padded with open-bus-like 0xFF rather than refused
	cart->prg = malloc(cart->prgSize);
	memset(cart->prg, 0xFF, cart->prgSize);
	size_t have = size > offset ? size - offset : 0;
	memcpy(cart->prg, data + offset, have < cart->prgSize ? have : cart->prgSize);
	offset += cart->prgSize;

	uint32_t chrAlloc = cart->chrSize ? cart->chrSize : 0x2000;
	cart->chr = calloc(1, chrAlloc);
	if (!cart->chrRam) {
		have = size > offset ? size - offset : 0;
		memcpy(cart->chr, data + offset, have < cart->chrSize ? have : cart->chrSize);
		offset += cart->chrSize;
	} else {
		cart->chrSize = chrAlloc;
	}

	// NES 2.0 miscellaneous ROM: what's left after CHR ROM
	cart->misc = NULL;
	cart->miscSize = 0;
	if (nes2 && (h[14] & 3) && size > offset) {
		cart->miscSize = size - offset;
		cart->misc = malloc(cart->miscSize);
		memcpy(cart->misc, data + offset, cart->miscSize);
	}

	// TQROM has 8 KB of CHR RAM beside its ROM even under an old header
	if (!cart->vramSize && cart->mapper == 119) {
		cart->vramSize = 0x2000;
	}
	if (cart->vramSize) {
		cart->vram = calloc(1, cart->vramSize);
	}
	if (cart->wramSize < 0x2000 && !nes2) {
		cart->wramSize = 0x2000;
	}
	// The CPU maps 4 KB pages, so smaller RAM is given a whole 8 KB
	if (cart->wramSize < 0x2000) {
		cart->wramSize = 0x2000;
	}
	cart->wram = calloc(1, cart->wramSize);

	cart->prgCrc = crc32(0, cart->prg, cart->prgSize);
	return true;
}

void cartFree(struct NES* nes) {
	free(nes->cart.prg);
	free(nes->cart.chr);
	free(nes->cart.wram);
	free(nes->cart.vram);
	free(nes->cart.misc);
	nes->cart.prg = nes->cart.chr = nes->cart.wram = nes->cart.vram = nes->cart.misc = NULL;
	nes->cart.miscSize = 0;
}
