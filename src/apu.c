/* Copyright (c) 2026 Magnominus
 * Timing of the frame counter, DMC, $4015 and the controllers follows TriCNES,
 * Copyright (c) 2025 Chris Siebert, under the MIT license (LICENSES/MIT.txt).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <math.h>

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

// Shifts of the DMC's LFSR from each state until it reaches 1 (and expires on the next), and the
// other way round for 1's cycle, which every rate's seed is on (checked)
static uint16_t lfsrToOne[512];
static uint16_t lfsrAt[512];
static bool lfsrOnCycle;

static uint16_t lfsrShift(uint16_t lfsr) {
	bool b0 = lfsr & 1, b4 = lfsr & 16;
	bool feedback = !((b0 && b4) || !(b0 || b4 || lfsr == 0));
	return (lfsr >> 1) | (feedback ? 0x100 : 0);
}

static void buildLfsr(void) {
	if (lfsrToOne[0]) {
		return;
	}
	for (int state = 0; state < 512; state++) {
		uint16_t l = state;
		int n = 0;
		while (l != 1 && n < 1000) {
			l = lfsrShift(l);
			n++;
		}
		lfsrToOne[state] = n;
	}
	// Back again, for the states on 1's cycle: the state that many shifts from 1
	uint16_t l = 1;
	do {
		lfsrAt[lfsrToOne[l]] = l;
		l = lfsrShift(l);
	} while (l != 1 && lfsrToOne[l] < 512);
	lfsrOnCycle = true;
	for (int i = 0; i < 16; i++) {
		lfsrOnCycle &= lfsrAt[lfsrToOne[dmcLfsrNtsc[i] & 0x1FF]] == dmcLfsrNtsc[i] &&
			lfsrAt[lfsrToOne[dmcLfsrPal[i] & 0x1FF]] == dmcLfsrPal[i];
	}
}

static int32_t pulseMix[31];
static int32_t tndMix[203];

static void buildMixer(void) {
	buildLfsr();
	if (pulseMix[1] != 0) {
		return;
	}
	for (int i = 1; i < 31; i++) {
		pulseMix[i] = (int32_t) (40000.0f * 95.52f / (8128.0f / i + 100.0f));
	}
	for (int i = 1; i < 203; i++) {
		tndMix[i] = (int32_t) (40000.0f * 163.67f / (24329.0f / i + 100.0f));
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
	apu->mixDirty = true;
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
	apu->mixDirty = true;
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
	apu->mixDirty = true;
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
}

// Halt bits written this cycle take effect after its length counter clock
static void applyHalts(struct Apu* apu) {
	apu->mixDirty = true;
	apu->haltPending = false;
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
	// With the silence flag set, the level stays where it is, as a $4011 write left it
	if (d->silent) {
	} else if (d->shifter & 1) {
		if (d->output <= 125) {
			d->output += 2;
			nes->apu.mixDirty = true;
		}
	} else if (d->output >= 2) {
		d->output -= 2;
		nes->apu.mixDirty = true;
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
	d->lfsr = lfsrShift(lfsr);
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
	if (half || apu->lengthReloadAny) {
		lengthCounters(apu, half);
		apu->lengthReloadAny = false;
	}
	if (apu->haltPending) {
		applyHalts(apu);
	}
}

// The frontend's mix: each channel scaled before the console's mixing curves, per side
static int mixSide(const struct Mixer* m, int side, const int* levels, int cart) {
	const float (*g)[2] = m->gain;
	float pulse = levels[0] * g[MIX_PULSE1][side] + levels[1] * g[MIX_PULSE2][side];
	float tnd = 3 * levels[2] * g[MIX_TRIANGLE][side] + 2 * levels[3] * g[MIX_NOISE][side] + levels[4] * g[MIX_DMC][side];
	float value = (pulse > 0 ? 40000.0f * 95.52f / (8128.0f / pulse + 100.0f) : 0) +
		(tnd > 0 ? 40000.0f * 163.67f / (24329.0f / tnd + 100.0f) : 0) + cart * g[MIX_CART][side];
	return (int) lrintf(value);
}

static void channelLevels(const struct Apu* apu, int levels[5]) {
	for (int i = 0; i < 2; i++) {
		const struct Pulse* p = &apu->pulse[i];
		levels[i] = p->lengthCounter && !pulseMuted(p) && dutyTable[p->duty][p->dutyStep] ? (p->constant ? p->volume : p->envelopeDecay) : 0;
	}
	levels[2] = triangleTable[apu->triangle.step];
	levels[3] = apu->noise.lengthCounter && !(apu->noise.shift & 1) ? (apu->noise.constant ? apu->noise.volume : apu->noise.envelopeDecay) : 0;
	levels[4] = apu->dmc.output;
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
	int value = pulseMix[pulse] + tndMix[3 * triangle + 2 * noise + apu->dmc.output];
	if (nes->board->audio) {
		value += nes->board->audio(nes);
	}
	return value;
}

__attribute__((noinline, cold)) static int mixStereo(struct NES* nes, int* right) {
	int levels[5];
	channelLevels(&nes->apu, levels);
	int cart = nes->board->audio ? nes->board->audio(nes) : 0;
	*right = mixSide(&nes->mixer, 1, levels, cart);
	return mixSide(&nes->mixer, 0, levels, cart);
}

static inline int16_t filter(int32_t* hpPrev, int32_t* hpOut, int32_t* lpOut, int32_t in) {
	// The console's filters: high-pass at about 37 Hz, low-pass at about 14 kHz
	*hpOut = *hpOut + in - *hpPrev - (*hpOut >> 8);
	*hpPrev = in;
	*lpOut += ((*hpOut - *lpOut) * 3) >> 2;
	int32_t s = *lpOut;
	return (int16_t) (s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

static inline void emitSample(struct NES* nes, int32_t left, int32_t right, bool stereo) {
	if (nes->audioCount + 2 <= nes->audioCapacity) {
		int16_t l = filter(&nes->hpPrev, &nes->hpOut, &nes->lpOut, left);
		struct Mixer* m = &nes->mixer;
		nes->audioOut[nes->audioCount++] = l;
		nes->audioOut[nes->audioCount++] = stereo ? filter(&m->hpPrev, &m->hpOut, &m->lpOut, right) : l;
	}
}

static inline int clockRateOf(const struct NES* nes) {
	return nes->region == REGION_NTSC ? 1789773 : (nes->region == REGION_PAL ? 1662607 : 1773448);
}

// Samples for the APU cycles since the last flush: each is the average level over the cycles since
// the one before, ending on the cycle that crosses its time. The right side only with the
// frontend's mixer; otherwise it's the left.
static inline void flush(struct NES* nes, bool stereo) {
	uint64_t end = nes->apu.cycles;
	uint64_t cycle = nes->audioCycle;
	int32_t level = nes->audioLevel, right = nes->mixer.levelRight;
	int rate = clockRateOf(nes);
	uint32_t change = 0;
	while (cycle < end) {
		int64_t need = (rate - nes->audioPhase + nes->sampleRate - 1) / nes->sampleRate;
		if (need < 1) {
			need = 1;
		}
		uint64_t stop = cycle + need < end ? cycle + need : end;
		// Sum the level over cycles cycle+1 .. stop
		uint64_t at = cycle;
		while (change < nes->audioChangeCount && nes->audioChanges[change].cycle <= stop) {
			uint64_t c = nes->audioChanges[change].cycle;
			nes->audioAccum += level * (int32_t) (c - 1 - at);
			if (stereo) {
				nes->mixer.accumRight += right * (int32_t) (c - 1 - at);
				right = nes->audioChanges[change].right;
			}
			at = c - 1;
			level = nes->audioChanges[change].level;
			change++;
		}
		nes->audioAccum += level * (int32_t) (stop - at);
		if (stereo) {
			nes->mixer.accumRight += right * (int32_t) (stop - at);
		}
		nes->audioAccumCount += stop - cycle;
		nes->audioPhase += (int64_t) nes->sampleRate * (int64_t) (stop - cycle);
		cycle = stop;
		if (nes->audioPhase >= rate) {
			nes->audioPhase -= rate;
			emitSample(nes, nes->audioAccum / nes->audioAccumCount, stereo ? nes->mixer.accumRight / nes->audioAccumCount : 0, stereo);
			nes->audioAccum = 0;
			nes->mixer.accumRight = 0;
			nes->audioAccumCount = 0;
		}
	}
	nes->audioCycle = end;
	nes->audioLevel = level;
	nes->mixer.levelRight = stereo ? right : level;
	nes->audioChangeCount = 0;
}

void apuFlushAudio(struct NES* nes) {
	if (nes->mixer.custom) {
		flush(nes, true);
	} else {
		flush(nes, false);
	}
}

static inline void levelChange(struct NES* nes, int32_t level, int32_t right) {
	if (nes->audioChangeCount == sizeof(nes->audioChanges) / sizeof(nes->audioChanges[0])) {
		// Full: flush the cycles before this one
		nes->apu.cycles--;
		apuFlushAudio(nes);
		nes->apu.cycles++;
	}
	nes->audioChanges[nes->audioChangeCount].cycle = nes->apu.cycles;
	nes->audioChanges[nes->audioChangeCount].level = level;
	nes->audioChanges[nes->audioChangeCount].right = right;
	nes->audioChangeCount++;
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
				apu->padShift[0] = inputReport(nes, 0);
				apu->padShift[1] = inputReport(nes, 1);
				inputLatch(nes);
			}
		} else {
			apu->strobed = false;
		}
		for (int i = 0; i < 2; i++) {
			struct Pulse* p = &apu->pulse[i];
			if (p->timer == 0) {
				p->timer = p->timerPeriod;
				p->dutyStep = (p->dutyStep - 1) & 7;
				apu->mixDirty = true;
			} else {
				p->timer--;
			}
		}
		struct Noise* n = &apu->noise;
		if (n->timer == 0) {
			n->timer = n->timerPeriod;
			uint16_t feedback = (n->shift ^ (n->shift >> (n->mode ? 6 : 1))) & 1;
			n->shift = (n->shift >> 1) | (feedback << 14);
			apu->mixDirty = true;
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
		apu->mixDirty = true;
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
			apu->mixDirty = true;
		}
	} else {
		t->timer--;
	}

	frameCounter(nes);
	apu->putCycle = !apu->putCycle;
	if (apu->mixDirty || nes->board->audio) {
		apu->mixDirty = false;
		if (!apu->stereo) {
			int32_t level = mix(nes);
			if (level != apu->lastMix) {
				apu->lastMix = level;
				levelChange(nes, level, level);
			}
		} else {
			int right;
			int32_t level = mixStereo(nes, &right);
			if (level != apu->lastMix || right != nes->mixer.lastRight) {
				apu->lastMix = level;
				nes->mixer.lastRight = right;
				levelChange(nes, level, right);
			}
		}
	}
}

// The most cycles that can pass with nothing to do but count: no delay running, no timer wrapping,
// no frame counter step, no output sample due. A get-cycle timer at t lasts this many cycles.
static inline int getCycles(const struct Apu* apu, int t) {
	return apu->putCycle ? 2 * t + 1 : 2 * t;
}

// Channels whose timers can wrap without the mix changing
static inline bool pulseSilent(struct Pulse* p) {
	return !p->lengthCounter || pulseMuted(p) || !(p->constant ? p->volume : p->envelopeDecay);
}

static inline bool noiseSilent(const struct Noise* n) {
	return !n->lengthCounter || !(n->constant ? n->volume : n->envelopeDecay);
}

static inline bool triangleFrozen(const struct Apu* apu) {
	const struct Triangle* t = &apu->triangle;
	return !t->lengthCounter || !t->linearCounter || t->timerPeriod < 2;
}

static int frameQuiet(const struct NES* nes) {
	const struct Apu* apu = &nes->apu;
	bool pal = nes->region != REGION_NTSC;
	const uint16_t* steps = apu->fiveStep ? (pal ? stepsPal5 : stepsNtsc5) : (pal ? stepsPal : stepsNtsc);
	int n = apu->fiveStep ? 5 : 6;
	for (int i = 0; i < n; i++) {
		if (steps[i] > apu->frameCounter) {
			return steps[i] - apu->frameCounter - 1;
		}
	}
	return 0;
}

// The DMC's output unit with nothing to play: its timer can wrap without anything changing but the
// shifter and the bit count
static inline bool dmcIdle(const struct Dmc* d) {
	return lfsrOnCycle && d->silent && !d->bytesRemaining && !d->setImplicitAbort;
}

static int bulkCycles(struct NES* nes) {
	struct Apu* apu = &nes->apu;
	const struct Dmc* d = &apu->dmc;
	if (apu->padShiftDelay[0] | apu->padShiftDelay[1] | d->enableDelay | d->loadDelay | d->cannotRun |
		apu->clearingFrameFlag | apu->quarterClock | apu->halfClock | apu->lengthReloadAny | apu->haltPending |
		apu->mixDirty | (apu->strobing && !apu->strobed) | d->setImplicitAbort) {
		return 0;
	}
	if (!(apu->frameReset & 0x80) || nes->board->audio) {
		return 0;
	}
	int k = frameQuiet(nes);
	int t;
	for (int i = 0; i < 2; i++) {
		if (!pulseSilent(&apu->pulse[i])) {
			t = getCycles(apu, apu->pulse[i].timer);
			k = t < k ? t : k;
		}
	}
	if (!noiseSilent(&apu->noise)) {
		t = getCycles(apu, apu->noise.timer);
		k = t < k ? t : k;
	}
	if (!dmcIdle(d)) {
		t = getCycles(apu, lfsrToOne[d->lfsr & 0x1FF]);
		k = t < k ? t : k;
	}
	if (!triangleFrozen(apu)) {
		t = apu->triangle.timer;
		k = t < k ? t : k;
	}
	return k;
}

// A timer counting n clocks: how many times it reloads on the way
static inline int timerAdvance(uint16_t* timer, uint16_t period, int n) {
	if (n <= *timer) {
		*timer -= n;
		return 0;
	}
	int rest = n - *timer - 1;
	*timer = period - rest % (period + 1);
	return 1 + rest / (period + 1);
}

static void bulkAdvance(struct NES* nes, int k) {
	struct Apu* apu = &nes->apu;
	int gets = apu->putCycle ? k / 2 : (k + 1) / 2;
	for (int i = 0; i < 2; i++) {
		struct Pulse* p = &apu->pulse[i];
		int wraps = timerAdvance(&p->timer, p->timerPeriod, gets);
		p->dutyStep = (p->dutyStep - wraps) & 7;
	}
	struct Noise* n = &apu->noise;
	for (int wraps = timerAdvance(&n->timer, n->timerPeriod, gets); wraps; wraps--) {
		uint16_t feedback = (n->shift ^ (n->shift >> (n->mode ? 6 : 1))) & 1;
		n->shift = (n->shift >> 1) | (feedback << 14);
	}
	struct Dmc* d = &apu->dmc;
	uint16_t at = lfsrToOne[d->lfsr & 0x1FF];
	if (lfsrOnCycle && lfsrAt[at] == d->lfsr) {
		int wraps = timerAdvance(&at, lfsrToOne[dmcSeed(nes, d->rateIndex) & 0x1FF], gets);
		d->lfsr = lfsrAt[at];
		// An idle output unit's steps (bulkCycles lets the timer wrap only then): the empty shifter
		// shifts and the bit count goes round, the level and the silence flag stay
		if (wraps) {
			d->shifter = wraps >= 8 ? 0 : d->shifter >> wraps;
			d->bitsRemaining = (uint8_t) ((((d->bitsRemaining - 1 - wraps) % 8) + 8) % 8 + 1);
		}
	} else {
		for (int i = 0; i < gets; i++) {
			d->lfsr = lfsrShift(d->lfsr);
		}
	}
	timerAdvance(&apu->triangle.timer, apu->triangle.timerPeriod, k);
	apu->frameCounter += k;
	apu->cycles += k;
	apu->putCycle ^= k & 1;
}

void apuRun(struct NES* nes, int cycles) {
	while (cycles > 0) {
		int k = bulkCycles(nes);
		if (k > 0) {
			k = k < cycles ? k : cycles;
			bulkAdvance(nes, k);
			cycles -= k;
		} else {
			apuCycle(nes);
			cycles--;
		}
	}
}

// Cycles that can pass before the APU changes something the CPU sees: a DMC DMA request, an IRQ, a
// delayed write landing
int apuQuietCycles(struct NES* nes) {
	const struct Apu* apu = &nes->apu;
	// A board's own sound moves on every cycle: the mix has to be taken with it
	if (nes->board->audio) {
		return 0;
	}
	const struct Dmc* d = &apu->dmc;
	if (d->enableDelay | d->loadDelay | d->cannotRun | apu->clearingFrameFlag | apu->quarterClock |
		apu->halfClock | d->setImplicitAbort | !(apu->frameReset & 0x80) | nes->cpu.dmcDma) {
		return 0;
	}
	int k = frameQuiet(nes);
	// The DMC's timer matters only at the tick that empties the shifter with sample bytes left, which
	// asks for a DMA; the ticks before it change nothing but the sound
	if (!d->bytesRemaining) {
		return k;
	}
	int period = 2 * (lfsrToOne[dmcSeed(nes, d->rateIndex) & 0x1FF] + 1);
	int t = getCycles(apu, lfsrToOne[d->lfsr & 0x1FF]) + (d->bitsRemaining - 1) * period;
	return t < k ? t : k;
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
		uint8_t bit = nes->input.device[port] == DEVICE_PAD ? apu->padShift[port] >> 31 : 0;
		uint8_t devices = inputBits(nes, port);
		apu->padShiftDelay[port] = 2;
		apu->strobed = false;
		if (cpu->oamDma && driven) {
			break;
		}
		value = bit | devices | (nes->cart.vs ? 0 : cpu->openBus & 0xE0);
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
	apu->mixDirty = true;
	if (address == 0x4016) {
		inputWrite(nes, value);
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
	if ((address & 3) == 0) {
		apu->haltPending = true;
	}
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
			apu->lengthReload[i] = apu->lengthReloadAny = true;
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
			apu->lengthReload[2] = apu->lengthReloadAny = true;
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
			apu->lengthReload[3] = apu->lengthReloadAny = true;
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
	// Power-on as FCEUmm does, with the frame IRQ off: hardware has it on, but code that never
	// meant to use it then hangs on clones' carts, and games that use it turn it on
	apu->irqInhibit = hard ? true : inhibit;
	apu->frameCounter = 0;
	apu->frameReset = 0xFF;
	apu->strobing = apu->strobed = false;
	apu->padShiftDelay[0] = apu->padShiftDelay[1] = 0;
	apu->irqDetector = false;
	apu->mixDirty = true;
	if (hard) {
		apu->putCycle = true;
	}
	nesSetIrq(nes, IRQ_FRAME | IRQ_DMC, false);
}
