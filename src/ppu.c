/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <string.h>

// The 2C02, one dot at a time: the background and sprite fetches put their addresses on the PPU bus
// as the hardware does, so boards that count A12 or latch on tiles see every one.

enum {
	CTRL_INCREMENT = 0x04,
	CTRL_SPRITE_TABLE = 0x08,
	CTRL_BG_TABLE = 0x10,
	CTRL_TALL = 0x20,
	CTRL_NMI = 0x80,
	MASK_GRAY = 0x01,
	MASK_BG_LEFT = 0x02,
	MASK_SPRITE_LEFT = 0x04,
	MASK_BG = 0x08,
	MASK_SPRITES = 0x10,
	STATUS_OVERFLOW = 0x20,
	STATUS_HIT = 0x40,
	STATUS_VBL = 0x80,
};

// Frames until a bit of the PPU's data latch fades to 0 (about 600 ms)
enum { OPEN_BUS_FRAMES = 36 };

static inline int vblankLine(const struct NES* nes) {
	return nes->region == REGION_DENDY ? 291 : 241;
}

static inline int preLine(const struct NES* nes) {
	return nes->region == REGION_NTSC ? 261 : 311;
}

static void updateNmi(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	nes->cpu.nmiLine = (ppu->status & STATUS_VBL) && (ppu->ctrl & CTRL_NMI);
}

static inline void setBus(struct NES* nes, uint16_t address) {
	nes->ppu.busAddress = address;
	if (nes->watchPpuBus) {
		nes->board->ppuBus(nes, address);
	}
}

uint8_t ppuBusRead(struct NES* nes, uint16_t address) {
	address &= 0x3FFF;
	if (nes->ppuReadHook) {
		return nes->board->ppuRead(nes, address);
	}
	uint8_t* page = nes->chrPages[address >> 10];
	return page ? page[address & 0x3FF] : (address & 0xFF);
}

static inline uint8_t fetchAt(struct NES* nes, uint16_t address) {
	setBus(nes, address);
	return ppuBusRead(nes, address);
}

static void busWriteVram(struct NES* nes, uint16_t address, uint8_t value) {
	address &= 0x3FFF;
	if (address >= 0x3F00) {
		int index = address & 0x1F;
		if (!(index & 3)) {
			index &= 0x0F;
		}
		nes->ppu.palette[index] = value & 0x3F;
		return;
	}
	int page = address >> 10;
	if (nes->chrWritable[page] && nes->chrPages[page]) {
		nes->chrPages[page][address & 0x3FF] = value;
	}
}

static inline uint8_t readPalette(struct Ppu* ppu, int index) {
	index &= 0x1F;
	if (!(index & 3)) {
		index &= 0x0F;
	}
	return ppu->palette[index];
}

static inline bool rendering(const struct Ppu* ppu) {
	return ppu->renderingEnabled;
}

static void incrementX(struct Ppu* ppu) {
	if ((ppu->v & 0x1F) == 31) {
		ppu->v = (ppu->v & ~0x1F) ^ 0x400;
	} else {
		ppu->v++;
	}
}

static void incrementY(struct Ppu* ppu) {
	if ((ppu->v & 0x7000) != 0x7000) {
		ppu->v += 0x1000;
		return;
	}
	ppu->v &= ~0x7000;
	int y = (ppu->v >> 5) & 0x1F;
	if (y == 29) {
		y = 0;
		ppu->v ^= 0x800;
	} else if (y == 31) {
		y = 0;
	} else {
		y++;
	}
	ppu->v = (ppu->v & ~0x3E0) | (y << 5);
}

static inline void refreshOpenBus(struct Ppu* ppu, uint8_t value, uint8_t bits) {
	ppu->openBus = (ppu->openBus & ~bits) | (value & bits);
	for (int i = 0; i < 8; i++) {
		if (bits & (1 << i)) {
			ppu->openBusAge[i] = ppu->frame & 0xFF;
		}
	}
}

