/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mmc3.h"

// MMC3 (4) and the boards built on it. A variant can rewrite each 8 KB PRG and 1 KB CHR bank the
// chip asks for, and take over mirroring and work RAM, which is how most pirate MMC3 boards differ.

static void defaultPrg(struct NES* nes, uint16_t address, int bank) {
	prg8(nes, address, bank);
}

static void defaultChr(struct NES* nes, uint16_t address, int bank) {
	chr1(nes, address, bank);
}

static const struct Mmc3Variant standard = { NULL, NULL, NULL, NULL };

static inline const struct Mmc3Variant* variant(struct NES* nes) {
	return nes->board->data ? nes->board->data : &standard;
}

void mmc3Sync(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	const struct Mmc3Variant* v = variant(nes);
	void (*prg)(struct NES*, uint16_t, int) = v->prg ? v->prg : defaultPrg;
	void (*chr)(struct NES*, uint16_t, int) = v->chr ? v->chr : defaultChr;

	if (m->select & 0x40) {
		prg(nes, 0x8000, 0xFE);
		prg(nes, 0xC000, m->banks[6]);
	} else {
		prg(nes, 0x8000, m->banks[6]);
		prg(nes, 0xC000, 0xFE);
	}
	prg(nes, 0xA000, m->banks[7]);
	prg(nes, 0xE000, 0xFF);

	uint16_t a12 = (m->select & 0x80) ? 0x1000 : 0;
	chr(nes, 0x0000 ^ a12, m->banks[0] & 0xFE);
	chr(nes, 0x0400 ^ a12, m->banks[0] | 1);
	chr(nes, 0x0800 ^ a12, m->banks[1] & 0xFE);
	chr(nes, 0x0C00 ^ a12, m->banks[1] | 1);
	for (int i = 0; i < 4; i++) {
		chr(nes, (0x1000 + i * 0x400) ^ a12, m->banks[2 + i]);
	}

	if (v->mirror) {
		v->mirror(nes);
	} else if (nes->cart.mirroring != MIRROR_FOUR) {
		nesSetMirroring(nes, (m->mirroring & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	}
	if (v->wram) {
		v->wram(nes);
	} else if (m->wramControl & 0x80) {
		nesMapWram(nes, 0x6000, 0x2000, 0, !(m->wramControl & 0x40));
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
	}
}

void mmc3Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Mmc3* m = STATE(Mmc3);
	if (address < 0x8000) {
		return;
	}
	switch (address & 0xE001) {
	case 0x8000:
		m->select = value;
		break;
	case 0x8001:
		m->banks[m->select & 7] = value;
		break;
	case 0xA000:
		m->mirroring = value;
		break;
	case 0xA001:
		m->wramControl = value;
		break;
	case 0xC000:
		m->latch = value;
		return;
	case 0xC001:
		m->counter = 0;
		m->reload = true;
		return;
	case 0xE000:
		m->irqEnabled = false;
		nesSetIrq(nes, IRQ_MAPPER, false);
		return;
	case 0xE001:
		m->irqEnabled = true;
		return;
	}
	mmc3Sync(nes);
}

static void mmc3Irq(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	if (m->counter == 0 && m->irqEnabled) {
		nesSetIrq(nes, IRQ_MAPPER, true);
	}
}

// M2 rises (clock 7 of each CPU cycle) in (from, to]
static int m2Rises(struct NES* nes, uint64_t from, uint64_t to) {
	int64_t period = nes->cpuPeriod;
	int64_t origin = (int64_t) nes->slotTick + 7;
	int64_t a = (int64_t) from - origin, b = (int64_t) to - origin;
	int64_t fa = a >= 0 ? a / period : -((-a + period - 1) / period);
	int64_t fb = b >= 0 ? b / period : -((-b + period - 1) / period);
	return fb - fa;
}

// A12 rising after three M2 rises with it low clocks the counter (after TriCNES)
void mmc3PpuBus(struct NES* nes, uint16_t address) {
	struct Mmc3* m = STATE(Mmc3);
	bool a12 = address & 0x1000;
	if (a12 == m->a12) {
		return;
	}
	if (!a12) {
		m->a12LowSince = nes->ppu.dotTick;
	} else if (m2Rises(nes, m->a12LowSince, nes->ppu.dotTick) >= 3) {
		if (m->reload) {
			m->counter = m->latch;
			m->reload = false;
			mmc3Irq(nes);
		} else if (--m->counter == 0) {
			mmc3Irq(nes);
		} else if (m->counter == 255) {
			m->counter = m->latch;
			mmc3Irq(nes);
		}
	}
	m->a12 = a12;
}

// Each counter clock needs A12 low for three M2 rises, so at least eight dots apart
int mmc3IrqQuietDots(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	if (!m->irqEnabled) {
		return 1 << 20;
	}
	int safe = (m->reload || m->counter == 0) ? m->latch : m->counter - 1;
	return safe * 8;
}

void mmc3Reset(struct NES* nes, bool hard) {
	struct Mmc3* m = STATE(Mmc3);
	if (hard) {
		static const uint8_t banks[8] = { 0, 2, 4, 5, 6, 7, 0, 1 };
		memcpy(m->banks, banks, sizeof(banks));
		m->wramControl = 0x80;
	}
	nes->watchPpuBus = true;
	mmc3Sync(nes);
}

#define MMC3_BOARD(var, label, variantData, writer) \
	const struct Board var = { .name = label, .reset = mmc3Reset, .write = writer, .ppuBus = mmc3PpuBus, \
		.irqQuietDots = mmc3IrqQuietDots, .sync = mmc3Sync, .stateSize = sizeof(struct Mmc3), .data = variantData }

MMC3_BOARD(boardMmc3, "MMC3", NULL, mmc3Write);

// TxSROM (118): each name table follows bit 7 of the CHR bank at its slot in $0000-$0FFF
static void txsromChr(struct NES* nes, uint16_t address, int bank) {
	chr1(nes, address, bank);
	if (address < 0x1000) {
		nesSetNametable(nes, address >> 10, nes->ppu.ntRam + ((bank >> 7) & 1) * 0x400);
	}
}

static void noMirroring(struct NES* nes) {
}

static const struct Mmc3Variant txsrom = { NULL, txsromChr, noMirroring, NULL };
MMC3_BOARD(boardTxsrom, "TxSROM", &txsrom, mmc3Write);

// TQROM (119): CHR banks with bit 6 set are 8 KB of CHR RAM
static void tqromChr(struct NES* nes, uint16_t address, int bank) {
	if (bank & 0x40) {
		nesMapVram(nes, address, 0x400, bank & 7);
	} else {
		chr1(nes, address, bank & 0x3F);
	}
}

static const struct Mmc3Variant tqrom = { NULL, tqromChr, NULL, NULL };
MMC3_BOARD(boardTqrom, "TQROM", &tqrom, mmc3Write);

// Namco 108 (206) and its relatives: MMC3's bank registers only, PRG mode 0, no CHR inversion,
// IRQ or work RAM, mirroring from the board
struct Namco108Variant {
	void (*chr)(struct NES*);
	bool screenSelect;
};

static void namco108Sync(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	const struct Namco108Variant* v = nes->board->data;
	prg8(nes, 0x8000, m->banks[6] & 0x0F);
	prg8(nes, 0xA000, m->banks[7] & 0x0F);
	prg8(nes, 0xC000, -2);
	prg8(nes, 0xE000, -1);
	if (v && v->chr) {
		v->chr(nes);
	} else {
		chr2(nes, 0x0000, (m->banks[0] & 0x3F) >> 1);
		chr2(nes, 0x0800, (m->banks[1] & 0x3F) >> 1);
		for (int i = 0; i < 4; i++) {
			chr1(nes, 0x1000 + i * 0x400, m->banks[2 + i] & 0x3F);
		}
	}
	if (v && v->screenSelect) {
		nesSetMirroring(nes, (m->select & 0x40) ? MIRROR_SCREEN_B : MIRROR_SCREEN_A);
	}
}

static void namco108Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Mmc3* m = STATE(Mmc3);
	if (address < 0x8000 || address >= 0xA000) {
		return;
	}
	if (address & 1) {
		m->banks[m->select & 7] = value;
	} else {
		m->select = value;
	}
	namco108Sync(nes);
}

