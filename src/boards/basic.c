/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

// The first boards: NROM, the discrete latches UxROM, CNROM and AxROM, and MMC1.

static void mapWram(struct NES* nes) {
	nesMapWram(nes, 0x6000, 0x2000, 0, true);
}

// NROM (0)
static void nromReset(struct NES* nes, bool hard) {
	nesMapPrg(nes, 0x8000, 0x4000, 0);
	nesMapPrg(nes, 0xC000, 0x4000, 1);
	mapWram(nes);
}

const struct Board boardNrom = { .name = "NROM", .reset = nromReset };

// UxROM (2): 16 KB at $8000, the last bank fixed at $C000
struct Latch {
	uint8_t value;
	bool conflicts;
};

static void uxromSync(struct NES* nes) {
	nesMapPrg(nes, 0x8000, 0x4000, STATE(Latch)->value);
	nesMapPrg(nes, 0xC000, 0x4000, -1);
}

static void uxromWrite(struct NES* nes, uint16_t address, uint8_t value) {
	if (address < 0x8000) {
		return;
	}
	STATE(Latch)->value = STATE(Latch)->conflicts ? busConflict(nes, address, value) : value;
	uxromSync(nes);
}

static void latchReset(struct NES* nes, bool hard) {
	STATE(Latch)->conflicts = nes->cart.submapper == 2;
	nes->board->sync(nes);
	mapWram(nes);
}

const struct Board boardUxrom = {
	.name = "UxROM", .reset = latchReset, .write = uxromWrite, .sync = uxromSync, .stateSize = sizeof(struct Latch),
};

// CNROM (3): 8 KB of CHR
static void cnromSync(struct NES* nes) {
	nesMapPrg(nes, 0x8000, 0x4000, 0);
	nesMapPrg(nes, 0xC000, 0x4000, 1);
	nesMapChr(nes, 0, 0x2000, STATE(Latch)->value);
}

static void cnromWrite(struct NES* nes, uint16_t address, uint8_t value) {
	if (address < 0x8000) {
		return;
	}
	STATE(Latch)->value = STATE(Latch)->conflicts ? busConflict(nes, address, value) : value;
	cnromSync(nes);
}

const struct Board boardCnrom = {
	.name = "CNROM", .reset = latchReset, .write = cnromWrite, .sync = cnromSync, .stateSize = sizeof(struct Latch),
};

// AxROM (7): 32 KB banks and one-screen mirroring
static void axromSync(struct NES* nes) {
	nesMapPrg(nes, 0x8000, 0x8000, STATE(Latch)->value & 0x0F);
	nesSetMirroring(nes, (STATE(Latch)->value & 0x10) ? MIRROR_SCREEN_B : MIRROR_SCREEN_A);
}

static void axromWrite(struct NES* nes, uint16_t address, uint8_t value) {
	if (address < 0x8000) {
		return;
	}
	STATE(Latch)->value = STATE(Latch)->conflicts ? busConflict(nes, address, value) : value;
	axromSync(nes);
}

const struct Board boardAxrom = {
	.name = "AxROM", .reset = latchReset, .write = axromWrite, .sync = axromSync, .stateSize = sizeof(struct Latch),
};

// MMC1 (1): a serial port into four registers. SUROM and SXROM take the 256 KB PRG half from CHR
// bank bit 4; the work RAM bank comes from bits 2-3 on SXROM, bit 3 on SOROM and bit 4 on SZROM
// (16 KB of RAM with CHR ROM). On those two the battery keeps only the second chip, which the bit
// set selects; it's kept first here, as the half saved.
struct Mmc1 {
	uint8_t shift, count;
	uint8_t control, chr0, chr1, prg;
	uint64_t lastWrite;
};