static uint8_t openBus(struct Ppu* ppu) {
	uint8_t value = ppu->openBus;
	for (int i = 0; i < 8; i++) {
		if ((uint8_t) (ppu->frame - ppu->openBusAge[i]) > OPEN_BUS_FRAMES) {
			value &= ~(1 << i);
		}
	}
	ppu->openBus = value;
	return value;
}

uint8_t ppuReadRegister(struct NES* nes, uint16_t address) {
	struct Ppu* ppu = &nes->ppu;
	uint8_t value;
	switch (address & 7) {
	case 2:
		value = (ppu->status & 0xE0) | (openBus(ppu) & 0x1F);
		// A read just as VBL would be set keeps it, and its NMI, from happening this frame
		if (ppu->scanline == vblankLine(nes) && ppu->dot == 1) {
			ppu->suppressVbl = true;
		}
		ppu->status &= ~STATUS_VBL;
		ppu->w = false;
		updateNmi(nes);
		refreshOpenBus(ppu, value, 0xE0);
		return value;
	case 4:
		value = ppu->oam[ppu->oamAddr];
		if ((ppu->oamAddr & 3) == 2) {
			value &= 0xE3;
		}
		refreshOpenBus(ppu, value, 0xFF);
		return value;
	case 7: {
		uint16_t v = ppu->v & 0x3FFF;
		if (v >= 0x3F00) {
			value = (readPalette(ppu, v) & 0x3F) | (openBus(ppu) & 0xC0);
			if (nes->ppu.mask & MASK_GRAY) {
				value &= 0x30 | (openBus(ppu) & 0xC0);
			}
			ppu->readBuffer = ppuBusRead(nes, v - 0x1000);
			refreshOpenBus(ppu, value, 0x3F);
		} else {
			value = ppu->readBuffer;
			ppu->readBuffer = ppuBusRead(nes, v);
			refreshOpenBus(ppu, value, 0xFF);
		}
		if (rendering(ppu) && (ppu->scanline < 240 || ppu->scanline == preLine(nes))) {
			incrementX(ppu);
			incrementY(ppu);
		} else {
			ppu->v = (ppu->v + ((ppu->ctrl & CTRL_INCREMENT) ? 32 : 1)) & 0x7FFF;
		}
		setBus(nes, ppu->v & 0x3FFF);
		return value;
	}
	default:
		return openBus(ppu);
	}
}

void ppuWriteRegister(struct NES* nes, uint16_t address, uint8_t value) {
	struct Ppu* ppu = &nes->ppu;
	refreshOpenBus(ppu, value, 0xFF);
	switch (address & 7) {
	case 0:
		ppu->ctrl = value;
		ppu->t = (ppu->t & ~0xC00) | ((value & 3) << 10);
		updateNmi(nes);
		break;
	case 1:
		ppu->mask = value;
		ppu->renderingEnabled = value & (MASK_BG | MASK_SPRITES);
		break;
	case 3:
		ppu->oamAddr = value;
		break;
	case 4:
		if (rendering(ppu) && (ppu->scanline < 240 || ppu->scanline == preLine(nes))) {
			// Writes during rendering only bump the high bits of the address
			ppu->oamAddr += 4;
		} else {
			ppu->oam[ppu->oamAddr++] = value;
		}
		break;
	case 5:
		if (!ppu->w) {
			ppu->t = (ppu->t & ~0x1F) | (value >> 3);
			ppu->fineX = value & 7;
		} else {
			ppu->t = (ppu->t & ~0x73E0) | ((value & 0xF8) << 2) | ((value & 7) << 12);
		}
		ppu->w = !ppu->w;
		break;
	case 6:
		if (!ppu->w) {
			ppu->t = (ppu->t & 0xFF) | ((value & 0x3F) << 8);
		} else {
			ppu->t = (ppu->t & 0x7F00) | value;
			ppu->v = ppu->t;
			if (!(rendering(ppu) && (ppu->scanline < 240 || ppu->scanline == preLine(nes)))) {
				setBus(nes, ppu->v & 0x3FFF);
			}
		}
		ppu->w = !ppu->w;
		break;
	case 7:
		busWriteVram(nes, ppu->v, value);
		if (rendering(ppu) && (ppu->scanline < 240 || ppu->scanline == preLine(nes))) {
			incrementX(ppu);
			incrementY(ppu);
		} else {
			ppu->v = (ppu->v + ((ppu->ctrl & CTRL_INCREMENT) ? 32 : 1)) & 0x7FFF;
		}
		setBus(nes, ppu->v & 0x3FFF);
		break;
	}
}

