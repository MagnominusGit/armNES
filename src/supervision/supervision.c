/* Copyright (c) 2000-2025 Peter Trauner and the MAME team (MAME's svision driver)
 * Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * The registers, the screen and the sound follow MAME's svision.cpp and svis_snd.cpp (GPL-2.0+). */
#include "supervision.h"

#include <stdlib.h>
#include <string.h>

#define M (sv->m)

enum {
	XSIZE = 0x00, XPOS = 0x02, YPOS = 0x03,
	JOYPAD = 0x20, LINK = 0x21, LINK_OUT = 0x22, TIMER = 0x23, TIMER_ACK = 0x24, DMA_ACK = 0x25, BANK = 0x26, STATUS = 0x27,
};
// BANK's bits: the frame NMI, the timer's and the sound DMA's IRQs, the LCD, the timer's slow prescaler,
// and the cartridge bank at $8000 in the top three.
enum { BANK_NMI = 1, BANK_TIMER_IRQ = 2, BANK_DMA_IRQ = 4, BANK_LCD = 8, BANK_TIMER_SLOW = 16 };

static void checkIrq(struct Supervision* sv) {
	bool irq = (M.timerShot && (M.reg[BANK] & BANK_TIMER_IRQ)) || (M.dmaFinished && (M.reg[BANK] & BANK_DMA_IRQ));
	sv->cpu.irq = irq;
}

static inline const uint8_t* bankAt8000(const struct Supervision* sv) {
	return sv->rom + ((((uint32_t) M.outer << 3 | M.reg[BANK] >> 5) % sv->banks) << 14);
}

// A cartridge over 128 KiB (a 4 in 1) starts in its last 128 KiB, where its first game's code is;
// how it moves to the others isn't known (MAME doesn't run them either).

// Memory as the sound DMA reads it: the cartridge through its own bank bits, else what the CPU sees
// (registers without their side effects).
static uint8_t peek(const struct Supervision* sv, uint16_t a) {
	switch (a >> 13) {
	case 0: return M.ram[a & 0x1fff];
	case 1: return M.reg[a & 0x1fff];
	case 2: return M.vram[a & 0x1fff];
	case 3: return 0;
	case 4: case 5: return bankAt8000(sv)[a & 0x3fff];
	default: return sv->rom[((sv->banks - 1) << 14) | (a & 0x3fff)];
	}
}

static uint8_t readReg(struct Supervision* sv, uint16_t offset) {
	uint8_t data = M.reg[offset];
	switch (offset) {
	case JOYPAD: return (uint8_t) ~M.buttons;
	case LINK: return (uint8_t) ((data & ~0x0f) | (M.reg[LINK_OUT] & 0x0f));
	case STATUS: return (uint8_t) ((data & ~3) | (M.timerShot ? 1 : 0) | (M.dmaFinished ? 2 : 0));
	case TIMER_ACK:
		M.timerShot = 0;
		checkIrq(sv);
		return data;
	case DMA_ACK:
		M.dmaFinished = 0;
		checkIrq(sv);
		return data;
	default: return data;
	}
}

static uint8_t busRead(void* context, uint16_t a) {
	struct Supervision* sv = context;
	if ((a >> 13) == 1) {
		return readReg(sv, a & 0x1fff);
	}
	return peek(sv, a);
}

// The sound registers. A square's period is (its 11 bits) x 32 clocks: in sound ticks of 32 clocks,
// the 11 bits; the noise and the DMA step every 256 x (1 + their divider) clocks, 8 x that in ticks.
static void squareWrite(struct Supervision* sv, int which, int offset, uint8_t data) {
	struct SvSquare* c = &M.square[which];
	c->reg[offset] = data;
	switch (offset) {
	case 0:
	case 1:
		c->size = (uint16_t) (c->reg[0] | (c->reg[1] & 7) << 8);
		c->pos = 0;
		break;
	case 2:
		c->on = data & 0x40;
		c->waveform = (data >> 4) & 3;
		c->volume = data & 0x0f;
		break;
	case 3:
		c->count = (uint16_t) (data + 1);
		break;
	}
}

static void dmaWrite(struct Supervision* sv, int offset, uint8_t data) {
	struct SvDma* d = &M.dma;
	d->reg[offset] = data;
	switch (offset) {
	case 0:
	case 1:
		d->start = (uint16_t) (d->reg[0] | d->reg[1] << 8);
		break;
	case 2:
		// In nibbles
		d->size = (uint32_t) (data ? data : 0x100) * 32;
		break;
	case 3:
		d->period = (uint16_t) (8 * (1 + (data & 3)));
		d->right = data & 4;
		d->left = data & 8;
		d->bank = (uint32_t) ((data >> 4) & 7) << 14;
		break;
	case 4:
		d->on = data & 0x80;
		if (d->on) {
			d->pos = 0;
			d->timer = 0;
		}
		break;
	}
}

