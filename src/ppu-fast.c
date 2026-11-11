/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"
#include "boards/mmc3.h"

#include <string.h>

// The fast PPU: dot-timed where games can see it (VBL, NMI, sprite 0 hit, scroll writes mid-line,
// the A12 edges mappers count) and batched where they can't: tiles are fetched once each, sprites
// are found once per line and drawn from a line of pixels.

enum { SPRITE_OPAQUE = 0x10, SPRITE_BEHIND = 0x20, SPRITE_ZERO = 0x40 };

static inline bool rendering(const struct Ppu* ppu) {
	return ppu->showBg || ppu->showSprites;
}

static inline int vblankLine(const struct NES* nes) {
	return nes->region == REGION_DENDY ? 291 : 241;
}

static inline int preLine(const struct NES* nes) {
	return nes->region == REGION_NTSC ? 261 : 311;
}

static inline uint8_t chrRead(struct NES* nes, uint16_t address) {
	if (nes->ppuReadHook) {
		return nes->board->ppuRead(nes, address);
	}
	uint8_t* page = nes->chrPages[address >> 10];
	return page ? page[address & 0x3FF] : 0;
}

// What the PPU puts on its address bus; boards that watch it see it on the next dot
static inline void busAddress(struct NES* nes, uint16_t address) {
	nes->ppu.addressBus = address;
}

// A dot shows the board the bus as the last one left it; an MMC3, which only acts on A12 changing, is
// shown only the changes
static inline void showBus(struct NES* nes) {
	uint16_t const address = nes->ppu.addressBus;
	if (nes->board->ppuBus == mmc3PpuBus) {
		bool const a12 = address & 0x1000;
		if (nes->busShown && a12 == nes->busA12Shown) {
			return;
		}
		nes->busShown = true;
		nes->busA12Shown = a12;
	}
	nes->board->ppuBus(nes, address);
}