static void namco108Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Mmc3));
	}
	namco108Sync(nes);
}

#define NAMCO108_BOARD(var, label, variantData) \
	const struct Board var = { .name = label, .reset = namco108Reset, .write = namco108Write, .sync = namco108Sync, \
		.stateSize = sizeof(struct Mmc3), .data = variantData }

NAMCO108_BOARD(boardNamco108, "Namco 108", NULL);

// 3446 (76): four 2 KB CHR banks from R2-R5
static void namco3446Chr(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	for (int i = 0; i < 4; i++) {
		chr2(nes, i * 0x800, m->banks[2 + i] & 0x3F);
	}
}

static const struct Namco108Variant namco3446 = { namco3446Chr, false };
NAMCO108_BOARD(boardNamco3446, "Namco 3446", &namco3446);

// 3433 (88) and 3453 (154): the 1 KB banks come from the upper 64 KB; 3453 also picks one screen
static void namco3433Chr(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	chr2(nes, 0x0000, (m->banks[0] & 0x3F) >> 1);
	chr2(nes, 0x0800, (m->banks[1] & 0x3F) >> 1);
	for (int i = 0; i < 4; i++) {
		chr1(nes, 0x1000 + i * 0x400, m->banks[2 + i] | 0x40);
	}
}

static const struct Namco108Variant namco3433 = { namco3433Chr, false };
NAMCO108_BOARD(boardNamco3433, "Namco 3433", &namco3433);
static const struct Namco108Variant namco3453 = { namco3433Chr, true };
NAMCO108_BOARD(boardNamco3453, "Namco 3453", &namco3453);

// 3425 (95): bit 5 of the 2 KB CHR banks picks each half's name table page
static void namco3425Chr(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	chr2(nes, 0x0000, (m->banks[0] & 0x1F) >> 1);
	chr2(nes, 0x0800, (m->banks[1] & 0x1F) >> 1);
	for (int i = 0; i < 4; i++) {
		chr1(nes, 0x1000 + i * 0x400, m->banks[2 + i] & 0x1F);
	}
	int a = (m->banks[0] >> 5) & 1, b = (m->banks[1] >> 5) & 1;
	mirrorPages(nes, a, a, b, b);
}

static const struct Namco108Variant namco3425 = { namco3425Chr, false };
NAMCO108_BOARD(boardNamco3425, "Namco 3425", &namco3425);
