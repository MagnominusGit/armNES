/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

#include <stdlib.h>

// Discrete boards and simple multicarts: a latch or two set by writes (the value, or the address
// written to), and banks mapped from them. Each is a write window and a sync function.

struct Discrete {
	uint8_t value, value2;
	uint16_t address;
	uint8_t outer;
};

enum {
	LATCH_ADDRESS = 1, // the address written to is what counts
	BUS_CONFLICT = 2,
	CHR_RAM_16K = 4,
	WRAM = 8,
};

struct DiscreteBoard {
	int mapper;
	uint16_t from, to;
	int flags;
	void (*sync)(struct NES*, struct Discrete*);
	const char* name;
};

static void mirrorHV(struct NES* nes, bool horizontal) {
	nesSetMirroring(nes, horizontal ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
}

static void screen(struct NES* nes, int page) {
	nesSetMirroring(nes, page ? MIRROR_SCREEN_B : MIRROR_SCREEN_A);
}

// Color Dreams (11)
static void syncColorDreams(struct NES* nes, struct Discrete* d) {
	prg32(nes, d->value & 3);
	chr8(nes, d->value >> 4);
}

// GxROM (66)
static void syncGxrom(struct NES* nes, struct Discrete* d) {
	prg32(nes, (d->value >> 4) & 3);
	chr8(nes, d->value & 3);
}

// Camerica BF909x (71): 16 KB at $8000; one version picks one screen at $9000
static void syncCamerica(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, d->value);
	prg16(nes, 0xC000, -1);
	if (d->outer & 0x80) {
		screen(nes, (d->outer >> 4) & 1);
	}
}

// NINA-03/06 (79, 146)
static void syncNina(struct NES* nes, struct Discrete* d) {
	prg32(nes, (d->value >> 3) & 1);
	chr8(nes, d->value & 7);
}

// Sachen/Hacker (113)
static void sync113(struct NES* nes, struct Discrete* d) {
	prg32(nes, (d->value >> 3) & 7);
	chr8(nes, (d->value & 7) | ((d->value >> 3) & 8));
	mirrorHV(nes, !(d->value & 0x80));
}

// Jaleco JF-05..07 (87): the two CHR bits swapped
static void sync87(struct NES* nes, struct Discrete* d) {
	chr8(nes, ((d->value & 1) << 1) | ((d->value >> 1) & 1));
}

// Jaleco JF-11/14 (140)
static void sync140(struct NES* nes, struct Discrete* d) {
	prg32(nes, (d->value >> 4) & 3);
	chr8(nes, d->value & 15);
}

// Bit Corp (38)
static void sync38(struct NES* nes, struct Discrete* d) {
	prg32(nes, d->value & 3);
	chr8(nes, (d->value >> 2) & 3);
}

// Bandai (70) and (152, with one screen)
static void sync70(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, d->value >> 4);
	prg16(nes, 0xC000, -1);
	chr8(nes, d->value & 15);
}

static void sync152(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, (d->value >> 4) & 7);
	prg16(nes, 0xC000, -1);
	chr8(nes, d->value & 15);
	screen(nes, d->value >> 7);
}

// Irem 74HC161/32 (78): one screen, or H/V on submapper 3
static void sync78(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, d->value & 7);
	prg16(nes, 0xC000, -1);
	chr8(nes, d->value >> 4);
	if (nes->cart.submapper == 3) {
		mirrorHV(nes, !(d->value & 8));
	} else {
		screen(nes, (d->value >> 3) & 1);
	}
}

// Sunsoft-2 (89, 93)
static void sync89(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, (d->value >> 4) & 7);
	prg16(nes, 0xC000, -1);
	chr8(nes, (d->value & 7) | ((d->value >> 4) & 8));
	screen(nes, (d->value >> 3) & 1);
}

static void sync93(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, (d->value >> 4) & 7);
	prg16(nes, 0xC000, -1);
}

// UN1ROM (94)
static void sync94(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, (d->value >> 2) & 7);
	prg16(nes, 0xC000, -1);
}

