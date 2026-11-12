/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

#include <stdlib.h>
#include <zlib.h>

// NSF music files: a tune's code and data with its INIT and PLAY routines. A driver of armNES's own
// at $4100 calls INIT for the song, then PLAY from a timer IRQ at the file's rate. The code sits in
// 4 KB banks at $8000-$FFFF switched at $5FF8-$5FFF ($6000-$7FFF too, in RAM, for the Disk
// System's), and the file names the sound chips it plays.

enum {
	CHIP_VRC6 = 1,
	CHIP_VRC7 = 2,
	CHIP_FDS = 4,
	CHIP_MMC5 = 8,
	CHIP_N163 = 16,
	CHIP_5B = 32,
};

// Assembled by armnes-kit/devtest/nsfdriver.py. At reset: the APU silenced, RAM cleared, INIT called
// with the song in A and the region in X ($41F0, $41F1, a JMP at $41F4), the timer started ($41F2)
// and the CPU left waiting. Each timer IRQ ($41F3 reads and clears it) calls PLAY (a JMP at $41F7).
static const uint8_t driver[] = {
	0x78, 0xD8, 0xA2, 0xFF, 0x9A, 0xA9, 0x00, 0x8D, 0x00, 0x20, 0x8D, 0x01, 0x20, 0xA2, 0x13, 0x9D,
	0x00, 0x40, 0xCA, 0x10, 0xFA, 0xA9, 0x0F, 0x8D, 0x15, 0x40, 0xA9, 0x40, 0x8D, 0x17, 0x40, 0xA9,
	0x00, 0xAA, 0x95, 0x00, 0x9D, 0x00, 0x01, 0x9D, 0x00, 0x02, 0x9D, 0x00, 0x03, 0x9D, 0x00, 0x04,
	0x9D, 0x00, 0x05, 0x9D, 0x00, 0x06, 0x9D, 0x00, 0x07, 0xE8, 0xD0, 0xE6, 0xAD, 0xF0, 0x41, 0xAE,
	0xF1, 0x41, 0xA0, 0x00, 0x20, 0xF4, 0x41, 0xA9, 0x01, 0x8D, 0xF2, 0x41, 0x58, 0x4C, 0x4D, 0x41,
	0x48, 0x8A, 0x48, 0x98, 0x48, 0xAD, 0xF3, 0x41, 0xF0, 0x03, 0x20, 0xF7, 0x41, 0x68, 0xA8, 0x68,
	0xAA, 0x68, 0x40,
};
#define DRIVER_RESET 0x4100
#define DRIVER_IRQ 0x4150
#define DRIVER_NMI 0x4162

// The file's header and the MMC5's 1 KB of RAM, kept in the cart's misc ROM
#define HEADER (nes->cart.misc)
#define EXRAM (nes->cart.misc + 0x80)

struct Nsf {
	// $6000-$FFFF in 4 KB banks
	uint8_t banks[10];
	uint8_t song, chips;
	uint32_t period, timer;
	bool timing, timerIrq;
	uint8_t multiply[2];
	struct Vrc6Sound vrc6;
	struct Vrc7Sound vrc7;
	struct FdsSound fds;
	struct Mmc5Sound mmc5;
	struct Namco163Sound n163;
	struct Sunsoft5b s5b;
};

#define NS STATE(Nsf)

static uint16_t word(const uint8_t* p) {
	return p[0] | p[1] << 8;
}

static bool diskNsf(struct NES* nes) {
	return HEADER[0x7B] & CHIP_FDS;
}

static void nsfSync(struct NES* nes) {
	struct Nsf* n = NS;
	bool disk = diskNsf(nes);
	for (int page = 6; page < 16; page++) {
		if (disk || page >= 8) {
			nesMapPrg(nes, page << 12, 0x1000, n->banks[page - 6]);
			nes->prgWritable[page] = disk;
		}
	}
	if (!disk) {
		wram8(nes);
	}
	nesMapChr(nes, 0, 0x2000, 0);
	nes->readHooks = 1 << 4 | 1 << 5 | 1 << 15;
}