// Sprites for the next line, with the hardware's overflow search and its diagonal bug.
static void evaluateSprites(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int height = (ppu->ctrl & CTRL_TALL) ? 16 : 8;
	int line = ppu->scanline;
	int count = 0;
	ppu->spriteZeroNext = false;
	memset(ppu->secondary, 0xFF, sizeof(ppu->secondary));
	int n = 0;
	for (; n < 64 && count < 8; n++) {
		int y = ppu->oam[n * 4];
		if (line - y >= 0 && line - y < height) {
			if (n == 0) {
				ppu->spriteZeroNext = true;
			}
			memcpy(&ppu->secondary[count * 4], &ppu->oam[n * 4], 4);
			count++;
		}
	}
	// Past eight, the search reads Y from the wrong byte as it goes
	int m = 0;
	for (; n < 64; n++) {
		int y = ppu->oam[n * 4 + m];
		if (line - y >= 0 && line - y < height) {
			if (!(ppu->status & STATUS_OVERFLOW)) {
				ppu->status |= STATUS_OVERFLOW;
			}
			break;
		}
		m = (m + 1) & 3;
	}
	ppu->secondaryCount = count;
}

static inline uint16_t spritePatternAddress(struct Ppu* ppu, int slot, int line) {
	uint8_t* sprite = &ppu->secondary[slot * 4];
	int tile = sprite[1];
	int row = (uint8_t) (line - sprite[0]);
	bool flipV = sprite[2] & 0x80;
	if (slot >= ppu->secondaryCount) {
		row = 0;
		flipV = false;
		tile = 0xFF;
	}
	if (ppu->ctrl & CTRL_TALL) {
		row &= 15;
		if (flipV) {
			row = 15 - row;
		}
		uint16_t table = (tile & 1) ? 0x1000 : 0;
		tile &= 0xFE;
		if (row >= 8) {
			tile++;
			row -= 8;
		}
		return table | (tile << 4) | row;
	}
	row &= 7;
	if (flipV) {
		row = 7 - row;
	}
	return ((ppu->ctrl & CTRL_SPRITE_TABLE) ? 0x1000 : 0) | (tile << 4) | row;
}

static inline uint8_t reverseBits(uint8_t b) {
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}

static void spriteFetch(struct NES* nes, int dot) {
	struct Ppu* ppu = &nes->ppu;
	int slot = (dot - 257) >> 3;
	int phase = (dot - 257) & 7;
	int line = ppu->scanline;
	switch (phase) {
	case 0:
		setBus(nes, 0x2000 | (ppu->v & 0xFFF));
		break;
	case 2:
		setBus(nes, 0x23C0 | (ppu->v & 0x0C00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 7));
		break;
	case 4:
	case 6: {
		uint16_t address = spritePatternAddress(ppu, slot, line) | (phase == 6 ? 8 : 0);
		uint8_t value = fetchAt(nes, address);
		if (slot < ppu->secondaryCount) {
			if (ppu->secondary[slot * 4 + 2] & 0x40) {
				value = reverseBits(value);
			}
			if (phase == 4) {
				ppu->sprites[slot].lo = value;
			} else {
				ppu->sprites[slot].hi = value;
				ppu->sprites[slot].attr = ppu->secondary[slot * 4 + 2];
				ppu->sprites[slot].x = ppu->secondary[slot * 4 + 3];
			}
		}
		break;
	}
	}
}