// UNROM with the first bank fixed (180)
static void sync180(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, 0);
	prg16(nes, 0xC000, d->value & 7);
}

// Sunsoft-1 (184)
static void sync184(struct NES* nes, struct Discrete* d) {
	prg32(nes, 0);
	chr4(nes, 0x0000, d->value & 7);
	chr4(nes, 0x1000, ((d->value >> 4) & 7) | 4);
}

// CNROM with a CHR enable for copy protection (185)
static uint8_t disabledChr[0x400];

static void sync185(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, 0);
	prg16(nes, 0xC000, -1);
	bool enabled;
	int sub = nes->cart.submapper;
	if (sub >= 4) {
		enabled = (d->value & 3) == sub - 4;
	} else {
		enabled = (d->value & 0x0F) && d->value != 0x13;
	}
	if (enabled) {
		chr8(nes, 0);
	} else {
		memset(disabledChr, 0xFF, sizeof(disabledChr));
		for (int i = 0; i < 8; i++) {
			nes->chrPages[i] = disabledChr;
			nes->chrWritable[i] = false;
		}
	}
}

// BNROM (34) and NINA-001 (34 with CHR ROM over 8 KB)
static void sync34(struct NES* nes, struct Discrete* d) {
	if (nes->cart.submapper == 1 || (nes->cart.submapper == 0 && nes->cart.chrSize > 0x2000)) {
		prg32(nes, d->outer & 1);
		chr4(nes, 0x0000, d->value & 15);
		chr4(nes, 0x1000, d->value2 & 15);
	} else {
		prg32(nes, d->value);
	}
}

// CPROM (13): 16 KB of CHR RAM, the upper 4 KB switched
static void sync13(struct NES* nes, struct Discrete* d) {
	prg32(nes, 0);
	chr4(nes, 0x0000, 0);
	chr4(nes, 0x1000, d->value & 3);
}

// Caltron (41): the outer register is the address written in $6000-$67FF
static void sync41(struct NES* nes, struct Discrete* d) {
	prg32(nes, d->address & 7);
	chr8(nes, ((d->address >> 1) & 0x0C) | (d->value & 3));
	mirrorHV(nes, d->address & 0x20);
}

// Color Dreams multicart (46)
static void sync46(struct NES* nes, struct Discrete* d) {
	prg32(nes, ((d->outer & 15) << 1) | (d->value & 1));
	chr8(nes, ((d->outer >> 4) << 3) | ((d->value >> 4) & 7));
}

// Address-latch multicarts
static void sync58(struct NES* nes, struct Discrete* d) {
	uint16_t a = d->address;
	if (a & 0x40) {
		prg16(nes, 0x8000, a & 7);
		prg16(nes, 0xC000, a & 7);
	} else {
		prg32(nes, (a & 6) >> 1);
	}
	chr8(nes, (a >> 3) & 7);
	mirrorHV(nes, a & 0x80);
}

static void sync61(struct NES* nes, struct Discrete* d) {
	uint16_t a = d->address;
	if (a & 0x10) {
		int bank = ((a & 0x0F) << 1) | ((a >> 5) & 1);
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, bank);
	} else {
		prg32(nes, a & 0x0F);
	}
	mirrorHV(nes, a & 0x80);
}

static void sync62(struct NES* nes, struct Discrete* d) {
	uint16_t a = d->address;
	int bank = ((a & 0x3F00) >> 8) | (a & 0x40);
	if (a & 0x20) {
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, bank);
	} else {
		prg32(nes, bank >> 1);
	}
	chr8(nes, ((a & 0x1F) << 2) | (d->value & 3));
	mirrorHV(nes, a & 0x80);
}

static void sync200(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, d->address & 7);
	prg16(nes, 0xC000, d->address & 7);
	chr8(nes, d->address & 7);
	mirrorHV(nes, d->address & 8);
}

static void sync201(struct NES* nes, struct Discrete* d) {
	prg32(nes, d->address & 0xFF);
	chr8(nes, d->address & 0xFF);
}

