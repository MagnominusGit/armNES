/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

// Mapper chips: MMC2/MMC4, Sunsoft FME-7, Konami VRC2/VRC4, Taito TC0190, Jaleco SS88006 and Namco
// 163/175/340, after the NESdev wiki's descriptions.

// MMC2 (9) and MMC4 (10): two CHR banks per half, switched by fetching tile $FD or $FE
struct Mmc2 {
	uint8_t prg, chr[4], latch[2], mirroring;
};

static void mmc2Sync(struct NES* nes) {
	struct Mmc2* m = STATE(Mmc2);
	if (nes->cart.mapper == 9) {
		prg8(nes, 0x8000, m->prg & 15);
		prg8(nes, 0xA000, -3);
		prg8(nes, 0xC000, -2);
		prg8(nes, 0xE000, -1);
	} else {
		prg16(nes, 0x8000, m->prg & 15);
		prg16(nes, 0xC000, -1);
	}
	chr4(nes, 0x0000, m->chr[m->latch[0]]);
	chr4(nes, 0x1000, m->chr[2 + m->latch[1]]);
	nesSetMirroring(nes, (m->mirroring & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
}

static void mmc2Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Mmc2* m = STATE(Mmc2);
	switch (address & 0xF000) {
	case 0xA000:
		m->prg = value;
		break;
	case 0xB000:
	case 0xC000:
	case 0xD000:
	case 0xE000:
		m->chr[((address >> 12) - 0xB)] = value & 0x1F;
		break;
	case 0xF000:
		m->mirroring = value;
		break;
	default:
		return;
	}
	mmc2Sync(nes);
}

static uint8_t mmc2PpuRead(struct NES* nes, uint16_t address) {
	struct Mmc2* m = STATE(Mmc2);
	uint8_t* page = nes->chrPages[(address >> 10) & 15];
	uint8_t value = page ? page[address & 0x3FF] : 0;
	// The bank switches after the fetch that triggers it
	uint16_t tile = address & 0x0FF8;
	int half = (address >> 12) & 1;
	if (address < 0x2000 && (tile == 0x0FD8 || tile == 0x0FE8)) {
		// MMC2's first half switches on one address only
		if (half || nes->cart.mapper == 10 || (address & 7) == 0) {
			uint8_t latch = tile == 0x0FE8;
			if (m->latch[half] != latch) {
				m->latch[half] = latch;
				chr4(nes, half ? 0x1000 : 0x0000, m->chr[half * 2 + latch]);
			}
		}
	}
	return value;
}

static void mmc2Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Mmc2));
	}
	nes->ppuReadHook = true;
	wram8(nes);
	mmc2Sync(nes);
}

const struct Board boardMmc2 = {
	.name = "MMC2/MMC4", .reset = mmc2Reset, .write = mmc2Write, .ppuRead = mmc2PpuRead, .sync = mmc2Sync,
	.stateSize = sizeof(struct Mmc2),
};

// Sunsoft FME-7 (69)
// Sunsoft 5B's sound (FME-7's sound version): an AY-3-8910's three square channels, noise and
// envelope, clocked at half the CPU's rate and counted in 16s: a tone flips every period, noise
// shifts every two, and the envelope's 32 steps (a YM2149's) take one each
struct Fme7 {
	uint8_t command, regs[16];
	uint16_t counter;
	bool irqEnabled, counting;
	struct Sunsoft5b audio;
};

// 3 dB per volume step, 0 to 15
static const uint16_t ayVolume[16] = { 0, 45, 63, 89, 126, 178, 251, 355, 501, 708, 1000, 1413, 1995, 2818, 3981, 5623 };

void sunsoft5bWrite(struct Sunsoft5b* a, uint8_t value) {
	a->regs[a->address & 15] = value;
	if ((a->address & 15) == 13) {
		a->envelopeStep = 0;
		a->envelopeHolding = false;
		a->envelopeTimer = 0;
	}
}

void sunsoft5bCycle(struct Sunsoft5b* a) {
	if (++a->divider < 16) {
		return;
	}
	a->divider = 0;
	for (int i = 0; i < 3; i++) {
		uint16_t period = a->regs[i * 2] | ((a->regs[i * 2 + 1] & 15) << 8);
		if (++a->toneTimer[i] >= (period ? period : 1)) {
			a->toneTimer[i] = 0;
			a->toneOut[i] = !a->toneOut[i];
		}
	}
	uint8_t noisePeriod = a->regs[6] & 0x1F;
	if (++a->noiseTimer >= (noisePeriod ? noisePeriod : 1) * 2) {
		a->noiseTimer = 0;
		uint32_t bit = (a->noiseShift ^ (a->noiseShift >> 3)) & 1;
		a->noiseShift = (a->noiseShift >> 1) | (bit << 16);
	}
	uint32_t envelopePeriod = a->regs[11] | (a->regs[12] << 8);
	if (!a->envelopeHolding && ++a->envelopeTimer >= (envelopePeriod ? envelopePeriod : 1)) {
		a->envelopeTimer = 0;
		if (++a->envelopeStep >= 32) {
			uint8_t shape = a->regs[13];
			if (!(shape & 8) || (shape & 1)) {
				a->envelopeHolding = true;
				a->envelopeStep = 31;
			} else {
				a->envelopeStep = 0;
			}
		}
	}
}

