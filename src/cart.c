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

bool cartLoad(struct NES* nes, const uint8_t* data, size_t size) {
	struct Cart* cart = &nes->cart;
	if (size < 16 || memcmp(data, "NES\x1A", 4)) {
		return false;
	}
	const uint8_t* h = data;
	cart->battery = h[6] & 2;
	bool nes2 = (h[7] & 0x0C) == 0x08;
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
		// Old headers often have junk in bytes 7-15
		if (memcmp(h + 12, "\0\0\0\0", 4)) {
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
	} else {
		cart->chrSize = chrAlloc;
	}

	if (cart->wramSize < 0x2000 && !nes2) {
		cart->wramSize = 0x2000;
	}
	cart->wram = calloc(1, cart->wramSize ? cart->wramSize : 0x2000);

	cart->prgCrc = crc32(0, cart->prg, cart->prgSize);
	return true;
}

void cartFree(struct NES* nes) {
	free(nes->cart.prg);
	free(nes->cart.chr);
	free(nes->cart.wram);
	nes->cart.prg = nes->cart.chr = nes->cart.wram = NULL;
}