static void sync202(struct NES* nes, struct Discrete* d) {
	int bank = (d->address >> 1) & 7;
	if ((d->address & 9) == 9) {
		prg32(nes, bank >> 1);
	} else {
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, bank);
	}
	chr8(nes, bank);
	mirrorHV(nes, d->address & 1);
}

static void sync203(struct NES* nes, struct Discrete* d) {
	prg16(nes, 0x8000, d->value >> 2);
	prg16(nes, 0xC000, d->value >> 2);
	chr8(nes, d->value & 3);
}

static void sync225(struct NES* nes, struct Discrete* d) {
	uint16_t a = d->address;
	int high = (a >> 14) & 1;
	int bank = ((a >> 7) & 0x1F) | (high << 5);
	if (a & 0x1000) {
		int half = (bank << 1) | ((a >> 6) & 1);
		prg16(nes, 0x8000, half);
		prg16(nes, 0xC000, half);
	} else {
		prg32(nes, bank);
	}
	chr8(nes, (a & 0x3F) | (high << 6));
	mirrorHV(nes, a & 0x2000);
}

static void sync226(struct NES* nes, struct Discrete* d) {
	uint8_t r0 = d->value, r1 = d->value2;
	int bank = (r0 & 0x1F) | ((r0 & 0x80) >> 2) | ((r1 & 1) << 6);
	if (r0 & 0x20) {
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, bank);
	} else {
		prg32(nes, bank >> 1);
	}
	mirrorHV(nes, !(r0 & 0x40));
}

static void sync227(struct NES* nes, struct Discrete* d) {
	uint16_t a = d->address;
	int bank = ((a >> 2) & 0x1F) | ((a & 0x100) >> 3);
	if (a & 0x80) {
		if (a & 1) {
			prg32(nes, bank >> 1);
		} else {
			prg16(nes, 0x8000, bank);
			prg16(nes, 0xC000, bank);
		}
	} else {
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, (a & 0x200) ? bank | 7 : bank & 0x38);
	}
	mirrorHV(nes, a & 2);
}

static void sync229(struct NES* nes, struct Discrete* d) {
	uint16_t a = d->address;
	if (a & 0x1E) {
		prg16(nes, 0x8000, a & 0x1F);
		prg16(nes, 0xC000, a & 0x1F);
	} else {
		prg32(nes, 0);
	}
	chr8(nes, a & 0x1F);
	mirrorHV(nes, a & 0x20);
}

static void sync231(struct NES* nes, struct Discrete* d) {
	uint16_t a = d->address;
	prg16(nes, 0x8000, a & 0x1E);
	prg16(nes, 0xC000, (a & 0x1E) | ((a >> 5) & 1));
	mirrorHV(nes, a & 0x80);
}

static void sync242(struct NES* nes, struct Discrete* d) {
	prg32(nes, (d->address >> 3) & 0x0F);
	mirrorHV(nes, d->address & 2);
}

// Camerica BF9096 (232): the outer 64 KB from $8000-$BFFF, the inner 16 KB from $C000
static void sync232(struct NES* nes, struct Discrete* d) {
	int outer = (d->outer >> 3) & 3;
	prg16(nes, 0x8000, (outer << 2) | (d->value & 3));
	prg16(nes, 0xC000, (outer << 2) | 3);
}

// 100-in-1 (15)
static void sync15(struct NES* nes, struct Discrete* d) {
	int bank = d->value & 0x3F, sub = d->value >> 7;
	switch (d->address & 3) {
	case 0:
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, bank | 1);
		break;
	case 1:
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, bank | 7);
		break;
	case 2:
		for (int i = 0; i < 4; i++) {
			prg8(nes, 0x8000 + i * 0x2000, (bank << 1) | sub);
		}
		break;
	case 3:
		prg16(nes, 0x8000, bank);
		prg16(nes, 0xC000, bank);
		break;
	}
	mirrorHV(nes, d->value & 0x40);
}

static void sync107(struct NES* nes, struct Discrete* d) {
	prg32(nes, d->value >> 1);
	chr8(nes, d->value);
}

