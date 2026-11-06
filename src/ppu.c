/* Copyright (c) 2026 Magnominus
 * The PPU model follows TriCNES, Copyright (c) 2025 Chris Siebert, under the MIT license
 * (LICENSES/MIT.txt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <string.h>

// The 2C02, stepped by the master clock: ppuStep() every dot, ppuHalfStep() half a dot later. The
// PPU's address bus carries the low address byte and then the data, so the octal latch holds the
// low byte the cartridge and name tables see.

// About 20 frames, in dots, for a bit of the PPU's data latch to fade
enum { BUS_DECAY_DOTS = 1786830 };

static inline bool rendering(const struct Ppu* ppu) {
	return ppu->showBg || ppu->showSprites;
}

static inline bool renderingDelayed(const struct Ppu* ppu) {
	return ppu->showBgDelayed || ppu->showSpritesDelayed;
}

static inline int vblankLine(const struct NES* nes) {
	return nes->region == REGION_DENDY ? 291 : 241;
}

static inline int preLine(const struct NES* nes) {
	return nes->region == REGION_NTSC ? 261 : 311;
}

static inline bool renderLine(const struct NES* nes) {
	return nes->ppu.scanline < 240 || nes->ppu.scanline == preLine(nes);
}

static uint8_t openBus(struct Ppu* ppu) {
	for (int i = 0; i < 8; i++) {
		if ((ppu->bus & (1 << i)) && ppu->dots - ppu->busRefresh[i] >= BUS_DECAY_DOTS) {
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

// The address the cartridge and name tables see: the high bits from the bus, the low from the latch
static inline uint16_t externalAddress(const struct Ppu* ppu) {
	return (ppu->addressBus & 0x3F00) | ppu->octalLatch;
}

uint8_t ppuBusRead(struct NES* nes, uint16_t address) {
	address &= 0x3FFF;
	if (nes->ppuReadHook) {
		return nes->board->ppuRead(nes, address);
	}
	uint8_t* page = nes->chrPages[address >> 10];
	return page ? page[address & 0x3FF] : (address & 0xFF);
}

static uint8_t fetchVideo(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	uint16_t address = externalAddress(ppu);
	uint8_t* page = nes->chrPages[(address >> 10) & 15];
	uint8_t value;
	if (nes->ppuReadHook) {
		value = nes->board->ppuRead(nes, address);
	} else {
		value = page ? page[address & 0x3FF] : (uint8_t) ppu->addressBus;
	}
	ppu->addressBus = (ppu->addressBus & 0xFF00) | value;
	return value;
}

static void writeVideo(struct NES* nes, uint8_t value) {
	struct Ppu* ppu = &nes->ppu;
	if (ppu->paletteEnable) {
		ppu->palette[ppu->addressBus & ((ppu->addressBus & 3) == 0 ? 0x0F : 0x1F)] = value & 0x3F;
	}
	if (ppu->ppuWrite) {
		uint16_t address = externalAddress(ppu);
		int page = (address >> 10) & 15;
		if (nes->chrWritable[page] && nes->chrPages[page]) {
			nes->chrPages[page][address & 0x3FF] = value;
		}
	}
}

static void incrementX(struct Ppu* ppu) {
	if ((ppu->v & 0x1F) == 31) {
		ppu->v = (ppu->v & 0xFFE0) ^ 0x400;
	} else {
		ppu->v++;
	}
	ppu->v &= 0x7FFF;
}

static void incrementY(struct Ppu* ppu) {
	if (ppu->copyV) {
		// A $2006 write landing on the same dot ANDs the old and new addresses
		ppu->v = ppu->value2006Before & ppu->value2006;
	} else if ((ppu->v & 0x7000) != 0x7000) {
		ppu->v += 0x1000;
	} else {
		ppu->v &= 0x0FFF;
		int y = (ppu->v & 0x3E0) >> 5;
		if (y == 29) {
			y = 0;
			ppu->v ^= 0x800;
		} else if (y == 31) {
			y = 0;
		} else {
			y++;
		}
		ppu->v = (ppu->v & 0xFC1F) | (y << 5);
	}
	ppu->v &= 0x7FFF;
}

static inline uint8_t flip(uint8_t b) {
	b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
	b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
	b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
	return b;
}

// The pattern address register: background or sprite row, by the dot
static void checkPar(struct Ppu* ppu) {
	if (ppu->dot < 256 || ppu->dot > 320) {
		ppu->parChr &= 0x0FF8;
		ppu->parChr |= ppu->bgTable ? 0x1000 : 0;
		ppu->parChr |= (ppu->v & 0x7000) >> 12;
		return;
	}
	bool flipY = ppu->spriteAttrNow & 0x80;
	int row = flipY ? 7 - (ppu->verticalOffset & 7) : (ppu->verticalOffset & 7);
	if (!ppu->tall) {
		ppu->parChr &= 0x0FF8;
		ppu->parChr |= ppu->spriteTable ? 0x1000 : 0;
		ppu->parChr |= row;
	} else {
		ppu->parChr &= 0x0FE8;
		ppu->parChr |= (ppu->spritePattern & 1) ? 0x1000 : 0;
		ppu->parChr |= row;
		ppu->parChr |= ((ppu->verticalOffset & 8) ^ (flipY ? 8 : 0)) << 1;
	}
}

static void loadShifters(struct Ppu* ppu) {
	ppu->bgL = (ppu->bgL & 0xFF00) | ppu->lowPlane;
	ppu->bgH = (ppu->bgH & 0xFF00) | ppu->highPlane;
	ppu->attrLatch = ppu->attribute;
}

static void backgroundFetch(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int tick = (ppu->dot + 7) & 7;
	if (ppu->dot >= 257 && ppu->dot <= 320) {
		tick = 1;
	}
	if (ppu->ale && ppu->ppuRead) {
		ppu->octalLatch = ppu->addressBus;
	}
	switch (tick) {
	case 0:
		ppu->parNt = 0x2000 | (ppu->v & 0xFFF);
		ppu->addressBus = ppu->parNt;
		break;
	case 1:
		ppu->parNt = 0x2000 | (ppu->v & 0xFFF);
		ppu->addressBus = (ppu->parNt & 0xFF00) | ppu->octalLatch;
		ppu->renderTemp = fetchVideo(nes);
		ppu->commitNt = true;
		break;
	case 2:
		ppu->parAt = 0x23C0 | (ppu->v & 0xC00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 7);
		ppu->addressBus = ppu->parAt;
		break;
	case 3:
		ppu->parAt = 0x23C0 | (ppu->v & 0xC00) | ((ppu->v >> 4) & 0x38) | ((ppu->v >> 2) & 7);
		ppu->addressBus = (ppu->parAt & 0xFF00) | ppu->octalLatch;
		ppu->renderTemp = fetchVideo(nes);
		ppu->commitAt = true;
		break;
	case 4:
		checkPar(ppu);
		ppu->parChr &= ~8;
		ppu->addressBus = ppu->parChr;
		break;
	case 5:
		ppu->addressBus = (ppu->parChr & 0xFF00) | ppu->octalLatch;
		ppu->renderTemp = fetchVideo(nes);
		ppu->commitLo = true;
		break;
	case 6:
		checkPar(ppu);
		ppu->parChr |= 8;
		ppu->addressBus = ppu->parChr;
		break;
	case 7:
		ppu->addressBus = (ppu->parChr & 0xFF00) | ppu->octalLatch;
		ppu->renderTemp = fetchVideo(nes);
		ppu->commitHi = true;
		break;
	}
	if (ppu->ale && !ppu->ppuRead) {
		ppu->octalLatch = ppu->addressBus;
	}
}

static void commitFetches(struct Ppu* ppu) {
	if (ppu->commitNt) {
		ppu->commitNt = false;
		ppu->parChr &= 0x100F;
		if (ppu->dot < 256 || ppu->dot > 320) {
			ppu->parChr |= (uint8_t) ppu->addressBus << 4;
		} else {
			ppu->parChr |= ppu->spritePattern << 4;
		}
	}
	if (ppu->commitAt) {
		ppu->commitAt = false;
		uint8_t at = ppu->renderTemp;
		if ((ppu->v & 3) >= 2) {
			at >>= 2;
		}
		if ((((ppu->v & 0x3E0) >> 5) & 3) >= 2) {
			at >>= 4;
		}
		ppu->attribute = at & 3;
	}
	if (ppu->commitLo) {
		ppu->commitLo = false;
		ppu->lowPlane = ppu->renderTemp;
	}
	if (ppu->commitHi) {
		ppu->commitHi = false;
		ppu->highPlane = ppu->renderTemp;
		loadShifters(ppu);
		incrementX(ppu);
	}
}

static void dummyNametableFetch(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (ppu->ppuRead) {
		ppu->octalLatch = ppu->addressBus;
	}
	if (ppu->dot == 0) {
		checkPar(ppu);
		ppu->parChr &= ~8;
		if (ppu->scanline != preLine(nes)) {
			ppu->addressBus = ppu->parChr;
		}
	} else {
		switch (ppu->dot - 337) {
		case 0:
		case 2:
			ppu->addressBus = 0x2000 | (ppu->v & 0xFFF);
			break;
		case 1:
			ppu->addressBus = 0x2000 | (ppu->v & 0xFFF);
			ppu->renderTemp = fetchVideo(nes);
			ppu->commitNt = true;
			break;
		case 3:
			ppu->renderTemp = fetchVideo(nes);
			break;
		}
	}
	if (ppu->ale && !ppu->ppuRead) {
		ppu->octalLatch = ppu->addressBus;
	}
}

static void shiftBackground(struct Ppu* ppu) {
	ppu->bgL <<= 1;
	ppu->bgH = (ppu->bgH << 1) | 1;
	ppu->atL = (ppu->atL << 1) | (ppu->attrLatch & 1);
	ppu->atH = (ppu->atH << 1) | ((ppu->attrLatch & 2) >> 1);
}

static void shiftSprites(struct Ppu* ppu) {
	if (ppu->dot > 256) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		if (ppu->spriteCounter[i] > 0 && !ppu->skippedDot) {
			ppu->spriteCounter[i]--;
		} else if (rendering(ppu)) {
			ppu->spriteL[i] <<= 1;
			ppu->spriteH[i] <<= 1;
		}
	}
}

static void corruptOam(struct Ppu* ppu) {
	if (ppu->corruptIndex == 0x20) {
		ppu->corruptIndex = 0;
	}
	for (int i = 0; i < 8; i++) {
		ppu->oam[ppu->corruptIndex * 8 + i] = ppu->oam[i];
	}
	ppu->oam2[ppu->corruptIndex] = ppu->oam2[0];
}

static void incrementOam2(struct Ppu* ppu) {
	if (!ppu->oam2Overflowed && ++ppu->oam2Addr == 0x20) {
		ppu->oam2Overflowed = true;
		ppu->oam2Addr = 0;
	}
}

static void markCorruption(struct Ppu* ppu) {
	ppu->corruptOnDisable = false;
	ppu->corruptOnDisableInstant = false;
	ppu->pendingCorrupt = true;
	ppu->corruptIndex = ppu->oam2Addr;
}

static void evaluateInRange(struct NES* nes, bool readOnly) {
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
				if (!ppu->corruptedOdd) {
					ppu->oamAddr++;
					incrementOam2(ppu);
				}
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
			if (!ppu->corruptedOdd && !readOnly) {
				if (ppu->oam2Overflowed && !ppu->nineObjects) {
					// The overflow search's diagonal walk
					if ((ppu->oamAddr & 3) == 3) {
						ppu->oamAddr++;
					} else {
						ppu->oamAddr += 5;
					}
				} else {
					ppu->oamAddr = (ppu->oamAddr + 4) & 0xFC;
				}
			}
		}
	} else {
		if (ppu->evalTick == 3) {
			int offset = ppu->scanline - ppu->oamBuffer;
			if (offset >= 0 && offset < height) {
				if (!ppu->corruptedOdd && !readOnly) {
					ppu->oamAddr += ppu->oam2Overflowed ? 4 : 1;
				}
			} else if (!ppu->oam2Overflowed) {
				if (!ppu->corruptedOdd && !readOnly) {
					ppu->oamAddr = (ppu->oamAddr + 1) & 0xFC;
				}
			} else {
				ppu->oamAddr = (ppu->oamAddr + 1) & 0xFC;
			}
		} else if (!ppu->corruptedOdd && !readOnly) {
			ppu->oamAddr++;
		}
		ppu->evalTick = (ppu->evalTick + 1) & 3;
		if (!ppu->oam2Overflowed && !readOnly) {
			incrementOam2(ppu);
		}
	}
	ppu->corruptedOdd = false;
	if (ppu->oamAddr < before && ppu->oamAddr < 4) {
		ppu->oamAddrOverflowed = true;
	}
	ppu->oamBufferIn = oam2Read;
}

static void spriteFetch(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int slot = ((ppu->dot - 1) & 0x38) >> 3;
	int height = ppu->tall ? 16 : 8;
	bool on = renderingDelayed(ppu);
	switch ((ppu->dot - 1) & 7) {
	case 0:
		if (on) {
			ppu->parNt = 0x2000 | (ppu->v & 0xFFF);
			ppu->addressBus = ppu->parNt;
			incrementOam2(ppu);
		}
		break;
	case 1:
		if (on) {
			ppu->verticalOffset = (uint16_t) ((ppu->scanline & 0xFF) - ppu->oamBuffer);
			backgroundFetch(nes);
			incrementOam2(ppu);
		}
		break;
	case 2:
		if (on) {
			ppu->spritePattern = ppu->oamBuffer;
			ppu->parNt = 0x2000 | (ppu->v & 0xFFF);
			ppu->addressBus = ppu->parNt;
			incrementOam2(ppu);
		}
		break;
	case 3:
		if (on) {
			ppu->spriteAttr[slot] = ppu->oamBuffer;
			ppu->spriteAttrNow = ppu->oamBuffer;
			backgroundFetch(nes);
		}
		break;
	case 4:
		if (on) {
			ppu->spriteCounter[slot] = ppu->oamBuffer;
			ppu->spriteX[slot] = ppu->oamBuffer;
			checkPar(ppu);
			ppu->parChr &= ~8;
			ppu->addressBus = ppu->parChr;
		}
		break;
	case 5:
		if (on) {
			ppu->spriteX[slot] = ppu->oamBuffer;
			checkPar(ppu);
			ppu->parChr &= ~8;
			ppu->addressBus = (ppu->parChr & 0xFF00) | ppu->octalLatch;
			uint8_t lo = fetchVideo(nes);
			if (ppu->spriteAttr[slot] & 0x40) {
				lo = flip(lo);
			}
			ppu->spriteL[slot] = ppu->verticalOffset < height ? lo : 0;
		}
		break;
	case 6:
		if (on) {
			ppu->spriteX[slot] = ppu->oamBuffer;
			checkPar(ppu);
			ppu->parChr |= 8;
			ppu->addressBus = ppu->parChr;
		}
		break;
	case 7:
		if (on) {
			ppu->spriteX[slot] = ppu->oamBuffer;
			checkPar(ppu);
			ppu->parChr |= 8;
			ppu->addressBus = (ppu->parChr & 0xFF00) | ppu->octalLatch;
			uint8_t hi = fetchVideo(nes);
			if (ppu->spriteAttr[slot] & 0x40) {
				hi = flip(hi);
			}
			ppu->spriteH[slot] = ppu->verticalOffset < height ? hi : 0;
			incrementOam2(ppu);
		}
		break;
	}
}

static void spriteEvaluation(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	bool readOnly = ppu->scanline == preLine(nes);
	int dot = ppu->dot;
	bool on = renderingDelayed(ppu);

	if ((ppu->showBgInstant || ppu->showSpritesInstant) && ppu->pendingCorrupt) {
		ppu->pendingCorrupt = false;
		if (!ppu->corruptOnEnable) {
			corruptOam(ppu);
		}
		ppu->corruptOnEnable = false;
	}

	if (dot <= 64) {
		// Clearing secondary OAM
		if (dot & 1) {
			if (on) {
				ppu->oamBufferIn = readOnly ? ppu->oam2[ppu->oam2Addr] : 0xFF;
				if (dot == 1) {
					ppu->oam2Addr = 0;
					ppu->evalTick = 0;
					ppu->oamAddrOverflowed = false;
				}
				if (ppu->corruptOnDisable || ppu->corruptOnDisableInstant) {
					markCorruption(ppu);
				}
			}
		} else if (dot > 0) {
			if (on) {
				if (!readOnly) {
					ppu->oam2[ppu->oam2Addr] = ppu->oamBuffer;
				}
				if (ppu->corruptOnDisable) {
					markCorruption(ppu);
				}
				incrementOam2(ppu);
				if (ppu->corruptOnDisableInstant && dot == 64) {
					ppu->corruptOnDisable = false;
					ppu->corruptOnDisableInstant = false;
					ppu->pendingCorrupt = true;
				}
			} else if (ppu->corruptOnDisable || ppu->corruptOnDisableInstant) {
				markCorruption(ppu);
			}
		} else if (on) {
			incrementOam2(ppu);
		}
	} else if (dot <= 256) {
		// Finding the sprites in range
		if (dot == 65 && on) {
			ppu->nineObjects = false;
			ppu->corruptedOdd = false;
			ppu->oamAddrOverflowed = false;
		}
		if (ppu->showBgInstant || ppu->showSpritesInstant || ppu->corruptOnDisableInstant) {
			if (dot & 1) {
				ppu->oamBufferIn = ppu->oam[ppu->oamAddr];
				if (ppu->corruptOnDisableInstant) {
					ppu->corruptOnDisable = false;
					if (!readOnly) {
						ppu->oamAddr++;
					}
					ppu->corruptedOdd = true;
				}
			} else {
				if (!ppu->oamAddrOverflowed) {
					evaluateInRange(nes, readOnly);
				} else {
					if (!ppu->corruptedOdd && !readOnly) {
						ppu->oamAddr = (ppu->oamAddr + 4) & 0xFC;
					}
					ppu->oamBufferIn = ppu->oam2[ppu->oam2Addr];
				}
				if (ppu->corruptOnDisableInstant) {
					ppu->corruptOnDisable = false;
					ppu->corruptOnDisableInstant = false;
					ppu->pendingCorrupt = true;
					if ((ppu->oam2Addr & 3) && !ppu->oamAddrOverflowed && !readOnly) {
						ppu->oam2Addr = (ppu->oam2Addr & 0xFC) + 4;
					}
					ppu->corruptIndex = ppu->oam2Addr;
					if (dot == 256) {
						ppu->corruptIndex = ppu->corruptedOdd ? 0 : 1;
					}
				}
				ppu->corruptOnDisableInstant = false;
			}
		}
	} else if (dot <= 320) {
		// Fetching the sprites found
		ppu->hasZero = ppu->nextHasZero;
		if (on) {
			ppu->oamAddr = 0;
		}
		if (ppu->corruptOnDisable && (nes->ppuClock == 0 || nes->ppuClock == 3)) {
			markCorruption(ppu);
		}
		if (ppu->ppuRead) {
			ppu->octalLatch = ppu->addressBus;
		}
		ppu->oamBufferIn = on ? ppu->oam2[ppu->oam2Addr] : ppu->oam[ppu->oamAddr];
		spriteFetch(nes);
		if (ppu->ale && !ppu->ppuRead) {
			ppu->octalLatch = ppu->addressBus;
		}
		if (ppu->corruptOnDisable && (nes->ppuClock == 1 || nes->ppuClock == 2)) {
			markCorruption(ppu);
		}
	} else {
		if (ppu->corruptOnDisable || ppu->corruptOnDisableInstant) {
			markCorruption(ppu);
		}
		ppu->oamBufferIn = rendering(ppu) ? ppu->oam2[ppu->oam2Addr] : ppu->oam[ppu->oamAddr];
		if (dot == 339 && !rendering(ppu)) {
			memset(ppu->spriteCounter, 0, sizeof(ppu->spriteCounter));
		}
	}
}

static void calculatePixel(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	uint8_t palette = 0, color = 0;
	if (ppu->showBg && (ppu->dot > 8 || ppu->showBgLeft)) {
		int bit = 15 - ppu->fineX;
		color = ((ppu->bgL >> bit) & 1) | (((ppu->bgH >> bit) & 1) << 1);
		palette = ((ppu->atL >> (7 - ppu->fineX)) & 1) | (((ppu->atH >> (7 - ppu->fineX)) & 1) << 1);
		if (!color) {
			palette = 0;
		}
	}
	if (ppu->showSprites && (ppu->dot > 8 || ppu->showSpritesLeft)) {
		uint8_t spriteColor = 0, spritePalette = 0;
		bool front = false;
		int i = 0;
		for (; i < 8; i++) {
			if (ppu->spriteCounter[i] != 0 && !ppu->skippedDot) {
				continue;
			}
			spriteColor = ((ppu->spriteL[i] >> 7) & 1) | (((ppu->spriteH[i] >> 7) & 1) << 1);
			spritePalette = (ppu->spriteAttr[i] & 3) | 4;
			front = !(ppu->spriteAttr[i] & 0x20);
			if (spriteColor) {
				break;
			}
		}
		if (ppu->canHit && i == 0 && ppu->hasZero && ppu->showBg && ppu->showSprites && color && spriteColor &&
			(ppu->showSpritesLeft || ppu->dot > 8) && ppu->dot < 256) {
			ppu->pendingHit = true;
			ppu->canHit = false;
		}
		if (spriteColor && (!color || front)) {
			color = spriteColor;
			palette = spritePalette;
		}
	}
	uint8_t address;
	if (rendering(ppu) && ppu->scanline < 240) {
		address = (palette << 2) | color;
	} else if ((ppu->v & 0x3F1F) >= 0x3F00) {
		address = ppu->v & 0x1F;
		if (!(address & 3)) {
			address &= 0x0F;
		}
	} else {
		address = 0;
	}
	ppu->dotColor = ppu->palette[address] & 0x3F;
}

static void drawToScreen(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (ppu->scanline >= 240) {
		return;
	}
	int dot = ppu->dot;
	bool shifted = ppu->scanline == 0 && ppu->oddFrame && rendering(ppu) && nes->region == REGION_NTSC;
	uint16_t emphasis = (ppu->emphRed ? 0x40 : 0) | (ppu->emphGreen ? 0x80 : 0) | (ppu->emphBlue ? 0x100 : 0);
	if (dot > 3 && dot <= 259) {
		uint8_t color = ppu->prev3;
		if (ppu->gray) {
			color &= 0x30;
		}
		if (!(shifted && dot == 4)) {
			nes->frameBuffer[ppu->scanline * NES_WIDTH + dot - 4 - shifted] = color | emphasis;
		}
	}
	if (shifted && dot == 259) {
		nes->frameBuffer[ppu->scanline * NES_WIDTH + 255] = ppu->palette[0] | emphasis;
	}
}

// $2007's latch chain, the first half
static void dataMachine(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	bool blank = !rendering(ppu) || (ppu->scanline >= 240 && ppu->scanline < preLine(nes));
	ppu->blnkLatch = blank;
	bool h0Dash = ((ppu->dot - 1) & 1) != 0;
	ppu->paletteEnable = (ppu->addressBus & 0x3F00) == 0x3F00 && ppu->blnkLatch;
	ppu->rdL[0] = ppu->rdSR;
	ppu->rd = false;
	ppu->rdL[2] = !ppu->rdL[1];
	ppu->rdL[4] = !ppu->rdL[3];
	ppu->pdRb = ppu->rdL[4] && !ppu->rdL[2];
	ppu->readAle = !ppu->rdL[4] && ppu->rdL[2];
	ppu->ppuRead = ppu->pdRb || (!blank && h0Dash);
	ppu->wrL[0] = ppu->wrSR;
	ppu->wr = false;
	ppu->wrL[2] = !ppu->wrL[1];
	ppu->wrL[4] = !ppu->wrL[3];
	ppu->writeAle = !ppu->wrL[4] && ppu->wrL[2];
	ppu->tstepLatch = ppu->dbPar;
	ppu->ale = ppu->readAle || ppu->writeAle || (!blank && !h0Dash);
	if ((ppu->readAle || ppu->writeAle) && !ppu->ppuRead) {
		ppu->addressBus = ppu->v;
		ppu->octalLatch = ppu->addressBus;
	}
}

static void dataMachineEnd(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (ppu->pdRb) {
		ppu->readBuffer = fetchVideo(nes);
		if (ppu->ale) {
			ppu->octalLatch = ppu->addressBus;
		}
	}
}

static void dataMachineHalf(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (ppu->tstepLatch || ppu->pdRb) {
		if (!ppu->blnkLatch) {
			incrementY(ppu);
		} else {
			ppu->v = (ppu->v + (ppu->increment32 ? 32 : 1)) & 0x7FFF;
		}
	}
	ppu->ale = ppu->readAle || ppu->writeAle;
	if (ppu->pdRb) {
		ppu->readBuffer = fetchVideo(nes);
		if (ppu->ale) {
			ppu->octalLatch = ppu->addressBus;
		}
	}
	ppu->rdL[1] = !ppu->rdL[0];
	ppu->rdL[3] = !ppu->rdL[2];
	if (!ppu->rdL[3]) {
		ppu->rdSR = false;
	}
	ppu->wrL[1] = !ppu->wrL[0];
	ppu->wrL[3] = !ppu->wrL[2];
	if (!ppu->wrL[3]) {
		ppu->wrSR = false;
	}
	ppu->dbPar = ppu->wrL[1] && !ppu->wrL[3];
	ppu->ppuWrite = !ppu->paletteEnable && ppu->dbPar;
	if (ppu->dbPar) {
		writeVideo(nes, ppu->writeData);
		if (ppu->ale) {
			ppu->octalLatch = ppu->writeData;
			writeVideo(nes, ppu->writeData);
		}
	}
}

void ppuStep(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	int vbl = vblankLine(nes);
	int pre = preLine(nes);
	ppu->dots++;

	if (ppu->delay2005 && --ppu->delay2005 == 0) {
		if (!ppu->w) {
			ppu->fineX = ppu->value2005 & 7;
			ppu->t = (ppu->t & 0x7FE0) | (ppu->value2005 >> 3);
		} else {
			ppu->t = (ppu->t & 0x0C1F) | ((ppu->value2005 & 0xF8) << 2) | ((ppu->value2005 & 7) << 12);
		}
		ppu->w = !ppu->w;
	}
	if (renderLine(nes) && rendering(ppu) && ppu->renderingCounter >= 1) {
		if (ppu->dot == 256) {
			incrementY(ppu);
		} else if (ppu->dot == 257) {
			ppu->v = (ppu->v & 0x7BE0) | (ppu->t & 0x041F);
		}
		if (ppu->dot >= 280 && ppu->dot <= 304 && ppu->scanline == pre) {
			ppu->v = (ppu->v & 0x041F) | (ppu->t & 0x7BE0);
		}
	}

	if (++ppu->dot > 340) {
		ppu->dot = 0;
		if (++ppu->scanline > pre) {
			ppu->scanline = 0;
			ppu->frame++;
		}
	}
	if (ppu->scanline == vbl) {
		if (ppu->dot == 0) {
			ppu->pendingVblank = true;
		} else if (ppu->dot == 1) {
			ppu->resetting = false;
			nes->frameDone = true;
		}
	} else if (ppu->scanline == pre - 1 && ppu->dot == 340) {
		ppu->oddFrame = !ppu->oddFrame;
	} else if (ppu->scanline == pre && ppu->dot == 1) {
		ppu->vblank = false;
		ppu->canHit = true;
		ppu->hit = false;
		ppu->overflow = false;
		ppu->hitDelayed = false;
	}

	ppu->vsetLatch1 = !ppu->vset;
	if (ppu->vset && !ppu->vsetLatch2) {
		ppu->vblank = true;
	}
	if (ppu->read2002) {
		ppu->read2002 = false;
		ppu->vblank = false;
	}
	ppu->overflowDelayed = ppu->overflow;
	if (nes->watchPpuBus) {
		nes->board->ppuBus(nes, ppu->addressBus);
	}

	if (ppu->oddFrame && rendering(ppu) && nes->region == REGION_NTSC) {
		if (ppu->scanline == 0 && ppu->dot == 0) {
			// Odd frames skip dot 0 of line 0 while rendering
			ppu->dot++;
			ppu->skippedDot = true;
		} else if (ppu->scanline == 0 && ppu->dot == 2) {
			ppu->skippedDot = false;
		}
	}

	if ((nes->ppuCpuClock & 3) != 3) {
		ppu->showBgDelayed = ppu->showBg;
		ppu->showSpritesDelayed = ppu->showSprites;
	}
	dataMachine(nes);
	ppu->oamLatch = ppu->oamBuffer;

	ppu->copyV = false;
	if (ppu->delay2006 && --ppu->delay2006 == 0) {
		ppu->copyV = true;
		ppu->v = ppu->t;
		ppu->addressBus = ppu->v;
	}
	if (ppu->oam2Reset) {
		ppu->oam2Reset--;
	}
	if (renderLine(nes)) {
		spriteEvaluation(nes);
		if (renderingDelayed(ppu) && (ppu->dot == 63 || ppu->dot == 255 || ppu->dot == 339)) {
			ppu->oam2Reset = 3;
		}
	}
	if ((nes->ppuCpuClock & 3) == 3) {
		ppu->showBgDelayed = ppu->showBg;
		ppu->showSpritesDelayed = ppu->showSprites;
	}
	if (!rendering(ppu)) {
		ppu->renderingCounter = 0;
		ppu->addressBus = ppu->v;
	} else if (ppu->renderingCounter < 5) {
		ppu->renderingCounter++;
	}

	if (ppu->delay2001 && --ppu->delay2001 == 0) {
		uint8_t value = ppu->value2001;
		ppu->showBgLeft = value & 0x02;
		ppu->showSpritesLeft = value & 0x04;
		ppu->showBg = value & 0x08;
		ppu->showSprites = value & 0x10;
		ppu->showBgInstant = ppu->showBg;
		ppu->showSpritesInstant = ppu->showSprites;
	}
	if (ppu->delay2001Oam && --ppu->delay2001Oam == 0) {
		if (ppu->wasRendering2001 && !(ppu->value2001 & 0x18) && renderLine(nes) && !ppu->pendingCorrupt) {
			ppu->corruptOnDisable = true;
		}
	}
	if (ppu->delay2001Emphasis && --ppu->delay2001Emphasis == 0) {
		ppu->gray = ppu->value2001 & 0x01;
		ppu->emphRed = ppu->value2001 & 0x20;
		ppu->emphGreen = ppu->value2001 & 0x40;
		ppu->emphBlue = ppu->value2001 & 0x80;
	}

	ppu->prev3 = ppu->prev2;
	ppu->prev2 = ppu->prev1;
	ppu->prev1 = ppu->dotColor;
	if (renderLine(nes) || (ppu->scanline == 240 && ppu->dot == 0)) {
		if ((ppu->dot >= 1 && ppu->dot <= 256) || (ppu->dot >= 321 && ppu->dot <= 336)) {
			if (renderingDelayed(ppu)) {
				backgroundFetch(nes);
			}
		} else if (ppu->dot >= 337 || ppu->dot == 0) {
			if (renderingDelayed(ppu)) {
				dummyNametableFetch(nes);
			}
		}
		if (ppu->dot > 0 && ppu->dot <= 256) {
			if (ppu->scanline < 241) {
				calculatePixel(nes);
			}
			shiftSprites(ppu);
		}
		drawToScreen(nes);
	}
	dataMachineEnd(nes);
}

void ppuHalfStep(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (renderLine(nes) && ((ppu->dot > 0 && ppu->dot <= 257) || (ppu->dot > 320 && ppu->dot <= 336)) &&
		rendering(ppu)) {
		shiftBackground(ppu);
	}
	if (ppu->oam2Reset && --ppu->oam2Reset == 0) {
		ppu->oam2Addr = 0;
		ppu->oam2Overflowed = false;
	}
	commitFetches(ppu);
	if (renderingDelayed(ppu) || rendering(ppu)) {
		ppu->oamBuffer = ppu->oamBufferIn;
	}
	ppu->vset = false;
	if (ppu->pendingVblank) {
		ppu->pendingVblank = false;
		ppu->vset = true;
	}
	ppu->vsetLatch2 = !ppu->vsetLatch1;
	ppu->hitDelayed = ppu->hit;
	if (ppu->pendingHit2) {
		ppu->pendingHit2 = false;
		ppu->hit = true;
	}
	if (ppu->pendingHit) {
		ppu->pendingHit = false;
		ppu->pendingHit2 = true;
	}
	dataMachineHalf(nes);
}

// Runs the master clock to the end of a register access (7 clocks in)
static void untilEndOfAccess(struct NES* nes) {
	for (int i = 0; i < 7; i++) {
		nesTick(nes);
	}
}

static uint8_t readOam(struct NES* nes) {
	struct Ppu* ppu = &nes->ppu;
	if (rendering(ppu) && ppu->scanline < 240) {
		return ppu->oamLatch;
	}
	return ppu->oam[ppu->oamAddr];
}

uint8_t ppuReadRegister(struct NES* nes, uint16_t address) {
	if (!nes->exactPpu) {
		return fastReadRegister(nes, address);
	}
	struct Ppu* ppu = &nes->ppu;
	uint8_t value;
	switch (address & 7) {
	case 2:
		// VBL is read at the start of the read, the sprite flags at the end
		value = ppu->vblank ? 0x80 : 0;
		ppu->read2002 = true;
		untilEndOfAccess(nes);
		value |= (ppu->hitDelayed ? 0x40 : 0) | (ppu->overflowDelayed ? 0x20 : 0) | (openBus(ppu) & 0x1F);
		ppu->w = false;
		setBus(ppu, value, 0xE0);
		return value;
	case 4:
		untilEndOfAccess(nes);
		value = readOam(nes);
		setBus(ppu, value, 0xFF);
		return value;
	case 7:
		if ((ppu->addressBus & 0x3FFF) >= 0x3F00) {
			uint16_t index = ppu->v & 0x1F;
			if (!(index & 3)) {
				index &= 0x0F;
			}
			value = (ppu->palette[index] & (ppu->gray ? 0x30 : 0x3F)) | (openBus(ppu) & 0xC0);
		} else {
			value = ppu->readBuffer;
		}
		setBus(ppu, value, 0xFF);
		untilEndOfAccess(nes);
		ppu->rdSR = true;
		ppu->rd = true;
		return value;
	default:
		return openBus(ppu);
	}
}

void ppuWriteRegister(struct NES* nes, uint16_t address, uint8_t value, uint8_t busBefore) {
	if (!nes->exactPpu) {
		fastWriteRegister(nes, address, value, busBefore);
		return;
	}
	struct Ppu* ppu = &nes->ppu;
	int phase = nes->ppuClock & 3;
	setBus(ppu, value, 0xFF);
	switch (address & 7) {
	case 0:
		if (ppu->resetting) {
			return;
		}
		ppu->t = (ppu->t & 0x73FF) | ((busBefore & 3) << 10);
		nesTick(nes);
		nesTick(nes);
		ppu->nmiEnabled = value & 0x80;
		ppu->increment32 = value & 0x04;
		ppu->tall = value & 0x20;
		ppu->spriteTable = value & 0x08;
		ppu->bgTable = value & 0x10;
		ppu->t = (ppu->t & 0x73FF) | ((value & 3) << 10);
		break;
	case 1: {
		if (ppu->resetting) {
			return;
		}
		static const uint8_t delays[4][3] = { { 2, 2, 2 }, { 2, 1, 3 }, { 3, 1, 3 }, { 2, 2, 2 } };
		ppu->delay2001 = delays[phase][0];
		ppu->delay2001Emphasis = delays[phase][1];
		ppu->delay2001Oam = delays[phase][2];
		ppu->wasRendering2001 = rendering(ppu);
		ppu->showBgInstant = ppu->showBg;
		ppu->showSpritesInstant = ppu->showSprites;
		bool now = value & 0x18;
		if (ppu->wasRendering2001 && !now) {
			if (ppu->scanline < 241 || ppu->scanline == preLine(nes)) {
				ppu->corruptOnDisableInstant = true;
			}
		} else if (!ppu->wasRendering2001 && now) {
			if ((ppu->scanline < 241 || ppu->scanline == preLine(nes)) && ppu->pendingCorrupt &&
				(nes->ppuClock == 1 || nes->ppuClock == 2)) {
				ppu->corruptOnEnable = true;
			}
		}
		if (ppu->delay2001Emphasis == 2) {
			ppu->gray = busBefore & 0x01;
			ppu->emphBlue = busBefore & 0x80;
		} else {
			ppu->delay2001Emphasis++;
		}
		ppu->emphRed = value & 0x20;
		ppu->emphGreen = value & 0x40;
		ppu->value2001 = value;
		break;
	}
	case 3:
		ppu->oamAddr = value;
		break;
	case 4:
		if ((ppu->scanline >= 240 && ppu->scanline < preLine(nes) && rendering(ppu)) || !rendering(ppu)) {
			if ((ppu->oamAddr & 3) == 2) {
				value &= 0xE3;
			}
			ppu->oam[ppu->oamAddr++] = value;
		} else {
			ppu->oamAddr = (ppu->oamAddr + 4) & 0xFC;
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
			ppu->value2006 = ppu->t;
			ppu->value2006Before = ppu->v;
			ppu->delay2006 = phase == 2 ? 5 : 4;
		}
		ppu->w = !ppu->w;
		break;
	case 7:
		ppu->writeData = value;
		untilEndOfAccess(nes);
		ppu->wr = true;
		ppu->wrSR = true;
		break;
	}
}

// Dots from here before the VBL flag can change on its own (none while a change is under way)
int ppuQuietDots(const struct NES* nes) {
	const struct Ppu* ppu = &nes->ppu;
	if (ppu->read2002 || ppu->vset || ppu->pendingVblank) {
		return 0;
	}
	int lines = preLine(nes) + 1;
	int here = ppu->scanline * 341 + ppu->dot;
	int toVbl = (vblankLine(nes) * 341 - here + lines * 341) % (lines * 341);
	int toPre = (preLine(nes) * 341 - here + lines * 341) % (lines * 341);
	int quiet = toVbl < toPre ? toVbl : toPre;
	return quiet > 4 ? quiet - 4 : 0;
}

bool ppuNmiOutput(const struct NES* nes) {
	return nes->ppu.nmiEnabled && nes->ppu.vblank;
}

void ppuReset(struct NES* nes, bool hard) {
	struct Ppu* ppu = &nes->ppu;
	if (hard) {
		memset(ppu, 0, sizeof(*ppu));
		static const uint8_t bootPalette[32] = {
			0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
			0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08,
		};
		memcpy(ppu->palette, bootPalette, sizeof(bootPalette));
		ppu->oddFrame = true;
		ppu->canHit = true;
		return;
	}
	ppu->nmiEnabled = ppu->increment32 = ppu->tall = ppu->spriteTable = ppu->bgTable = false;
	ppu->t = 0;
	ppu->gray = ppu->emphRed = ppu->emphGreen = ppu->emphBlue = false;
	ppu->showBg = ppu->showSprites = ppu->showBgLeft = ppu->showSpritesLeft = false;
	ppu->delay2005 = 0;
	ppu->fineX = 0;
	ppu->readBuffer = 0;
	ppu->oddFrame = false;
	ppu->dot = 0;
	ppu->scanline = 0;
	ppu->w = false;
	ppu->resetting = true;
}