static inline void backgroundFetch(struct NES* nes, int dot) {
	struct Ppu* ppu = &nes->ppu;
	switch ((dot - 1) & 7) {
	case 0:
		ppu->shiftLo |= ppu->bgLo;
		ppu->shiftHi |= ppu->bgHi;
		ppu->attrLo |= (ppu->atByte & 1) ? 0xFF : 0;
		ppu->attrHi |= (ppu->atByte & 2) ? 0xFF : 0;
		setBus(nes, 0x2000 | (ppu->v & 0xFFF));
		break;
	case 1:
		ppu->ntByte = ppuBusRead(nes, ppu->busAddress);
		break;
	case 2:
		setBus(nes, 0x23C0 | (ppu->v & 0x0C00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 7));
		break;
	case 3: {
		uint8_t at = ppuBusRead(nes, ppu->busAddress);
		int shift = ((ppu->v >> 4) & 4) | (ppu->v & 2);
		ppu->atByte = (at >> shift) & 3;
		break;
	}
	case 4:
		setBus(nes, ((ppu->ctrl & CTRL_BG_TABLE) ? 0x1000 : 0) | (ppu->ntByte << 4) | (ppu->v >> 12));
		break;
	case 5:
		ppu->bgLo = ppuBusRead(nes, ppu->busAddress);
		break;
	case 6:
		setBus(nes, ppu->busAddress | 8);
		break;
	case 7:
		ppu->bgHi = ppuBusRead(nes, ppu->busAddress);
		incrementX(ppu);
		break;
	}
}

static inline void drawPixel(struct NES* nes, int x) {
	struct Ppu* ppu = &nes->ppu;
	uint8_t color;
	if (!rendering(ppu)) {
		uint16_t v = ppu->v & 0x3FFF;
		color = v >= 0x3F00 ? readPalette(ppu, v) : ppu->palette[0];
	} else {
		int bg = 0;
		if ((ppu->mask & MASK_BG) && (x >= 8 || (ppu->mask & MASK_BG_LEFT))) {
			int bit = 15 - ppu->fineX;
			bg = ((ppu->shiftLo >> bit) & 1) | (((ppu->shiftHi >> bit) & 1) << 1);
			if (bg) {
				bg |= (((ppu->attrLo >> bit) & 1) | (((ppu->attrHi >> bit) & 1) << 1)) << 2;
			}
		}
		int sprite = 0;
		bool behind = false;
		if ((ppu->mask & MASK_SPRITES) && (x >= 8 || (ppu->mask & MASK_SPRITE_LEFT))) {
			for (int i = 0; i < ppu->spriteCount; i++) {
				struct SpriteLine* s = &ppu->sprites[i];
				int offset = x - s->x;
				if (offset < 0 || offset > 7) {
					continue;
				}
				int p = ((s->lo >> (7 - offset)) & 1) | (((s->hi >> (7 - offset)) & 1) << 1);
				if (!p) {
					continue;
				}
				if (i == 0 && ppu->spriteZeroLine && (bg & 3) && x != 255) {
					ppu->status |= STATUS_HIT;
				}
				sprite = 0x10 | ((s->attr & 3) << 2) | p;
				behind = s->attr & 0x20;
				break;
			}
		}
		int index;
		if (sprite && (!behind || !(bg & 3))) {
			index = sprite;
		} else if (bg & 3) {
			index = bg;
		} else {
			index = 0;
		}
		color = readPalette(ppu, index);
	}
	if (ppu->mask & MASK_GRAY) {
		color &= 0x30;
	}
	nes->frameBuffer[ppu->scanline * NES_WIDTH + x] = color | ((ppu->mask & 0xE0) << 1);
}

static inline void shift(struct Ppu* ppu) {
	ppu->shiftLo <<= 1;
	ppu->shiftHi <<= 1;
	ppu->attrLo <<= 1;
	ppu->attrHi <<= 1;
}