static void noiseWrite(struct Supervision* sv, int offset, uint8_t data) {
	struct SvNoise* n = &M.noise;
	n->reg[offset] = data;
	switch (offset) {
	case 0:
		n->volume = data & 0x0f;
		n->period = (uint16_t) (8 * (1 + (data >> 4)));
		break;
	case 1:
		n->count = (uint16_t) (data + 1);
		break;
	case 2:
		n->wide = data & 1;
		n->play = data & 2;
		n->right = data & 4;
		n->left = data & 8;
		n->on = data & 0x10;
		n->state = 1;
		break;
	}
	n->timer = 0;
}

static void writeReg(struct Supervision* sv, uint16_t offset, uint8_t data) {
	M.reg[offset] = data;
	switch (offset) {
	case BANK:
		checkIrq(sv);
		break;
	case TIMER: {
		int32_t delay = data ? data : 0x100;
		M.timerLeft = delay * (M.reg[BANK] & BANK_TIMER_SLOW ? 0x4000 : 0x100);
		M.timerOn = 1;
		break;
	}
	case 0x10: case 0x11: case 0x12: case 0x13: squareWrite(sv, 0, offset & 3, data); break;
	case 0x14: case 0x15: case 0x16: case 0x17: squareWrite(sv, 1, offset & 3, data); break;
	case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: dmaWrite(sv, offset - 0x18, data); break;
	case 0x28: case 0x29: case 0x2a: noiseWrite(sv, offset - 0x28, data); break;
	}
}

static void busWrite(void* context, uint16_t a, uint8_t data) {
	struct Supervision* sv = context;

	switch (a >> 13) {
	case 0: M.ram[a & 0x1fff] = data; break;
	case 1: writeReg(sv, a & 0x1fff, data); break;
	case 2: M.vram[a & 0x1fff] = data; break;
	}
}

// One sound tick (32 clocks): each channel's level, summed into the next sample.
static void soundTick(struct Supervision* sv) {
	int32_t left = 0, right = 0;
	for (int j = 0; j < 2; j++) {
		struct SvSquare* c = &M.square[j];
		if (!c->size) {
			continue;
		}
		if (c->on || c->count) {
			bool high;
			switch (c->waveform) {
			case 0: high = c->pos <= (28 * c->size) >> 5; break;
			case 1: high = c->pos <= (24 * c->size) >> 5; break;
			case 2: high = c->pos <= c->size / 2; break;
			default: high = c->pos <= (9 * c->size) >> 5; break;
			}
			int32_t s = high ? c->volume << 8 : 0;
			if (j == 0) {
				right += s;
			} else {
				left += s;
			}
		}
		if (++c->pos >= c->size) {
			c->pos = 0;
		}
	}
	struct SvNoise* n = &M.noise;
	if (n->on && (n->play || n->count)) {
		int32_t s = n->value ? n->volume << 8 : 0;
		left += n->left ? s : 0;
		right += n->right ? s : 0;
		if (++n->timer >= n->period) {
			n->timer = 0;
			uint16_t top = n->wide ? 0x2000 : 0x40;
			n->value = (n->state & top) != 0;
			bool b1 = (n->state & top) != 0, b2 = (n->state & (top >> 1)) != 0;
			n->state = (uint16_t) ((n->state << 1) + (b1 != b2 ? 1 : 0));
		}
	}
	struct SvDma* d = &M.dma;
	if (d->on) {
		uint16_t address = (uint16_t) (d->start + d->pos / 2);
		uint8_t sample = address >= 0x8000 && address < 0xc000
			                 ? sv->rom[(d->bank | (address & 0x3fff)) % sv->romSize]
			                 : peek(sv, address);
		int32_t s = (d->pos & 1 ? sample & 0x0f : sample >> 4) << 8;
		left += d->left ? s : 0;
		right += d->right ? s : 0;
		if (++d->timer >= d->period) {
			d->timer = 0;
			if (++d->pos >= d->size) {
				d->on = 0;
				M.dmaFinished = 1;
				checkIrq(sv);
			}
		}
	}
	M.sumLeft += left;
	M.sumRight += right;
	if (++M.summed == 4) {
		int16_t* out = &sv->sound[2 * sv->soundFrames];
		if (sv->soundFrames < (int) (sizeof(sv->sound) / sizeof(*sv->sound) / 2)) {
			out[0] = (int16_t) (M.sumLeft / 4);
			out[1] = (int16_t) (M.sumRight / 4);
			sv->soundFrames++;
		}
		M.sumLeft = M.sumRight = M.summed = 0;
	}
}

