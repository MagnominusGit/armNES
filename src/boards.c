/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <string.h>

// The common boards. Each keeps its registers in nes->boardState and maps from them in sync(), so
// a state load only needs the registers.

#define STATE(type) ((struct type*) nes->boardState)

static void mapWram(struct NES* nes) {
	nesMapWram(nes, 0x6000, 0x2000, 0, true);
}

// Discrete boards write through the ROM: the value is what both drive on the bus
static inline uint8_t busConflict(struct NES* nes, uint16_t address, uint8_t value) {
	uint8_t* page = nes->prgPages[address >> 12];
	return page ? value & page[address & 0xFFF] : value;
}

// NROM (0)
static void nromReset(struct NES* nes, bool hard) {
	nesMapPrg(nes, 0x8000, 0x4000, 0);
	nesMapPrg(nes, 0xC000, 0x4000, 1);
	mapWram(nes);
}

static const struct Board nrom = { .name = "NROM", .reset = nromReset };

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

static const struct Board uxrom = {
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

static const struct Board cnrom = {
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

static const struct Board axrom = {
	.name = "AxROM", .reset = latchReset, .write = axromWrite, .sync = axromSync, .stateSize = sizeof(struct Latch),
};

// MMC1 (1): a serial port into four registers. SUROM and SXROM take the 256 KB PRG half from CHR
// bank bit 4, SOROM and SXROM the work RAM bank from bits 2-3.
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
	if (nes->cart.wramSize > 0x2000) {
		wramBank = (m->chr0 >> 2) & 3;
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

static const struct Board mmc1 = {
	.name = "MMC1", .reset = mmc1Reset, .write = mmc1Write, .sync = mmc1Sync, .stateSize = sizeof(struct Mmc1),
};

// MMC3 (4): eight bank registers, and a counter clocked by A12 rising on the PPU bus.
struct Mmc3 {
	uint8_t select;
	uint8_t banks[8];
	uint8_t mirroring;
	uint8_t wramControl;
	uint8_t latch, counter;
	bool reload, irqEnabled;
	bool a12;
	uint8_t m2Filter;
};

static void mmc3Sync(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	int prgMode = (m->select >> 6) & 1;
	nesMapPrg(nes, prgMode ? 0xC000 : 0x8000, 0x2000, m->banks[6]);
	nesMapPrg(nes, 0xA000, 0x2000, m->banks[7]);
	nesMapPrg(nes, prgMode ? 0x8000 : 0xC000, 0x2000, -2);
	nesMapPrg(nes, 0xE000, 0x2000, -1);

	int chrA12 = (m->select & 0x80) ? 0x1000 : 0;
	nesMapChr(nes, 0x0000 ^ chrA12, 0x800, m->banks[0] >> 1);
	nesMapChr(nes, 0x0800 ^ chrA12, 0x800, m->banks[1] >> 1);
	nesMapChr(nes, 0x1000 ^ chrA12, 0x400, m->banks[2]);
	nesMapChr(nes, 0x1400 ^ chrA12, 0x400, m->banks[3]);
	nesMapChr(nes, 0x1800 ^ chrA12, 0x400, m->banks[4]);
	nesMapChr(nes, 0x1C00 ^ chrA12, 0x400, m->banks[5]);

	if (nes->cart.mirroring != MIRROR_FOUR) {
		nesSetMirroring(nes, (m->mirroring & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	}
	if (m->wramControl & 0x80) {
		nesMapWram(nes, 0x6000, 0x2000, 0, !(m->wramControl & 0x40));
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
	}
}

static void mmc3Write(struct NES* nes, uint16_t address, uint8_t value) {
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

// A12 rising after three M2 rises with it low clocks the counter (after TriCNES)
static void mmc3PpuBus(struct NES* nes, uint16_t address) {
	struct Mmc3* m = STATE(Mmc3);
	bool a12 = address & 0x1000;
	if (!m->a12 && a12 && m->m2Filter == 3) {
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
	if (a12) {
		m->m2Filter = 0;
	}
	m->a12 = a12;
}

static void mmc3M2Rise(struct NES* nes) {
	struct Mmc3* m = STATE(Mmc3);
	if (!(nes->ppu.addressBus & 0x1000) && m->m2Filter < 3) {
		m->m2Filter++;
	}
}

static void mmc3Reset(struct NES* nes, bool hard) {
	struct Mmc3* m = STATE(Mmc3);
	if (hard) {
		static const uint8_t banks[8] = { 0, 2, 4, 5, 6, 7, 0, 1 };
		memcpy(m->banks, banks, sizeof(banks));
		m->wramControl = 0x80;
	}
	nes->watchPpuBus = true;
	mmc3Sync(nes);
}

static const struct Board mmc3 = {
	.name = "MMC3", .reset = mmc3Reset, .write = mmc3Write, .ppuBus = mmc3PpuBus, .m2Rise = mmc3M2Rise,
	.sync = mmc3Sync,
	.stateSize = sizeof(struct Mmc3),
};

const struct Board* boardFind(int mapper, int submapper) {
	switch (mapper) {
	case 0:
		return &nrom;
	case 1:
		return &mmc1;
	case 2:
		return &uxrom;
	case 3:
		return &cnrom;
	case 4:
		return &mmc3;
	case 7:
		return &axrom;
	default:
		return NULL;
	}
}