int sunsoft5bOutput(const struct Sunsoft5b* a) {
	uint8_t shape = a->regs[13];
	int level = a->envelopeStep >> 1;
	bool attack = shape & 4;
	if (a->envelopeHolding) {
		// After the first cycle: hold at the end, or at the start for shapes that alternate
		bool flip = (shape & 8) && (shape & 2);
		level = (attack ^ flip) ? 15 : 0;
		if (!(shape & 8)) {
			level = 0;
		}
	} else {
		bool alternate = (shape & 8) && (shape & 2) && (a->envelopeStep >= 16);
		level = (attack ^ alternate) ? level : 15 - level;
	}
	int out = 0;
	for (int i = 0; i < 3; i++) {
		bool toneOff = a->regs[7] & (1 << i), noiseOff = a->regs[7] & (8 << i);
		bool on = (toneOff || a->toneOut[i]) && (noiseOff || (a->noiseShift & 1));
		if (on) {
			uint8_t volume = a->regs[8 + i];
			out += ayVolume[(volume & 0x10) ? level : volume & 15];
		}
	}
	return out;
}

static void fme7Sync(struct NES* nes) {
	struct Fme7* f = STATE(Fme7);
	for (int i = 0; i < 8; i++) {
		chr1(nes, i * 0x400, f->regs[i]);
	}
	uint8_t r8 = f->regs[8];
	if (r8 & 0x40) {
		if (r8 & 0x80) {
			nesMapWram(nes, 0x6000, 0x2000, r8 & 0x3F, true);
		} else {
			nesUnmapPrg(nes, 0x6000, 0x2000);
		}
	} else {
		prg8(nes, 0x6000, r8 & 0x3F);
	}
	prg8(nes, 0x8000, f->regs[9] & 0x3F);
	prg8(nes, 0xA000, f->regs[10] & 0x3F);
	prg8(nes, 0xC000, f->regs[11] & 0x3F);
	prg8(nes, 0xE000, -1);
	static const enum Mirroring mirrors[4] = { MIRROR_VERTICAL, MIRROR_HORIZONTAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B };
	nesSetMirroring(nes, mirrors[f->regs[12] & 3]);
}

static void fme7Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Fme7* f = STATE(Fme7);
	if (address >= 0xC000) {
		if (address < 0xE000) {
			f->audio.address = value;
		} else {
			sunsoft5bWrite(&f->audio, value);
		}
		return;
	}
	switch (address & 0xE000) {
	case 0x8000:
		f->command = value & 15;
		return;
	case 0xA000:
		f->regs[f->command] = value;
		switch (f->command) {
		case 13:
			f->irqEnabled = value & 1;
			f->counting = value & 0x80;
			nesSetIrq(nes, IRQ_MAPPER, false);
			return;
		case 14:
			f->counter = (f->counter & 0xFF00) | value;
			return;
		case 15:
			f->counter = (f->counter & 0x00FF) | (value << 8);
			return;
		}
		fme7Sync(nes);
		return;
	}
}

static void fme7Cycle(struct NES* nes) {
	struct Fme7* f = STATE(Fme7);
	sunsoft5bCycle(&f->audio);
	if (f->counting && f->counter-- == 0 && f->irqEnabled) {
		nesSetIrq(nes, IRQ_MAPPER, true);
	}
}

static int fme7Audio(struct NES* nes) {
	return sunsoft5bOutput(&STATE(Fme7)->audio);
}

static void fme7Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Fme7));
		STATE(Fme7)->audio.noiseShift = 1;
	}
	fme7Sync(nes);
}

const struct Board boardFme7 = {
	.name = "FME-7", .reset = fme7Reset, .write = fme7Write, .cpuCycle = fme7Cycle, .audio = fme7Audio, .sync = fme7Sync,
	.stateSize = sizeof(struct Fme7),
};

// Konami's IRQ (VRC4, VRC6, VRC7): a counter clocked every CPU cycle or every scanline's worth of
// them (341/3), reloading and raising the IRQ when it passes $FF
struct VrcIrq {
	uint8_t latch, counter, control;
	int16_t prescaler;
};

static void vrcIrqControl(struct NES* nes, struct VrcIrq* irq, uint8_t value) {
	irq->control = value & 7;
	if (value & 2) {
		irq->counter = irq->latch;
		irq->prescaler = 341;
	}
	nesSetIrq(nes, IRQ_MAPPER, false);
}

static void vrcIrqAck(struct NES* nes, struct VrcIrq* irq) {
	irq->control = (irq->control & ~2) | ((irq->control & 1) << 1);
	nesSetIrq(nes, IRQ_MAPPER, false);
}

static void vrcIrqCycle(struct NES* nes, struct VrcIrq* irq) {
	if (!(irq->control & 2)) {
		return;
	}
	if (!(irq->control & 4)) {
		irq->prescaler -= 3;
		if (irq->prescaler > 0) {
			return;
		}
		irq->prescaler += 341;
	}
	if (irq->counter == 0xFF) {
		irq->counter = irq->latch;
		nesSetIrq(nes, IRQ_MAPPER, true);
	} else {
		irq->counter++;
	}
}

// VRC2 and VRC4 (21, 22, 23, 25): the boards wire different address lines to the chip's A0 and A1;
// with no submapper both candidates are ORed together
struct Vrc24 {
	uint8_t prg[2], mirroring, swap;
	uint16_t chr[8];
	struct VrcIrq irq;
};

struct VrcWiring {
	uint8_t a0, a1, a0b, a1b;
	bool vrc2;
	uint8_t chrShift;
};