static void mmc1Sync(struct NES* nes) {
	struct Mmc1* m = STATE(Mmc1);
	static const enum Mirroring mirrors[4] = { MIRROR_SCREEN_A, MIRROR_SCREEN_B, MIRROR_VERTICAL, MIRROR_HORIZONTAL };
	nesSetMirroring(nes, mirrors[m->control & 3]);

	int outer = nes->cart.prgSize > 0x40000 ? (m->chr0 & 0x10) : 0;
	int prg = (m->prg & 0x0F) | outer;
	switch ((m->control >> 2) & 3) {
	case 0:
	case 1:
		nesMapPrg(nes, 0x8000, 0x8000, prg >> 1);
		break;
	case 2:
		nesMapPrg(nes, 0x8000, 0x4000, outer);
		nesMapPrg(nes, 0xC000, 0x4000, prg);
		break;
	case 3:
		nesMapPrg(nes, 0x8000, 0x4000, prg);
		nesMapPrg(nes, 0xC000, 0x4000, outer | 0x0F);
		break;
	}

	if (m->control & 0x10) {
		nesMapChr(nes, 0x0000, 0x1000, m->chr0);
		nesMapChr(nes, 0x1000, 0x1000, m->chr1);
	} else {
		nesMapChr(nes, 0x0000, 0x2000, m->chr0 >> 1);
	}

	int wramBank = 0;
	if (nes->cart.wramSize > 0x4000) {
		wramBank = (m->chr0 >> 2) & 3;
	} else if (nes->cart.wramSize > 0x2000) {
		wramBank = ((m->chr0 >> (nes->cart.chrRam ? 3 : 4)) & 1) ^ 1;
	}
	bool enabled = !(m->prg & 0x10) || nes->cart.submapper == 5;
	if (enabled) {
		nesMapWram(nes, 0x6000, 0x2000, wramBank, true);
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
	}
}

static void mmc1Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Mmc1* m = STATE(Mmc1);
	if (address < 0x8000) {
		return;
	}
	// The second write of a read-modify-write lands on the next cycle and is ignored
	uint64_t now = nes->cpu.cycles;
	bool consecutive = now == m->lastWrite + 1;
	m->lastWrite = now;
	if (consecutive) {
		return;
	}
	if (value & 0x80) {
		m->shift = 0;
		m->count = 0;
		m->control |= 0x0C;
		mmc1Sync(nes);
		return;
	}
	m->shift |= (value & 1) << m->count;
	if (++m->count < 5) {
		return;
	}
	switch ((address >> 13) & 3) {
	case 0:
		m->control = m->shift;
		break;
	case 1:
		m->chr0 = m->shift;
		break;
	case 2:
		m->chr1 = m->shift;
		break;
	case 3:
		m->prg = m->shift;
		break;
	}
	m->shift = 0;
	m->count = 0;
	mmc1Sync(nes);
}

static void mmc1Reset(struct NES* nes, bool hard) {
	struct Mmc1* m = STATE(Mmc1);
	m->control |= 0x0C;
	m->shift = m->count = 0;
	if (hard) {
		m->control = 0x0C;
		m->chr0 = m->chr1 = m->prg = 0;
	}
	mmc1Sync(nes);
}

const struct Board boardMmc1 = {
	.name = "MMC1", .reset = mmc1Reset, .write = mmc1Write, .sync = mmc1Sync, .stateSize = sizeof(struct Mmc1),
};

// MMC1's registers written whole, not serially, with more CHR bank bits: an old 111 board, used for
// a translation with 256 KB of CHR
static void mmc1ParallelWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Mmc1* m = STATE(Mmc1);
	if (address < 0x8000) {
		return;
	}
	uint8_t* regs[4] = { &m->control, &m->chr0, &m->chr1, &m->prg };
	*regs[(address >> 13) & 3] = value;
	mmc1Sync(nes);
}

const struct Board boardMmc1Parallel = {
	.name = "MMC1 (parallel)", .reset = mmc1Reset, .write = mmc1ParallelWrite, .sync = mmc1Sync,
	.stateSize = sizeof(struct Mmc1),
};

