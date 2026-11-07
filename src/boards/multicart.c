/* Copyright (c) 2026 Magnominus
 * Ported from FCEUmm's fk23c.c (Copyright (C) 2006 CaH4e3, Copyright (C) 2020) and 268.c
 * (Copyright (C) 2022), under the GNU General Public License, version 2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "mmc3.h"

#include <stdlib.h>

// The big MMC3-based multicart ASICs: FK23C (176) and COOLBOY/MINDKIDS (268). Their IRQ is MMC3's;
// they put outer banks, NROM/UNROM/CNROM modes and extra registers around it.

// FK23C: the MMC3 bank registers (12 in its extended mode) and the outer registers at $5xxx
struct Fk23c {
	struct Mmc3 mmc3;
	uint8_t regs[8], mmc3Regs[12], latch, dipswitch, subType;
	bool afterPower;
};

#define FK STATE(Fk23c)

static bool fkWramExtended(struct NES* nes) {
	return (FK->mmc3.wramControl & 0x20) && FK->subType == 2;
}

static void fkChr(struct NES* nes, uint16_t address, int bank) {
	struct Fk23c* f = FK;
	bool ram = !nes->cart.chrRam && nes->cart.vram && (f->regs[0] & 0x20) && !(f->regs[0] & 0x40);
	if (fkWramExtended(nes) && (f->mmc3.wramControl & 4) && bank < 8) {
		ram = true;
	}
	if (ram) {
		nesMapVram(nes, address, 0x400, bank);
	} else {
		chr1(nes, address, bank);
	}
}

static void fkSyncChr(struct NES* nes) {
	struct Fk23c* f = FK;
	int sub = f->subType;
	uint32_t outer = f->regs[2] | (sub == 3 ? f->regs[6] << 8 : 0);
	if (f->regs[0] & 0x40) {
		bool cnrom = !(f->regs[0] & 0x20) && (sub == 1 || sub == 5);
		uint32_t mask = cnrom ? ((f->regs[0] & 0x10) ? 1 : 3) : 0;
		uint32_t bank = ((sub == 5 ? outer : (outer & ~mask)) | (f->latch & mask)) << 3;
		for (int i = 0; i < 8; i++) {
			fkChr(nes, i * 0x400, bank + i);
		}
		return;
	}
	uint16_t base = (f->mmc3.select & 0x80) ? 0x1000 : 0;
	uint32_t mask = (f->regs[0] & 0x10) ? 0x7F : 0xFF;
	outer = (outer << 3) & ~mask;
	uint8_t* r = f->mmc3Regs;
	if (f->regs[3] & 2) {
		fkChr(nes, base ^ 0x0000, (r[0] & mask) | outer);
		fkChr(nes, base ^ 0x0400, (r[10] & mask) | outer);
		fkChr(nes, base ^ 0x0800, (r[1] & mask) | outer);
		fkChr(nes, base ^ 0x0C00, (r[11] & mask) | outer);
	} else {
		fkChr(nes, base ^ 0x0000, ((r[0] & 0xFE) & mask) | outer);
		fkChr(nes, base ^ 0x0400, ((r[0] | 1) & mask) | outer);
		fkChr(nes, base ^ 0x0800, ((r[1] & 0xFE) & mask) | outer);
		fkChr(nes, base ^ 0x0C00, ((r[1] | 1) & mask) | outer);
	}
	for (int i = 0; i < 4; i++) {
		fkChr(nes, base ^ (0x1000 + i * 0x400), (r[2 + i] & mask) | outer);
	}
}

static void fkSyncPrg(struct NES* nes) {
	struct Fk23c* f = FK;
	int mode = f->regs[0] & 7;
	uint32_t mask = 0x3F >> mode;
	uint32_t base = f->regs[1] & 0x7F;
	switch (f->subType) {
	case 1:
		// All 8 bank bits in mode 0 (behaviour per Mesen)
		if (mode == 0) {
			mask = 0xFF;
		}
		break;
	case 2:
		base |= (f->regs[0] << 4 & 0x080) | (f->regs[0] << 1 & 0x100) | (f->regs[2] << 3 & 0x600) |
			(f->regs[2] << 6 & 0x800);
		break;
	case 3:
		if (mode == 0) {
			mask = 0xFF;
		}
		base |= f->regs[5] << 7;
		break;
	case 4:
		base |= f->regs[2] & 0x80;
		break;
	case 5:
		base = (base & 0x1F) | (f->regs[5] << 5);
		break;
	}
	switch (mode) {
	case 0:
	case 1:
	case 2: {
		uint16_t swap = (f->mmc3.select & 0x40) ? 0x4000 : 0;
		base = (base << 1) & ~mask;
		uint8_t* r = f->mmc3Regs;
		bool extended = f->regs[3] & 2;
		prg8(nes, 0x8000 ^ swap, (r[6] & mask) | base);
		prg8(nes, 0xA000, (r[7] & mask) | base);
		prg8(nes, 0xC000 ^ swap, ((extended ? r[8] : 0xFE) & mask) | base);
		prg8(nes, 0xE000, ((extended ? r[9] : 0xFF) & mask) | base);
		break;
	}
	case 3:
		prg16(nes, 0x8000, base);
		prg16(nes, 0xC000, base);
		break;
	case 4:
		prg32(nes, base >> 1);
		break;
	default:
		prg16(nes, 0x8000, (f->latch & 7) | (base & ~7));
		prg16(nes, 0xC000, 7 | base);
		break;
	}
}

static void fkSync(struct NES* nes) {
	struct Fk23c* f = FK;
	fkSyncPrg(nes);
	fkSyncChr(nes);
	if (f->mmc3.wramControl & 0x80 || fkWramExtended(nes)) {
		if (fkWramExtended(nes)) {
			nesMapWram(nes, 0x4000, 0x2000, (f->mmc3.wramControl & 3) + 1, true);
			nesMapWram(nes, 0x6000, 0x2000, f->mmc3.wramControl & 3, true);
		} else {
			nesMapWram(nes, 0x6000, 0x2000, 0, true);
		}
	}
	static const enum Mirroring mirrors[4] = { MIRROR_VERTICAL, MIRROR_HORIZONTAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B };
	nesSetMirroring(nes, mirrors[f->mmc3.mirroring & (f->subType == 2 ? 3 : 1)]);
}

static void fkWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Fk23c* f = FK;
	if (address >= 0x4800 && address < 0x5000) {
		if (f->subType == 5) {
			f->regs[5] = value;
			fkSyncPrg(nes);
		}
		return;
	}
	if (address >= 0x5000 && address < 0x6000) {
		if (f->afterPower && address > 0x5010 && address != 0x5FF3) {
			f->afterPower = false;
			// Carts that write past $5020 have DIP switches picking which address the registers decode
			if (address >= 0x5020) {
				f->dipswitch = jumperSetting(nes) & 7;
				nes->hasJumper = true;
			}
		}
		bool enabled = (f->mmc3.wramControl & 0x40) || !fkWramExtended(nes);
		if (enabled && (address & (0x10 << f->dipswitch))) {
			f->regs[address & (f->subType == 3 ? 7 : 3)] = value;
			fkSyncPrg(nes);
			fkSyncChr(nes);
		}
		return;
	}
	if (address < 0x8000) {
		return;
	}
	f->latch = value;
	if ((f->regs[0] & 0x40) && !(f->regs[0] & 0x20) && (f->subType == 1 || f->subType == 5)) {
		fkSyncChr(nes);
	}
	if ((f->regs[0] & 7) == 5) {
		fkSyncPrg(nes);
	}
	switch (address & 0xE001) {
	case 0x8000:
		if (address & 2) {
			return;
		}
		if (f->subType == 2 && (value == 0x46 || value == 0x47)) {
			value ^= 1;
		}
		f->mmc3.select = value;
		fkSyncPrg(nes);
		fkSyncChr(nes);
		break;
	case 0x8001: {
		if (address & 2) {
			return;
		}
		int index = f->mmc3.select & ((f->regs[3] & 2) ? 0x0F : 0x07);
		if (index < 12) {
			f->mmc3Regs[index] = value;
			fkSyncPrg(nes);
			fkSyncChr(nes);
		}
		break;
	}
	case 0xA000:
		f->mmc3.mirroring = value;
		fkSync(nes);
		break;
	case 0xA001:
		f->mmc3.wramControl = (value & 0x20) ? value : value & 0xC0;
		fkSync(nes);
		break;
	default:
		// The IRQ registers are MMC3's
		mmc3Write(nes, address, value);
		break;
	}
}

static void fkRegisters(struct NES* nes) {
	struct Fk23c* f = FK;
	memset(f->regs, 0, sizeof(f->regs));
	if (f->subType == 1) {
		f->regs[1] = 0xFF;
	}
	static const uint8_t initial[12] = { 0, 2, 4, 5, 6, 7, 0, 1, 0xFE, 0xFF, 0xFF, 0xFF };
	memcpy(f->mmc3Regs, initial, sizeof(initial));
	f->mmc3.wramControl = 0x80;
	f->mmc3.select = f->mmc3.mirroring = f->mmc3.counter = f->mmc3.latch = 0;
	f->mmc3.irqEnabled = false;
}

static void fkReset(struct NES* nes, bool hard) {
	struct Fk23c* f = FK;
	struct Cart* cart = &nes->cart;
	if (hard) {
		memset(f, 0, sizeof(*f));
		bool nes2 = cart->submapper || cart->vramSize;
		if (cart->mapper == 176 && cart->submapper) {
			f->subType = cart->submapper;
			f->afterPower = f->subType != 2;
		} else if (cart->battery && !nes2) {
			// Waixing boards: 32 KB of battery-backed RAM
			f->subType = 2;
		} else {
			uint32_t prg = cart->prgSize >> 14, chr = cart->chrRam ? 0 : cart->chrSize >> 13;
			f->subType = (prg == 128 && (chr == 256 || chr == 128 || chr == 64 || chr == 0)) || (prg == 64 && chr == 64)
				? 0 : 1;
			f->afterPower = true;
		}
		if (cart->chrRam && cart->chrSize < 0x20000) {
			// iNES files with no CHR ROM get 128 KB of CHR RAM
			cart->chrSize = 0x20000;
			cart->chr = realloc(cart->chr, cart->chrSize);
			memset(cart->chr, 0, cart->chrSize);
		}
	}
	fkRegisters(nes);
	nes->watchPpuBus = true;
	fkSync(nes);
}

const struct Board boardFk23c = {
	.name = "FK23C", .reset = fkReset, .write = fkWrite, .ppuBus = mmc3PpuBus, .busA12 = true, .irqQuietDots = mmc3IrqQuietDots,
	.sync = fkSync, .stateSize = sizeof(struct Fk23c),
};

// COOLBOY/MINDKIDS (268): four outer registers at $6000 (submapper 0) or $5000 (submapper 1) masking
// MMC3's banks, with a GNROM-style 32 KB mode
#define EX(i) STATE(Mmc3)->extra[i]

static void m268Prg(struct NES* nes, uint16_t address, int bank) {
	int sub = nes->cart.submapper & ~1;
	uint8_t* e = STATE(Mmc3)->extra;
	int maskMmc3 = ((e[3] & 0x10) ? 0 : 0x0F) | ((e[0] & 0x40) ? 0 : 0x10) | ((e[1] & 0x80) ? 0 : 0x20) |
		((e[1] & 0x40) ? 0x40 : 0) | ((e[1] & 0x20) ? 0x80 : 0);
	int maskGnrom, offset;
	switch (sub) {
	case 2:
		maskGnrom = (e[3] & 0x10) ? ((e[1] & 0x10) ? 1 : 3) : 0;
		offset = (e[3] & 0x00E) | (e[0] << 4 & 0x070) | (e[1] << 4 & 0x080) | (e[1] << 6 & 0x100) |
			(e[1] << 8 & 0x200) | (e[0] << 6 & 0xC00);
		break;
	case 4:
		maskGnrom = (e[3] & 0x10) ? ((e[1] & 2) ? 3 : 1) : 0;
		offset = (e[3] & 0x00E) | (e[0] << 4 & 0x070) | (e[0] << 3 & 0x180);
		break;
	case 8:
		maskGnrom = (e[3] & 0x10) ? ((e[1] & 2) ? 3 : 1) : 0;
		offset = (e[3] & 0x00E) | (e[0] << 4 & 0x070) | (e[1] << 3 & 0x080) | (e[6] << 8 & 0x300) |
			(e[6] << 6 & 0xC00);
		break;
	default:
		maskGnrom = (e[3] & 0x10) ? ((e[1] & 2) ? 3 : 1) : 0;
		offset = (e[3] & 0x000E) | (e[0] << 4 & 0x0070) | (e[1] << 3 & 0x0080) | (e[1] << 6 & 0x0300) |
			(e[0] << 6 & 0x0C00) | (~e[1] << 12 & 0x1000);
		break;
	}
	offset &= ~(maskMmc3 | maskGnrom);
	prg8(nes, address, (bank & maskMmc3) | offset | ((address >> 13) & maskGnrom));
}

static void m268Chr(struct NES* nes, uint16_t address, int bank) {
	uint8_t* e = STATE(Mmc3)->extra;
	int maskMmc3 = (e[3] & 0x10) ? 0 : (e[0] & 0x80) ? 0x7F : 0xFF;
	int maskGnrom = (e[3] & 0x10) ? 7 : 0;
	int offset = (e[0] << 9 & 0xC00) | (e[0] << 4 & 0x380) | (e[2] << 3 & 0x078);
	offset &= ~(maskMmc3 | maskGnrom);
	int value = (bank & maskMmc3) | offset | ((address >> 10) & maskGnrom);
	if (nes->cart.vram && (e[4] & 1) && (bank & 0xFE) == (e[4] & 0xFE)) {
		nesMapVram(nes, address, 0x400, value);
	} else {
		chr1(nes, address, value);
	}
}

static void m268Mirror(struct NES* nes) {
	uint8_t* e = STATE(Mmc3)->extra;
	if ((nes->cart.submapper & ~1) == 10 && !(e[0] & 0x20)) {
		nesSetMirroring(nes, (e[0] & 0x10) ? MIRROR_SCREEN_B : MIRROR_SCREEN_A);
	} else {
		nesSetMirroring(nes, (STATE(Mmc3)->mirroring & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	}
}

static void m268Wram(struct NES* nes) {
	uint8_t c = STATE(Mmc3)->wramControl;
	bool readable = c & 0xA0, writable = ((c & 0x80) && !(c & 0x40)) || (c & 0x20);
	if (readable || writable) {
		nesMapWram(nes, 0x6000, 0x2000, 0, writable);
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
	}
}

static void m268Write(struct NES* nes, uint16_t address, uint8_t value) {
	uint8_t* e = STATE(Mmc3)->extra;
	bool registers = (nes->cart.submapper & 1) ? (address >= 0x5000 && address < 0x6000)
											   : (address >= 0x6000 && address < 0x8000);
	if (!registers) {
		mmc3Write(nes, address, value);
		return;
	}
	int index = address & 7;
	if ((e[3] & 0x80) && index != 2) {
		return;
	}
	if (index == 2) {
		if (e[2] & 0x80) {
			value = (value & 0x0F) | (e[2] & ~0x0F);
		}
		value &= ((~e[2] >> 3) & 0x0E) | 0xF1;
	}
	if ((nes->cart.submapper & ~1) == 8 && index == 1 && (value & 4) && (value & 8)) {
		e[6] = value;
	}
	if (index <= 5) {
		e[index] = value;
	}
	mmc3Sync(nes);
}

static void m268Reset(struct NES* nes, bool hard) {
	memset(STATE(Mmc3)->extra, 0, 8);
	mmc3Reset(nes, hard);
}

static const struct Mmc3Variant m268 = { m268Prg, m268Chr, m268Mirror, m268Wram };

const struct Board boardCoolboy = {
	.name = "COOLBOY/MINDKIDS", .reset = m268Reset, .write = m268Write, .ppuBus = mmc3PpuBus, .busA12 = true,
	.irqQuietDots = mmc3IrqQuietDots, .sync = mmc3Sync, .stateSize = sizeof(struct Mmc3), .data = &m268,
};

// UNL-8237 (215): MMC3 behind permuted register addresses and indices, with outer banks, an NROM
// mode and a protection read at $5xxx (after FCEUmm's 8237.c, Copyright (C) 2011 CaH4e3)
static const uint8_t regPermutations[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 2, 6, 1, 7, 3, 4, 5 }, { 0, 5, 4, 1, 7, 2, 6, 3 }, { 0, 6, 3, 7, 5, 2, 4, 1 },
	{ 0, 2, 5, 3, 6, 1, 7, 4 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};
static const uint8_t addressPermutations[8][8] = {
	{ 0, 1, 2, 3, 4, 5, 6, 7 }, { 3, 2, 0, 4, 1, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 5, 0, 1, 2, 3, 7, 6, 4 },
	{ 3, 1, 0, 5, 2, 4, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 }, { 0, 1, 2, 3, 4, 5, 6, 7 },
};
static const uint8_t protection[8][8] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 3, 0 }, { 0, 0, 0, 0, 3, 4, 0, 0 }, { 0, 0, 0, 1, 0, 4, 5, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 0, 0, 0, 0, 0 }, { 0, 0, 0, 1, 2, 4, 15, 0 },
};

static void m215Chr(struct NES* nes, uint16_t address, int bank) {
	uint8_t* e = STATE(Mmc3)->extra;
	int outer = nes->cart.submapper == 1 ? (e[1] & 0x0E) << 7 : (e[1] & 0x0C) << 6;
	chr1(nes, address, (e[0] & 0x40) ? outer | (bank & 0x7F) | ((e[1] & 0x20) << 2) : outer | bank);
}

static void m215Prg(struct NES* nes, uint16_t address, int bank) {
	uint8_t* e = STATE(Mmc3)->extra;
	int outer = (e[1] & 3) << 5;
	if (nes->cart.submapper == 1) {
		outer |= (e[1] & 8) << 4;
	}
	bool small = e[0] & 0x40;
	int inner = small ? (bank & 0x0F) | (e[1] & 0x10) : bank & 0x1F;
	if (e[0] & 0x80) {
		int nrom = (outer >> 1) | (small ? (e[0] & 7) | ((e[1] & 0x10) >> 1) : e[0] & 0x0F);
		if (e[0] & 0x20) {
			prg32(nes, nrom >> 1);
		} else {
			prg16(nes, 0x8000, nrom);
			prg16(nes, 0xC000, nrom);
		}
	} else {
		prg8(nes, address, outer | inner);
	}
}

static uint8_t m215Read(struct NES* nes, uint16_t address) {
	return (protection[STATE(Mmc3)->extra[3] & 7][address & 7] & 0x0F) | 0x50;
}

static void m215Write(struct NES* nes, uint16_t address, uint8_t value) {
	uint8_t* e = STATE(Mmc3)->extra;
	if (address >= 0x5000 && address < 0x6000) {
		switch (address & 0xF007) {
		case 0x5000:
			e[0] = value;
			mmc3Sync(nes);
			break;
		case 0x5001:
			e[1] = value;
			mmc3Sync(nes);
			break;
		case 0x5002:
			e[3] = value;
			break;
		case 0x5007:
			e[2] = value;
			break;
		}
		return;
	}
	if (address < 0x8000) {
		return;
	}
	uint8_t reg = addressPermutations[e[2] & 7][((address >> 12) & 6) | (address & 1)];
	if (reg == 0) {
		value = (value & 0xC0) | regPermutations[e[2] & 7][value & 7];
	}
	mmc3Write(nes, 0x8000 | ((reg & 6) << 12) | (reg & 1), value);
}

static void m215Reset(struct NES* nes, bool hard) {
	uint8_t* e = STATE(Mmc3)->extra;
	e[0] = e[2] = 0;
	e[1] = 0x0F;
	e[3] = 7;
	nes->readHooks |= 1 << 5;
	mmc3Reset(nes, hard);
}

static const struct Mmc3Variant m215 = { m215Prg, m215Chr, NULL, NULL };

const struct Board board8237 = {
	.name = "UNL-8237", .reset = m215Reset, .write = m215Write, .read = m215Read, .ppuBus = mmc3PpuBus, .busA12 = true,
	.irqQuietDots = mmc3IrqQuietDots, .sync = mmc3Sync, .stateSize = sizeof(struct Mmc3), .data = &m215,
};