static struct VrcWiring vrcWiring(const struct NES* nes) {
	int mapper = nes->cart.mapper, sub = nes->cart.submapper;
	switch (mapper) {
	case 21:
		return sub == 1 ? (struct VrcWiring) { 1, 2, 1, 2, false, 0 }
			: sub == 2 ? (struct VrcWiring) { 6, 7, 6, 7, false, 0 } : (struct VrcWiring) { 1, 2, 6, 7, false, 0 };
	case 22:
		return (struct VrcWiring) { 1, 0, 1, 0, true, 1 };
	case 23:
		return sub == 1 ? (struct VrcWiring) { 0, 1, 0, 1, false, 0 }
			: sub == 2 ? (struct VrcWiring) { 2, 3, 2, 3, false, 0 }
			: sub == 3 ? (struct VrcWiring) { 0, 1, 0, 1, true, 0 } : (struct VrcWiring) { 0, 1, 2, 3, false, 0 };
	default:
		return sub == 1 ? (struct VrcWiring) { 1, 0, 1, 0, false, 0 }
			: sub == 2 ? (struct VrcWiring) { 3, 2, 3, 2, false, 0 }
			: sub == 3 ? (struct VrcWiring) { 1, 0, 1, 0, true, 0 } : (struct VrcWiring) { 1, 0, 3, 2, false, 0 };
	}
}

static void vrc24Sync(struct NES* nes) {
	struct Vrc24* v = STATE(Vrc24);
	struct VrcWiring w = vrcWiring(nes);
	if (v->swap & 2) {
		prg8(nes, 0x8000, -2);
		prg8(nes, 0xC000, v->prg[0]);
	} else {
		prg8(nes, 0x8000, v->prg[0]);
		prg8(nes, 0xC000, -2);
	}
	prg8(nes, 0xA000, v->prg[1]);
	prg8(nes, 0xE000, -1);
	for (int i = 0; i < 8; i++) {
		chr1(nes, i * 0x400, v->chr[i] >> w.chrShift);
	}
	if (w.vrc2) {
		nesSetMirroring(nes, (v->mirroring & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	} else {
		static const enum Mirroring mirrors[4] = { MIRROR_VERTICAL, MIRROR_HORIZONTAL, MIRROR_SCREEN_A,
			MIRROR_SCREEN_B };
		nesSetMirroring(nes, mirrors[v->mirroring & 3]);
	}
}

static void vrc24Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Vrc24* v = STATE(Vrc24);
	if (address < 0x8000) {
		return;
	}
	struct VrcWiring w = vrcWiring(nes);
	int sub = (((address >> w.a0) | (address >> w.a0b)) & 1) | ((((address >> w.a1) | (address >> w.a1b)) & 1) << 1);
	switch (address & 0xF000) {
	case 0x8000:
		v->prg[0] = value & 0x1F;
		break;
	case 0x9000:
		if (w.vrc2 || sub < 2) {
			v->mirroring = value;
		} else {
			v->swap = value;
		}
		break;
	case 0xA000:
		v->prg[1] = value & 0x1F;
		break;
	case 0xF000:
		if (w.vrc2) {
			return;
		}
		switch (sub) {
		case 0:
			v->irq.latch = (v->irq.latch & 0xF0) | (value & 15);
			break;
		case 1:
			v->irq.latch = (v->irq.latch & 0x0F) | (value << 4);
			break;
		case 2:
			vrcIrqControl(nes, &v->irq, value);
			break;
		case 3:
			vrcIrqAck(nes, &v->irq);
			break;
		}
		return;
	default: {
		int index = (((address >> 12) - 0xB) << 1) | (sub >> 1);
		if (sub & 1) {
			v->chr[index] = (v->chr[index] & 0x0F) | ((value & 0x1F) << 4);
		} else {
			v->chr[index] = (v->chr[index] & 0x1F0) | (value & 15);
		}
		break;
	}
	}
	vrc24Sync(nes);
}

static void vrc24Cycle(struct NES* nes) {
	vrcIrqCycle(nes, &STATE(Vrc24)->irq);
}

static void vrc24Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Vrc24));
	}
	wram8(nes);
	vrc24Sync(nes);
}

const struct Board boardVrc24 = {
	.name = "VRC2/VRC4", .reset = vrc24Reset, .write = vrc24Write, .cpuCycle = vrc24Cycle, .sync = vrc24Sync,
	.stateSize = sizeof(struct Vrc24),
};

// VRC2 alone has no IRQ, so it skips the per-cycle clock
const struct Board boardVrc2 = {
	.name = "VRC2", .reset = vrc24Reset, .write = vrc24Write, .sync = vrc24Sync, .stateSize = sizeof(struct Vrc24),
};

// Taito TC0190 (33)
struct Tc0190 {
	uint8_t prg[2], chr[6], mirroring;
};

static void tc0190Sync(struct NES* nes) {
	struct Tc0190* t = STATE(Tc0190);
	prg8(nes, 0x8000, t->prg[0]);
	prg8(nes, 0xA000, t->prg[1]);
	prg8(nes, 0xC000, -2);
	prg8(nes, 0xE000, -1);
	chr2(nes, 0x0000, t->chr[0]);
	chr2(nes, 0x0800, t->chr[1]);
	for (int i = 0; i < 4; i++) {
		chr1(nes, 0x1000 + i * 0x400, t->chr[2 + i]);
	}
	nesSetMirroring(nes, t->mirroring ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
}

static void tc0190Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Tc0190* t = STATE(Tc0190);
	switch (address & 0xA003) {
	case 0x8000:
		t->prg[0] = value & 0x3F;
		t->mirroring = value & 0x40;
		break;
	case 0x8001:
		t->prg[1] = value & 0x3F;
		break;
	case 0x8002:
	case 0x8003:
		t->chr[address & 1] = value;
		break;
	case 0xA000:
	case 0xA001:
	case 0xA002:
	case 0xA003:
		t->chr[2 + (address & 3)] = value;
		break;
	default:
		return;
	}
	tc0190Sync(nes);
}

