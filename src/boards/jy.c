/* Copyright (c) 2026 Magnominus
 * Ported from FCEUmm's jyasic.c (Copyright (C) 2002 Xodnizel, Copyright (C) 2005 CaH4e3,
 * Copyright (C) 2019 Libretro Team), under the GNU General Public License, version 2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

// J.Y. Company's ASIC (35, 90, 209, 211): four PRG and four CHR modes, name tables from CHR ROM, an
// MMC2-style CHR latch, a multiplier, and an IRQ counter clocked by CPU cycles or PPU A12 rises.

struct Jy {
	uint8_t mode[4], prg[4];
	uint16_t chr[8], nt[4];
	uint8_t latch[2], mul[2], adder, test, dip;
	uint8_t irqControl, irqPrescaler, irqCounter, irqXor;
	bool irqEnabled, a12;
};

#define JY STATE(Jy)

static bool jyExtendedMirroring(const struct NES* nes) {
	return nes->cart.mapper != 90;
}

static uint8_t reverse7(uint8_t v) {
	return ((v << 6) & 0x40) | ((v << 4) & 0x20) | ((v << 2) & 0x10) | (v & 0x08) | ((v >> 2) & 0x04) |
		((v >> 4) & 0x02) | ((v >> 6) & 0x01);
}

static void jySyncPrg(struct NES* nes, int and, int or) {
	struct Jy* j = JY;
	uint8_t last = (j->mode[0] & 4) ? j->prg[3] : 0xFF;
	uint8_t bank6000 = 0;
	switch (j->mode[0] & 3) {
	case 0:
		prg32(nes, ((last & and) >> 2) | (or >> 2));
		bank6000 = (j->prg[3] << 2) | 3;
		break;
	case 1:
		prg16(nes, 0x8000, ((j->prg[1] & and) >> 1) | (or >> 1));
		prg16(nes, 0xC000, ((last & and) >> 1) | (or >> 1));
		bank6000 = (j->prg[3] << 1) | 1;
		break;
	case 2:
		for (int i = 0; i < 3; i++) {
			prg8(nes, 0x8000 + i * 0x2000, (j->prg[i] & and) | or);
		}
		prg8(nes, 0xE000, (last & and) | or);
		bank6000 = j->prg[3];
		break;
	case 3:
		for (int i = 0; i < 3; i++) {
			prg8(nes, 0x8000 + i * 0x2000, (reverse7(j->prg[i]) & and) | or);
		}
		prg8(nes, 0xE000, (reverse7(last) & and) | or);
		bank6000 = reverse7(j->prg[3]);
		break;
	}
	if (j->mode[0] & 0x80) {
		prg8(nes, 0x6000, (bank6000 & and) | or);
	} else if (nes->cart.mapper == 35) {
		wram8(nes);
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
	}
}

static void jySyncChr(struct NES* nes, int and, int or) {
	struct Jy* j = JY;
	if ((j->mode[3] & 0x80) && (j->mode[0] & 0x18) == 0x08) {
		for (int b = 0; b < 8; b += 4) {
			chr4(nes, 0x400 * b, ((j->chr[(j->latch[b / 4] & 2) | b] & and) >> 2) | (or >> 2));
		}
		return;
	}
	switch (j->mode[0] & 0x18) {
	case 0x00:
		chr8(nes, ((j->chr[0] & and) >> 3) | (or >> 3));
		break;
	case 0x08:
		for (int b = 0; b < 8; b += 4) {
			chr4(nes, 0x400 * b, ((j->chr[b] & and) >> 2) | (or >> 2));
		}
		break;
	case 0x10:
		for (int b = 0; b < 8; b += 2) {
			chr2(nes, 0x400 * b, ((j->chr[b] & and) >> 1) | (or >> 1));
		}
		break;
	case 0x18:
		for (int b = 0; b < 8; b++) {
			chr1(nes, 0x400 * b, (j->chr[b] & and) | or);
		}
		break;
	}
}

static void jySyncNt(struct NES* nes, int and, int or) {
	struct Jy* j = JY;
	if ((j->mode[0] & 0x20) || (j->mode[1] & 0x08)) {
		mirrorPages(nes, j->nt[0] & 1, j->nt[1] & 1, j->nt[2] & 1, j->nt[3] & 1);
		if (j->mode[0] & 0x20) {
			for (int i = 0; i < 4; i++) {
				bool fromRom = ((j->nt[i] & 0x80) ^ (j->mode[2] & 0x80)) | (j->mode[0] & 0x40);
				if (fromRom) {
					nesMapChr(nes, 0x2000 + i * 0x400, 0x400, (j->nt[i] & and) | or);
					nes->chrPages[12 + i] = nes->chrPages[8 + i];
					nes->chrWritable[8 + i] = nes->chrWritable[12 + i] = false;
				}
			}
		}
		return;
	}
	static const enum Mirroring mirrors[4] = { MIRROR_VERTICAL, MIRROR_HORIZONTAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B };
	nesSetMirroring(nes, mirrors[j->mode[1] & 3]);
}

static void jySync(struct NES* nes) {
	struct Jy* j = JY;
	jySyncPrg(nes, 0x3F, (j->mode[3] << 5) & ~0x3F);
	if (j->mode[3] & 0x20) {
		jySyncChr(nes, 0x1FF, (j->mode[3] << 6) & 0x600);
		jySyncNt(nes, 0x1FF, (j->mode[3] << 6) & 0x600);
	} else {
		int or = ((j->mode[3] << 8) & 0x100) | ((j->mode[3] << 6) & 0x600);
		jySyncChr(nes, 0x0FF, or);
		jySyncNt(nes, 0x0FF, or);
	}
}

static void jyClock(struct NES* nes) {
	struct Jy* j = JY;
	if (!j->irqEnabled) {
		return;
	}
	uint8_t mask = (j->irqControl & 4) ? 0x07 : 0xFF;
	switch (j->irqControl & 0xC0) {
	case 0x40: {
		uint8_t next = (j->irqPrescaler + 1) & mask;
		j->irqPrescaler = (j->irqPrescaler & ~mask) | next;
		if (next == 0 && ((j->irqControl & 8) ? j->irqCounter : ++j->irqCounter) == 0) {
			nesSetIrq(nes, IRQ_MAPPER, true);
		}
		break;
	}
	case 0x80: {
		uint8_t next = (j->irqPrescaler - 1) & mask;
		j->irqPrescaler = (j->irqPrescaler & ~mask) | next;
		if (next == mask && ((j->irqControl & 8) ? j->irqCounter : --j->irqCounter) == 0xFF) {
			nesSetIrq(nes, IRQ_MAPPER, true);
		}
		break;
	}
	}
}

static void jyCycle(struct NES* nes) {
	if ((JY->irqControl & 3) == 0) {
		jyClock(nes);
	}
}

static void jyPpuBus(struct NES* nes, uint16_t address) {
	struct Jy* j = JY;
	bool a12 = address & 0x1000;
	if (a12 && !j->a12 && (j->irqControl & 3) == 1) {
		jyClock(nes);
	}
	j->a12 = a12;
}

static uint8_t jyPpuRead(struct NES* nes, uint16_t address) {
	struct Jy* j = JY;
	uint8_t* page = nes->chrPages[(address >> 10) & 15];
	uint8_t value = page ? page[address & 0x3FF] : 0;
	if ((j->mode[3] & 0x80) && (j->mode[0] & 0x18) == 0x08 &&
		((address & 0x2FF0) == 0x0FD0 || (address & 0x2FF0) == 0x0FE0)) {
		j->latch[(address >> 12) & 1] = ((address >> 10) & 4) | ((address >> 4) & 2);
		jySync(nes);
	}
	return value;
}

static uint8_t jyRead(struct NES* nes, uint16_t address) {
	struct Jy* j = JY;
	if (address < 0x5000 || address >= 0x6000) {
		return nes->cpu.openBus;
	}
	if ((address & 0x3FF) == 0 && address != 0x5800) {
		return j->dip | (nes->cpu.openBus & 0x3F);
	}
	if (address & 0x800) {
		switch (address & 3) {
		case 0:
			return (j->mul[0] * j->mul[1]) & 0xFF;
		case 1:
			return (j->mul[0] * j->mul[1]) >> 8;
		case 2:
			return j->adder;
		default:
			return j->test;
		}
	}
	return nes->cpu.openBus;
}

static void jyWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Jy* j = JY;
	if (address >= 0x5000 && address < 0x6000) {
		switch (address & 3) {
		case 0:
			j->mul[0] = value;
			break;
		case 1:
			j->mul[1] = value;
			break;
		case 2:
			j->adder += value;
			break;
		case 3:
			j->test = value;
			j->adder = 0;
			break;
		}
		return;
	}
	if (address < 0x8000 || (address & 0x0800 && (address & 0xF000) != 0xC000)) {
		return;
	}
	switch (address & 0xF000) {
	case 0x8000:
		j->prg[address & 3] = value;
		break;
	case 0x9000:
		j->chr[address & 7] = (j->chr[address & 7] & 0xFF00) | value;
		break;
	case 0xA000:
		j->chr[address & 7] = (j->chr[address & 7] & 0x00FF) | (value << 8);
		break;
	case 0xB000:
		if (address & 4) {
			j->nt[address & 3] = (j->nt[address & 3] & 0x00FF) | (value << 8);
		} else {
			j->nt[address & 3] = (j->nt[address & 3] & 0xFF00) | value;
		}
		break;
	case 0xC000:
		switch (address & 7) {
		case 0:
			j->irqEnabled = value & 1;
			if (!j->irqEnabled) {
				j->irqPrescaler = 0;
				nesSetIrq(nes, IRQ_MAPPER, false);
			}
			break;
		case 1:
			j->irqControl = value;
			break;
		case 2:
			j->irqEnabled = false;
			j->irqPrescaler = 0;
			nesSetIrq(nes, IRQ_MAPPER, false);
			break;
		case 3:
			j->irqEnabled = true;
			break;
		case 4:
			j->irqPrescaler = value ^ j->irqXor;
			break;
		case 5:
			j->irqCounter = value ^ j->irqXor;
			break;
		case 6:
			j->irqXor = value;
			break;
		}
		return;
	case 0xD000:
		j->mode[address & 3] = value;
		if (!jyExtendedMirroring(nes)) {
			j->mode[0] &= ~0x20;
			j->mode[1] &= ~0x08;
		}
		break;
	default:
		return;
	}
	jySync(nes);
}

static void jyReset(struct NES* nes, bool hard) {
	struct Jy* j = JY;
	uint8_t dip = hard ? 0 : (j->dip + 0x40) & 0xC0;
	memset(j, 0, sizeof(*j));
	j->dip = dip;
	nes->readHooks |= 1 << 5;
	nes->watchPpuBus = true;
	nes->ppuReadHook = true;
	jySync(nes);
}

const struct Board boardJy = {
	.name = "J.Y. Company", .reset = jyReset, .write = jyWrite, .read = jyRead, .ppuBus = jyPpuBus,
	.ppuRead = jyPpuRead, .cpuCycle = jyCycle, .sync = jySync, .stateSize = sizeof(struct Jy),
};