static uint8_t nsfRead(struct NES* nes, uint16_t address) {
	struct Nsf* n = NS;
	uint8_t open = nes->cpu.openBus;
	uint8_t* page = nes->prgPages[address >> 12];
	if (address >= 0xFFFA) {
		// The driver's reset and NMI, and its IRQ for the play timer; BRK and the file's own IRQs
		// go to the file's handler when it has one
		static const uint16_t vectors[3] = { DRIVER_NMI, DRIVER_RESET, DRIVER_IRQ };
		uint16_t own = page[0xFFE] | page[0xFFF] << 8;
		uint16_t vector = address >= 0xFFFE && !n->timerIrq && own ? own : vectors[(address - 0xFFFA) >> 1];
		return address & 1 ? vector >> 8 : vector & 0xFF;
	}
	if (address >= 0x4100 && address < 0x4100 + sizeof(driver)) {
		return driver[address - 0x4100];
	}
	switch (address) {
	case 0x41F0:
		return n->song;
	case 0x41F1:
		return nes->region != REGION_NTSC;
	case 0x41F3: {
		bool was = n->timerIrq;
		n->timerIrq = false;
		nesSetIrq(nes, IRQ_MAPPER, false);
		return was;
	}
	case 0x41F4:
	case 0x41F7:
		return 0x4C;
	case 0x41F5:
	case 0x41F6:
	case 0x41F8:
	case 0x41F9: {
		// INIT's or PLAY's address
		const uint8_t* target = HEADER + (address < 0x41F7 ? 0x0A : 0x0C);
		return target[(address - 0x41F5) % 3];
	}
	}
	if ((n->chips & CHIP_FDS) && address >= 0x4040 && address <= 0x4092) {
		return fdsSoundRead(&n->fds, address, open);
	}
	if ((n->chips & CHIP_N163) && address >= 0x4800 && address < 0x5000) {
		return namco163SoundRead(&n->n163);
	}
	if (n->chips & CHIP_MMC5) {
		if (address == 0x5015) {
			return mmc5SoundStatus(&n->mmc5);
		}
		if (address == 0x5205 || address == 0x5206) {
			unsigned product = n->multiply[0] * n->multiply[1];
			return address == 0x5205 ? product & 0xFF : product >> 8;
		}
		if (address >= 0x5C00 && address < 0x5FF6) {
			return EXRAM[address - 0x5C00];
		}
	}
	return page ? page[address & 0xFFF] : open;
}

static void nsfWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Nsf* n = NS;
	if (address == 0x41F2) {
		// The play rate, in CPU cycles
		bool pal = nes->region != REGION_NTSC;
		uint16_t micros = word(HEADER + (pal ? 0x78 : 0x6E));
		if (!micros) {
			micros = pal ? 19997 : 16639;
		}
		double clock = nes->region == REGION_NTSC ? 1789773 : nes->region == REGION_PAL ? 1662607 : 1773448;
		n->period = (uint32_t) (micros * clock / 1000000 + 0.5);
		n->timer = n->period;
		n->timing = true;
		return;
	}
	if (address >= 0x5FF6 && address <= 0x5FFF) {
		if (address >= 0x5FF8 || diskNsf(nes)) {
			n->banks[address - 0x5FF6] = value;
			nsfSync(nes);
		}
		return;
	}
	uint8_t chips = n->chips;
	if (chips & CHIP_FDS && address >= 0x4040 && address <= 0x408A) {
		fdsSoundWrite(&n->fds, address, value);
	}
	if (chips & CHIP_N163) {
		if (address >= 0x4800 && address < 0x5000) {
			namco163SoundWrite(&n->n163, value);
		} else if (address >= 0xF800) {
			n->n163.address = value;
		}
	}
	if (chips & CHIP_MMC5) {
		if (address >= 0x5000 && address <= 0x5015) {
			mmc5SoundWrite(&n->mmc5, address, value);
		} else if (address == 0x5205 || address == 0x5206) {
			n->multiply[address - 0x5205] = value;
		} else if (address >= 0x5C00 && address < 0x5FF6) {
			EXRAM[address - 0x5C00] = value;
		}
	}
	if (chips & CHIP_VRC6 && address >= 0x9000 && address < 0xC000 && (address & 0xFFF) < 3) {
		vrc6SoundWrite(&n->vrc6, (address >> 12) - 9, address & 3, value);
	}
	if (chips & CHIP_VRC7) {
		if (address == 0x9010) {
			n->vrc7.address = value;
		} else if (address == 0x9030) {
			vrc7SoundWrite(&n->vrc7, value);
		}
	}
	if (chips & CHIP_5B) {
		if ((address & 0xE000) == 0xC000) {
			n->s5b.address = value;
		} else if ((address & 0xE000) == 0xE000) {
			sunsoft5bWrite(&n->s5b, value);
		}
	}
}