static void tc0190Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Tc0190));
	}
	tc0190Sync(nes);
}

const struct Board boardTc0190 = {
	.name = "TC0190", .reset = tc0190Reset, .write = tc0190Write, .sync = tc0190Sync,
	.stateSize = sizeof(struct Tc0190),
};

// Jaleco SS88006 (18): registers written a nibble at a time, a down-counting IRQ of 4 to 16 bits
struct Ss88006 {
	uint8_t prg[3], chr[8], mirroring, control;
	uint16_t reload, counter;
};

static void ss88006Sync(struct NES* nes) {
	struct Ss88006* s = STATE(Ss88006);
	for (int i = 0; i < 3; i++) {
		prg8(nes, 0x8000 + i * 0x2000, s->prg[i]);
	}
	prg8(nes, 0xE000, -1);
	for (int i = 0; i < 8; i++) {
		chr1(nes, i * 0x400, s->chr[i]);
	}
	static const enum Mirroring mirrors[4] = { MIRROR_HORIZONTAL, MIRROR_VERTICAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B };
	nesSetMirroring(nes, mirrors[s->mirroring & 3]);
}

static inline void setNibble(uint8_t* reg, int high, uint8_t value) {
	*reg = high ? (*reg & 0x0F) | ((value & 15) << 4) : (*reg & 0xF0) | (value & 15);
}

static void ss88006Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Ss88006* s = STATE(Ss88006);
	int a = address & 0xF003;
	if (a >= 0x8000 && a < 0x9002) {
		int index = ((a - 0x8000) >> 11) | ((a >> 1) & 1);
		setNibble(&s->prg[index > 2 ? 2 : index], a & 1, value);
	} else if (a >= 0xA000 && a < 0xE000) {
		int index = (((a >> 12) - 0xA) << 1) | ((a >> 1) & 1);
		setNibble(&s->chr[index], a & 1, value);
	} else if (a >= 0xE000 && a <= 0xE003) {
		int shift = (a & 3) * 4;
		s->reload = (s->reload & ~(15 << shift)) | ((value & 15) << shift);
		return;
	} else if (a == 0xF000) {
		s->counter = s->reload;
		nesSetIrq(nes, IRQ_MAPPER, false);
		return;
	} else if (a == 0xF001) {
		s->control = value;
		nesSetIrq(nes, IRQ_MAPPER, false);
		return;
	} else if (a == 0xF002) {
		s->mirroring = value;
	} else {
		return;
	}
	ss88006Sync(nes);
}

static void ss88006Cycle(struct NES* nes) {
	struct Ss88006* s = STATE(Ss88006);
	if (!(s->control & 1)) {
		return;
	}
	uint16_t mask = (s->control & 8) ? 0x000F : (s->control & 4) ? 0x00FF : (s->control & 2) ? 0x0FFF : 0xFFFF;
	uint16_t low = (s->counter & mask) - 1;
	if ((s->counter & mask) == 0) {
		nesSetIrq(nes, IRQ_MAPPER, true);
	}
	s->counter = (s->counter & ~mask) | (low & mask);
}

static void ss88006Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Ss88006));
	}
	wram8(nes);
	ss88006Sync(nes);
}

const struct Board boardSs88006 = {
	.name = "SS88006", .reset = ss88006Reset, .write = ss88006Write, .cpuCycle = ss88006Cycle, .sync = ss88006Sync,
	.stateSize = sizeof(struct Ss88006),
};

// Namco 163 (19) and 175/340 (210): 1 KB CHR banks that can be CIRAM, a 15-bit IRQ counter and
// 128 bytes of RAM that hold the wavetable channels
struct Namco163 {
	uint8_t chr[8], nt[4], prg[3];
	uint16_t counter;
	struct Namco163Sound sound;
	uint8_t mirroring;
};

// The RAM through the data port, stepping the address when it's set to
uint8_t namco163SoundRead(struct Namco163Sound* n) {
	uint8_t value = n->ram[n->address & 0x7F];
	if (n->address & 0x80) {
		n->address = 0x80 | ((n->address + 1) & 0x7F);
	}
	return value;
}

void namco163SoundWrite(struct Namco163Sound* n, uint8_t value) {
	n->ram[n->address & 0x7F] = value;
	if (n->address & 0x80) {
		n->address = 0x80 | ((n->address + 1) & 0x7F);
	}
}

// One channel stepped every 15 cycles, from the last down
void namco163SoundCycle(struct Namco163Sound* n, bool disabled) {
	if (++n->tick < 15 || disabled) {
		return;
	}
	n->tick = 0;
	int channels = ((n->ram[0x7F] >> 4) & 7) + 1;
	int c = 7 - n->channel;
	uint8_t* r = &n->ram[0x40 + c * 8];
	uint32_t freq = r[0] | (r[2] << 8) | ((r[4] & 3) << 16);
	uint32_t length = (256 - (r[4] & 0xFC)) << 16;
	uint32_t phase = r[1] | (r[3] << 8) | (r[5] << 16);
	phase = (phase + freq) % length;
	r[1] = phase;
	r[3] = phase >> 8;
	r[5] = phase >> 16;
	uint8_t index = ((phase >> 16) + r[6]) & 0xFF;
	int sample = (n->ram[index >> 1] >> ((index & 1) * 4)) & 15;
	n->output[c] = (sample - 8) * (r[7] & 15);
	n->channel = (n->channel + 1) % channels;
}

