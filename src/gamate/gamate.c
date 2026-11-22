/* Copyright (c) 2007-2025 Peter Trauner, David Haywood, Peter Wilhelmsen and the MAME team
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * The memory map and timer follow MAME's gamate.cpp (GPL-2.0+); the screen, the cartridge banking and
 * the protection follow its gamate_v.cpp, bus/gamate/rom.cpp and gamate_protection.cpp (BSD-3-Clause),
 * after Kevin Horton's "Gamate Inside" notes. */
#include "gamate.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define M (gm->m)

enum { PLAIN, BANKED, FOUR_IN_1 };
// The LCD control register's bits: blank, add 32 to the address, the window, swapped planes
enum { LCD_BLANK = 0x80, LCD_DOWN = 0x40, LCD_WINDOW = 0x20, LCD_SWAP = 0x10 };
// The timer's IRQ: every 16384 cycles, held for 10
enum { IRQ_PERIOD = 16384, IRQ_HOLD = 10 };

static const uint8_t protectionString[15] = { 'B', 'I', 'T', ' ', 'C', 'O', 'R', 'P', 'O', 'R', 'A', 'T', 'I', 'O', 'N' };

// The cartridge's protection: the BIOS sends "BIT CORPORATION" a bit at a time (bit 2 of a write) and
// reads back 'G' (in bit 1); until then the cartridge answers nothing else.
static void protectionWrite(struct Gamate* gm, int bit) {
	if (M.protectionBit < 8) {
		M.protectionByte |= (uint8_t) (bit << (7 - M.protectionBit));
		M.protectionBit++;
		return;
	}
	if (!M.protectionFailed && M.protectionCount < 15 && M.protectionByte != protectionString[M.protectionCount]) {
		M.protectionFailed = 1;
	}
	M.protectionBit = 0;
	M.protectionByte = 0;
	M.protectionCount++;
	if (!M.protectionFailed && M.protectionCount == 15) {
		M.protectionByte = 'G';
		M.protectionReady = 1;
	}
}

static int protectionRead(struct Gamate* gm) {
	if (!M.protectionReady) {
		return 0;
	}
	int bit = M.protectionByte >> (7 - M.protectionBit) & 1;
	if (++M.protectionBit == 8) {
		M.protectionPassed = 1;
		M.protectionBit = 0;
		M.protectionByte = 0;
	}
	return bit;
}

// $6000-$DFFF: the first 16 KiB fixed (or a 4 in 1's game), the rest banked
static uint8_t cartRead(struct Gamate* gm, uint16_t offset) {
	if (!M.protectionPassed) {
		return (uint8_t) (protectionRead(gm) << 1);
	}
	uint32_t const mask = gm->romSize - 1;
	switch (gm->type) {
	case PLAIN: return gm->rom[offset & mask];
	case FOUR_IN_1:
		if (offset < 0x4000) {
			return gm->rom[((uint32_t) M.multibank * 0x4000 + offset) & mask];
		}
		return gm->rom[((uint32_t) M.bank * 0x4000 + (offset & 0x3fff)) & mask];
	default:
		if (offset < 0x4000) {
			return gm->rom[offset & mask];
		}
		return gm->rom[((uint32_t) M.bank * 0x4000 + (offset & 0x3fff)) & mask];
	}
}

static void cartWrite(struct Gamate* gm, uint16_t offset, uint8_t data) {
	if (!M.protectionPassed) {
		protectionWrite(gm, data >> 2 & 1);
		return;
	}
	if (gm->type != PLAIN && offset == 0x6000) {
		M.bank = data;
	} else if (gm->type == FOUR_IN_1 && offset == 0x2000) {
		M.multibank = data;
	}
}

static void stepVramAddress(struct Gamate* gm) {
	M.vramAddress = (uint16_t) ((M.vramAddress + (M.lcd & LCD_DOWN ? 0x20 : 1)) & 0x1fff);
}

