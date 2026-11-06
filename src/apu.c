/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <string.h>

// The 2A03's sound: two pulses, a triangle, noise and the sample channel (DMC), clocked every CPU
// cycle, with the frame counter's quarter and half frames and its IRQ.

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
static const uint16_t dmcNtsc[16] = { 428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54 };
static const uint16_t dmcPal[16] = { 398, 354, 316, 298, 276, 236, 210, 198, 176, 148, 132, 118, 98, 78, 66, 50 };

// Frame counter steps in CPU cycles: 4-step then 5-step
static const uint32_t stepsNtsc[2][6] = {
	{ 7457, 14913, 22371, 29828, 29829, 29830 },
	{ 7457, 14913, 22371, 29829, 37281, 37282 },
};
static const uint32_t stepsPal[2][6] = {
	{ 8313, 16627, 24939, 33252, 33253, 33254 },
	{ 8313, 16627, 24939, 33253, 41565, 41566 },
};

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

static void halfFrame(struct Apu* apu) {
	apu->halfClocked = true;
	for (int i = 0; i < 2; i++) {
		struct Pulse* p = &apu->pulse[i];
		if (!p->lengthHalt && p->lengthCounter) {
			p->lengthCounter--;
		}
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
	if (!apu->triangle.control && apu->triangle.lengthCounter) {
		apu->triangle.lengthCounter--;
	}
	if (!apu->noise.lengthHalt && apu->noise.lengthCounter) {
		apu->noise.lengthCounter--;
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

static bool* lengthHalt(struct Apu* apu, int channel) {
	switch (channel) {
	case 0:
		return &apu->pulse[0].lengthHalt;
	case 1:
		return &apu->pulse[1].lengthHalt;
	case 2:
		return &apu->triangle.control;
	default:
		return &apu->noise.lengthHalt;
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

static void dmcStartReader(struct NES* nes) {
	struct Dmc* d = &nes->apu.dmc;
	if (d->bufferEmpty && d->bytesRemaining && !nes->cpu.dmcDma) {
		nes->cpu.dmcDma = true;
		nes->cpu.dmaHalt = true;
		nes->cpu.dmaDummy = true;
	}
}

void apuDmcDmaDone(struct NES* nes, uint8_t value) {
	struct Dmc* d = &nes->apu.dmc;
	if (!d->bytesRemaining) {
		return;
	}
	d->sampleBuffer = value;
	d->bufferEmpty = false;
	d->currentAddress = d->currentAddress == 0xFFFF ? 0x8000 : d->currentAddress + 1;
	d->bytesRemaining--;
	if (!d->bytesRemaining) {
		if (d->loop) {
			d->currentAddress = d->sampleAddress;
			d->bytesRemaining = d->sampleLength;
		} else if (d->irqEnabled) {
			nesSetIrq(nes, IRQ_DMC, true);
		}
	}
}

static void clockDmc(struct NES* nes) {
	struct Dmc* d = &nes->apu.dmc;
	if (!d->silence) {
		if (d->shift & 1) {
			if (d->output <= 125) {
				d->output += 2;
			}
		} else if (d->output >= 2) {
			d->output -= 2;
		}
	}
	d->shift >>= 1;
	if (d->bitsRemaining) {
		d->bitsRemaining--;
	}
	if (!d->bitsRemaining) {
		d->bitsRemaining = 8;
		if (d->bufferEmpty) {
			d->silence = true;
		} else {
			d->silence = false;
			d->shift = d->sampleBuffer;
			d->bufferEmpty = true;
			dmcStartReader(nes);
		}
	}
}

static void clockFrameCounter(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	const uint32_t* steps = (nes->region == REGION_NTSC ? stepsNtsc : stepsPal)[apu->fiveStep];
	apu->frameCycle++;
	if (apu->frameCycle != steps[apu->frameStep]) {
		return;
	}
	int step = apu->frameStep;
	if (!apu->fiveStep && step >= 3 && !apu->irqInhibit) {
		nesSetIrq(nes, IRQ_FRAME, true);
	}
	if (step == 0 || step == 2) {
		quarterFrame(apu);
	} else if (step == 1 || (!apu->fiveStep && step == 4) || (apu->fiveStep && step == 4)) {
		quarterFrame(apu);
		halfFrame(apu);
	}
	apu->frameStep++;
	if (apu->frameStep == 6) {
		apu->frameStep = 0;
		apu->frameCycle = 0;
	}
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

void apuCycle(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	apu->cycles++;

	// $4017 writes take effect a few cycles later
	if (apu->frameResetDelay > 0 && --apu->frameResetDelay == 0) {
		apu->fiveStep = apu->frameValue & 0x80;
		apu->frameStep = 0;
		// This cycle counts as the sequence's first
		apu->frameCycle = (uint32_t) -1;
		if (apu->fiveStep) {
			quarterFrame(apu);
			halfFrame(apu);
		}
	}
	uint8_t before[4];
	for (int i = 0; i < 4; i++) {
		before[i] = *lengthCounter(apu, i);
	}
	apu->halfClocked = false;
	clockFrameCounter(nes);

	// Length counter loads and halts written this cycle land after the frame counter has clocked;
	// a load is lost if the counter was clocked from non-zero on the same cycle
	for (int i = 0; i < 4; i++) {
		if (apu->lengthLoadFlag[i]) {
			apu->lengthLoadFlag[i] = false;
			uint8_t* length = lengthCounter(apu, i);
			if (channelEnabled(apu, i) && !(apu->halfClocked && before[i])) {
				*length = apu->lengthLoadPending[i];
			}
		}
		*lengthHalt(apu, i) = apu->lengthHaltPending[i];
	}

	// Triangle: every CPU cycle
	struct Triangle* t = &apu->triangle;
	if (t->timer == 0) {
		t->timer = t->timerPeriod;
		if (t->lengthCounter && t->linearCounter && t->timerPeriod >= 2) {
			t->step = (t->step + 1) & 31;
		}
	} else {
		t->timer--;
	}

	// Pulses and noise: every other cycle
	if (apu->cycles & 1) {
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
	}

	// DMC: its own timer, counted in CPU cycles
	struct Dmc* d = &apu->dmc;
	if (d->transferDelay > 0 && --d->transferDelay == 0) {
		dmcStartReader(nes);
	}
	if (--d->timer == 0) {
		d->timer = d->rate;
		clockDmc(nes);
	}

	outputSample(nes, mix(nes));
}

uint8_t apuReadStatus(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	uint8_t value = 0;
	if (apu->pulse[0].lengthCounter) {
		value |= 1;
	}
	if (apu->pulse[1].lengthCounter) {
		value |= 2;
	}
	if (apu->triangle.lengthCounter) {
		value |= 4;
	}
	if (apu->noise.lengthCounter) {
		value |= 8;
	}
	if (apu->dmc.bytesRemaining) {
		value |= 0x10;
	}
	if (nes->cpu.irqLine & IRQ_FRAME) {
		value |= 0x40;
	}
	if (nes->cpu.irqLine & IRQ_DMC) {
		value |= 0x80;
	}
	nesSetIrq(nes, IRQ_FRAME, false);
	return value;
}

static void writeLength(struct Apu* apu, int channel, uint8_t value) {
	apu->lengthLoadPending[channel] = lengthTable[value >> 3];
	// A load on the cycle the counter is clocked from non-zero is ignored; this lands after the clock
	apu->lengthLoadFlag[channel] = true;
}

void apuWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Apu* apu = &nes->apu;
	switch (address) {
	case 0x4000:
	case 0x4004: {
		struct Pulse* p = &apu->pulse[(address >> 2) & 1];
		p->duty = value >> 6;
		apu->lengthHaltPending[(address >> 2) & 1] = value & 0x20;
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
		writeLength(apu, i, value);
		break;
	}
	case 0x4008:
		apu->lengthHaltPending[2] = value & 0x80;
		apu->triangle.linearReload = value & 0x7F;
		break;
	case 0x400A:
		apu->triangle.timerPeriod = (apu->triangle.timerPeriod & 0x700) | value;
		break;
	case 0x400B:
		apu->triangle.timerPeriod = (apu->triangle.timerPeriod & 0xFF) | ((value & 7) << 8);
		apu->triangle.linearReloadFlag = true;
		writeLength(apu, 2, value);
		break;
	case 0x400C:
		apu->lengthHaltPending[3] = value & 0x20;
		apu->noise.constant = value & 0x10;
		apu->noise.volume = value & 15;
		break;
	case 0x400E:
		apu->noise.mode = value & 0x80;
		apu->noise.timerPeriod = (nes->region == REGION_NTSC ? noiseNtsc : noisePal)[value & 15] / 2 - 1;
		break;
	case 0x400F:
		apu->noise.envelopeStart = true;
		writeLength(apu, 3, value);
		break;
	case 0x4010:
		apu->dmc.irqEnabled = value & 0x80;
		apu->dmc.loop = value & 0x40;
		apu->dmc.rate = (nes->region == REGION_NTSC ? dmcNtsc : dmcPal)[value & 15];
		if (!apu->dmc.irqEnabled) {
			nesSetIrq(nes, IRQ_DMC, false);
		}
		break;
	case 0x4011:
		apu->dmc.output = value & 0x7F;
		break;
	case 0x4012:
		apu->dmc.sampleAddress = 0xC000 | (value << 6);
		break;
	case 0x4013:
		apu->dmc.sampleLength = (value << 4) | 1;
		break;
	case 0x4015:
		apu->pulse[0].enabled = value & 1;
		apu->pulse[1].enabled = value & 2;
		apu->triangle.enabled = value & 4;
		apu->noise.enabled = value & 8;
		for (int i = 0; i < 4; i++) {
			if (!channelEnabled(apu, i)) {
				*lengthCounter(apu, i) = 0;
				apu->lengthLoadFlag[i] = false;
			}
		}
		nesSetIrq(nes, IRQ_DMC, false);
		if (!(value & 0x10)) {
			apu->dmc.bytesRemaining = 0;
		} else if (!apu->dmc.bytesRemaining) {
			apu->dmc.currentAddress = apu->dmc.sampleAddress;
			apu->dmc.bytesRemaining = apu->dmc.sampleLength;
			// The reader starts two or three cycles later, by the CPU cycle's parity
			apu->dmc.transferDelay = (nes->cpu.cycles & 1) ? 3 : 2;
		}
		break;
	case 0x4017:
		apu->frameValue = value;
		apu->irqInhibit = value & 0x40;
		if (apu->irqInhibit) {
			nesSetIrq(nes, IRQ_FRAME, false);
		}
		apu->frameResetDelay = (nes->cpu.cycles & 1) ? 3 : 4;
		break;
	}
}

void apuReset(struct NES* nes, bool hard) {
	struct Apu* apu = &nes->apu;
	buildMixer();
	uint8_t frameValue = hard ? 0 : apu->frameValue;
	memset(&apu->pulse, 0, sizeof(apu->pulse));
	memset(&apu->triangle, 0, sizeof(apu->triangle));
	memset(&apu->noise, 0, sizeof(apu->noise));
	memset(&apu->dmc, 0, sizeof(apu->dmc));
	if (!hard) {
		apu->triangle.step = 0;
	}
	apu->pulse[1].second = true;
	apu->noise.shift = 1;
	apu->noise.timerPeriod = noiseNtsc[0] / 2 - 1;
	apu->dmc.rate = (nes->region == REGION_NTSC ? dmcNtsc : dmcPal)[0];
	apu->dmc.timer = apu->dmc.rate;
	apu->dmc.bufferEmpty = true;
	apu->dmc.bitsRemaining = 8;
	apu->dmc.silence = true;
	memset(apu->lengthHaltPending, 0, sizeof(apu->lengthHaltPending));
	memset(apu->lengthLoadFlag, 0, sizeof(apu->lengthLoadFlag));
	apu->frameValue = frameValue;
	apu->irqInhibit = frameValue & 0x40;
	apu->fiveStep = false;
	apu->frameStep = 0;
	apu->frameCycle = 0;
	// The frame counter's reset write takes effect as if $4017 was written just before
	apu->frameResetDelay = 3;
	nes->cpu.irqLine &= ~(IRQ_FRAME | IRQ_DMC);
}