int namco163SoundOutput(const struct Namco163Sound* n) {
	int channels = ((n->ram[0x7F] >> 4) & 7) + 1, sum = 0;
	for (int c = 8 - channels; c < 8; c++) {
		sum += n->output[c];
	}
	return sum * 64 / channels;
}

static void namco163Sync(struct NES* nes) {
	struct Namco163* n = STATE(Namco163);
	bool is163 = nes->cart.mapper == 19;
	for (int i = 0; i < 8; i++) {
		bool ciramAllowed = is163 && !(n->prg[1] & (i < 4 ? 0x40 : 0x80));
		if (ciramAllowed && n->chr[i] >= 0xE0) {
			nes->chrPages[i] = nes->ppu.ntRam + (n->chr[i] & 1) * 0x400;
			nes->chrWritable[i] = true;
		} else {
			chr1(nes, i * 0x400, n->chr[i]);
		}
	}
	if (is163) {
		for (int i = 0; i < 4; i++) {
			if (n->nt[i] >= 0xE0) {
				nesSetNametable(nes, i, nes->ppu.ntRam + (n->nt[i] & 1) * 0x400);
			} else {
				nesMapChr(nes, 0x2000 + i * 0x400, 0x400, n->nt[i]);
				nes->chrPages[12 + i] = nes->chrPages[8 + i];
				nes->chrWritable[8 + i] = nes->chrWritable[12 + i] = false;
			}
		}
	} else if (nes->cart.submapper == 2) {
		static const enum Mirroring mirrors[4] = { MIRROR_SCREEN_A, MIRROR_VERTICAL, MIRROR_SCREEN_B,
			MIRROR_HORIZONTAL };
		nesSetMirroring(nes, mirrors[n->prg[0] >> 6]);
	}
	for (int i = 0; i < 3; i++) {
		prg8(nes, 0x8000 + i * 0x2000, n->prg[i] & 0x3F);
	}
	prg8(nes, 0xE000, -1);
}

static bool hasNamco175Ram(const struct NES* nes) {
	return nes->cart.mapper == 210 && nes->cart.submapper == 1;
}

static uint8_t namco163Read(struct NES* nes, uint16_t address) {
	struct Namco163* n = STATE(Namco163);
	// Namco 175: 2 KB of RAM over $6000-$7FFF, read and written while $C000's bit 0 allows
	if (hasNamco175Ram(nes) && (address & 0xE000) == 0x6000) {
		return n->nt[0] & 1 ? nes->cart.wram[address & 0x7FF] : nes->cpu.openBus;
	}
	switch (address & 0xF800) {
	case 0x4800:
		return namco163SoundRead(&n->sound);
	case 0x5000:
		return n->counter & 0xFF;
	case 0x5800:
		return n->counter >> 8;
	}
	return nes->cpu.openBus;
}

static void namco163Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Namco163* n = STATE(Namco163);
	if (hasNamco175Ram(nes) && (address & 0xE000) == 0x6000) {
		if (n->nt[0] & 1) {
			nes->cart.wram[address & 0x7FF] = value;
		}
		return;
	}
	switch (address & 0xF800) {
	case 0x4800:
		namco163SoundWrite(&n->sound, value);
		return;
	case 0x5000:
		n->counter = (n->counter & 0xFF00) | value;
		nesSetIrq(nes, IRQ_MAPPER, false);
		return;
	case 0x5800:
		n->counter = (n->counter & 0x00FF) | (value << 8);
		nesSetIrq(nes, IRQ_MAPPER, false);
		return;
	case 0xF800:
		n->sound.address = value;
		return;
	}
	if (address >= 0x8000 && address < 0xC000) {
		n->chr[(address - 0x8000) >> 11] = value;
	} else if (address >= 0xC000 && address < 0xE000) {
		n->nt[(address - 0xC000) >> 11] = value;
	} else if (address >= 0xE000 && address < 0xF800) {
		n->prg[(address - 0xE000) >> 11] = value;
	} else {
		return;
	}
	namco163Sync(nes);
}

static void namco163Cycle(struct NES* nes) {
	struct Namco163* n = STATE(Namco163);
	if ((n->counter & 0x8000) && (n->counter & 0x7FFF) != 0x7FFF && ++n->counter == 0xFFFF) {
		nesSetIrq(nes, IRQ_MAPPER, true);
	}
	namco163SoundCycle(&n->sound, n->prg[0] & 0x40);
}

static int namco163Audio(struct NES* nes) {
	if (nes->cart.mapper != 19) {
		return 0;
	}
	return namco163SoundOutput(&STATE(Namco163)->sound);
}