static uint8_t gmBusRead(void* context, uint16_t a) {
	struct Gamate* gm = context;
	if (a < 0x2000) {
		return M.ram[a & 0x3ff];
	}
	if (a >= 0xe000) {
		return gm->bios[a & 0xfff];
	}
	if (a >= 0x6000) {
		return cartRead(gm, (uint16_t) (a - 0x6000));
	}
	switch (a & 0xfc00) {
	case 0x4000: return M.ay.regs[a & 15];
	case 0x4400: return (uint8_t) ~M.buttons;
	case 0x5000:
		if ((a & 7) == 6) {
			uint8_t data = M.vram[(M.vramAddress << 1) | M.plane];
			stepVramAddress(gm);
			return data;
		}
		return 0;
	}
	if (a == 0x5800) {
		M.cardAvailable = 1;
		return 0;
	}
	if (a == 0x5a00) {
		return M.cardAvailable ? 3 : 1;
	}
	return 0;
}

static void gmBusWrite(void* context, uint16_t a, uint8_t data) {
	struct Gamate* gm = context;
	if (a < 0x2000) {
		M.ram[a & 0x3ff] = data;
		return;
	}
	if (a >= 0x6000 && a < 0xe000) {
		cartWrite(gm, (uint16_t) (a - 0x6000), data);
		return;
	}
	switch (a & 0xfc00) {
	case 0x4000:
		M.ay.address = (uint8_t) (a & 15);
		sunsoft5bWrite(&M.ay, data);
		break;
	case 0x5000:
		switch (a & 7) {
		case 1: M.lcd = data; break;
		case 2: M.scrollX = data; break;
		case 3: M.scrollY = data; break;
		case 4:
			M.plane = data >> 7;
			M.vramAddress = (uint16_t) ((M.vramAddress & 0x1fe0) | (data & 0x1f));
			break;
		case 5: M.vramAddress = (uint16_t) ((M.vramAddress & 0x1f) | data << 5); break;
		case 7:
			M.vram[(M.vramAddress << 1) | M.plane] = data;
			stepVramAddress(gm);
			break;
		}
		break;
	}
}

// One line of the screen, with the scroll and window as they are now. Below Y scroll $C8 the picture
// wraps at row $C8, and the window puts rows $D0-$DF, unscrolled, at the top 16 lines; at $C8 and above
// (no game known to use it) the top 1-8 lines come from rows $F8-$FF, or it's as Y scroll 0.
static void drawLine(struct Gamate* gm, int line) {
	int x = M.scrollX, y;
	if (M.scrollY < 0xc8) {
		y = line + M.scrollY;
		y -= y >= 0xc8 ? 0xc8 : 0;
		if ((M.lcd & LCD_WINDOW) && line < 0x10) {
			x = 0;
			y = 0xd0 + line;
		}
	} else if (M.scrollY & 0x08) {
		y = 0;
	} else {
		int const fixed = M.scrollY & 7;
		if (line <= fixed) {
			x = 0;
			y = 0xf8 + line + (7 - fixed);
		} else {
			y = line;
		}
	}
	uint8_t* out = gm->picture + line * GM_WIDTH;
	if (M.lcd & LCD_BLANK) {
		memset(out, 0, GM_WIDTH);
		return;
	}
	const uint8_t* row = M.vram + (((y & 0xff) * 0x20) << 1);
	bool const swap = M.lcd & LCD_SWAP;
	for (int i = 0; i < GM_WIDTH; i++) {
		int const px = (x + i) & 0xff;
		const uint8_t* pair = row + ((px >> 3) << 1);
		int const shift = 7 - (px & 7);
		int const p0 = pair[0] >> shift & 1, p1 = pair[1] >> shift & 1;
		out[i] = (uint8_t) (swap ? p1 | p0 << 1 : p0 | p1 << 1);
	}
}

// One AY tick (16 cycles): its channels' levels into the sums, A to the left, B to the right and C to
// both at half, as MAME routes them.
static void soundTick(struct Gamate* gm) {
	int levels[3];
	sunsoft5bLevels(&M.ay, levels);
	for (int i = 0; i < 3; i++) {
		levels[i] = gm->muted & (1 << i) ? 0 : levels[i];
	}
	M.sumLeft += 2 * levels[0] + levels[2];
	M.sumRight += 2 * levels[1] + levels[2];
	if (++M.summed == 4) {
		if (gm->soundFrames < (int) (sizeof(gm->sound) / sizeof(*gm->sound) / 2)) {
			gm->sound[gm->soundFrames * 2] = (int16_t) (M.sumLeft / 8);
			gm->sound[gm->soundFrames * 2 + 1] = (int16_t) (M.sumRight / 8);
			gm->soundFrames++;
		}
		M.sumLeft = M.sumRight = M.summed = 0;
	}
}

