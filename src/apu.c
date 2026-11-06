/* Copyright (c) 2026 Magnominus
 * Timing of the frame counter, DMC, $4015 and the controllers follows TriCNES,
 * Copyright (c) 2025 Chris Siebert, under the MIT license (LICENSES/MIT.txt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <string.h>

// The 2A03's sound and controller ports, ticked once per CPU cycle after the CPU's access. Cycles
// alternate between "get" and "put"; the pulse, noise and DMC timers run on get cycles.

static const uint8_t lengthTable[32] = {
	10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
	12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30,
};

static const uint8_t dutyTable[4][8] = {
	{ 0, 0, 0, 0, 0, 0, 0, 1 },
	{ 0, 0, 0, 0, 0, 0, 1, 1 },
	{ 0, 0, 0, 0, 1, 1, 1, 1 },
	{ 1, 1, 1, 1, 1, 1, 0, 0 },
};

static const uint8_t triangleTable[32] = {
	15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
};

static const uint16_t noiseNtsc[16] = { 4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068 };
static const uint16_t noisePal[16] = { 4, 8, 14, 30, 60, 88, 118, 148, 188, 236, 354, 472, 708, 944, 1890, 3778 };

// The DMC's rate timer is a 9-bit LFSR; these seeds count it down in the table's APU cycles
static const uint16_t dmcLfsrNtsc[16] = {
	0x173, 0x08A, 0x143, 0x0DB, 0x1EE, 0x03F, 0x07D, 0x1D1, 0x009, 0x0DC, 0x0F1, 0x0F9, 0x08D, 0x189, 0x18E, 0x156,
};
static const uint16_t dmcLfsrPal[16] = {
	0x1D7, 0x1CC, 0x136, 0x185, 0x10F, 0x1BC, 0x074, 0x099, 0x0A4, 0x18B, 0x1E7, 0x167, 0x0A8, 0x071, 0x1B1, 0x1D5,
};

static inline uint16_t dmcSeed(const struct NES* nes, int index) {
	return (nes->region == REGION_NTSC ? dmcLfsrNtsc : dmcLfsrPal)[index];
}

// Frame counter steps in CPU cycles: 4-step and 5-step, NTSC and PAL
static const uint16_t stepsNtsc[6] = { 7457, 14913, 22371, 29828, 29829, 29830 };
static const uint16_t stepsNtsc5[5] = { 7457, 14913, 22371, 37281, 37282 };
static const uint16_t stepsPal[6] = { 8313, 16627, 24939, 33252, 33253, 33254 };
static const uint16_t stepsPal5[5] = { 8313, 16627, 24939, 41565, 41566 };

static float pulseMix[31];
static float tndMix[203];

static void buildMixer(void) {
	if (pulseMix[1] != 0) {
		return;
	}
	for (int i = 1; i < 31; i++) {
		pulseMix[i] = 95.52f / (8128.0f / i + 100.0f);
	}
	for (int i = 1; i < 203; i++) {
		tndMix[i] = 163.67f / (24329.0f / i + 100.0f);
	}
}

static uint8_t* lengthCounter(struct Apu* apu, int channel) {
	switch (channel) {
	case 0:
		return &apu->pulse[0].lengthCounter;
	case 1:
		return &apu->pulse[1].lengthCounter;
	case 2:
		return &apu->triangle.lengthCounter;
	default:
		return &apu->noise.lengthCounter;
	}
}

static bool channelEnabled(struct Apu* apu, int channel) {
	switch (channel) {
	case 0:
		return apu->pulse[0].enabled;
	case 1:
		return apu->pulse[1].enabled;
	case 2:
		return apu->triangle.enabled;
	default:
		return apu->noise.enabled;
	}
}

static bool lengthHalted(struct Apu* apu, int channel) {
	switch (channel) {
	case 0:
		return apu->pulse[0].lengthHalt;
	case 1:
		return apu->pulse[1].lengthHalt;
	case 2:
		return apu->triangle.control;
	default:
		return apu->noise.lengthHalt;
	}
}

static void clockEnvelope(bool* start, uint8_t* divider, uint8_t* decay, uint8_t period, bool loop) {
	if (*start) {
		*start = false;
		*decay = 15;
		*divider = period;
	} else if (*divider == 0) {
		*divider = period;
		if (*decay) {
			(*decay)--;
		} else if (loop) {
			*decay = 15;
		}
	} else {
		(*divider)--;
	}
}

static uint16_t sweepTarget(struct Pulse* p) {
	uint16_t change = p->timerPeriod >> p->sweepShift;
	if (p->sweepNegate) {
		return p->timerPeriod - change - (p->second ? 0 : 1);
	}
	return p->timerPeriod + change;
}

static bool pulseMuted(struct Pulse* p) {
	return p->timerPeriod < 8 || (!p->sweepNegate && sweepTarget(p) > 0x7FF);
}

static void quarterFrame(struct Apu* apu) {
	for (int i = 0; i < 2; i++) {
		struct Pulse* p = &apu->pulse[i];
		clockEnvelope(&p->envelopeStart, &p->envelopeDivider, &p->envelopeDecay, p->volume, p->lengthHalt);
	}
	struct Noise* n = &apu->noise;
	clockEnvelope(&n->envelopeStart, &n->envelopeDivider, &n->envelopeDecay, n->volume, n->lengthHalt);
	struct Triangle* t = &apu->triangle;
	if (t->linearReloadFlag) {
		t->linearCounter = t->linearReload;
	} else if (t->linearCounter) {
		t->linearCounter--;
	}
	if (!t->control) {
		t->linearReloadFlag = false;
	}
}

static void sweeps(struct Apu* apu) {
	for (int i = 0; i < 2; i++) {
		struct Pulse* p = &apu->pulse[i];
		if (p->sweepCounter == 0 && p->sweepEnabled && p->sweepShift && !pulseMuted(p)) {
			p->timerPeriod = sweepTarget(p);
		}
		if (p->sweepCounter == 0 || p->sweepReload) {
			p->sweepCounter = p->sweepPeriod;
			p->sweepReload = false;
		} else {
			p->sweepCounter--;
		}
	}
}

// Length counters: a reload written this cycle wins over a half-frame clock only from zero
static void lengthCounters(struct Apu* apu, bool half) {
	for (int i = 0; i < 4; i++) {
		uint8_t* length = lengthCounter(apu, i);
		if (half) {
			if (apu->lengthReload[i] && *length == 0) {
				*length = apu->lengthReloadValue[i];
			} else {
				apu->lengthReload[i] = false;
			}
			if (!channelEnabled(apu, i)) {
				*length = 0;
			}
			if (*length && !lengthHalted(apu, i) && !apu->lengthReload[i]) {
				(*length)--;
			}
		} else if (apu->lengthReload[i]) {
			*length = apu->lengthReloadValue[i];
		}
		apu->lengthReload[i] = false;
	}
	apu->pulse[0].lengthHalt = apu->registers[0x0] & 0x20;
	apu->pulse[1].lengthHalt = apu->registers[0x4] & 0x20;
	apu->triangle.control = apu->registers[0x8] & 0x80;
	apu->noise.lengthHalt = apu->registers[0xC] & 0x20;
}

static void startSample(struct Dmc* d) {
	d->currentAddress = d->sampleAddress;
	d->bytesRemaining = d->sampleLength;
}

static void requestDmcDma(struct NES* nes) {
	nes->cpu.dmcDma = true;
	nes->cpu.dmcHalt = true;
}

static void dmcStepSample(struct NES* nes) {
	struct Dmc* d = &nes->apu.dmc;
	if (d->shifter & 1) {
		if (d->output <= 125) {
			d->output += 2;
		}
	} else if (d->output >= 2) {
		d->output -= 2;
	}
	d->shifter >>= 1;
	if (--d->bitsRemaining == 0) {
		d->bitsRemaining = 8;
		if (d->bytesRemaining || d->setImplicitAbort) {
			if (!nes->cpu.dmcDma && d->cannotRun != 2) {
				requestDmcDma(nes);
			}
			if (d->setImplicitAbort) {
				d->implicitAbort = true;
				d->setImplicitAbort = false;
			}
			d->shifter = d->buffer;
			d->silent = false;
		} else {
			d->silent = true;
		}
	}
}

static void dmcStepLfsr(struct NES* nes) {
	struct Dmc* d = &nes->apu.dmc;
	uint16_t lfsr = d->lfsr;
	if (lfsr == 1) {
		d->lfsr = dmcSeed(nes, d->rateIndex);
		dmcStepSample(nes);
		return;
	}
	bool b0 = lfsr & 1, b4 = lfsr & 16;
	bool feedback = !((b0 && b4) || !(b0 || b4 || lfsr == 0));
	d->lfsr = (lfsr >> 1) | (feedback ? 0x100 : 0);
}

// The DMC DMA's get cycle: the next sample byte into the buffer
void apuDmcDmaGet(struct NES* nes) {
	struct Dmc* d = &nes->apu.dmc;
	d->buffer = busRead(nes, d->currentAddress);
	d->currentAddress = d->currentAddress == 0xFFFF ? 0x8000 : d->currentAddress + 1;
	if (d->bytesRemaining) {
		d->bytesRemaining--;
	}
	if (!d->bytesRemaining) {
		if (!d->loop) {
			d->enabled = false;
			if (d->irqEnabled) {
				nes->apu.dmcFlag = true;
				nes->apu.irqDetector = true;
			}
		} else {
			startSample(d);
		}
	}
	nes->cpu.dmcDma = false;
	nes->cpu.oamAligned = false;
	d->cannotRun = 2;
}

// M2 going low: the detector reaches the IRQ line, and a frame flag left set keeps the detector set
void apuLatchIrq(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	nesSetIrq(nes, IRQ_FRAME, apu->irqDetector);
	if (apu->frameFlag && !apu->irqInhibit) {
		apu->irqDetector = true;
	}
}

static void frameCounter(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	if (!(apu->frameReset & 0x80)) {
		apu->frameReset--;
		if (apu->frameReset & 0x80) {
			apu->frameCounter = 0;
		}
	}
	apu->frameCounter++;
	bool pal = nes->region != REGION_NTSC;
	uint16_t c = apu->frameCounter;
	if (apu->fiveStep) {
		const uint16_t* s = pal ? stepsPal5 : stepsNtsc5;
		if (c == s[0] || c == s[2]) {
			apu->quarterClock = true;
		} else if (c == s[1] || c == s[3]) {
			apu->quarterClock = apu->halfClock = true;
		} else if (c == s[4]) {
			apu->frameCounter = 0;
		}
	} else {
		const uint16_t* s = pal ? stepsPal : stepsNtsc;
		if (c == s[0] || c == s[2]) {
			apu->quarterClock = true;
		} else if (c == s[1]) {
			apu->quarterClock = apu->halfClock = true;
		} else if (c == s[3]) {
			apu->frameFlag = true;
		} else if (c == s[4]) {
			apu->quarterClock = apu->halfClock = true;
			apu->frameFlag = true;
			apu->irqDetector |= !apu->irqInhibit;
		} else if (c == s[5]) {
			apu->frameFlag = !apu->irqInhibit;
			apu->irqDetector |= !apu->irqInhibit;
			apu->frameCounter = 0;
		}
	}
	if (apu->quarterClock) {
		apu->quarterClock = false;
		quarterFrame(apu);
	}
	bool half = apu->halfClock;
	if (half) {
		apu->halfClock = false;
		sweeps(apu);
	}
	lengthCounters(apu, half);
}

static int mix(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	int pulse = 0;
	for (int i = 0; i < 2; i++) {
		struct Pulse* p = &apu->pulse[i];
		if (p->lengthCounter && !pulseMuted(p) && dutyTable[p->duty][p->dutyStep]) {
			pulse += p->constant ? p->volume : p->envelopeDecay;
		}
	}
	int triangle = triangleTable[apu->triangle.step];
	int noise = 0;
	if (apu->noise.lengthCounter && !(apu->noise.shift & 1)) {
		noise = apu->noise.constant ? apu->noise.volume : apu->noise.envelopeDecay;
	}
	float out = pulseMix[pulse] + tndMix[3 * triangle + 2 * noise + apu->dmc.output];
	int value = (int) (out * 40000.0f);
	if (nes->board->audio) {
		value += nes->board->audio(nes);
	}
	return value;
}

static void outputSample(struct NES* nes, int value) {
	nes->audioAccum += value;
	nes->audioAccumCount++;
	int clockRate = nes->region == REGION_NTSC ? 1789773 : (nes->region == REGION_PAL ? 1662607 : 1773448);
	nes->audioPhase += nes->sampleRate;
	if (nes->audioPhase < clockRate) {
		return;
	}
	nes->audioPhase -= clockRate;
	int32_t in = nes->audioAccum / nes->audioAccumCount;
	nes->audioAccum = 0;
	nes->audioAccumCount = 0;
	// The console's filters: high-pass at about 37 Hz, low-pass at about 14 kHz
	nes->hpOut = nes->hpOut + in - nes->hpPrev - (nes->hpOut >> 8);
	nes->hpPrev = in;
	nes->lpOut += ((nes->hpOut - nes->lpOut) * 3) >> 2;
	int32_t s = nes->lpOut;
	if (s > 32767) {
		s = 32767;
	} else if (s < -32768) {
		s = -32768;
	}
	if (nes->audioCount + 2 <= nes->audioCapacity) {
		nes->audioOut[nes->audioCount++] = s;
		nes->audioOut[nes->audioCount++] = s;
	}
}

static uint8_t padBits(uint8_t pad) {
	// Shifted out from bit 7: A first
	uint8_t out = 0;
	for (int i = 0; i < 8; i++) {
		if (pad & (1 << i)) {
			out |= 0x80 >> i;
		}
	}
	return out;
}

void apuCycle(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	struct Dmc* d = &apu->dmc;
	apu->cycles++;

	if (!apu->strobing) {
		for (int i = 0; i < 2; i++) {
			if (apu->padShiftDelay[i] && --apu->padShiftDelay[i] == 0) {
				apu->padShift[i] = (apu->padShift[i] << 1) | 1;
			}
		}
	} else {
		apu->padShiftDelay[0] = apu->padShiftDelay[1] = 0;
	}

	if (!apu->putCycle) {
		// Get to put
		if (apu->strobing) {
			if (!apu->strobed) {
				apu->strobed = true;
				apu->padShift[0] = padBits(nes->pads[0]);
				apu->padShift[1] = padBits(nes->pads[1]);
			}
		} else {
			apu->strobed = false;
		}
		for (int i = 0; i < 2; i++) {
			struct Pulse* p = &apu->pulse[i];
			if (p->timer == 0) {
				p->timer = p->timerPeriod;
				p->dutyStep = (p->dutyStep - 1) & 7;
			} else {
				p->timer--;
			}
		}
		struct Noise* n = &apu->noise;
		if (n->timer == 0) {
			n->timer = n->timerPeriod;
			uint16_t feedback = (n->shift ^ (n->shift >> (n->mode ? 6 : 1))) & 1;
			n->shift = (n->shift >> 1) | (feedback << 14);
		} else {
			n->timer--;
		}
		dmcStepLfsr(nes);
		if (d->cannotRun) {
			d->cannotRun -= 2;
		}
	} else {
		// Put to get
		if (apu->clearingFrameFlag) {
			apu->clearingFrameFlag = false;
			apu->frameFlag = false;
			apu->irqDetector = false;
		}
		if (d->loadDelay && --d->loadDelay == 0 && !nes->cpu.dmcDma) {
			requestDmcDma(nes);
			d->shifter = d->buffer;
			d->silent = false;
		}
	}
	if (d->enableDelay && --d->enableDelay == 0) {
		d->enabled = d->enabledPending;
		if (!d->enabled) {
			d->bytesRemaining = 0;
		}
	}

	struct Triangle* t = &apu->triangle;
	if (t->timer == 0) {
		t->timer = t->timerPeriod;
		if (t->lengthCounter && t->linearCounter && t->timerPeriod >= 2) {
			t->step = (t->step + 1) & 31;
		}
	} else {
		t->timer--;
	}

	frameCounter(nes);
	apu->putCycle = !apu->putCycle;
	outputSample(nes, mix(nes));
}

// A read while the CPU's address bus is on $4000-$401F also reaches the APU's registers, by the low
// bits of the address actually read; DMA reads land here too.
uint8_t apuConflictRead(struct NES* nes, uint8_t value, uint16_t address, bool driven) {
	struct Apu* apu = &nes->apu;
	struct Cpu* cpu = &nes->cpu;
	switch (address & 0x1F) {
	case 0x15: {
		uint8_t status = cpu->internalBus & 0x20;
		status |= apu->dmcFlag ? 0x80 : 0;
		status |= apu->frameFlag ? 0x40 : 0;
		status |= apu->dmc.bytesRemaining && apu->dmc.enabledPending ? 0x10 : 0;
		status |= apu->noise.lengthCounter ? 0x08 : 0;
		status |= apu->triangle.lengthCounter ? 0x04 : 0;
		status |= apu->pulse[1].lengthCounter ? 0x02 : 0;
		status |= apu->pulse[0].lengthCounter ? 0x01 : 0;
		apu->clearingFrameFlag = true;
		cpu->internalBus = status;
		return status;
	}
	case 0x16:
	case 0x17: {
		int port = address & 1;
		uint8_t bit = (apu->padShift[port] & 0x80) ? 1 : 0;
		apu->padShiftDelay[port] = 2;
		apu->strobed = false;
		if (cpu->oamDma && driven) {
			break;
		}
		value = bit | (cpu->openBus & 0xE0);
		cpu->openBus = value;
		break;
	}
	}
	cpu->internalBus = value;
	return value;
}

void apuWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Apu* apu = &nes->apu;
	struct Dmc* d = &apu->dmc;
	if (address == 0x4016) {
		apu->strobing = value & 1;
		if (!apu->strobing) {
			apu->strobed = false;
		}
		return;
	}
	if (address == 0x4017) {
		apu->fiveStep = value & 0x80;
		apu->irqInhibit = value & 0x40;
		if (apu->fiveStep) {
			apu->quarterClock = apu->halfClock = true;
		}
		if (apu->irqInhibit) {
			apu->frameFlag = false;
			apu->irqDetector = false;
		}
		apu->frameReset = apu->putCycle ? 3 : 4;
		return;
	}
	if (address > 0x4015) {
		return;
	}
	apu->registers[address & 0xF] = value;
	switch (address) {
	case 0x4000:
	case 0x4004: {
		struct Pulse* p = &apu->pulse[(address >> 2) & 1];
		p->duty = value >> 6;
		p->constant = value & 0x10;
		p->volume = value & 15;
		break;
	}
	case 0x4001:
	case 0x4005: {
		struct Pulse* p = &apu->pulse[(address >> 2) & 1];
		p->sweepEnabled = value & 0x80;
		p->sweepPeriod = (value >> 4) & 7;
		p->sweepNegate = value & 8;
		p->sweepShift = value & 7;
		p->sweepReload = true;
		break;
	}
	case 0x4002:
	case 0x4006: {
		struct Pulse* p = &apu->pulse[(address >> 2) & 1];
		p->timerPeriod = (p->timerPeriod & 0x700) | value;
		break;
	}
	case 0x4003:
	case 0x4007: {
		int i = (address >> 2) & 1;
		struct Pulse* p = &apu->pulse[i];
		p->timerPeriod = (p->timerPeriod & 0xFF) | ((value & 7) << 8);
		p->dutyStep = 0;
		p->envelopeStart = true;
		if (p->enabled) {
			apu->lengthReloadValue[i] = lengthTable[value >> 3];
			apu->lengthReload[i] = true;
		}
		break;
	}
	case 0x4008:
		apu->triangle.linearReload = value & 0x7F;
		break;
	case 0x400A:
		apu->triangle.timerPeriod = (apu->triangle.timerPeriod & 0x700) | value;
		break;
	case 0x400B:
		apu->triangle.timerPeriod = (apu->triangle.timerPeriod & 0xFF) | ((value & 7) << 8);
		apu->triangle.linearReloadFlag = true;
		if (apu->triangle.enabled) {
			apu->lengthReloadValue[2] = lengthTable[value >> 3];
			apu->lengthReload[2] = true;
		}
		break;
	case 0x400C:
		apu->noise.constant = value & 0x10;
		apu->noise.volume = value & 15;
		break;
	case 0x400E:
		apu->noise.mode = value & 0x80;
		apu->noise.timerPeriod = (nes->region == REGION_NTSC ? noiseNtsc : noisePal)[value & 15] / 2 - 1;
		break;
	case 0x400F:
		apu->noise.envelopeStart = true;
		if (apu->noise.enabled) {
			apu->lengthReloadValue[3] = lengthTable[value >> 3];
			apu->lengthReload[3] = true;
		}
		break;
	case 0x4010:
		d->irqEnabled = value & 0x80;
		d->loop = value & 0x40;
		d->rateIndex = value & 15;
		if (!d->irqEnabled) {
			apu->dmcFlag = false;
			apu->irqDetector = false;
		}
		break;
	case 0x4011:
		d->output = value & 0x7F;
		break;
	case 0x4012:
		d->sampleAddress = 0xC000 | (value << 6);
		break;
	case 0x4013:
		d->sampleLength = (value << 4) | 1;
		break;
	case 0x4014:
		nes->cpu.oamDma = true;
		nes->cpu.oamFirst = true;
		nes->cpu.oamIndex = 0;
		nes->cpu.oamPage = value;
		break;
	case 0x4015:
		d->enabledPending = value & 0x10;
		apu->noise.enabled = value & 8;
		apu->triangle.enabled = value & 4;
		apu->pulse[1].enabled = value & 2;
		apu->pulse[0].enabled = value & 1;
		d->enableDelay = apu->putCycle ? 3 : 4;
		if (d->enabledPending && !d->bytesRemaining) {
			startSample(d);
			if (d->silent) {
				d->loadDelay = 2;
			}
		}
		for (int i = 0; i < 4; i++) {
			if (!channelEnabled(apu, i)) {
				*lengthCounter(apu, i) = 0;
			}
		}
		apu->dmcFlag = false;
		apu->irqDetector = false;
		// Disabling just as the timer fires a DMA lets that DMA start before the disable lands
		if (!d->enabledPending &&
			((d->lfsr == 1 && !apu->putCycle) || (d->lfsr == dmcSeed(nes, d->rateIndex) && apu->putCycle))) {
			d->enableDelay = apu->putCycle ? 5 : 6;
		}
		if (d->enabledPending && ((d->lfsr == 17 && !apu->putCycle) || (d->lfsr == 8 && apu->putCycle))) {
			d->setImplicitAbort = true;
		}
		break;
	}
}

void apuReset(struct NES* nes, bool hard) {
	struct Apu* apu = &nes->apu;
	buildMixer();
	bool fiveStep = apu->fiveStep, inhibit = apu->irqInhibit;
	memset(&apu->pulse, 0, sizeof(apu->pulse));
	memset(&apu->triangle, 0, sizeof(apu->triangle));
	memset(&apu->noise, 0, sizeof(apu->noise));
	memset(&apu->dmc, 0, sizeof(apu->dmc));
	memset(apu->lengthReload, 0, sizeof(apu->lengthReload));
	memset(apu->registers, 0, sizeof(apu->registers));
	apu->pulse[1].second = true;
	apu->noise.shift = 1;
	apu->noise.timerPeriod = noiseNtsc[0] / 2 - 1;
	apu->dmc.rateIndex = 0;
	apu->dmc.lfsr = dmcSeed(nes, 0);
	apu->dmc.bitsRemaining = 8;
	apu->dmc.silent = true;
	apu->dmc.sampleAddress = 0xC000;
	apu->dmc.currentAddress = 0xC000;
	apu->dmc.sampleLength = 1;
	apu->dmcFlag = false;
	apu->frameFlag = false;
	apu->clearingFrameFlag = false;
	apu->quarterClock = apu->halfClock = false;
	// A reset rewrites $4017 with its last value (zero at power-on)
	apu->fiveStep = hard ? false : fiveStep;
	apu->irqInhibit = hard ? false : inhibit;
	apu->frameCounter = 0;
	apu->frameReset = 0xFF;
	apu->strobing = apu->strobed = false;
	apu->padShiftDelay[0] = apu->padShiftDelay[1] = 0;
	apu->irqDetector = false;
	if (hard) {
		apu->putCycle = false;
	}
	nesSetIrq(nes, IRQ_FRAME | IRQ_DMC, false);
}