static void namco163Reset(struct NES* nes, bool hard) {
	if (hard) {
		// A battery keeps the sound RAM through a power cycle
		struct Namco163* n = STATE(Namco163);
		uint8_t ram[sizeof(n->sound.ram)];
		memcpy(ram, n->sound.ram, sizeof(ram));
		memset(nes->boardState, 0, sizeof(struct Namco163));
		if (nes->cart.mapper == 19 && nes->cart.battery) {
			memcpy(n->sound.ram, ram, sizeof(ram));
		}
	}
	if (nes->cart.mapper == 19) {
		nes->readHooks |= 1 << 4 | 1 << 5;
	}
	if (hasNamco175Ram(nes)) {
		// The RAM starts enabled, until $C000 says otherwise
		if (hard) {
			STATE(Namco163)->nt[0] = 1;
		}
		nesUnmapPrg(nes, 0x6000, 0x2000);
		nes->readHooks |= 1 << 6 | 1 << 7;
	} else {
		wram8(nes);
	}
	namco163Sync(nes);
}

// With a battery, the 128 bytes of sound RAM keep a game's save (some have no other)
static uint8_t* namco163BatteryRam(struct NES* nes, size_t* size) {
	if (nes->cart.mapper != 19 || !nes->cart.battery) {
		return NULL;
	}
	*size = sizeof(STATE(Namco163)->sound.ram);
	return STATE(Namco163)->sound.ram;
}

const struct Board boardNamco163 = {
	.name = "Namco 163", .reset = namco163Reset, .write = namco163Write, .read = namco163Read,
	.cpuCycle = namco163Cycle, .audio = namco163Audio, .sync = namco163Sync, .stateSize = sizeof(struct Namco163),
	.batteryRam = namco163BatteryRam,
};

const struct Board boardNamco175 = {
	.name = "Namco 175/340", .reset = namco163Reset, .write = namco163Write, .read = namco163Read, .sync = namco163Sync,
	.stateSize = sizeof(struct Namco163),
};

// VRC1 (75): 4 KB CHR banks with their fifth bits in the mirroring register
struct Vrc1 {
	uint8_t prg[3], chr[2], control;
};

static void vrc1Sync(struct NES* nes) {
	struct Vrc1* v = STATE(Vrc1);
	for (int i = 0; i < 3; i++) {
		prg8(nes, 0x8000 + i * 0x2000, v->prg[i]);
	}
	prg8(nes, 0xE000, -1);
	chr4(nes, 0x0000, (v->chr[0] & 15) | ((v->control & 2) << 3));
	chr4(nes, 0x1000, (v->chr[1] & 15) | ((v->control & 4) << 2));
	if (nes->cart.mirroring != MIRROR_FOUR) {
		nesSetMirroring(nes, (v->control & 1) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	}
}

static void vrc1Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Vrc1* v = STATE(Vrc1);
	switch (address & 0xF000) {
	// The chip has 4 PRG bits; the rest reach the larger ROMs re-releases use, and wrap to the same
	// banks on the original carts
	case 0x8000:
		v->prg[0] = value;
		break;
	case 0x9000:
		v->control = value;
		break;
	case 0xA000:
		v->prg[1] = value;
		break;
	case 0xC000:
		v->prg[2] = value;
		break;
	case 0xE000:
		v->chr[0] = value;
		break;
	case 0xF000:
		v->chr[1] = value;
		break;
	default:
		return;
	}
	vrc1Sync(nes);
}

static void vrc1Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Vrc1));
	}
	vrc1Sync(nes);
}

const struct Board boardVrc1 = {
	.name = "VRC1", .reset = vrc1Reset, .write = vrc1Write, .sync = vrc1Sync, .stateSize = sizeof(struct Vrc1),
};

// VRC3 (73): a 16 KB PRG bank and a 16-bit (or 8-bit) up-counting IRQ
struct Vrc3 {
	uint8_t prg, control;
	uint16_t latch, counter;
};

static void vrc3Sync(struct NES* nes) {
	prg16(nes, 0x8000, STATE(Vrc3)->prg);
	prg16(nes, 0xC000, -1);
}

static void vrc3Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Vrc3* v = STATE(Vrc3);
	if (address >= 0x8000 && address < 0xC000) {
		int shift = ((address >> 12) - 8) * 4;
		v->latch = (v->latch & ~(15 << shift)) | ((value & 15) << shift);
		return;
	}
	switch (address & 0xF000) {
	case 0xC000:
		v->control = value & 7;
		if (value & 2) {
			v->counter = v->latch;
		}
		nesSetIrq(nes, IRQ_MAPPER, false);
		break;
	case 0xD000:
		v->control = (v->control & ~2) | ((v->control & 1) << 1);
		nesSetIrq(nes, IRQ_MAPPER, false);
		break;
	case 0xF000:
		v->prg = value & 7;
		vrc3Sync(nes);
		break;
	}
}

static void vrc3Cycle(struct NES* nes) {
	struct Vrc3* v = STATE(Vrc3);
	if (!(v->control & 2)) {
		return;
	}
	if (v->control & 4) {
		uint8_t low = v->counter + 1;
		if (low == 0) {
			low = v->latch;
			nesSetIrq(nes, IRQ_MAPPER, true);
		}
		v->counter = (v->counter & 0xFF00) | low;
	} else if (++v->counter == 0) {
		v->counter = v->latch;
		nesSetIrq(nes, IRQ_MAPPER, true);
	}
}

static void vrc3Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Vrc3));
	}
	wram8(nes);
	vrc3Sync(nes);
}

const struct Board boardVrc3 = {
	.name = "VRC3", .reset = vrc3Reset, .write = vrc3Write, .cpuCycle = vrc3Cycle, .sync = vrc3Sync,
	.stateSize = sizeof(struct Vrc3),
};

// VRC6 (24, and 26 with A0 and A1 swapped): two pulse channels and a sawtooth beside the APU
struct Vrc6 {
	uint8_t prg[2], chr[8], banking;
	struct VrcIrq irq;
	struct Vrc6Sound sound;
};