static void nsfCycle(struct NES* nes) {
	struct Nsf* n = NS;
	if (n->timing && --n->timer == 0) {
		n->timer = n->period;
		n->timerIrq = true;
		nesSetIrq(nes, IRQ_MAPPER, true);
	}
	uint8_t chips = n->chips;
	if (chips & CHIP_VRC6) {
		vrc6SoundCycle(&n->vrc6);
	}
	if (chips & CHIP_VRC7) {
		vrc7SoundCycle(&n->vrc7, false);
	}
	if (chips & CHIP_FDS) {
		fdsSoundCycle(&n->fds);
	}
	if (chips & CHIP_MMC5) {
		mmc5SoundCycle(&n->mmc5);
	}
	if (chips & CHIP_N163) {
		namco163SoundCycle(&n->n163, false);
	}
	if (chips & CHIP_5B) {
		sunsoft5bCycle(&n->s5b);
	}
}

static int nsfAudio(struct NES* nes) {
	struct Nsf* n = NS;
	uint8_t chips = n->chips;
	int out = 0;
	if (chips & CHIP_VRC6) {
		out += vrc6SoundOutput(&n->vrc6);
	}
	if (chips & CHIP_VRC7) {
		out += vrc7SoundOutput(&n->vrc7);
	}
	if (chips & CHIP_FDS) {
		out += n->fds.filtered;
	}
	if (chips & CHIP_MMC5) {
		out += mmc5SoundOutput(&n->mmc5);
	}
	if (chips & CHIP_N163) {
		out += namco163SoundOutput(&n->n163);
	}
	if (chips & CHIP_5B) {
		out += sunsoft5bOutput(&n->s5b);
	}
	return out;
}

// Every reset starts the selected song afresh: the banks, RAM and chips as the file begins them
static void nsfReset(struct NES* nes, bool hard) {
	struct Nsf* n = NS;
	struct Cart* cart = &nes->cart;
	uint8_t song = hard ? (uint8_t) (HEADER[7] ? HEADER[7] - 1 : 0) : n->song;
	memset(n, 0, sizeof(*n));
	n->song = song;
	n->chips = HEADER[0x7B] & 0x3F;
	const uint8_t* init = HEADER + 0x70;
	bool banked = false;
	for (int i = 0; i < 8; i++) {
		banked |= init[i] != 0;
	}
	bool disk = diskNsf(nes);
	for (int i = 0; i < 10; i++) {
		// Unbanked files fill $8000 (or the Disk System's $6000) onward in order
		n->banks[i] = disk ? (banked ? init[i < 2 ? i + 6 : i - 2] : i) : (i < 2 ? 0 : banked ? init[i - 2] : i - 2);
	}
	if (disk) {
		// The code is in RAM: back to the file's copy
		memcpy(cart->prg, cart->prg + cart->prgSize, cart->prgSize);
	}
	memset(cart->wram, 0, cart->wramSize);
	memset(EXRAM, 0, 0x400);
	fdsSoundReset(&n->fds);
	if (n->chips & CHIP_VRC7) {
		vrc7SoundReset(&n->vrc7, true);
	}
	n->s5b.noiseShift = 1;
	nesSetIrq(nes, IRQ_MAPPER, false);
	nsfSync(nes);
}

