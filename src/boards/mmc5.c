/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

#include <stdlib.h>

// MMC5 (5), after the NESdev wiki: four PRG modes over ROM and RAM, four CHR modes with separate
// sprite and background banks for 8x16 sprites, 1 KB of ExRAM (as a name table, for extended
// attributes, or as RAM), fill mode, a scanline IRQ and a multiplier. The split screen and the
// sound channels aren't here yet.

struct Mmc5 {
	uint8_t prgMode, chrMode, protect[2], exramMode, ntMapping, fillTile, fillAttr;
	uint8_t prg[5];
	uint16_t chrA[8], chrB[4];
	uint8_t chrHigh;
	bool lastWasB;
	uint8_t irqTarget;
	bool irqEnabled, irqPending, inFrame;
	uint8_t lineCounter;
	uint8_t mul[2];
	// Extended attributes: the tile being fetched
	uint8_t exTile;
};

#define M5 STATE(Mmc5)

static uint8_t* exram(struct NES* nes) {
	return nes->cart.extraNt;
}

static uint8_t* fillTable(struct NES* nes) {
	return nes->cart.extraNt + 0x400;
}

static bool wramWritable(struct NES* nes) {
	return (M5->protect[0] & 3) == 2 && (M5->protect[1] & 3) == 1;
}

// A PRG bank register: bit 7 picks ROM, otherwise RAM (always RAM at $6000)
static void mapPrg(struct NES* nes, uint16_t address, uint32_t size, uint8_t reg, bool alwaysRom) {
	if (alwaysRom || (reg & 0x80)) {
		nesMapPrg(nes, address, size, (reg & 0x7F) / (size / 0x2000));
	} else {
		nesMapWram(nes, address, size, (reg & 0x0F) / (size / 0x2000), wramWritable(nes));
	}
}

static void mmc5SyncPrg(struct NES* nes) {
	struct Mmc5* m = M5;
	nesMapWram(nes, 0x6000, 0x2000, m->prg[0] & 0x0F, wramWritable(nes));
	switch (m->prgMode & 3) {
	case 0:
		mapPrg(nes, 0x8000, 0x8000, m->prg[4], true);
		break;
	case 1:
		mapPrg(nes, 0x8000, 0x4000, m->prg[2], false);
		mapPrg(nes, 0xC000, 0x4000, m->prg[4], true);
		break;
	case 2:
		mapPrg(nes, 0x8000, 0x4000, m->prg[2], false);
		mapPrg(nes, 0xC000, 0x2000, m->prg[3], false);
		mapPrg(nes, 0xE000, 0x2000, m->prg[4], true);
		break;
	case 3:
		mapPrg(nes, 0x8000, 0x2000, m->prg[1], false);
		mapPrg(nes, 0xA000, 0x2000, m->prg[2], false);
		mapPrg(nes, 0xC000, 0x2000, m->prg[3], false);
		mapPrg(nes, 0xE000, 0x2000, m->prg[4], true);
		break;
	}
}

// The 1 KB CHR bank at a slot, from set A (sprites, and everything in 8x8 mode when last written)
// or set B (the background with 8x16 sprites)
static int chrBank(struct NES* nes, int slot, bool setB) {
	struct Mmc5* m = M5;
	switch (m->chrMode & 3) {
	case 0:
		return (setB ? m->chrB[3] : m->chrA[7]) * 8 + slot;
	case 1:
		return (setB ? m->chrB[3] : m->chrA[slot < 4 ? 3 : 7]) * 4 + (slot & 3);
	case 2:
		return (setB ? m->chrB[(slot & 3) < 2 ? 1 : 3] : m->chrA[(slot | 1)]) * 2 + (slot & 1);
	default:
		return setB ? m->chrB[slot & 3] : m->chrA[slot];
	}
}

static void mmc5SyncNametables(struct NES* nes) {
	struct Mmc5* m = M5;
	for (int i = 0; i < 4; i++) {
		int source = (m->ntMapping >> (i * 2)) & 3;
		uint8_t* page = source == 0 ? nes->ppu.ntRam : source == 1 ? nes->ppu.ntRam + 0x400
			: source == 2 ? (m->exramMode < 2 ? exram(nes) : fillTable(nes)) : fillTable(nes);
		nesSetNametable(nes, i, page);
	}
	memset(fillTable(nes), m->fillTile, 0x3C0);
	memset(fillTable(nes) + 0x3C0, (m->fillAttr & 3) * 0x55, 0x40);
}

static void mmc5Sync(struct NES* nes) {
	mmc5SyncPrg(nes);
	for (int i = 0; i < 8; i++) {
		chr1(nes, i * 0x400, chrBank(nes, i, false));
	}
	mmc5SyncNametables(nes);
}

// Every PPU read comes through here: sprite or background banks, and extended attributes
static uint8_t mmc5PpuRead(struct NES* nes, uint16_t address) {
	struct Mmc5* m = M5;
	const struct Ppu* ppu = &nes->ppu;
	// Outside rendering (reads through $2007) the last set written is used, and no attributes
	bool active = (ppu->showBg || ppu->showSprites) && (ppu->scanline < 240 || ppu->scanline >= 261);
	if (address >= 0x2000) {
		uint8_t* page = nes->chrPages[(address >> 10) & 15];
		uint8_t value = page ? page[address & 0x3FF] : 0;
		if (m->exramMode == 1 && active && !ppu->spriteFetch && (address & 0x3FF) < 0x3C0) {
			m->exTile = exram(nes)[address & 0x3FF];
		} else if (m->exramMode == 1 && active && !ppu->spriteFetch) {
			return ((m->exTile >> 6) & 3) * 0x55;
		}
		return value;
	}
	const struct Cart* cart = &nes->cart;
	uint32_t offset;
	if (m->exramMode == 1 && active && !ppu->spriteFetch) {
		uint32_t bank = (m->exTile & 0x3F) | ((m->chrHigh & 3) << 6);
		offset = bank * 0x1000 + (address & 0xFFF);
	} else {
		bool setB = ppu->tall && active ? !ppu->spriteFetch : m->lastWasB;
		offset = chrBank(nes, address >> 10, setB) * 0x400 + (address & 0x3FF);
	}
	return cart->chr[offset % cart->chrSize];
}