static void sync240(struct NES* nes, struct Discrete* d) {
	prg32(nes, d->value >> 4);
	chr8(nes, d->value & 15);
}

static void sync241(struct NES* nes, struct Discrete* d) {
	prg32(nes, d->value);
}

static const struct DiscreteBoard boards[] = {
	{ 11, 0x8000, 0xFFFF, BUS_CONFLICT, syncColorDreams, "Color Dreams" },
	{ 13, 0x8000, 0xFFFF, BUS_CONFLICT | CHR_RAM_16K, sync13, "CPROM" },
	{ 15, 0x8000, 0xFFFF, LATCH_ADDRESS, sync15, "K-1029" },
	{ 34, 0x8000, 0xFFFF, BUS_CONFLICT | WRAM, sync34, "BNROM/NINA-001" },
	{ 38, 0x7000, 0x7FFF, 0, sync38, "Bit Corp" },
	{ 41, 0x8000, 0xFFFF, 0, sync41, "Caltron" },
	{ 46, 0x8000, 0xFFFF, 0, sync46, "Color Dreams multicart" },
	{ 58, 0x8000, 0xFFFF, LATCH_ADDRESS, sync58, "58" },
	{ 61, 0x8000, 0xFFFF, LATCH_ADDRESS, sync61, "61" },
	{ 62, 0x8000, 0xFFFF, LATCH_ADDRESS, sync62, "62" },
	{ 66, 0x8000, 0xFFFF, BUS_CONFLICT, syncGxrom, "GxROM" },
	{ 70, 0x8000, 0xFFFF, BUS_CONFLICT, sync70, "Bandai 74161" },
	{ 71, 0xC000, 0xFFFF, 0, syncCamerica, "Camerica" },
	{ 78, 0x8000, 0xFFFF, BUS_CONFLICT, sync78, "Irem 74161" },
	{ 79, 0x4100, 0x5FFF, 0, syncNina, "NINA-03/06" },
	{ 87, 0x6000, 0x7FFF, 0, sync87, "Jaleco JF-05" },
	{ 89, 0x8000, 0xFFFF, BUS_CONFLICT, sync89, "Sunsoft-2" },
	{ 93, 0x8000, 0xFFFF, BUS_CONFLICT, sync93, "Sunsoft-2 (CHR RAM)" },
	{ 94, 0x8000, 0xFFFF, BUS_CONFLICT, sync94, "UN1ROM" },
	{ 107, 0x8000, 0xFFFF, 0, sync107, "Mapper 107" },
	{ 113, 0x4100, 0x5FFF, 0, sync113, "Sachen/Hacker" },
	{ 140, 0x6000, 0x7FFF, 0, sync140, "Jaleco JF-11" },
	{ 146, 0x4100, 0x5FFF, 0, syncNina, "Sachen 3015" },
	{ 152, 0x8000, 0xFFFF, BUS_CONFLICT, sync152, "Bandai 74161 (one screen)" },
	{ 180, 0x8000, 0xFFFF, BUS_CONFLICT, sync180, "UNROM (fixed bank at $8000)" },
	{ 184, 0x6000, 0x7FFF, 0, sync184, "Sunsoft-1" },
	{ 185, 0x8000, 0xFFFF, BUS_CONFLICT, sync185, "CNROM (protected)" },
	{ 200, 0x8000, 0xFFFF, LATCH_ADDRESS, sync200, "200" },
	{ 201, 0x8000, 0xFFFF, LATCH_ADDRESS, sync201, "201" },
	{ 202, 0x8000, 0xFFFF, LATCH_ADDRESS, sync202, "202" },
	{ 203, 0x8000, 0xFFFF, 0, sync203, "203" },
	{ 225, 0x8000, 0xFFFF, LATCH_ADDRESS, sync225, "225" },
	{ 226, 0x8000, 0xFFFF, 0, sync226, "226" },
	{ 227, 0x8000, 0xFFFF, LATCH_ADDRESS, sync227, "227" },
	{ 229, 0x8000, 0xFFFF, LATCH_ADDRESS, sync229, "229" },
	{ 231, 0x8000, 0xFFFF, LATCH_ADDRESS, sync231, "231" },
	{ 232, 0x8000, 0xFFFF, 0, sync232, "Camerica BF9096" },
	{ 240, 0x4020, 0x5FFF, 0, sync240, "240" },
	{ 241, 0x8000, 0xFFFF, WRAM, sync241, "241" },
	{ 242, 0x8000, 0xFFFF, LATCH_ADDRESS, sync242, "242" },
};