static void renderDot(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int dot = ppu->dot;
	bool visible = ppu->scanline < 240;
	bool pre = ppu->scanline == preLine(nes);

	if (!rendering(ppu) || !(visible || pre)) {
		if (visible && dot >= 1 && dot <= 256) {
			drawPixel(nes, dot - 1);
		}
		return;
	}

	// Shift, then fetch (which reloads the shifters at the start of each tile), then the pixel
	if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) {
		shift(ppu);
	}
	if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 336)) {
		backgroundFetch(nes, dot);
	} else if (dot == 337 || dot == 339) {
		setBus(nes, 0x2000 | (ppu->v & 0xFFF));
	} else if (dot == 338 || dot == 340) {
		ppu->ntByte = ppuBusRead(nes, ppu->busAddress);
	}
	if (visible && dot >= 1 && dot <= 256) {
		drawPixel(nes, dot - 1);
	}
	if (dot == 256) {
		incrementY(ppu);
		if (visible) {
			evaluateSprites(nes);
		} else {
			ppu->secondaryCount = 0;
			memset(ppu->secondary, 0xFF, sizeof(ppu->secondary));
			ppu->spriteZeroNext = false;
		}
	} else if (dot == 257) {
		ppu->v = (ppu->v & ~0x41F) | (ppu->t & 0x41F);
		// Shifter reload of the last tile fetched on this line
		ppu->shiftLo |= ppu->bgLo;
		ppu->shiftHi |= ppu->bgHi;
		ppu->attrLo |= (ppu->atByte & 1) ? 0xFF : 0;
		ppu->attrHi |= (ppu->atByte & 2) ? 0xFF : 0;
	}
	if (dot >= 257 && dot <= 320) {
		ppu->oamAddr = 0;
		spriteFetch(nes, dot);
		if (dot == 320) {
			ppu->spriteCount = ppu->secondaryCount;
			ppu->spriteZeroLine = ppu->spriteZeroNext;
		}
	}
	if (pre && dot >= 280 && dot <= 304) {
		ppu->v = (ppu->v & ~0x7BE0) | (ppu->t & 0x7BE0);
	}
}

static void step(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int vbl = vblankLine(nes);
	int pre = preLine(nes);

	if (ppu->scanline < 240 || ppu->scanline == pre) {
		renderDot(nes);
	}

	if (ppu->scanline == vbl && ppu->dot == 1) {
		if (!ppu->suppressVbl) {
			ppu->status |= STATUS_VBL;
			updateNmi(nes);
		}
		ppu->suppressVbl = false;
		nes->frameDone = true;
	} else if (ppu->scanline == pre && ppu->dot == 1) {
		ppu->status &= ~(STATUS_VBL | STATUS_HIT | STATUS_OVERFLOW);
		updateNmi(nes);
	}

	ppu->dot++;
	if (ppu->scanline == pre && ppu->dot == 340 && ppu->oddFrame && rendering(ppu) &&
		nes->region == REGION_NTSC) {
		// Odd frames skip the last dot of the pre-render line while rendering
		ppu->dot = 341;
	}
	if (ppu->dot > 340) {
		ppu->dot = 0;
		ppu->scanline++;
		if (ppu->scanline > pre) {
			ppu->scanline = 0;
			ppu->frame++;
			ppu->oddFrame = !ppu->oddFrame;
		}
	}
}

void ppuRun(struct NES* nes, uint64_t until) {
	struct Ppu* ppu = &nes->ppu;
	while (ppu->clock + ppu->dotClocks <= until) {
		ppu->clock += ppu->dotClocks;
		step(nes);
	}
}

void ppuReset(struct NES* nes, bool hard) {
	struct Ppu* ppu = &nes->ppu;
	ppu->ctrl = ppu->mask = 0;
	ppu->renderingEnabled = false;
	ppu->w = false;
	ppu->readBuffer = 0;
	ppu->oddFrame = false;
	if (hard) {
		ppu->status = 0;
		ppu->oamAddr = 0;
		ppu->v = ppu->t = 0;
		ppu->fineX = 0;
		static const uint8_t bootPalette[32] = {
			0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
			0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08,
		};
		memcpy(ppu->palette, bootPalette, sizeof(bootPalette));
		memset(ppu->oam, 0, sizeof(ppu->oam));
		memset(ppu->ntRam, 0, sizeof(ppu->ntRam));
	}
	ppu->scanline = 0;
	ppu->dot = 0;
	ppu->dotClocks = nes->region == REGION_NTSC ? 4 : 5;
	ppu->clock = 0;
	ppu->spriteCount = 0;
	ppu->secondaryCount = 0;
}
