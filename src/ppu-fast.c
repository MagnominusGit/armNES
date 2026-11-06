/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

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

static uint16_t spriteAddress(const struct Ppu* ppu, int slot, int line) {
	const uint8_t* sprite = &ppu->oam2[slot * 4];
	int tile = sprite[1];
	int row = (uint8_t) (line - sprite[0]);
	bool flipV = sprite[2] & 0x80;
	if (slot >= ppu->nextCount) {
		// Empty slots fetch tile $FF
		tile = 0xFF;
		row = 0;
		flipV = false;
	}
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
		nes->board->ppuBus(nes, ppu->addressBus);
	}
	if (ppu->delay2001 | ppu->delay2001Emphasis | ppu->delay2005 | ppu->delay2006 | ppu->vramReadStep | ppu->vramWriteStep) {
		registerEffects(nes);
	}
	if (++ppu->dot > 340) {
		ppu->dot = 0;
		if (++ppu->scanline > pre) {
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
	if (dot == 0 && line <= 240 && nes->board->scanline && (rendering(ppu) || line == 240)) {
		nes->board->scanline(nes, line);
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
			renderDot(nes, dot, visible, line == pre);
			if (line == pre && dot == 339 && ppu->oddFrame && nes->region == REGION_NTSC) {
				// Odd frames skip the last dot of the pre-render line while rendering
				ppu->dot = 340;
				ppu->skippedDot = true;
			}
		} else if (visible && dot >= 1 && dot <= 256) {
			drawPixel(nes, dot - 1);
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

// Eight dots of a visible line with nothing else going on: a whole tile
static void renderTile(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int x = ppu->dot;
	uint16_t* out = &nes->frameBuffer[ppu->scanline * NES_WIDTH + x];
	uint16_t emphasis = (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
	uint8_t grayMask = ppu->gray ? 0x30 : 0x3F;
	uint64_t pixels = 0;
	if (ppu->showBg && (x >= 8 || ppu->showBgLeft)) {
		int s = 8 - ppu->fineX;
		uint64_t color = spread[(uint8_t) (ppu->bgL >> s)] | spread[(uint8_t) (ppu->bgH >> s)] << 1;
		uint64_t palette = (spread[(uint8_t) (ppu->atL >> s)] | spread[(uint8_t) (ppu->atH >> s)] << 1) << 2;
		uint64_t opaque = ((color | color >> 1) & 0x0101010101010101ull) * 0xFF;
		pixels = color | (palette & opaque);
	}
	const uint8_t* sprites = NULL;
	if (ppu->showSprites && (x >= 8 || ppu->showSpritesLeft) && ppu->lineHasSprites) {
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
	drawTile(out, pixels, sprites, ppu->palette, grayMask, emphasis);
	ppu->bgL <<= 8;
	ppu->bgH <<= 8;
	ppu->atL <<= 8;
	ppu->atH <<= 8;
	uint16_t table = ppu->bgTable ? 0x1000 : 0;
	uint8_t nt = chrRead(nes, 0x2000 | (ppu->v & 0xFFF));
	uint8_t at = chrRead(nes, 0x23C0 | (ppu->v & 0xC00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 7));
	ppu->ntLatch = nt;
	ppu->atLatch = (at >> (((ppu->v >> 4) & 4) | (ppu->v & 2))) & 3;
	ppu->loLatch = chrRead(nes, table | (nt << 4) | (ppu->v >> 12));
	ppu->hiLatch = chrRead(nes, table | (nt << 4) | (ppu->v >> 12) | 8);
	reload(ppu);
	// The bus as the last fetch left it; with A12 low throughout, a board watching it saw nothing
	ppu->addressBus = table | (nt << 4) | ((ppu->v >> 12) & 7) | 8;
	incrementX(ppu);
	ppu->dot += 8;
	ppu->dots += 8;
	ppu->dotTick += 8 * nes->ppuPeriod;
	if (ppu->dot == 256) {
		incrementY(ppu);
		evaluateSprites(nes);
	}
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

// Dots 257-340 of a visible line for a board watching the bus: each dot shows it the address the
// last one left
static void renderHblankWatched(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	for (int dot = 257; dot <= 340; dot++) {
		ppu->dotTick += nes->ppuPeriod;
		nes->board->ppuBus(nes, ppu->addressBus);
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
	ppu->dots += 84;
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
					// Nothing happens in VBL until the line before pre-render ends
					int left = (pre - 1 - line) * 341 + 340 - dot;
					int n = dots < left ? dots : left;
					int lines = (dot + n) / 341;
					ppu->scanline += lines;
					ppu->dot = (dot + n) % 341;
					ppu->dots += n;
					ppu->dotTick += (uint64_t) n * nes->ppuPeriod;
					dots -= n;
					continue;
				}
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
			} else if (line < 240 && rendering(ppu) && !(line == 0 && ppu->skippedDot)) {
				// dot is the last one done, so tiles start on dots 1, 9, ... 249
				if (dot < 256 && (dot & 7) == 0 && dots >= 8 &&
					(!nes->watchPpuBus || (!ppu->bgTable && !(ppu->addressBus & 0x1000)))) {
					renderTile(nes);
					dots -= 8;
					continue;
				}
				if (dot == 256 && dots >= 84) {
					if (nes->watchPpuBus) {
						renderHblankWatched(nes);
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

static uint8_t openBus(struct Ppu* ppu) {
	for (int i = 0; i < 8; i++) {
		if ((ppu->bus & (1 << i)) && ppu->dots - ppu->busRefresh[i] >= 1786830) {
			ppu->bus &= ~(1 << i);
		}
	}
	return ppu->bus;
}

static void setBus(struct Ppu* ppu, uint8_t value, uint8_t bits) {
	openBus(ppu);
	ppu->bus = (ppu->bus & ~bits) | (value & bits);
	for (int i = 0; i < 8; i++) {
		if (bits & (1 << i)) {
			ppu->busRefresh[i] = ppu->dots;
		}
	}
}

static inline bool renderingLine(const struct NES* nes) {
	return rendering(&nes->ppu) && (nes->ppu.scanline < 240 || nes->ppu.scanline == preLine(nes));
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
			value = ppu->dot >= 1 && ppu->dot <= 64 ? 0xFF : value;
		}
		setBus(ppu, value, 0xFF);
		return value;
	case 7: {
		nesPpuSync(nes);
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