static void cpuFromMachine(struct Gamate* gm) {
	struct W65c02* c = &gm->cpu;
	c->pc = M.pc;
	c->a = M.a;
	c->x = M.x;
	c->y = M.y;
	c->s = M.s;
	c->p = M.p;
	c->nmi = M.nmi;
	c->irq = M.irq;
	c->waiting = M.waiting;
	c->stopped = M.stopped;
	c->read = gmBusRead;
	c->write = gmBusWrite;
	c->context = gm;
}

static void machineFromCpu(struct Gamate* gm) {
	const struct W65c02* c = &gm->cpu;
	M.pc = c->pc;
	M.a = c->a;
	M.x = c->x;
	M.y = c->y;
	M.s = c->s;
	M.p = c->p;
	M.nmi = c->nmi;
	M.irq = c->irq;
	M.waiting = c->waiting;
	M.stopped = c->stopped;
}

bool gmLoad(struct Gamate* gm, const uint8_t* bios, const uint8_t* rom, size_t size) {
	gmUnload(gm);
	if (!bios || !size || size > 0x80000) {
		return false;
	}
	// A power of two, a smaller cartridge repeated
	uint32_t romSize = 0x4000;
	while (romSize < size) {
		romSize <<= 1;
	}
	gm->rom = malloc(romSize);
	if (!gm->rom) {
		return false;
	}
	for (uint32_t at = 0; at < romSize; at++) {
		gm->rom[at] = rom[at % size];
	}
	gm->romSize = romSize;
	// MAME's software list: up to 32 KiB plain, the 4 in 1 by its CRC, the others banked
	gm->type = size <= 0x8000 ? PLAIN : crc32(0, rom, (uInt) size) == 0x10e81571 ? FOUR_IN_1 : BANKED;
	memcpy(gm->bios, bios, GM_BIOS_SIZE);
	gmReset(gm);
	return true;
}

void gmUnload(struct Gamate* gm) {
	free(gm->rom);
	gm->rom = NULL;
	gm->romSize = 0;
}

void gmReset(struct Gamate* gm) {
	memset(&gm->m, 0, sizeof(gm->m));
	memset(&gm->cpu, 0, sizeof(gm->cpu));
	// RAM seems to hold $FF at power-on; Y scroll starts at 10
	memset(M.ram, 0xff, sizeof(M.ram));
	M.scrollY = 10;
	M.ay.noiseShift = 1;
	M.irqLeft = 1000;
	cpuFromMachine(gm);
	w65c02Reset(&gm->cpu);
	machineFromCpu(gm);
}

void gmRunFrame(struct Gamate* gm, uint8_t buttons) {
	cpuFromMachine(gm);
	M.buttons = buttons;
	gm->soundFrames = 0;
	M.line = 0;
	while (M.frameCycles < GM_FRAME_CYCLES) {
		int const cycles = w65c02Step(&gm->cpu);
		M.frameCycles += cycles;
		while (M.line < GM_HEIGHT && M.frameCycles >= (M.line + 1) * GM_LINE_CYCLES) {
			drawLine(gm, M.line++);
		}
		if (M.irqHeld > 0 && (M.irqHeld -= cycles) <= 0) {
			gm->cpu.irq = false;
		}
		if ((M.irqLeft -= cycles) <= 0) {
			M.irqLeft += IRQ_PERIOD;
			M.irqHeld = IRQ_HOLD;
			gm->cpu.irq = true;
		}
		for (int i = 0; i < cycles; i++) {
			sunsoft5bCycle(&M.ay);
		}
		M.soundCycles += cycles;
		while (M.soundCycles >= 16) {
			M.soundCycles -= 16;
			soundTick(gm);
		}
	}
	while (M.line < GM_HEIGHT) {
		drawLine(gm, M.line++);
	}
	M.frameCycles -= GM_FRAME_CYCLES;
	machineFromCpu(gm);
}

size_t gmStateSize(void) {
	return sizeof(struct GmMachine);
}

void gmSave(const struct Gamate* gm, void* out) {
	memcpy(out, &gm->m, sizeof(gm->m));
}

bool gmLoadState(struct Gamate* gm, const void* in, size_t size) {
	if (size < sizeof(gm->m)) {
		return false;
	}
	memcpy(&gm->m, in, sizeof(gm->m));
	cpuFromMachine(gm);
	return true;
}