// The square and noise channels' lengths count down once a frame.
static void soundFrame(struct Supervision* sv) {
	for (int j = 0; j < 2; j++) {
		if (M.square[j].count) {
			M.square[j].count--;
		}
	}
	if (M.noise.count) {
		M.noise.count--;
	}
}

// The LCD as the frame ends: 160 lines of 160 pixels, four to a byte (the lowest bits leftmost),
// 0x30 bytes a line, from the scroll registers.
static void draw(struct Supervision* sv) {
	if (!(M.reg[BANK] & BANK_LCD)) {
		memset(sv->picture, 0, sizeof(sv->picture));
		return;
	}
	int j = M.reg[XPOS] / 4 + M.reg[YPOS] * 0x30;
	int shift = M.reg[XPOS] & 3;
	int end = ((M.reg[XSIZE] | 3) < 163 ? (M.reg[XSIZE] | 3) : 163) - 3;
	for (int y = 0; y < SV_HEIGHT; y++) {
		uint8_t* line = &sv->picture[y * SV_WIDTH];
		for (int x = 0; x < SV_WIDTH; x++) {
			int k = x + shift;
			line[x] = x + 3 < end + 3 ? (sv->m.vram[(j + k / 4) & 0x1fff] >> ((k & 3) * 2)) & 3 : 0;
		}
		j += 0x30;
		if (j >= 0x1fe0) {
			j = 0;
		}
	}
}

static void cpuFromMachine(struct Supervision* sv) {
	struct W65c02* c = &sv->cpu;
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
	c->read = busRead;
	c->write = busWrite;
	c->context = sv;
}

static void machineFromCpu(struct Supervision* sv) {
	const struct W65c02* c = &sv->cpu;
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

bool svLoad(struct Supervision* sv, const uint8_t* rom, size_t size) {
	svUnload(sv);
	if (!size || size > 0x80000) {
		return false;
	}
	// Whole 16 KiB banks, a smaller cartridge repeated through its last one.
	uint32_t banks = (uint32_t) ((size + 0x3fff) >> 14);
	sv->rom = malloc((size_t) banks << 14);
	if (!sv->rom) {
		return false;
	}
	for (size_t at = 0; at < (size_t) banks << 14; at++) {
		sv->rom[at] = rom[at % size];
	}
	sv->romSize = banks << 14;
	sv->banks = banks;
	svReset(sv);
	return true;
}

void svUnload(struct Supervision* sv) {
	free(sv->rom);
	sv->rom = NULL;
	sv->romSize = sv->banks = 0;
}

void svReset(struct Supervision* sv) {
	memset(&sv->m, 0, sizeof(sv->m));
	memset(&sv->cpu, 0, sizeof(sv->cpu));
	M.noise.period = M.dma.period = 8;
	M.outer = (uint8_t) (sv->banks > 8 ? sv->banks / 8 - 1 : 0);
	cpuFromMachine(sv);
	w65c02Reset(&sv->cpu);
	machineFromCpu(sv);
}

void svRunFrame(struct Supervision* sv, uint8_t buttons) {
	cpuFromMachine(sv);
	M.buttons = buttons;
	sv->soundFrames = 0;
	// 4 MHz over 61 frames a second: 65573 or 65574 clocks a frame.
	M.frameRemainder += SV_CLOCK % SV_FPS;
	int32_t frame = SV_CLOCK / SV_FPS + (M.frameRemainder >= SV_FPS ? 1 : 0);
	if (M.frameRemainder >= SV_FPS) {
		M.frameRemainder -= SV_FPS;
	}
	while (M.frameCycles < frame) {
		int cycles = w65c02Step(&sv->cpu);
		M.frameCycles += cycles;
		if (M.timerOn) {
			M.timerLeft -= cycles;
			if (M.timerLeft <= 0) {
				M.timerOn = 0;
				M.timerShot = 1;
				checkIrq(sv);
			}
		}
		M.soundCycles += cycles;
		while (M.soundCycles >= 32) {
			M.soundCycles -= 32;
			soundTick(sv);
		}
	}
	M.frameCycles -= frame;
	draw(sv);
	if (M.reg[BANK] & BANK_NMI) {
		sv->cpu.nmi = true;
	}
	soundFrame(sv);
	machineFromCpu(sv);
}

size_t svStateSize(void) {
	return sizeof(struct SvMachine);
}

void svSave(const struct Supervision* sv, void* out) {
	memcpy(out, &sv->m, sizeof(sv->m));
}

bool svLoadState(struct Supervision* sv, const void* in, size_t size) {
	if (size < sizeof(sv->m)) {
		return false;
	}
	memcpy(&sv->m, in, sizeof(sv->m));
	cpuFromMachine(sv);
	return true;
}