// A pulse's (channel 0 and 1) or the sawtooth's (2) register 0 to 2
void vrc6SoundWrite(struct Vrc6Sound* v, int channel, int reg, uint8_t value) {
	if (channel < 2) {
		struct Vrc6Pulse* p = &v->pulse[channel];
		if (reg == 0) {
			p->control = value;
		} else if (reg == 1) {
			p->period = (p->period & 0xF00) | value;
		} else {
			p->period = (p->period & 0x0FF) | ((value & 15) << 8);
			p->enabled = value & 0x80;
			if (!p->enabled) {
				p->step = 0;
			}
		}
	} else if (reg == 0) {
		v->sawRate = value & 0x3F;
	} else if (reg == 1) {
		v->sawPeriod = (v->sawPeriod & 0xF00) | value;
	} else {
		v->sawPeriod = (v->sawPeriod & 0x0FF) | ((value & 15) << 8);
		v->sawEnabled = value & 0x80;
		if (!v->sawEnabled) {
			v->sawStep = 0;
			v->sawAccumulator = 0;
		}
	}
}

void vrc6SoundCycle(struct Vrc6Sound* v) {
	for (int i = 0; i < 2; i++) {
		struct Vrc6Pulse* p = &v->pulse[i];
		if (p->enabled && p->timer-- == 0) {
			p->timer = p->period;
			p->step = (p->step + 1) & 15;
		}
	}
	if (v->sawEnabled && v->sawTimer-- == 0) {
		v->sawTimer = v->sawPeriod;
		if (++v->sawStep == 14) {
			v->sawStep = 0;
			v->sawAccumulator = 0;
		} else if (!(v->sawStep & 1)) {
			v->sawAccumulator += v->sawRate;
		}
	}
}

int vrc6SoundOutput(const struct Vrc6Sound* v) {
	int out = 0;
	for (int i = 0; i < 2; i++) {
		const struct Vrc6Pulse* p = &v->pulse[i];
		if (p->enabled && ((p->control & 0x80) || p->step <= ((p->control >> 4) & 7))) {
			out += p->control & 15;
		}
	}
	if (v->sawEnabled) {
		out += v->sawAccumulator >> 3;
	}
	return out * 380;
}

static void vrc6Sync(struct NES* nes) {
	struct Vrc6* v = STATE(Vrc6);
	prg16(nes, 0x8000, v->prg[0]);
	prg8(nes, 0xC000, v->prg[1]);
	prg8(nes, 0xE000, -1);
	for (int i = 0; i < 8; i++) {
		chr1(nes, i * 0x400, v->chr[i]);
	}
	static const enum Mirroring mirrors[4] = { MIRROR_VERTICAL, MIRROR_HORIZONTAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B };
	nesSetMirroring(nes, mirrors[(v->banking >> 2) & 3]);
	if (v->banking & 0x80) {
		wram8(nes);
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
	}
}

static void vrc6Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Vrc6* v = STATE(Vrc6);
	if (address < 0x8000) {
		return;
	}
	int sub = nes->cart.mapper == 26 ? ((address & 1) << 1) | ((address >> 1) & 1) : address & 3;
	switch (address & 0xF000) {
	case 0x8000:
		v->prg[0] = value & 15;
		break;
	case 0x9000:
	case 0xA000:
	case 0xB000:
		if (sub < 3) {
			vrc6SoundWrite(&v->sound, (address >> 12) - 9, sub, value);
			return;
		}
		if ((address & 0xF000) != 0xB000) {
			return;
		}
		v->banking = value;
		break;
	case 0xC000:
		v->prg[1] = value & 0x1F;
		break;
	case 0xD000:
	case 0xE000:
		v->chr[((address >> 12) - 0xD) * 4 + sub] = value;
		break;
	case 0xF000:
		if (sub == 0) {
			v->irq.latch = value;
		} else if (sub == 1) {
			vrcIrqControl(nes, &v->irq, value);
		} else if (sub == 2) {
			vrcIrqAck(nes, &v->irq);
		}
		return;
	}
	vrc6Sync(nes);
}

static void vrc6Cycle(struct NES* nes) {
	struct Vrc6* v = STATE(Vrc6);
	vrcIrqCycle(nes, &v->irq);
	vrc6SoundCycle(&v->sound);
}

static int vrc6Audio(struct NES* nes) {
	return vrc6SoundOutput(&STATE(Vrc6)->sound);
}

static void vrc6Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Vrc6));
	}
	vrc6Sync(nes);
}

const struct Board boardVrc6 = {
	.name = "VRC6", .reset = vrc6Reset, .write = vrc6Write, .cpuCycle = vrc6Cycle, .audio = vrc6Audio,
	.sync = vrc6Sync, .stateSize = sizeof(struct Vrc6),
};

// VRC7 (85): VRC6's banking layout with an FM synthesizer, emu2413 in its VRC7 mode at the OPLL's
// own rate (3.58 MHz / 72, once every 36 CPU cycles). States keep the synthesizer's own state too.
#include "sound/emu2413.h"

struct Vrc7 {
	uint8_t prg[3], chr[8], control;
	struct VrcIrq irq;
	struct Vrc7Sound sound;
};

static OPLL* opll;

void vrc7SoundReset(struct Vrc7Sound* v, bool hard) {
	if (!opll) {
		opll = OPLL_new(3579545, 49716);
		OPLL_setChipType(opll, 1);
	}
	if (hard) {
		memset(v, 0, sizeof(*v));
		OPLL_reset(opll);
	}
}