static inline uint8_t paletteRead(const struct Ppu* ppu, int index) {
	index &= 0x1F;
	if (!(index & 3)) {
		index &= 0x0F;
	}
	return ppu->palette[index];
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

static inline uint8_t reverseBits(uint8_t b) {
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}

// The next line's sprites, with the overflow search's diagonal bug
static void evaluateSprites(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int height = ppu->tall ? 16 : 8;
	int line = ppu->scanline;
	int count = 0;
	ppu->nextZero = false;
	int n = 0;
	for (; n < 64 && count < 8; n++) {
		int row = line - ppu->oam[n * 4];
		if (row >= 0 && row < height) {
			if (n == 0) {
				ppu->nextZero = true;
			}
			memcpy(&ppu->oam2[count * 4], &ppu->oam[n * 4], 4);
			count++;
		}
	}
	for (int m = 0; n < 64; n++) {
		int row = line - ppu->oam[n * 4 + m];
		if (row >= 0 && row < height) {
			ppu->overflow = true;
			break;
		}
		m = (m + 1) & 3;
	}
	ppu->nextCount = count;
}

static uint16_t patternAddress(const struct Ppu* ppu, int tile, int row, bool flipV) {
	if (ppu->tall) {
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
	return (ppu->spriteTable ? 0x1000 : 0) | (tile << 4) | row;
}

static uint16_t spriteAddress(const struct Ppu* ppu, int slot, int line) {
	const uint8_t* sprite = &ppu->oam2[slot * 4];
	if (slot >= ppu->nextCount) {
		// Empty slots fetch tile $FF
		return patternAddress(ppu, 0xFF, 0, false);
	}
	return patternAddress(ppu, sprite[1], (uint8_t) (line - sprite[0]), sprite[2] & 0x80);
}

// With the sprite limit lifted: the line's sprites past the eighth, behind the eight the PPU shows.
// Only the picture changes; their patterns come straight from the CHR pages, so boards that watch
// fetches or supply graphics as they're read keep the limit.
static void extraSprites(struct NES* nes, int line, int height) {
	struct Ppu* ppu = &nes->ppu;
	int found = 0;
	for (int n = 0; n < 64; n++) {
		const uint8_t* sprite = &ppu->oam[n * 4];
		int row = (uint8_t) (line - sprite[0]);
		if (row >= height || ++found <= 8) {
			continue;
		}
		uint16_t address = patternAddress(ppu, sprite[1], row, sprite[2] & 0x80);
		const uint8_t* page = nes->chrPages[address >> 10];
		if (!page) {
			continue;
		}
		uint8_t lo = page[address & 0x3FF], hi = page[(address | 8) & 0x3FF];
		uint8_t attr = sprite[2];
		if (attr & 0x40) {
			lo = reverseBits(lo);
			hi = reverseBits(hi);
		}
		uint8_t flags = SPRITE_OPAQUE | ((attr & 3) << 2) | ((attr & 0x20) ? SPRITE_BEHIND : 0);
		int x = sprite[3];
		for (int i = 0; i < 8 && x + i < 256; i++) {
			int color = ((lo >> (7 - i)) & 1) | (((hi >> (7 - i)) & 1) << 1);
			if (color && !ppu->lineSprites[x + i]) {
				ppu->lineSprites[x + i] = flags | color;
				ppu->lineHasSprites = true;
			}
		}
	}
}

// The pattern fetches for the next line's sprites, drawn into a line of pixels
static void fetchSprites(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int line = ppu->scanline & 0xFF;
	int height = ppu->tall ? 16 : 8;
	memset(ppu->lineSprites, 0, sizeof(ppu->lineSprites));
	ppu->lineHasSprites = false;
	// Fetched in slot order, as boards that latch on tiles see them
	uint8_t los[8], his[8];
	ppu->spriteFetch = true;
	bool fetched[8] = { false };
	for (int slot = 0; slot < ppu->nextCount; slot++) {
		if ((uint8_t) (line - ppu->oam2[slot * 4]) >= height) {
			continue;
		}
		uint16_t address = spriteAddress(ppu, slot, line);
		los[slot] = chrRead(nes, address);
		his[slot] = chrRead(nes, address | 8);
		fetched[slot] = true;
	}
	ppu->spriteFetch = false;
	for (int slot = ppu->nextCount - 1; slot >= 0; slot--) {
		if (!fetched[slot]) {
			continue;
		}
		uint8_t lo = los[slot], hi = his[slot];
		const uint8_t* sprite = &ppu->oam2[slot * 4];
		uint8_t attr = sprite[2];
		if (attr & 0x40) {
			lo = reverseBits(lo);
			hi = reverseBits(hi);
		}
		if (!(lo | hi)) {
			continue;
		}
		ppu->lineHasSprites = true;
		uint8_t flags = SPRITE_OPAQUE | ((attr & 3) << 2) | ((attr & 0x20) ? SPRITE_BEHIND : 0) |
			(slot == 0 && ppu->nextZero ? SPRITE_ZERO : 0);
		int x = sprite[3];
		for (int i = 0; i < 8 && x + i < 256; i++) {
			int color = ((lo >> (7 - i)) & 1) | (((hi >> (7 - i)) & 1) << 1);
			if (color) {
				// Lower slots win, and they're drawn last
				ppu->lineSprites[x + i] = flags | color;
			}
		}
	}
	if (nes->noSpriteLimit && ppu->nextCount == 8 && !nes->ppuReadHook) {
		extraSprites(nes, line, height);
	}
	ppu->lineHasZero = ppu->nextZero;
}

// Address bus activity of the sprite fetches, for boards that watch A12
static void spriteFetchBus(struct NES* nes, int dot) {
	struct Ppu* ppu = &nes->ppu;
	int slot = (dot - 257) >> 3;
	switch ((dot - 257) & 7) {
	case 0:
	case 2:
		busAddress(nes, 0x2000 | (ppu->v & 0xFFF));
		break;
	case 4:
		busAddress(nes, spriteAddress(ppu, slot, ppu->scanline & 0xFF));
		break;
	case 6:
		busAddress(nes, spriteAddress(ppu, slot, ppu->scanline & 0xFF) | 8);
		break;
	}
}

// The background fetches of a tile's eight dots: name table, attribute, then the two pattern
// planes, the last of which loads the shifters and moves to the next tile
static inline void fetchTile(struct NES* nes, int dot) {
	struct Ppu* ppu = &nes->ppu;
	bool watch = nes->watchPpuBus;
	switch (dot & 7) {
	case 1:
		if (watch) {
			busAddress(nes, 0x2000 | (ppu->v & 0xFFF));
		}
		break;
	case 2:
		ppu->ntLatch = chrRead(nes, 0x2000 | (ppu->v & 0xFFF));
		break;
	case 3:
		if (watch) {
			busAddress(nes, 0x23C0 | (ppu->v & 0xC00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 7));
		}
		break;
	case 4: {
		uint8_t at = chrRead(nes, 0x23C0 | (ppu->v & 0xC00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 7));
		ppu->atLatch = (at >> (((ppu->v >> 4) & 4) | (ppu->v & 2))) & 3;
		break;
	}
	case 5:
		if (watch) {
			busAddress(nes, (ppu->bgTable ? 0x1000 : 0) | (ppu->ntLatch << 4) | (ppu->v >> 12));
		}
		break;
	case 6:
		ppu->loLatch = chrRead(nes, (ppu->bgTable ? 0x1000 : 0) | (ppu->ntLatch << 4) | (ppu->v >> 12));
		break;
	case 7:
		if (watch) {
			busAddress(nes, (ppu->bgTable ? 0x1000 : 0) | (ppu->ntLatch << 4) | (ppu->v >> 12) | 8);
		}
		break;
	case 0:
		ppu->hiLatch = chrRead(nes, (ppu->bgTable ? 0x1000 : 0) | (ppu->ntLatch << 4) | (ppu->v >> 12) | 8);
		break;
	}
}

static inline void reload(struct Ppu* ppu) {
	ppu->bgL = (ppu->bgL & 0xFF00) | ppu->loLatch;
	ppu->bgH = (ppu->bgH & 0xFF00) | ppu->hiLatch;
	ppu->atL = (ppu->atL & 0xFF00) | ((ppu->atLatch & 1) ? 0xFF : 0);
	ppu->atH = (ppu->atH & 0xFF00) | ((ppu->atLatch & 2) ? 0xFF : 0);
}

static inline void shift(struct Ppu* ppu) {
	ppu->bgL <<= 1;
	ppu->bgH <<= 1;
	ppu->atL <<= 1;
	ppu->atH <<= 1;
}

static inline void drawPixel(struct NES* nes, int x) {
	struct Ppu* ppu = &nes->ppu;
	uint8_t color;
	if (!rendering(ppu)) {
		color = (ppu->v & 0x3F00) == 0x3F00 ? paletteRead(ppu, ppu->v) : ppu->palette[0];
	} else {
		int bg = 0;
		if (ppu->showBg && (x >= 8 || ppu->showBgLeft)) {
			int bit = 15 - ppu->fineX;
			bg = ((ppu->bgL >> bit) & 1) | (((ppu->bgH >> bit) & 1) << 1);
			if (bg) {
				bg |= (((ppu->atL >> bit) & 1) | (((ppu->atH >> bit) & 1) << 1)) << 2;
			}
		}
		uint8_t sprite = ppu->lineSprites[x];
		int index = bg;
		if ((sprite & SPRITE_OPAQUE) && ppu->showSprites && (x >= 8 || ppu->showSpritesLeft)) {
			if ((sprite & SPRITE_ZERO) && bg && ppu->showBg && x != 255 && !ppu->hit && !ppu->pendingHit) {
				// The flag shows a couple of dots after the pixel
				ppu->pendingHit = true;
				ppu->hitDelay = 3;
			}
			if (!bg || !(sprite & SPRITE_BEHIND)) {
				index = 0x10 | (sprite & 0x0F);
			}
		}
		color = paletteRead(ppu, index);
	}
	if (ppu->gray) {
		color &= 0x30;
	}
	uint16_t emphasis = (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
	uint16_t* line = &nes->frameBuffer[ppu->scanline * NES_WIDTH];
	if (ppu->scanline == 0 && ppu->skippedDot) {
		// After the skipped dot, line 0 comes out a pixel early and ends on the backdrop
		if (x) {
			line[x - 1] = color | emphasis;
		}
		if (x == 255) {
			line[255] = ppu->palette[0] | emphasis;
		}
		return;
	}
	line[x] = color | emphasis;
}

// One dot of a line that renders (visible or pre-render) with rendering on: the pixel from the
// shifters, the fetch, the shift, and every eighth dot the next tile into the shifters
static inline void renderDot(struct NES* nes, int dot, bool visible, bool pre) {
	struct Ppu* ppu = &nes->ppu;
	if (dot >= 1 && dot <= 256) {
		if (visible) {
			drawPixel(nes, dot - 1);
		}
		fetchTile(nes, dot);
		shift(ppu);
		if (!(dot & 7)) {
			reload(ppu);
			incrementX(ppu);
		}
		if (dot == 256) {
			incrementY(ppu);
			if (visible) {
				evaluateSprites(nes);
			} else {
				// The pre-render line fetches what line 239 left in secondary OAM
				ppu->nextZero = false;
			}
		}
	} else if (dot <= 320) {
		if (dot == 257) {
			shift(ppu);
			ppu->v = (ppu->v & ~0x41F) | (ppu->t & 0x41F);
		}
		ppu->oamAddr = 0;
		if (nes->watchPpuBus) {
			spriteFetchBus(nes, dot);
		}
		if (pre && dot >= 280 && dot <= 304) {
			ppu->v = (ppu->v & ~0x7BE0) | (ppu->t & 0x7BE0);
		}
		if (dot == 320) {
			fetchSprites(nes);
		}
	} else if (dot <= 336) {
		fetchTile(nes, dot);
		shift(ppu);
		if (!(dot & 7)) {
			reload(ppu);
			incrementX(ppu);
		}
	} else if (nes->watchPpuBus && (dot == 337 || dot == 339)) {
		busAddress(nes, 0x2000 | (ppu->v & 0xFFF));
	}
}

// The Balanced mode: sprites as the hardware handles them, a dot at a time (after ppu.c, without
// its half-dot latches and OAM corruption). Secondary OAM is cleared on dots 1-64, filled from OAM on
// 65-256 (odd dots read, even dots check and copy, with the overflow search's diagonal walk), then
// fetched slot by slot on 257-320 into eight shifters with X counters.

static inline void dotIncrementOam2(struct Ppu* ppu) {
	if (!ppu->oam2Overflowed && ++ppu->oam2Addr == 0x20) {
		ppu->oam2Overflowed = true;
		ppu->oam2Addr = 0;
	}
}

static inline void dotEvaluate(struct NES* nes, bool readOnly) {
	struct Ppu* ppu = &nes->ppu;
	int height = ppu->tall ? 16 : 8;
	uint8_t before = ppu->oamAddr;
	if (!ppu->oam2Overflowed && !readOnly) {
		ppu->oam2[ppu->oam2Addr] = ppu->oamBuffer;
	}
	uint8_t oam2Read = ppu->oam2[ppu->oam2Addr];
	if (ppu->evalTick == 0) {
		ppu->verticalOffset = (uint16_t) ((ppu->scanline & 0xFF) - ppu->oamBuffer);
		if (!ppu->nineObjects && !readOnly && ppu->verticalOffset < height) {
			if (!ppu->oam2Overflowed) {
				ppu->oamAddr++;
				dotIncrementOam2(ppu);
				if (ppu->dot == 66) {
					ppu->nextHasZero = true;
				}
			} else {
				ppu->nineObjects = true;
				ppu->overflow = true;
				ppu->oamAddr++;
			}
			ppu->evalTick++;
		} else {
			if (ppu->dot == 66) {
				ppu->nextHasZero = false;
			}
			if (!readOnly) {
				if (ppu->oam2Overflowed && !ppu->nineObjects) {
					// The overflow search's diagonal walk
					ppu->oamAddr += (ppu->oamAddr & 3) == 3 ? 1 : 5;
				} else {
					ppu->oamAddr = (ppu->oamAddr + 4) & 0xFC;
				}
			}
		}
	} else {
		if (ppu->evalTick == 3) {
			int offset = ppu->scanline - ppu->oamBuffer;
			if (offset >= 0 && offset < height) {
				if (!readOnly) {
					ppu->oamAddr += ppu->oam2Overflowed ? 4 : 1;
				}
			} else {
				ppu->oamAddr = (ppu->oamAddr + 1) & 0xFC;
			}
		} else if (!readOnly) {
			ppu->oamAddr++;
		}
		ppu->evalTick = (ppu->evalTick + 1) & 3;
		if (!ppu->oam2Overflowed && !readOnly) {
			dotIncrementOam2(ppu);
		}
	}
	if (ppu->oamAddr < before && ppu->oamAddr < 4) {
		ppu->oamAddrOverflowed = true;
	}
	ppu->oamBuffer = oam2Read;
}

// A sprite pattern's address for the slot being fetched
static uint16_t dotPatternAddress(const struct Ppu* ppu) {
	bool flipY = ppu->spriteAttrNow & 0x80;
	int row = flipY ? 7 - (ppu->verticalOffset & 7) : (ppu->verticalOffset & 7);
	if (!ppu->tall) {
		return (ppu->spriteTable ? 0x1000 : 0) | (ppu->spritePattern << 4) | row;
	}
	return ((ppu->spritePattern & 1) ? 0x1000 : 0) | ((ppu->spritePattern & 0xFE) << 4) |
		(((ppu->verticalOffset & 8) ^ (flipY ? 8 : 0)) << 1) | row;
}

static inline void dotSpriteFetch(struct NES* nes, int dot) {
	struct Ppu* ppu = &nes->ppu;
	int slot = ((dot - 1) & 0x38) >> 3;
	int height = ppu->tall ? 16 : 8;
	// Each dot uses the byte the dot before read, and reads the next
	uint8_t const read = ppu->oam2[ppu->oam2Addr];
	switch ((dot - 1) & 7) {
	case 0:
		busAddress(nes, 0x2000 | (ppu->v & 0xFFF));
		dotIncrementOam2(ppu);
		break;
	case 1:
		ppu->verticalOffset = (uint16_t) ((ppu->scanline & 0xFF) - ppu->oamBuffer);
		dotIncrementOam2(ppu);
		break;
	case 2:
		ppu->spritePattern = ppu->oamBuffer;
		busAddress(nes, 0x2000 | (ppu->v & 0xFFF));
		dotIncrementOam2(ppu);
		break;
	case 3:
		ppu->spriteAttr[slot] = ppu->oamBuffer;
		ppu->spriteAttrNow = ppu->oamBuffer;
		break;
	case 4:
		ppu->spriteCounter[slot] = ppu->oamBuffer;
		ppu->spriteX[slot] = ppu->oamBuffer;
		busAddress(nes, dotPatternAddress(ppu));
		break;
	case 5: {
		ppu->spriteFetch = true;
		uint8_t lo = chrRead(nes, dotPatternAddress(ppu));
		ppu->spriteFetch = false;
		ppu->spriteL[slot] = ppu->verticalOffset < height ? (ppu->spriteAttr[slot] & 0x40 ? reverseBits(lo) : lo) : 0;
		break;
	}
	case 6:
		busAddress(nes, dotPatternAddress(ppu) | 8);
		break;
	case 7: {
		ppu->spriteFetch = true;
		uint8_t hi = chrRead(nes, dotPatternAddress(ppu) | 8);
		ppu->spriteFetch = false;
		ppu->spriteH[slot] = ppu->verticalOffset < height ? (ppu->spriteAttr[slot] & 0x40 ? reverseBits(hi) : hi) : 0;
		dotIncrementOam2(ppu);
		break;
	}
	}
	ppu->oamBuffer = read;
}

// Secondary OAM's address goes back to 0, and its overflowed flag clears, the dot after 63, 255 or 339
// if rendering was on then
static inline void dotOam2Reset(struct Ppu* ppu, int dot) {
	if (ppu->oam2Reset && (dot == 64 || dot == 256 || dot == 340)) {
		ppu->oam2Reset = 0;
		ppu->oam2Addr = 0;
		ppu->oam2Overflowed = false;
	}
}

// One dot of sprite work on a line that renders
static inline void dotSprites(struct NES* nes, int dot, bool pre) {
	struct Ppu* ppu = &nes->ppu;
	if (dot == 63 || dot == 255 || dot == 339) {
		ppu->oam2Reset = 1;
	}
	if (dot == 0) {
		dotIncrementOam2(ppu);
	} else if (dot <= 64) {
		if (dot & 1) {
			ppu->oamBuffer = pre ? ppu->oam2[ppu->oam2Addr] : 0xFF;
			if (dot == 1) {
				ppu->oam2Addr = 0;
				ppu->evalTick = 0;
				ppu->oamAddrOverflowed = false;
			}
		} else {
			if (!pre) {
				ppu->oam2[ppu->oam2Addr] = ppu->oamBuffer;
			}
			dotIncrementOam2(ppu);
		}
	} else if (dot <= 256) {
		if (dot == 65) {
			ppu->nineObjects = false;
			ppu->oamAddrOverflowed = false;
		}
		if (dot & 1) {
			ppu->oamBuffer = ppu->oam[ppu->oamAddr];
		} else if (!ppu->oamAddrOverflowed) {
			dotEvaluate(nes, pre);
		} else {
			if (!pre) {
				ppu->oamAddr = (ppu->oamAddr + 4) & 0xFC;
			}
			ppu->oamBuffer = ppu->oam2[ppu->oam2Addr];
		}
	} else if (dot <= 320) {
		ppu->hasZero = ppu->nextHasZero;
		ppu->oamAddr = 0;
		dotSpriteFetch(nes, dot);
	} else {
		ppu->oamBuffer = ppu->oam2[ppu->oam2Addr];
	}
}

// The eight slots a byte each in a word: 1 in each byte that isn't 0
static inline uint64_t slotsNonzero(uint64_t x) {
	return (((x & 0x7F7F7F7F7F7F7F7Full) + 0x7F7F7F7F7F7F7F7Full) | x) >> 7 & 0x0101010101010101ull;
}

// The sprite shifters after a pixel: a slot counts its X down, then shifts out its pattern; all
// eight at once
static inline void dotShiftSprites(struct Ppu* ppu) {
	uint64_t counters, lo, hi;
	memcpy(&lo, ppu->spriteL, 8);
	memcpy(&hi, ppu->spriteH, 8);
	// Slots with nothing left to draw can't show; the next fetch reloads their counters
	if (!(lo | hi)) {
		return;
	}
	memcpy(&counters, ppu->spriteCounter, 8);
	uint64_t const counting = ppu->scanline == 0 && ppu->skippedDot ? 0 : slotsNonzero(counters);
	uint64_t const keep = counting * 0xFF;
	counters -= counting;
	lo = (lo & keep) | ((lo << 1) & 0xFEFEFEFEFEFEFEFEull & ~keep);
	hi = (hi & keep) | ((hi << 1) & 0xFEFEFEFEFEFEFEFEull & ~keep);
	memcpy(ppu->spriteCounter, &counters, 8);
	memcpy(ppu->spriteL, &lo, 8);
	memcpy(ppu->spriteH, &hi, 8);
}

static void dotIdleSprites(struct Ppu* ppu, int dot) {
	if (dot >= 1 && dot <= 256) {
		for (int i = 0; i < 8; i++) {
			if (ppu->spriteCounter[i] && !(ppu->scanline == 0 && ppu->skippedDot)) {
				ppu->spriteCounter[i]--;
			}
		}
	} else if (dot == 339) {
		memset(ppu->spriteCounter, 0, sizeof(ppu->spriteCounter));
	}
}

// The pixel at x from the background shifters and the sprite slots
static inline void dotPixel(struct NES* nes, int x) {
	struct Ppu* ppu = &nes->ppu;
	int bg = 0;
	if (ppu->showBg && (x >= 8 || ppu->showBgLeft)) {
		int bit = 15 - ppu->fineX;
		bg = ((ppu->bgL >> bit) & 1) | (((ppu->bgH >> bit) & 1) << 1);
		if (bg) {
			bg |= (((ppu->atL >> bit) & 1) | (((ppu->atH >> bit) & 1) << 1)) << 2;
		}
	}
	int index = bg;
	if (ppu->showSprites && (x >= 8 || ppu->showSpritesLeft)) {
		// The first slot past its X with a color, all eight at once
		uint64_t counters, lo, hi;
		memcpy(&lo, ppu->spriteL, 8);
		memcpy(&hi, ppu->spriteH, 8);
		memcpy(&counters, ppu->spriteCounter, 8);
		uint64_t const colors = !(lo | hi) ? 0 : ((lo >> 7 & 0x0101010101010101ull) | (hi >> 6 & 0x0202020202020202ull)) &
			(slotsNonzero(counters) ^ 0x0101010101010101ull) * 3;
		if (colors) {
			int const i = __builtin_ctzll(colors) >> 3, color = colors >> (i * 8) & 3;
			if (i == 0 && ppu->hasZero && bg && ppu->showBg && x != 255 && !ppu->hit && !ppu->pendingHit) {
				ppu->pendingHit = true;
				ppu->hitDelay = 3;
			}
			if (!bg || !(ppu->spriteAttr[i] & 0x20)) {
				index = 0x10 | ((ppu->spriteAttr[i] & 3) << 2) | color;
			}
		}
	}
	uint8_t color = paletteRead(ppu, index);
	if (ppu->gray) {
		color &= 0x30;
	}
	uint16_t emphasis = (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
	uint16_t* line = &nes->frameBuffer[ppu->scanline * NES_WIDTH];
	if (ppu->scanline == 0 && ppu->skippedDot) {
		if (x) {
			line[x - 1] = color | emphasis;
		}
		if (x == 255) {
			line[255] = ppu->palette[0] | emphasis;
		}
		return;
	}
	line[x] = color | emphasis;
}

// One dot of a line that renders, in the Balanced mode
static inline void renderDotBalanced(struct NES* nes, int dot, bool visible, bool pre) {
	struct Ppu* ppu = &nes->ppu;
	if (dot >= 1 && dot <= 256) {
		if (visible) {
			dotPixel(nes, dot - 1);
		}
		dotShiftSprites(ppu);
		fetchTile(nes, dot);
		shift(ppu);
		if (!(dot & 7)) {
			reload(ppu);
			incrementX(ppu);
		}
		if (dot == 256) {
			incrementY(ppu);
		}
		dotSprites(nes, dot, pre);
	} else if (dot <= 320) {
		if (dot == 257) {
			shift(ppu);
			ppu->v = (ppu->v & ~0x41F) | (ppu->t & 0x41F);
		}
		if (pre && dot >= 280 && dot <= 304) {
			ppu->v = (ppu->v & ~0x7BE0) | (ppu->t & 0x7BE0);
		}
		dotSprites(nes, dot, pre);
	} else if (dot <= 336) {
		fetchTile(nes, dot);
		shift(ppu);
		if (!(dot & 7)) {
			reload(ppu);
			incrementX(ppu);
		}
		dotSprites(nes, dot, pre);
	} else {
		if (nes->watchPpuBus && (dot == 337 || dot == 339)) {
			busAddress(nes, 0x2000 | (ppu->v & 0xFFF));
		}
		dotSprites(nes, dot, pre);
	}
	dotOam2Reset(ppu, dot);
}

// The PPU outputs each pixel three dots after choosing its color, with the emphasis and grayscale
// of that moment: when they change, the pixels drawn in the last three dots take the new ones
static void retint(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (ppu->scanline >= 240 || !(ppu->scanline || !ppu->skippedDot)) {
		return;
	}
	uint16_t emphasis = (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
	uint16_t* line = &nes->frameBuffer[ppu->scanline * NES_WIDTH];
	for (int x = ppu->dot - 3; x <= ppu->dot - 1; x++) {
		if (x >= 0 && x < 256) {
			line[x] = (line[x] & (ppu->gray ? 0x30 : 0x3F)) | emphasis;
		}
	}
}

static void vramIncrement(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (rendering(ppu) && (ppu->scanline < 240 || ppu->scanline == preLine(nes))) {
		incrementY(ppu);
	} else {
		ppu->v = (ppu->v + (ppu->increment32 ? 32 : 1)) & 0x7FFF;
	}
}

// Register writes and $2007 accesses take effect a few dots later, as in the half-dot model
static void registerEffects(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (ppu->delay2005 && --ppu->delay2005 == 0) {
		if (!ppu->w) {
			ppu->fineX = ppu->value2005 & 7;
			ppu->t = (ppu->t & 0x7FE0) | (ppu->value2005 >> 3);
		} else {
			ppu->t = (ppu->t & 0x0C1F) | ((ppu->value2005 & 0xF8) << 2) | ((ppu->value2005 & 7) << 12);
		}
		ppu->w = !ppu->w;
	}
	if (ppu->delay2006 && --ppu->delay2006 == 0) {
		ppu->v = ppu->t;
		if (!rendering(ppu)) {
			busAddress(nes, ppu->v & 0x3FFF);
		}
	}
	if (ppu->delay2001 && --ppu->delay2001 == 0) {
		uint8_t value = ppu->value2001;
		ppu->showBgLeft = value & 0x02;
		ppu->showSpritesLeft = value & 0x04;
		ppu->showBg = value & 0x08;
		ppu->showSprites = value & 0x10;
	}
	if (ppu->delay2001Emphasis && --ppu->delay2001Emphasis == 0) {
		ppu->gray = ppu->value2001 & 0x01;
		ppu->emphRed = ppu->value2001 & 0x20;
		ppu->emphGreen = ppu->value2001 & 0x40;
		ppu->emphBlue = ppu->value2001 & 0x80;
		retint(nes);
	}
	if (ppu->vramReadStep) {
		switch (++ppu->vramReadStep) {
		case 3:
			ppu->vramReadAddress = ppu->v & 0x3FFF;
			break;
		case 6:
			ppu->readBuffer = chrRead(nes, ppu->vramReadAddress);
			vramIncrement(nes);
			ppu->vramReadStep = 0;
			break;
		}
	}
	if (ppu->vramWriteStep) {
		switch (++ppu->vramWriteStep) {
		case 3:
			ppu->vramWriteAddress = ppu->v & 0x3FFF;
			break;
		case 5: {
			uint16_t v = ppu->vramWriteAddress;
			if (v >= 0x3F00) {
				ppu->palette[v & ((v & 3) == 0 ? 0x0F : 0x1F)] = ppu->writeData & 0x3F;
			} else {
				int page = v >> 10;
				if (nes->chrWritable[page] && nes->chrPages[page]) {
					nes->chrPages[page][v & 0x3FF] = ppu->writeData;
				}
			}
			break;
		}
		case 6:
			vramIncrement(nes);
			ppu->vramWriteStep = 0;
			break;
		}
	}
}

static inline void step(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int pre = preLine(nes);
	ppu->dotTick += nes->ppuPeriod;
	// Boards see the bus as the last dot left it
	if (nes->watchPpuBus) {
		showBus(nes);
	}
	if (ppu->delay2001 | ppu->delay2001Emphasis | ppu->delay2005 | ppu->delay2006 | ppu->vramReadStep | ppu->vramWriteStep) {
		registerEffects(nes);
	}
	if (++ppu->dot > 340) {
		ppu->dot = 0;
		if (nesOverclockHold(nes, ppu->scanline)) {
		} else if (++ppu->scanline > pre) {
			ppu->scanline = 0;
			ppu->frame++;
		} else if (ppu->scanline == pre) {
			ppu->oddFrame = !ppu->oddFrame;
		} else if (ppu->scanline == 1) {
			ppu->skippedDot = false;
		}
	}
	int line = ppu->scanline, dot = ppu->dot;
	ppu->dots++;
	if (dot == 0 && line <= 240 && nes->board->scanline && (rendering(ppu) || line == 240) && !nes->overclockHolding) {
		nes->board->scanline(nes, line);
	}
	if (dot == 0 && line == 240) {
		nesOverclockPoint(nes, true);
	}
	if (ppu->pendingHit && --ppu->hitDelay == 0) {
		ppu->pendingHit = false;
		ppu->hit = true;
	}
	bool visible = line < 240;
	if (visible || line == pre) {
		if (dot == 0 && visible && rendering(ppu)) {
			// Dot 0 puts the background pattern table on the bus
			busAddress(nes, (ppu->bgTable ? 0x1000 : 0) | (ppu->ntLatch << 4) | (ppu->v >> 12));
		}
		if (rendering(ppu)) {
			if (nes->dotPpu) {
				renderDotBalanced(nes, dot, visible, line == pre);
			} else {
				renderDot(nes, dot, visible, line == pre);
			}
			if (line == pre && dot == 339 && ppu->oddFrame && nes->region == REGION_NTSC) {
				// Odd frames skip the last dot of the pre-render line while rendering
				ppu->dot = 340;
				ppu->skippedDot = true;
			}
		} else {
			if (visible && dot >= 1 && dot <= 256) {
				drawPixel(nes, dot - 1);
			}
			// Balanced: with rendering off the sprite counters go on counting, and are cleared at dot 339
			if (nes->dotPpu) {
				dotIdleSprites(ppu, dot);
				dotOam2Reset(ppu, dot);
			}
		}
		if (line == pre && dot == 1) {
			ppu->vblank = false;
			ppu->hit = false;
			ppu->pendingHit = false;
			ppu->overflow = false;
		}
	} else if (line == vblankLine(nes) && dot == 1) {
		if (!ppu->read2002) {
			ppu->vblank = true;
		}
		ppu->read2002 = false;
		ppu->resetting = false;
		nes->frameDone = true;
		nesOverclockPoint(nes, false);
	}
	if (!rendering(ppu) && nes->watchPpuBus) {
		busAddress(nes, ppu->v & 0x3FFF);
	}
}

static inline bool quiet(const struct NES* nes) {
	const struct Ppu* ppu = &nes->ppu;
	return !(ppu->delay2001 | ppu->delay2001Emphasis | ppu->delay2005 | ppu->delay2006 | ppu->vramReadStep |
		ppu->vramWriteStep | ppu->pendingHit);
}

// Each bit of a byte spread to its own byte, most significant first: eight pixels at once
static uint64_t spread[256];

static void buildSpread(void) {
	if (spread[1]) {
		return;
	}
	for (int b = 0; b < 256; b++) {
		uint64_t v = 0;
		for (int i = 0; i < 8; i++) {
			if (b & (0x80 >> i)) {
				v |= (uint64_t) 1 << (i * 8);
			}
		}
		spread[b] = v;
	}
}

// Eight pixels from their background indices and sprite pixels (or none): palette entries 0, 4,
// 8 and 12 show the backdrop, sprites win where opaque unless behind an opaque background
static inline void drawTileScalar(uint16_t* out, uint64_t pixels, const uint8_t* sprites, const uint8_t* palette,
	uint8_t grayMask, uint16_t emphasis) {
	for (int i = 0; i < 8; i++) {
		uint8_t bg = pixels >> (i * 8);
		uint8_t index = bg & 3 ? bg : 0;
		if (sprites) {
			uint8_t sprite = sprites[i];
			if ((sprite & SPRITE_OPAQUE) && (!(bg & 3) || !(sprite & SPRITE_BEHIND))) {
				index = 0x10 | (sprite & 0x0F);
			}
		}
		out[i] = (palette[index] & grayMask) | emphasis;
	}
}

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>

static inline uint8x8_t lookup32(const uint8_t* palette, uint8x8_t index) {
#ifdef __aarch64__
	uint8x16x2_t table = { { vld1q_u8(palette), vld1q_u8(palette + 16) } };
	return vqtbl2_u8(table, index);
#else
	uint8x8x4_t table = { { vld1_u8(palette), vld1_u8(palette + 8), vld1_u8(palette + 16), vld1_u8(palette + 24) } };
	return vtbl4_u8(table, index);
#endif
}

static void drawTile(uint16_t* out, uint64_t pixels, const uint8_t* sprites, const uint8_t* palette,
	uint8_t grayMask, uint16_t emphasis) {
	uint8x8_t bg = vcreate_u8(pixels);
	uint8x8_t bgOpaque = vtst_u8(bg, vdup_n_u8(3));
	uint8x8_t index = vand_u8(bg, bgOpaque);
	if (sprites) {
		uint8x8_t sprite = vld1_u8(sprites);
		uint8x8_t opaque = vtst_u8(sprite, vdup_n_u8(SPRITE_OPAQUE));
		uint8x8_t behind = vtst_u8(sprite, vdup_n_u8(SPRITE_BEHIND));
		uint8x8_t show = vand_u8(opaque, vorr_u8(vmvn_u8(bgOpaque), vmvn_u8(behind)));
		uint8x8_t spriteIndex = vorr_u8(vand_u8(sprite, vdup_n_u8(0x0F)), vdup_n_u8(0x10));
		index = vbsl_u8(show, spriteIndex, index);
	}
	uint8x8_t color = vand_u8(lookup32(palette, index), vdup_n_u8(grayMask));
	vst1q_u16(out, vorrq_u16(vmovl_u8(color), vdupq_n_u16(emphasis)));
}
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

__attribute__((target("sse4.1"))) static void drawTileSse(uint16_t* out, uint64_t pixels, const uint8_t* sprites,
	const uint8_t* palette, uint8_t grayMask, uint16_t emphasis) {
	__m128i zero = _mm_setzero_si128();
	__m128i bg = _mm_cvtsi64_si128((long long) pixels);
	__m128i bgClear = _mm_cmpeq_epi8(_mm_and_si128(bg, _mm_set1_epi8(3)), zero);
	__m128i index = _mm_andnot_si128(bgClear, bg);
	if (sprites) {
		uint64_t raw;
		memcpy(&raw, sprites, 8);
		__m128i sprite = _mm_cvtsi64_si128((long long) raw);
		__m128i opaque = _mm_cmpeq_epi8(_mm_and_si128(sprite, _mm_set1_epi8(SPRITE_OPAQUE)), _mm_set1_epi8(SPRITE_OPAQUE));
		__m128i front = _mm_cmpeq_epi8(_mm_and_si128(sprite, _mm_set1_epi8(SPRITE_BEHIND)), zero);
		__m128i show = _mm_and_si128(opaque, _mm_or_si128(bgClear, front));
		__m128i spriteIndex = _mm_or_si128(_mm_and_si128(sprite, _mm_set1_epi8(0x0F)), _mm_set1_epi8(0x10));
		index = _mm_blendv_epi8(index, spriteIndex, show);
	}
	// Two 16-entry lookups, the upper half chosen by bit 4
	__m128i low = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*) palette), _mm_and_si128(index, _mm_set1_epi8(0x0F)));
	__m128i high =
		_mm_shuffle_epi8(_mm_loadu_si128((const __m128i*) (palette + 16)), _mm_and_si128(index, _mm_set1_epi8(0x0F)));
	__m128i upper = _mm_cmpeq_epi8(_mm_and_si128(index, _mm_set1_epi8(0x10)), _mm_set1_epi8(0x10));
	__m128i color = _mm_and_si128(_mm_blendv_epi8(low, high, upper), _mm_set1_epi8(grayMask));
	_mm_storeu_si128((__m128i*) out, _mm_or_si128(_mm_cvtepu8_epi16(color), _mm_set1_epi16(emphasis)));
}

static void drawTile(uint16_t* out, uint64_t pixels, const uint8_t* sprites, const uint8_t* palette,
	uint8_t grayMask, uint16_t emphasis) {
	static int sse = -1;
	if (sse < 0) {
		sse = __builtin_cpu_supports("sse4.1");
	}
	if (sse) {
		drawTileSse(out, pixels, sprites, palette, grayMask, emphasis);
	} else {
		drawTileScalar(out, pixels, sprites, palette, grayMask, emphasis);
	}
}
#else
#define drawTile drawTileScalar
#endif

// Up to a tile's end on a visible line, dot by dot but with only what such a dot does: the sprite 0
// flag coming due, the pixel, the fetch, the shift, and at the end the next tile and the X step
static void renderSpan(struct NES* nes, int n) {
	struct Ppu* ppu = &nes->ppu;
	for (int i = 0; i < n; i++) {
		int dot = ++ppu->dot;
		ppu->dots++;
		ppu->dotTick += nes->ppuPeriod;
		if (ppu->pendingHit && --ppu->hitDelay == 0) {
			ppu->pendingHit = false;
			ppu->hit = true;
		}
		drawPixel(nes, dot - 1);
		fetchTile(nes, dot);
		shift(ppu);
		if (!(dot & 7)) {
			reload(ppu);
			incrementX(ppu);
			if (dot == 256) {
				incrementY(ppu);
				evaluateSprites(nes);
			}
		}
	}
}

// Up to count whole tiles of a visible line with nothing else going on (the CPU can't touch the PPU
// meanwhile, so what a tile depends on holds for the run). Stops early when a sprite 0 hit comes
// due, which the dots after it count down. Returns the tiles drawn. In a frame that won't be shown
// only what the game can see is done: the sprite 0 test and, for boards that don't see the fetches,
// the last two tiles' fetches, which the PPU goes on from.
static int renderTiles(struct NES* nes, int count) {
	struct Ppu* ppu = &nes->ppu;
	int x = ppu->dot;
	uint16_t* out = &nes->frameBuffer[ppu->scanline * NES_WIDTH];
	uint16_t emphasis = (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
	uint8_t grayMask = ppu->gray ? 0x30 : 0x3F;
	uint16_t table = ppu->bgTable ? 0x1000 : 0;
	int s = 8 - ppu->fineX;
	bool spritesOn = ppu->showSprites && ppu->lineHasSprites;
	uint16_t bgL = ppu->bgL, bgH = ppu->bgH, atL = ppu->atL, atH = ppu->atH, v = ppu->v;
	bool const skip = nes->skipPixels;
	bool const zeroTest = spritesOn && ppu->lineHasZero && !ppu->hit && !ppu->pendingHit && ppu->showBg;
	bool const fetchAll = !skip || zeroTest || nes->ppuReadHook;
	int const end = x + 8 * count < 256 ? x + 8 * count : 256;
	int done = 0;
	while (done < count) {
		uint64_t pixels = 0;
		if ((!skip || zeroTest) && ppu->showBg && (x >= 8 || ppu->showBgLeft)) {
			uint64_t color = spread[(uint8_t) (bgL >> s)] | spread[(uint8_t) (bgH >> s)] << 1;
			uint64_t palette = (spread[(uint8_t) (atL >> s)] | spread[(uint8_t) (atH >> s)] << 1) << 2;
			uint64_t opaque = ((color | color >> 1) & 0x0101010101010101ull) * 0xFF;
			pixels = color | (palette & opaque);
		}
		const uint8_t* sprites = NULL;
		if (spritesOn && (x >= 8 || ppu->showSpritesLeft)) {
			sprites = &ppu->lineSprites[x];
			if (ppu->lineHasZero && !ppu->hit && !ppu->pendingHit && ppu->showBg) {
				for (int i = 0; i < 8; i++) {
					if ((sprites[i] & SPRITE_ZERO) && ((pixels >> (i * 8)) & 3) && x + i != 255) {
						// The flag shows three dots after the pixel's; this tile has run 7 - i of them
						int delay = 3 - (7 - i);
						if (delay <= 0) {
							ppu->hit = true;
						} else {
							ppu->pendingHit = true;
							ppu->hitDelay = delay;
						}
						break;
					}
				}
			}
		}
		if (!skip) {
			drawTile(out + x, pixels, sprites, ppu->palette, grayMask, emphasis);
		}
		bgL <<= 8;
		bgH <<= 8;
		atL <<= 8;
		atH <<= 8;
		// A board's read hook may look at where the PPU is
		ppu->dot = x, ppu->v = v;
		bool const fetch = fetchAll || x >= end - 16;
		uint8_t nt = fetch ? chrRead(nes, 0x2000 | (v & 0xFFF)) : 0;
		uint8_t at = fetch ? chrRead(nes, 0x23C0 | (v & 0xC00) | ((v >> 4) & 0x38) | ((v >> 2) & 7)) : 0;
		uint8_t atLatch = (at >> (((v >> 4) & 4) | (v & 2))) & 3;
		ppu->ntLatch = nt;
		ppu->atLatch = atLatch;
		uint8_t lo = fetch ? chrRead(nes, table | (nt << 4) | (v >> 12)) : 0;
		uint8_t hi = fetch ? chrRead(nes, table | (nt << 4) | (v >> 12) | 8) : 0;
		ppu->loLatch = lo;
		ppu->hiLatch = hi;
		bgL |= lo;
		bgH |= hi;
		atL |= (atLatch & 1) ? 0xFF : 0;
		atH |= (atLatch & 2) ? 0xFF : 0;
		// The bus as the last fetch left it; with A12 low throughout, a board watching it saw nothing
		ppu->addressBus = table | (nt << 4) | ((v >> 12) & 7) | 8;
		v = (v & 0x1F) == 31 ? (v & ~0x1F) ^ 0x400 : v + 1;
		x += 8;
		done++;
		if (x == 256 || ppu->pendingHit) {
			break;
		}
	}
	ppu->bgL = bgL, ppu->bgH = bgH, ppu->atL = atL, ppu->atH = atH, ppu->v = v;
	ppu->dot = x;
	ppu->dots += 8 * done;
	ppu->dotTick += (uint64_t) (8 * done) * nes->ppuPeriod;
	if (x == 256) {
		incrementY(ppu);
		evaluateSprites(nes);
	}
	return done;
}

// Dots 257-340 of a visible line, for boards that don't watch the bus
static void renderHblank(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	shift(ppu);
	ppu->v = (ppu->v & ~0x41F) | (ppu->t & 0x41F);
	ppu->oamAddr = 0;
	ppu->dot = 320;
	fetchSprites(nes);
	for (int dot = 321; dot <= 336; dot++) {
		ppu->dot = dot;
		fetchTile(nes, dot);
		shift(ppu);
		if (!(dot & 7)) {
			reload(ppu);
			incrementX(ppu);
		}
	}
	ppu->dot = 340;
	ppu->dots += 84;
	ppu->dotTick += 84 * nes->ppuPeriod;
}

// Dots of 257-340 of a visible line for a board watching the bus, n of them from where the PPU is:
// each dot shows it the address the last one left
static void renderHblankWatched(struct NES* nes, int n) {
	struct Ppu* ppu = &nes->ppu;
	for (int dot = ppu->dot + 1, end = ppu->dot + n; dot <= end; dot++) {
		ppu->dotTick += nes->ppuPeriod;
		showBus(nes);
		ppu->dot = dot;
		if (dot <= 320) {
			if (dot == 257) {
				shift(ppu);
				ppu->v = (ppu->v & ~0x41F) | (ppu->t & 0x41F);
			}
			ppu->oamAddr = 0;
			spriteFetchBus(nes, dot);
			if (dot == 320) {
				fetchSprites(nes);
			}
		} else if (dot <= 336) {
			fetchTile(nes, dot);
			shift(ppu);
			if (!(dot & 7)) {
				reload(ppu);
				incrementX(ppu);
			}
		} else if (dot == 337 || dot == 339) {
			busAddress(nes, 0x2000 | (ppu->v & 0xFFF));
		}
	}
	ppu->dots += n;
}

// Dots 257-340 of a visible line for an MMC3, which only sees A12 change: the sprite fetches slot by
// slot, the board shown each change at the dot it comes, then the rest dot by dot
static void renderHblankMmc3(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	uint64_t const start = ppu->dotTick;
	int const period = nes->ppuPeriod, line = ppu->scanline & 0xFF;
	// Dot 257 shows what dot 256 left
	ppu->dotTick = start + period;
	showBus(nes);
	shift(ppu);
	ppu->v = (ppu->v & ~0x41F) | (ppu->t & 0x41F);
	ppu->oamAddr = 0;
	for (int slot = 0; slot < 8; slot++) {
		int const dot = 257 + 8 * slot;
		// The name table fetches, then the pattern's, each seen from the dot after
		ppu->addressBus = 0x2000 | (ppu->v & 0xFFF);
		ppu->dotTick = start + (uint64_t) (dot + 1 - 256) * period;
		showBus(nes);
		uint16_t const pattern = spriteAddress(ppu, slot, line);
		ppu->addressBus = pattern;
		ppu->dotTick = start + (uint64_t) (dot + 5 - 256) * period;
		showBus(nes);
		ppu->addressBus = pattern | 8;
	}
	ppu->dot = 320;
	ppu->dotTick = start + 64 * period;
	fetchSprites(nes);
	ppu->dots += 64;
	renderHblankWatched(nes, 20);
}

// Balanced: dots of a line that renders with nothing else due (no register effect pending), to before
// the line's end or the pre-render line's skippable dot; a board watching the bus sees it each dot
// Balanced: a tile's eight dots of a line that renders (base a multiple of 8, base + 8 no further
// than 256), the fetch for each known where it's compiled
static inline __attribute__((always_inline)) void balancedTile(struct NES* nes, int base, bool visible, bool pre) {
	struct Ppu* ppu = &nes->ppu;
	int const period = nes->ppuPeriod;
	bool const watch = nes->watchPpuBus;
	// With no sprite pattern left in any slot the tile's eight pixels are the background's, drawn at once
	uint64_t lo, hi;
	memcpy(&lo, ppu->spriteL, 8);
	memcpy(&hi, ppu->spriteH, 8);
	bool const whole = visible && !(lo | hi);
	if (whole) {
		uint64_t pixels = 0;
		if (ppu->showBg && (base >= 8 || ppu->showBgLeft)) {
			int const sh = 8 - ppu->fineX;
			uint64_t color = spread[(uint8_t) (ppu->bgL >> sh)] | spread[(uint8_t) (ppu->bgH >> sh)] << 1;
			uint64_t palette = (spread[(uint8_t) (ppu->atL >> sh)] | spread[(uint8_t) (ppu->atH >> sh)] << 1) << 2;
			uint64_t opaque = ((color | color >> 1) & 0x0101010101010101ull) * 0xFF;
			pixels = color | (palette & opaque);
		}
		uint16_t const emphasis = (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
		drawTile(&nes->frameBuffer[ppu->scanline * NES_WIDTH + base], pixels, NULL, ppu->palette, ppu->gray ? 0x30 : 0x3F, emphasis);
	}
#pragma GCC unroll 8
	for (int k = 1; k <= 8; k++) {
		int const dot = base + k;
		ppu->dotTick += period;
		if (watch) {
			showBus(nes);
		}
		ppu->dot = dot;
		ppu->dots++;
		if (ppu->pendingHit && --ppu->hitDelay == 0) {
			ppu->pendingHit = false;
			ppu->hit = true;
		}
		if (visible && !whole) {
			dotPixel(nes, dot - 1);
		}
		dotShiftSprites(ppu);
		fetchTile(nes, k);
		shift(ppu);
		if (k == 8) {
			reload(ppu);
			incrementX(ppu);
			if (dot == 256) {
				incrementY(ppu);
			}
		}
		dotSprites(nes, dot, pre);
		dotOam2Reset(ppu, dot);
	}
}

static void balancedSpan(struct NES* nes, int n, bool visible, bool pre) {
	struct Ppu* ppu = &nes->ppu;
	int const period = nes->ppuPeriod;
	bool const watch = nes->watchPpuBus;
	// Whole tiles where they fit
	while (n >= 8 && !(ppu->dot & 7) && ppu->dot < 256) {
		if (visible) {
			balancedTile(nes, ppu->dot, true, false);
		} else {
			balancedTile(nes, ppu->dot, false, pre);
		}
		n -= 8;
	}
	for (int i = 0; i < n; i++) {
		ppu->dotTick += period;
		if (watch) {
			showBus(nes);
		}
		int const dot = ++ppu->dot;
		ppu->dots++;
		if (ppu->pendingHit && --ppu->hitDelay == 0) {
			ppu->pendingHit = false;
			ppu->hit = true;
		}
		renderDotBalanced(nes, dot, visible, pre);
	}
}

void fastRun(struct NES* nes, int dots) {
	struct Ppu* ppu = &nes->ppu;
	buildSpread();
	int pre = preLine(nes), vbl = vblankLine(nes);
	while (dots > 0) {
		if (quiet(nes)) {
			int line = ppu->scanline, dot = ppu->dot;
			if (line > vbl || (line == vbl && dot >= 1)) {
				if (line < pre - 1 || (line == pre - 1 && dot < 340)) {
					// Nothing happens in VBL until the line before pre-render ends, or an overclocked
					// frame's held line does
					int last = nes->overclockRepeat && !nes->overclockPostRender && line <= 242 ? 242 : pre - 1;
					int left = (last - line) * 341 + 340 - dot;
					if (left <= 0) {
						step(nes);
						dots--;
						continue;
					}
					int n = dots < left ? dots : left;
					int lines = (dot + n) / 341;
					ppu->scanline += lines;
					ppu->dot = (dot + n) % 341;
					ppu->dots += n;
					ppu->dotTick += (uint64_t) n * nes->ppuPeriod;
					dots -= n;
					continue;
				}
			} else if (nes->dotPpu) {
				// Balanced: render lines' dots in a run, the rest one at a time
				bool const preLine_ = line == pre;
				if ((line < 240 || preLine_) && rendering(ppu) && !(line == 0 && ppu->skippedDot) &&
					dot >= (preLine_ ? 1 : 0)) {
					int const end = preLine_ ? 338 : 340;
					if (dot < end) {
						int n = end - dot < dots ? end - dot : dots;
						balancedSpan(nes, n, line < 240, preLine_);
						dots -= n;
						continue;
					}
				}
			} else if (line == 240 && dot < 340 && (!nes->watchPpuBus || (nes->overclockRepeat && nes->overclockPostRender))) {
				// The line after the picture (or an overclocked frame's held one): idle to its last dot
				int n = 340 - dot;
				n = dots < n ? dots : n;
				ppu->dot += n;
				ppu->dots += n;
				ppu->dotTick += (uint64_t) n * nes->ppuPeriod;
				dots -= n;
				continue;
			} else if ((line < 240 || line == pre) && rendering(ppu) && !nes->watchPpuBus && dot >= 257 && dot < 319 &&
				!(line == pre && dot < 304)) {
				// Between the sprite search and the sprite fetch nothing shows without a board watching
				int n = 319 - dot;
				n = dots < n ? dots : n;
				ppu->dot += n;
				ppu->dots += n;
				ppu->dotTick += (uint64_t) n * nes->ppuPeriod;
				ppu->oamAddr = 0;
				dots -= n;
				continue;
			} else if (line < 240 && dot < 340 && !rendering(ppu) && !nes->watchPpuBus && !(line == 0 && ppu->skippedDot)) {
				// Rendering off: the backdrop (or the palette entry v points at) to the end of the line
				int n = dots < 340 - dot ? dots : 340 - dot;
				int end = dot + n < 256 ? dot + n : 256;
				if (dot < end) {
					uint8_t color = (ppu->v & 0x3F00) == 0x3F00 ? paletteRead(ppu, ppu->v) : ppu->palette[0];
					if (ppu->gray) {
						color &= 0x30;
					}
					uint16_t pixel = color | (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
					uint16_t* out = &nes->frameBuffer[line * NES_WIDTH];
					for (int x = dot; x < end; x++) {
						out[x] = pixel;
					}
				}
				ppu->dot += n;
				ppu->dots += n;
				ppu->dotTick += (uint64_t) n * nes->ppuPeriod;
				dots -= n;
				continue;
			} else if (line < 240 && rendering(ppu) && !(line == 0 && ppu->skippedDot)) {
				// dot is the last one done, so tiles start on dots 1, 9, ... 249
				if (dot < 256 && (dot & 7) == 0 && dots >= 8 &&
					(!nes->watchPpuBus || (!ppu->bgTable && !(ppu->addressBus & 0x1000)))) {
					dots -= 8 * renderTiles(nes, dots / 8);
					continue;
				}
				if (dot == 256 && dots >= 84 && nes->board->ppuBus == mmc3PpuBus) {
					renderHblankMmc3(nes);
					dots -= 84;
					continue;
				}
				if (dot >= 256 && dot < 340 && nes->watchPpuBus) {
					int n = dots < 340 - dot ? dots : 340 - dot;
					renderHblankWatched(nes, n);
					dots -= n;
					continue;
				}
				if (dot >= 1 && dot < 256 && ((dot & 7) || dots < 8) &&
					(!nes->watchPpuBus || (nes->board->busA12 && !ppu->bgTable && !(ppu->addressBus & 0x1000)))) {
					// Part of a tile, to its end or as far as asked
					int n = 8 - (dot & 7);
					n = dots < n ? dots : n;
					renderSpan(nes, n);
					dots -= n;
					continue;
				}
				if (dot == 256 && dots >= 84) {
					if (nes->board->ppuBus == mmc3PpuBus) {
						renderHblankMmc3(nes);
					} else if (nes->watchPpuBus) {
						renderHblankWatched(nes, 84);
					} else {
						renderHblank(nes);
					}
					dots -= 84;
					continue;
				}
			}
		}
		step(nes);
		dots--;
	}
}

// The rest of a register access: 7 master clocks in order, or when VBL can't change meanwhile just
// the PPU run ahead
static void untilEndOfAccess(struct NES* nes, int clocks) {
	if (nes->ppuLag + clocks + nes->cpuPeriod < nes->ppuQuietTicks) {
		nesPpuSyncAhead(nes, clocks);
		return;
	}
	for (int i = 0; i < clocks; i++) {
		nesTick(nes);
	}
}

static uint8_t openBusAt(struct Ppu* ppu, uint64_t now) {
	for (int i = 0; i < 8; i++) {
		if ((ppu->bus & (1 << i)) && now - ppu->busRefresh[i] >= 1786830) {
			ppu->bus &= ~(1 << i);
		}
	}
	return ppu->bus;
}

static uint8_t openBus(struct Ppu* ppu) {
	return openBusAt(ppu, ppu->dots);
}

static void setBusAt(struct Ppu* ppu, uint8_t value, uint8_t bits, uint64_t now) {
	openBusAt(ppu, now);
	ppu->bus = (ppu->bus & ~bits) | (value & bits);
	for (int i = 0; i < 8; i++) {
		if (bits & (1 << i)) {
			ppu->busRefresh[i] = now;
		}
	}
}

static void setBus(struct Ppu* ppu, uint8_t value, uint8_t bits) {
	setBusAt(ppu, value, bits, ppu->dots);
}

static inline bool renderingLine(const struct NES* nes) {
	return rendering(&nes->ppu) && (nes->ppu.scanline < 240 || nes->ppu.scanline == preLine(nes));
}

// $2002 with the PPU behind: when none of its flags can change before the read lands, the PPU needn't
// catch up for it. VBL and the pre-render line are past the quiet window; on a rendering line the
// sprite flags change no sooner than its first sprite 0 pixel or the sprite search at dot 256, or the
// next line's.
bool fastStatusAhead(struct NES* nes, uint8_t* out) {
	struct Ppu* ppu = &nes->ppu;
	int period = nes->ppuPeriod;
	int ticks = nes->ppuLag + 7;
	if (ticks < 0 || ticks + 4 * period >= nes->ppuQuietTicks || !quiet(nes)) {
		return false;
	}
	int ahead = ticks / period + 2;
	int line = ppu->scanline, dot = ppu->dot;
	if (rendering(ppu) && (line < 240 || line == preLine(nes))) {
		int horizon = 341 - dot;
		if (line < 240 && dot < 256) {
			horizon = 256 - dot;
			if (ppu->lineHasZero && !ppu->hit) {
				for (int x = dot; x < 256 && x - dot + 1 < horizon; x++) {
					if (ppu->lineSprites[x] & SPRITE_ZERO) {
						horizon = x - dot + 1;
						break;
					}
				}
			}
		}
		if (ahead >= horizon) {
			return false;
		}
	}
	uint64_t now = ppu->dots + ticks / period;
	uint8_t value = (ppu->vblank ? 0x80 : 0) | (ppu->hit ? 0x40 : 0) | (ppu->overflow ? 0x20 : 0) |
		(openBusAt(ppu, now) & 0x1F);
	ppu->vblank = false;
	ppu->w = false;
	setBusAt(ppu, value, 0xE0, now);
	*out = value;
	return true;
}

// Balanced: a line's sprite 0 flag is the first evaluation's, on dot 66 of the line before. It's
// OAM[0]'s when that evaluation starts from OAM address 0, which the fetches on dots 257-320 leave
// behind; the pre-render line's evaluation starts wherever the CPU left it.
static int dotsToLine(const struct NES* nes, int line, int target) {
	int const lines = line >= 240 ? preLine(nes) + 1 - line + target : target - line;
	return lines * 341 - nes->ppu.dot - 1;
}

static int balancedHitHorizon(const struct NES* nes, int line, int first, int last) {
	const struct Ppu* ppu = &nes->ppu;
	if (line >= 0 && ppu->hasZero && ppu->dot < 256) {
		return 0;
	}
	int const next = line >= 240 ? 0 : line + 1;
	int best = -1, from = next + 1;
	if (next < 240) {
		if (line < 240 && ppu->dot >= 66) {
			best = ppu->nextHasZero ? dotsToLine(nes, line, next) : -1;
		} else if (line >= 0 && !ppu->oamAddr) {
			from = next;
		} else {
			best = dotsToLine(nes, line, next);
		}
	}
	int const hitLine = first > from ? first : from;
	if (hitLine <= last && hitLine < 240) {
		int const dots = dotsToLine(nes, line, hitLine);
		best = best < 0 || dots < best ? dots : best;
	}
	return best;
}

// Dots from where the PPU is before sprite 0 can hit, or -1 if it can't before the next pre-render line:
// not before its first line, and on its lines not before its first pixel on this one or the next line.
// None while a register write is still landing.
int ppuHitHorizon(const struct NES* nes) {
	const struct Ppu* ppu = &nes->ppu;
	// A register write still on its way may turn rendering on
	if (!quiet(nes)) {
		return 0;
	}
	// The pre-render line comes before line 0, and can be a dot short
	int line = ppu->scanline == preLine(nes) ? -1 : ppu->scanline;
	if (ppu->hit || !ppu->showBg || !ppu->showSprites) {
		return -1;
	}
	int first = ppu->oam[0] + 1, last = ppu->oam[0] + (ppu->tall ? 16 : 8);
	if (nes->dotPpu) {
		return balancedHitHorizon(nes, line, first, last);
	}
	if (ppu->oam[0] >= 239 || line >= 240) {
		return -1;
	}
	if (line < first) {
		return (first - line) * 341 - ppu->dot - 1;
	}
	if (line > last) {
		return -1;
	}
	if (ppu->dot < 256 && ppu->lineHasZero) {
		for (int x = ppu->dot; x < 256; x++) {
			if (ppu->lineSprites[x] & SPRITE_ZERO) {
				return x - ppu->dot;
			}
		}
	}
	return 341 - ppu->dot;
}

// $2004 with the PPU behind, when it's in vblank (or not rendering) all the way to the write: the
// byte goes into OAM without the PPU catching up
bool fastOamWriteAhead(struct NES* nes, uint8_t value) {
	struct Ppu* ppu = &nes->ppu;
	int ticks = nes->ppuLag;
	if (ticks < 0 || ticks + 4 * nes->ppuPeriod >= nes->ppuQuietTicks || !quiet(nes) ||
		(rendering(ppu) && (ppu->scanline <= 240 || ppu->scanline == preLine(nes)))) {
		return false;
	}
	setBusAt(ppu, value, 0xFF, ppu->dots + ticks / nes->ppuPeriod);
	if ((ppu->oamAddr & 3) == 2) {
		value &= 0xE3;
	}
	ppu->oam[ppu->oamAddr++] = value;
	return true;
}

uint8_t fastReadRegister(struct NES* nes, uint16_t address) {
	struct Ppu* ppu = &nes->ppu;
	uint8_t value;
	switch (address & 7) {
	case 2:
		// VBL is read at the start of the read, the sprite flags at its end
		nesPpuSync(nes);
		value = ppu->vblank ? 0x80 : 0;
		// A read just before VBL sets keeps it from setting this frame
		if (ppu->scanline == vblankLine(nes) && ppu->dot == 0) {
			ppu->read2002 = true;
		}
		ppu->vblank = false;
		untilEndOfAccess(nes, 7);
		value |= (ppu->hit ? 0x40 : 0) | (ppu->overflow ? 0x20 : 0) | (openBus(ppu) & 0x1F);
		ppu->w = false;
		setBus(ppu, value, 0xE0);
		return value;
	case 4:
		nesPpuSyncAhead(nes, 1);
		value = ppu->oam[ppu->oamAddr];
		if (renderingLine(nes) && ppu->scanline < 240) {
			// Balanced: what sprite evaluation or fetching has on the OAM bus
			value = nes->dotPpu ? ppu->oamBuffer : ppu->dot >= 1 && ppu->dot <= 64 ? 0xFF : value;
		}
		setBus(ppu, value, 0xFF);
		return value;
	case 7: {
		nesPpuSync(nes);
		// A read still under way when another comes (a DMA repeating the CPU's) finishes first
		if (ppu->vramReadStep) {
			if (ppu->vramReadStep < 3) {
				ppu->vramReadAddress = ppu->v & 0x3FFF;
			}
			ppu->readBuffer = chrRead(nes, ppu->vramReadAddress);
			vramIncrement(nes);
			ppu->vramReadStep = 0;
		}
		uint16_t v = ppu->v & 0x3FFF;
		if (!rendering(ppu) && v >= 0x3F00) {
			value = (paletteRead(ppu, v) & (ppu->gray ? 0x30 : 0x3F)) | (openBus(ppu) & 0xC0);
		} else {
			value = ppu->readBuffer;
		}
		setBus(ppu, value, 0xFF);
		untilEndOfAccess(nes, 7);
		ppu->vramReadStep = 1;
		return value;
	}
	default:
		return openBus(ppu);
	}
}

void fastWriteRegister(struct NES* nes, uint16_t address, uint8_t value, uint8_t busBefore) {
	struct Ppu* ppu = &nes->ppu;
	nesPpuSync(nes);
	int phase = nes->ppuClock & 3;
	setBus(ppu, value, 0xFF);
	switch (address & 7) {
	case 0:
		if (ppu->resetting) {
			return;
		}
		// The name table bits see the bus as it was, the rest of the write lands two clocks in
		ppu->t = (ppu->t & 0x73FF) | ((busBefore & 3) << 10);
		untilEndOfAccess(nes, 2);
		ppu->nmiEnabled = value & 0x80;
		ppu->increment32 = value & 0x04;
		ppu->spriteTable = value & 0x08;
		ppu->bgTable = value & 0x10;
		ppu->tall = value & 0x20;
		ppu->t = (ppu->t & 0x73FF) | ((value & 3) << 10);
		break;
	case 1: {
		if (ppu->resetting) {
			return;
		}
		static const uint8_t delays[4][2] = { { 2, 2 }, { 2, 1 }, { 3, 1 }, { 2, 2 } };
		ppu->value2001 = value;
		ppu->delay2001 = delays[phase][0];
		ppu->delay2001Emphasis = delays[phase][1];
		if (ppu->delay2001Emphasis == 2) {
			ppu->gray = busBefore & 0x01;
			ppu->emphBlue = busBefore & 0x80;
		} else {
			ppu->delay2001Emphasis++;
		}
		ppu->emphRed = value & 0x20;
		ppu->emphGreen = value & 0x40;
		retint(nes);
		break;
	}
	case 3:
		ppu->oamAddr = value;
		break;
	case 4:
		if (rendering(ppu) && (ppu->scanline < 240 || ppu->scanline == preLine(nes))) {
			ppu->oamAddr = (ppu->oamAddr + 4) & 0xFC;
		} else {
			if ((ppu->oamAddr & 3) == 2) {
				value &= 0xE3;
			}
			ppu->oam[ppu->oamAddr++] = value;
		}
		break;
	case 5:
		if (ppu->resetting) {
			return;
		}
		ppu->delay2005 = phase == 2 ? 2 : 1;
		ppu->value2005 = value;
		if (!ppu->w) {
			ppu->fineX = busBefore & 7;
			ppu->t = (ppu->t & 0x7FE0) | (busBefore >> 3);
		} else {
			ppu->t = (ppu->t & 0x0C1F) | ((busBefore & 0xF8) << 2) | ((busBefore & 7) << 12);
		}
		break;
	case 6:
		if (ppu->resetting) {
			return;
		}
		if (!ppu->w) {
			ppu->t = (ppu->t & 0x00FF) | ((value & 0x3F) << 8);
		} else {
			ppu->t = (ppu->t & 0x7F00) | value;
			ppu->delay2006 = phase == 2 ? 5 : 4;
		}
		ppu->w = !ppu->w;
		break;
	case 7:
		ppu->writeData = value;
		untilEndOfAccess(nes, 7);
		ppu->vramWriteStep = 1;
		break;
	}
}