static uint8_t mmc5Read(struct NES* nes, uint16_t address) {
	struct Mmc5* m = M5;
	if (address >= 0x5C00 && address < 0x6000) {
		return m->exramMode >= 2 ? exram(nes)[address & 0x3FF] : nes->cpu.openBus;
	}
	switch (address) {
	case 0x5204: {
		uint8_t value = (m->irqPending ? 0x80 : 0) | (m->inFrame ? 0x40 : 0);
		m->irqPending = false;
		nesSetIrq(nes, IRQ_MAPPER, false);
		return value;
	}
	case 0x5205:
		return (m->mul[0] * m->mul[1]) & 0xFF;
	case 0x5206:
		return (m->mul[0] * m->mul[1]) >> 8;
	}
	return nes->cpu.openBus;
}

static void mmc5Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Mmc5* m = M5;
	if (address >= 0x5C00 && address < 0x6000) {
		if (m->exramMode != 3) {
			exram(nes)[address & 0x3FF] = value;
		}
		return;
	}
	if (address < 0x5100 || address >= 0x5C00) {
		return;
	}
	switch (address) {
	case 0x5100:
		m->prgMode = value & 3;
		break;
	case 0x5101:
		m->chrMode = value & 3;
		break;
	case 0x5102:
	case 0x5103:
		m->protect[address - 0x5102] = value;
		break;
	case 0x5104:
		m->exramMode = value & 3;
		break;
	case 0x5105:
		m->ntMapping = value;
		break;
	case 0x5106:
		m->fillTile = value;
		break;
	case 0x5107:
		m->fillAttr = value;
		break;
	case 0x5113:
	case 0x5114:
	case 0x5115:
	case 0x5116:
	case 0x5117:
		m->prg[address - 0x5113] = value;
		break;
	case 0x5130:
		m->chrHigh = value & 3;
		return;
	case 0x5203:
		m->irqTarget = value;
		return;
	case 0x5204:
		m->irqEnabled = value & 0x80;
		nesSetIrq(nes, IRQ_MAPPER, m->irqEnabled && m->irqPending);
		return;
	case 0x5205:
	case 0x5206:
		m->mul[address - 0x5205] = value;
		return;
	default:
		if (address >= 0x5120 && address <= 0x5127) {
			m->chrA[address - 0x5120] = value | (m->chrHigh << 8);
			m->lastWasB = false;
		} else if (address >= 0x5128 && address <= 0x512B) {
			m->chrB[address - 0x5128] = value | (m->chrHigh << 8);
			m->lastWasB = true;
		} else {
			return;
		}
		break;
	}
	mmc5Sync(nes);
}

// The start of each rendered line: the in-frame flag and the line counter
static void mmc5Scanline(struct NES* nes, int line) {
	struct Mmc5* m = M5;
	if (line >= 240) {
		m->inFrame = false;
		return;
	}
	if (!m->inFrame) {
		m->inFrame = true;
		m->lineCounter = 0;
		m->irqPending = false;
		nesSetIrq(nes, IRQ_MAPPER, false);
		return;
	}
	if (++m->lineCounter == m->irqTarget) {
		m->irqPending = true;
		if (m->irqEnabled) {
			nesSetIrq(nes, IRQ_MAPPER, true);
		}
	}
}

static int mmc5IrqQuietDots(struct NES* nes) {
	struct Mmc5* m = M5;
	if (!m->irqEnabled || m->irqPending) {
		return 1 << 20;
	}
	int line = nes->ppu.scanline;
	if (line >= 240 || !m->inFrame) {
		return m->irqTarget ? 341 : 1 << 20;
	}
	int lines = m->irqTarget - m->lineCounter;
	return lines > 1 ? (lines - 1) * 341 : 0;
}

static void mmc5Reset(struct NES* nes, bool hard) {
	struct Mmc5* m = M5;
	struct Cart* cart = &nes->cart;
	if (hard) {
		memset(m, 0, sizeof(*m));
		m->prgMode = 3;
		m->prg[4] = 0xFF;
		memset(exram(nes), 0, 0x800);
		// Old headers don't say how much RAM: give it the most a board has
		if (cart->wramSize < 0x10000 && !cart->submapper) {
			cart->wram = realloc(cart->wram, 0x10000);
			memset(cart->wram + cart->wramSize, 0, 0x10000 - cart->wramSize);
			cart->wramSize = 0x10000;
			if (cart->battery) {
				cart->batterySize = 0x10000;
			}
		}
	}
	nes->readHooks |= 1 << 5;
	nes->ppuReadHook = true;
	mmc5Sync(nes);
}

const struct Board boardMmc5 = {
	.name = "MMC5", .reset = mmc5Reset, .write = mmc5Write, .read = mmc5Read, .ppuRead = mmc5PpuRead,
	.scanline = mmc5Scanline, .irqQuietDots = mmc5IrqQuietDots, .sync = mmc5Sync, .stateSize = sizeof(struct Mmc5),
};