// Field by field, so a state reads the same on every host. The rest (patch and wave table pointers,
// levels and rates worked out from these) emu2413 works out again on its refresh.
size_t vrc7SoundState(uint8_t* data, bool load) {
	size_t size = 0;
#define FIELD(x) \
	do { \
		if (data && load) { \
			memcpy(&(x), data + size, sizeof(x)); \
		} else if (data) { \
			memcpy(data + size, &(x), sizeof(x)); \
		} \
		size += sizeof(x); \
	} while (0)
	FIELD(opll->adr);
	FIELD(opll->out_time);
	FIELD(opll->reg);
	FIELD(opll->test_flag);
	FIELD(opll->slot_key_status);
	FIELD(opll->rhythm_mode);
	FIELD(opll->eg_counter);
	FIELD(opll->pm_phase);
	FIELD(opll->am_phase);
	FIELD(opll->lfo_am);
	FIELD(opll->noise);
	FIELD(opll->short_noise);
	FIELD(opll->patch_number);
	FIELD(opll->patch[0]);
	FIELD(opll->patch[1]);
	FIELD(opll->ch_out);
	FIELD(opll->mix_out);
	for (int i = 0; i < 18; i++) {
		OPLL_SLOT* s = &opll->slot[i];
		FIELD(s->type);
		FIELD(s->output);
		FIELD(s->pg_phase);
		FIELD(s->pg_out);
		FIELD(s->pg_keep);
		FIELD(s->blk_fnum);
		FIELD(s->fnum);
		FIELD(s->blk);
		FIELD(s->eg_state);
		FIELD(s->volume);
		FIELD(s->key_flag);
		FIELD(s->sus_flag);
		FIELD(s->eg_out);
	}
#undef FIELD
	if (data && load) {
		OPLL_forceRefresh(opll);
	}
	return size;
}

void vrc7SoundWrite(struct Vrc7Sound* v, uint8_t value) {
	if (opll) {
		OPLL_writeReg(opll, v->address, value);
	}
}

void vrc7SoundCycle(struct Vrc7Sound* v, bool silenced) {
	if (++v->clock == 36) {
		v->clock = 0;
		v->sample = silenced || !opll ? 0 : OPLL_calc(opll);
	}
}

int vrc7SoundOutput(const struct Vrc7Sound* v) {
	return v->sample * 6;
}

static void vrc7Banks(struct NES* nes) {
	struct Vrc7* v = STATE(Vrc7);
	for (int i = 0; i < 3; i++) {
		prg8(nes, 0x8000 + i * 0x2000, v->prg[i]);
	}
	prg8(nes, 0xE000, -1);
	for (int i = 0; i < 8; i++) {
		chr1(nes, i * 0x400, v->chr[i]);
	}
	static const enum Mirroring mirrors[4] = { MIRROR_VERTICAL, MIRROR_HORIZONTAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B };
	nesSetMirroring(nes, mirrors[v->control & 3]);
	if (v->control & 0x80) {
		wram8(nes);
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
	}
}

static size_t vrc7SaveExtra(struct NES* nes, uint8_t* out) {
	return vrc7SoundState(out, false);
}

static void vrc7LoadExtra(struct NES* nes, const uint8_t* in) {
	vrc7SoundState((uint8_t*) in, true);
}

static void vrc7Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct Vrc7* v = STATE(Vrc7);
	if (address < 0x8000) {
		return;
	}
	// The register select line: A4 on VRC7a, A3 on VRC7b, either without a submapper
	int line = nes->cart.submapper == 1 ? 0x08 : nes->cart.submapper == 2 ? 0x10 : 0x18;
	bool odd = address & line;
	switch (address & 0xF000) {
	case 0x8000:
		v->prg[odd] = value & 0x3F;
		break;
	case 0x9000:
		if (!odd) {
			v->prg[2] = value & 0x3F;
		} else if (address & 0x20) {
			vrc7SoundWrite(&v->sound, value);
			return;
		} else {
			v->sound.address = value;
			return;
		}
		break;
	case 0xA000:
	case 0xB000:
	case 0xC000:
	case 0xD000:
		v->chr[((address >> 12) - 0xA) * 2 + odd] = value;
		break;
	case 0xE000:
		if (odd) {
			v->irq.latch = value;
			return;
		}
		v->control = value;
		break;
	case 0xF000:
		if (odd) {
			vrcIrqAck(nes, &v->irq);
		} else {
			vrcIrqControl(nes, &v->irq, value);
		}
		return;
	}
	vrc7Banks(nes);
}

static void vrc7Cycle(struct NES* nes) {
	struct Vrc7* v = STATE(Vrc7);
	vrcIrqCycle(nes, &v->irq);
	vrc7SoundCycle(&v->sound, v->control & 0x40);
}

static int vrc7Audio(struct NES* nes) {
	return vrc7SoundOutput(&STATE(Vrc7)->sound);
}

static void vrc7Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Vrc7));
	}
	vrc7SoundReset(&STATE(Vrc7)->sound, hard);
	vrc7Banks(nes);
}

const struct Board boardVrc7 = {
	.name = "VRC7", .reset = vrc7Reset, .write = vrc7Write, .cpuCycle = vrc7Cycle, .audio = vrc7Audio,
	.sync = vrc7Banks, .stateSize = sizeof(struct Vrc7), .saveExtra = vrc7SaveExtra, .loadExtra = vrc7LoadExtra,
};