static const struct DiscreteBoard* discrete(struct NES* nes) {
	return nes->board->data;
}

static void discreteSync(struct NES* nes) {
	const struct DiscreteBoard* b = discrete(nes);
	b->sync(nes, STATE(Discrete));
}

static void discreteWrite(struct NES* nes, uint16_t address, uint8_t value) {
	const struct DiscreteBoard* b = discrete(nes);
	struct Discrete* d = STATE(Discrete);
	switch (b->mapper) {
	case 34:
		// NINA-001's registers sit at the top of work RAM
		if (address >= 0x7FFD && address <= 0x7FFF) {
			if (address == 0x7FFD) {
				d->outer = value;
			} else if (address == 0x7FFE) {
				d->value = value;
			} else {
				d->value2 = value;
			}
			discreteSync(nes);
			return;
		}
		break;
	case 41:
		if (address >= 0x6000 && address < 0x6800) {
			d->address = address;
			discreteSync(nes);
		} else if (address >= 0x8000 && (d->address & 4)) {
			d->value = value;
			discreteSync(nes);
		}
		return;
	case 46:
		if (address >= 0x6000 && address < 0x8000) {
			d->outer = value;
			discreteSync(nes);
			return;
		}
		break;
	case 71:
		if (address >= 0x9000 && address < 0xA000) {
			d->outer = 0x80 | value;
			discreteSync(nes);
			return;
		}
		break;
	case 79:
	case 113:
	case 146:
		if ((address & 0xE100) != 0x4100) {
			return;
		}
		break;
	case 226:
		if (address >= 0x8000) {
			if (address & 1) {
				d->value2 = value;
			} else {
				d->value = value;
			}
			discreteSync(nes);
		}
		return;
	case 232:
		if (address >= 0x8000 && address < 0xC000) {
			d->outer = value;
			discreteSync(nes);
			return;
		}
		break;
	}
	if (address < b->from || address > b->to) {
		return;
	}
	if (b->flags & BUS_CONFLICT && nes->cart.submapper != 1) {
		value = busConflict(nes, address, value);
	}
	d->value = value;
	d->address = address;
	discreteSync(nes);
}

static void discreteReset(struct NES* nes, bool hard) {
	const struct DiscreteBoard* b = discrete(nes);
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Discrete));
	}
	if (b->flags & CHR_RAM_16K && nes->cart.chrRam && nes->cart.chrSize < 0x4000) {
		nes->cart.chrSize = 0x4000;
		nes->cart.chr = realloc(nes->cart.chr, 0x4000);
		memset(nes->cart.chr, 0, 0x4000);
	}
	nesMapPrg(nes, 0x8000, 0x8000, 0);
	chr8(nes, 0);
	if (b->flags & WRAM || b->from < 0x8000) {
		wram8(nes);
	}
	// Multicarts start on their menu, in the last bank where it's 16 KB
	discreteSync(nes);
}

static struct Board instances[sizeof(boards) / sizeof(boards[0])];

const struct Board* discreteFind(int mapper, int submapper) {
	for (size_t i = 0; i < sizeof(boards) / sizeof(boards[0]); i++) {
		if (boards[i].mapper == mapper) {
			struct Board* b = &instances[i];
			b->name = boards[i].name;
			b->reset = discreteReset;
			b->write = discreteWrite;
			b->sync = discreteSync;
			b->stateSize = sizeof(struct Discrete);
			b->data = &boards[i];
			return b;
		}
	}
	return NULL;
}
