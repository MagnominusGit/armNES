/* Copyright (c) 2026 Magnominus
 * Ported from FCEUmm's sachen.c (Copyright (C) 2002 Xodnizel, Copyright (C) 2020), under the GNU
 * General Public License, version 2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

// Sachen's boards: the 8259 family (137-139, 141), single-latch boards at $4100 or $8000 (133,
// 145, 148, 149), TCA01 (143) and the 74LS374N family (150, 243).

struct Sachen {
	uint8_t command, latch[8], dip;
};

#define SA STATE(Sachen)

static void s8259Sync(struct NES* nes) {
	struct Sachen* s = SA;
	int mapper = nes->cart.mapper;
	int type = mapper == 141 ? 0 : mapper == 138 ? 1 : mapper == 139 ? 2 : 3;
	uint8_t* l = s->latch;
	prg32(nes, l[5] & 7);
	if (!nes->cart.chrRam) {
		for (int x = 0; x < 4; x++) {
			int bank = ((l[7] & 1) ? l[0] & 7 : l[x] & 7) | ((l[4] & 7) << 3);
			switch (type) {
			case 0:
				chr2(nes, 0x800 * x, (bank << 1) | (x & 1));
				break;
			case 1:
				chr2(nes, 0x800 * x, bank);
				break;
			case 2:
				chr2(nes, 0x800 * x, (bank << 2) | (x & 3));
				break;
			default:
				bank = l[x] & 7;
				if (x == 1) {
					bank |= (l[4] & 1) << 4;
				} else if (x == 2) {
					bank |= (l[4] & 2) << 3;
				} else if (x == 3) {
					bank |= ((l[4] & 4) << 2) | ((l[6] & 1) << 3);
				}
				chr1(nes, 0x400 * x, bank);
				chr4(nes, 0x1000, -1);
				break;
			}
		}
	}
	if (l[7] & 1) {
		nesSetMirroring(nes, MIRROR_VERTICAL);
		return;
	}
	switch ((l[7] >> 1) & 3) {
	case 0:
		nesSetMirroring(nes, mapper == 137 ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
		break;
	case 1:
		nesSetMirroring(nes, mapper == 137 ? MIRROR_VERTICAL : MIRROR_HORIZONTAL);
		break;
	case 2:
		mirrorPages(nes, 0, 1, 1, 1);
		break;
	case 3:
		nesSetMirroring(nes, MIRROR_SCREEN_A);
		break;
	}
}

static void s8259Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Sachen* s = SA;
	if (address < 0x4100 || address >= 0x8000) {
		return;
	}
	if ((address & 0x4101) == 0x4100) {
		s->command = value & 7;
	} else {
		s->latch[s->command] = value;
		s8259Sync(nes);
	}
}

static void s8259Reset(struct NES* nes, bool hard) {
	memset(SA, 0, sizeof(struct Sachen));
	chr8(nes, 0);
	s8259Sync(nes);
}

const struct Board boardS8259 = {
	.name = "Sachen 8259", .reset = s8259Reset, .write = s8259Write, .sync = s8259Sync,
	.stateSize = sizeof(struct Sachen),
};

// One latch: SA0161M/SA0037 PRG bit 3, CHR bits 0-2; SA72007/SA0036 CHR bit 7; SA72008 PRG bit 2,
// CHR bits 0-1
static void sachenLatchSync(struct NES* nes) {
	uint8_t v = SA->latch[0];
	switch (nes->cart.mapper) {
	case 133:
		prg32(nes, (v >> 2) & 1);
		chr8(nes, v & 3);
		break;
	case 145:
	case 149:
		prg32(nes, 0);
		chr8(nes, v >> 7);
		break;
	default:
		prg32(nes, (v >> 3) & 1);
		chr8(nes, v & 7);
		break;
	}
}

static void sachenLatchWrite(struct NES* nes, uint16_t address, uint8_t value) {
	int mapper = nes->cart.mapper;
	bool atRom = mapper == 148 || mapper == 149;
	if (atRom ? address >= 0x8000 : (address >= 0x4100 && address < 0x6000 && (address & 0x100))) {
		SA->latch[0] = value;
		sachenLatchSync(nes);
	}
}

static void sachenLatchReset(struct NES* nes, bool hard) {
	SA->latch[0] = 0;
	sachenLatchSync(nes);
}

const struct Board boardSachenLatch = {
	.name = "Sachen latch", .reset = sachenLatchReset, .write = sachenLatchWrite, .sync = sachenLatchSync,
	.stateSize = sizeof(struct Sachen),
};

// TCA01 (143): reads of $4100-$5FFF return the inverted address bits as protection
static uint8_t tca01Read(struct NES* nes, uint16_t address) {
	if ((address & 0x4100) == 0x4100) {
		return (nes->cpu.openBus & 0xC0) | (~address & 0x3F);
	}
	return nes->cpu.openBus;
}

static void tca01Reset(struct NES* nes, bool hard) {
	prg16(nes, 0x8000, 0);
	prg16(nes, 0xC000, 1);
	chr8(nes, 0);
	nes->readHooks |= 1 << 4 | 1 << 5;
}

const struct Board boardTca01 = { .name = "TCA01", .reset = tca01Reset, .read = tca01Read };

// 74LS374N (150) and SA-020A (243): eight 3-bit latches, and a DIP switch that changes on reset
static void s374Sync(struct NES* nes) {
	uint8_t* l = SA->latch;
	uint32_t chr = nes->cart.mapper == 150 ? (l[6] & 3) | ((l[4] << 2) & 4) | (l[2] << 3)
										   : (l[2] & 1) | ((l[4] << 1) & 2) | (l[6] << 2);
	prg32(nes, (l[2] & 1) | l[5]);
	chr8(nes, chr);
	switch ((l[7] >> 1) & 3) {
	case 0:
		mirrorPages(nes, 0, 1, 1, 1);
		break;
	case 1:
		nesSetMirroring(nes, MIRROR_HORIZONTAL);
		break;
	case 2:
		nesSetMirroring(nes, MIRROR_VERTICAL);
		break;
	case 3:
		nesSetMirroring(nes, MIRROR_SCREEN_A);
		break;
	}
}

static uint8_t s374Read(struct NES* nes, uint16_t address) {
	struct Sachen* s = SA;
	if ((address & 0xC101) == 0x4101) {
		uint8_t mask = (s->dip & 1) ? 3 : 7;
		return (s->latch[s->command] & mask) | (nes->cpu.openBus & ~mask);
	}
	return nes->cpu.openBus;
}

static void s374Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Sachen* s = SA;
	if (address < 0x4100 || address >= 0x8000) {
		return;
	}
	if (s->dip & 1) {
		value |= 4;
	}
	switch (address & 0xC101) {
	case 0x4100:
		s->command = value & 7;
		break;
	case 0x4101:
		s->latch[s->command] = value & 7;
		s374Sync(nes);
		break;
	}
}

static void s374Reset(struct NES* nes, bool hard) {
	struct Sachen* s = SA;
	uint8_t dip = hard ? 0 : s->dip ^ 1;
	memset(s, 0, sizeof(*s));
	s->dip = dip;
	if (nes->cart.mapper == 150) {
		nes->readHooks |= 1 << 4 | 1 << 5;
	}
	s374Sync(nes);
}

const struct Board boardS74ls374n = {
	.name = "Sachen 74LS374N", .reset = s374Reset, .write = s374Write, .read = s374Read, .sync = s374Sync,
	.stateSize = sizeof(struct Sachen),
};