static size_t nsfSaveExtra(struct NES* nes, uint8_t* out) {
	return NS->chips & CHIP_VRC7 ? vrc7SoundState(out, false) : 0;
}

static void nsfLoadExtra(struct NES* nes, const uint8_t* in) {
	if (NS->chips & CHIP_VRC7) {
		vrc7SoundState((uint8_t*) in, true);
	}
}

const struct Board boardNsf = {
	.name = "NSF", .reset = nsfReset, .write = nsfWrite, .read = nsfRead, .cpuCycle = nsfCycle, .audio = nsfAudio,
	.sync = nsfSync, .stateSize = sizeof(struct Nsf), .saveExtra = nsfSaveExtra, .loadExtra = nsfLoadExtra,
};

bool nsfCartLoad(struct NES* nes, const uint8_t* data, size_t size) {
	struct Cart* cart = &nes->cart;
	if (size <= 0x80 || memcmp(data, "NESM\x1A", 5)) {
		return false;
	}
	const uint8_t* h = data;
	uint16_t load = word(h + 8);
	bool disk = h[0x7B] & CHIP_FDS;
	bool banked = false;
	for (int i = 0; i < 8; i++) {
		banked |= h[0x70 + i] != 0;
	}
	uint16_t base = disk ? 0x6000 : 0x8000;
	if (load < base && !banked) {
		return false;
	}
	size_t length = size - 0x80;
	// NSF2 files give the program's length, with metadata after it
	uint32_t programLength = h[0x7D] | h[0x7E] << 8 | h[0x7F] << 16;
	if (h[5] >= 2 && programLength && programLength < length) {
		length = programLength;
	}
	uint32_t padding = banked ? load & 0xFFF : load - base;
	uint32_t prgSize = (uint32_t) ((padding + length + 0xFFF) & ~0xFFF);
	uint32_t least = disk ? 0xA000 : 0x8000;
	if (prgSize < least) {
		prgSize = least;
	}
	memset(cart->header, 0, sizeof(cart->header));
	cart->hasTrainer = false;
	cart->battery = false;
	cart->batterySize = 0;
	cart->vram = cart->disk = NULL;
	cart->vramSize = 0;
	cart->diskSides = 0;
	cart->mapper = NSF_MAPPER;
	cart->submapper = 0;
	cart->jumper = -1;
	cart->nes2 = false;
	cart->region = (h[0x7A] & 3) == 1 ? REGION_PAL : REGION_NTSC;
	cart->mirroring = MIRROR_VERTICAL;
	// The Disk System's is RAM, with the file's copy after it to start each song from
	cart->prgSize = prgSize;
	cart->prg = calloc(disk ? 2 : 1, prgSize);
	memcpy(cart->prg + padding, data + 0x80, length);
	if (disk) {
		memcpy(cart->prg + prgSize, cart->prg, prgSize);
	}
	cart->chrSize = 0x2000;
	cart->chrRam = true;
	cart->chr = calloc(1, cart->chrSize);
	cart->wramSize = 0x2000;
	cart->wram = calloc(1, cart->wramSize);
	cart->miscSize = 0x80 + 0x400;
	cart->misc = calloc(1, cart->miscSize);
	memcpy(cart->misc, h, 0x80);
	cart->prgCrc = crc32(0, data + 0x80, (uInt) length);
	return true;
}

bool nsfInfo(struct NES* nes, struct NsfInfo* info) {
	if (nes->board != &boardNsf) {
		return false;
	}
	// The names are 32 bytes, not always ended
	static char names[3][33];
	for (int i = 0; i < 3; i++) {
		memcpy(names[i], HEADER + 0x0E + i * 32, 32);
		names[i][32] = 0;
	}
	info->title = names[0];
	info->artist = names[1];
	info->copyright = names[2];
	info->songs = HEADER[6] ? HEADER[6] : 1;
	info->song = NS->song;
	return true;
}

void nsfSelect(struct NES* nes, int song) {
	if (nes->board == &boardNsf) {
		NS->song = (uint8_t) song;
		nesReset(nes, false);
	}
}
