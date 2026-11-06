/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <stdlib.h>
#include <string.h>

// The master clock: NTSC runs the CPU at 1/12 and the PPU at 1/4 of 21.477 MHz, PAL at 1/16 and 1/5
// of 26.6 MHz, and Dendy's CPU at 1/15 of PAL's.

void nesInit(struct NES* nes) {
	memset(nes, 0, sizeof(*nes));
	nes->jumper = -1;
	nes->frameBuffer = calloc(NES_WIDTH * NES_HEIGHT, sizeof(uint16_t));
	nes->sampleRate = 48000;
}

void nesFree(struct NES* nes) {
	cartFree(nes);
	free(nes->frameBuffer);
	nes->frameBuffer = NULL;
}

void nesSetIrq(struct NES* nes, int source, bool on) {
	if (on) {
		nes->cpu.irqLine |= source;
	} else {
		nes->cpu.irqLine &= ~source;
	}
}

static int bankCount(uint32_t total, uint32_t size) {
	int count = total / size;
	return count ? count : 1;
}

static int wrapBank(int bank, int count) {
	if (bank < 0) {
		bank += count * ((-bank + count - 1) / count);
	}
	return bank % count;
}

void nesMapPrg(struct NES* nes, uint16_t address, uint32_t size, int bank) {
	struct Cart* cart = &nes->cart;
	bank = wrapBank(bank, bankCount(cart->prgSize, size));
	uint32_t offset = bank * size;
	for (uint32_t i = 0; i < size; i += 0x1000) {
		int page = (address + i) >> 12;
		uint32_t at = (offset + i) % cart->prgSize;
		nes->prgPages[page] = cart->prg + at;
		nes->prgWritable[page] = false;
	}
}

void nesMapWram(struct NES* nes, uint16_t address, uint32_t size, int bank, bool writable) {
	struct Cart* cart = &nes->cart;
	uint32_t total = cart->wramSize ? cart->wramSize : 0x2000;
	bank = wrapBank(bank, bankCount(total, size));
	uint32_t offset = bank * size;
	for (uint32_t i = 0; i < size; i += 0x1000) {
		int page = (address + i) >> 12;
		nes->prgPages[page] = cart->wram + (offset + i) % total;
		nes->prgWritable[page] = writable;
	}
}

void nesUnmapPrg(struct NES* nes, uint16_t address, uint32_t size) {
	for (uint32_t i = 0; i < size; i += 0x1000) {
		int page = (address + i) >> 12;
		nes->prgPages[page] = NULL;
		nes->prgWritable[page] = false;
	}
}

void nesMapChr(struct NES* nes, uint16_t address, uint32_t size, int bank) {
	struct Cart* cart = &nes->cart;
	bank = wrapBank(bank, bankCount(cart->chrSize, size));
	uint32_t offset = bank * size;
	for (uint32_t i = 0; i < size; i += 0x400) {
		int page = (address + i) >> 10;
		nes->chrPages[page] = cart->chr + (offset + i) % cart->chrSize;
		nes->chrWritable[page] = cart->chrRam;
	}
}

void nesMapVram(struct NES* nes, uint16_t address, uint32_t size, int bank) {
	struct Cart* cart = &nes->cart;
	if (!cart->vram) {
		nesMapChr(nes, address, size, bank);
		return;
	}
	bank = wrapBank(bank, bankCount(cart->vramSize, size));
	uint32_t offset = bank * size;
	for (uint32_t i = 0; i < size; i += 0x400) {
		int page = (address + i) >> 10;
		nes->chrPages[page] = cart->vram + (offset + i) % cart->vramSize;
		nes->chrWritable[page] = true;
	}
}

void nesSetNametable(struct NES* nes, int index, uint8_t* page) {
	nes->chrPages[8 + index] = page;
	nes->chrPages[12 + index] = page;
	nes->chrWritable[8 + index] = true;
	nes->chrWritable[12 + index] = true;
}

void nesSetMirroring(struct NES* nes, enum Mirroring mirroring) {
	uint8_t* a = nes->ppu.ntRam;
	uint8_t* b = nes->ppu.ntRam + 0x400;
	switch (mirroring) {
	case MIRROR_HORIZONTAL:
		nesSetNametable(nes, 0, a);
		nesSetNametable(nes, 1, a);
		nesSetNametable(nes, 2, b);
		nesSetNametable(nes, 3, b);
		break;
	case MIRROR_VERTICAL:
		nesSetNametable(nes, 0, a);
		nesSetNametable(nes, 1, b);
		nesSetNametable(nes, 2, a);
		nesSetNametable(nes, 3, b);
		break;
	case MIRROR_SCREEN_A:
		for (int i = 0; i < 4; i++) {
			nesSetNametable(nes, i, a);
		}
		break;
	case MIRROR_SCREEN_B:
		for (int i = 0; i < 4; i++) {
			nesSetNametable(nes, i, b);
		}
		break;
	case MIRROR_FOUR:
		nesSetNametable(nes, 0, a);
		nesSetNametable(nes, 1, b);
		nesSetNametable(nes, 2, nes->cart.extraNt);
		nesSetNametable(nes, 3, nes->cart.extraNt + 0x400);
		break;
	}
}

void nesSetRegion(struct NES* nes, enum Region region) {
	nes->region = region;
	switch (region) {
	case REGION_NTSC:
		nes->cpuPeriod = 12;
		nes->ppuPeriod = 4;
		break;
	case REGION_PAL:
		nes->cpuPeriod = 16;
		nes->ppuPeriod = 5;
		break;
	case REGION_DENDY:
		nes->cpuPeriod = 15;
		nes->ppuPeriod = 5;
		break;
	}
}

bool nesLoad(struct NES* nes, const uint8_t* data, size_t size) {
	if (!cartLoad(nes, data, size)) {
		return false;
	}
	nes->board = boardFind(nes->cart.mapper, nes->cart.submapper);
	if (!nes->board) {
		cartFree(nes);
		return false;
	}
	nesSetRegion(nes, nes->cart.region);
	nesReset(nes, true);
	return true;
}

void nesReset(struct NES* nes, bool hard) {
	if (hard) {
		// Power-on RAM: a pattern most games accept
		memset(nes->ram, 0, sizeof(nes->ram));
		memset(nes->boardState, 0, sizeof(nes->boardState));
		memset(nes->prgPages, 0, sizeof(nes->prgPages));
		memset(nes->prgWritable, 0, sizeof(nes->prgWritable));
		memset(nes->chrPages, 0, sizeof(nes->chrPages));
		nes->hasJumper = false;
		nes->readHooks = 0;
		nes->watchPpuBus = false;
		nes->ppuReadHook = false;
		nes->masterClock = 0;
		nes->cpu.cycles = 0;
		nes->apu.cycles = 0;
	}
	ppuReset(nes, hard);
	apuReset(nes, hard);
	if (hard) {
		nes->cpuClock = 0;
		nes->ppuClock = 0;
		nes->ppuCpuClock = 0;
		nes->ppuLag = 0;
		nes->ppuQuietTicks = 0;
		nes->ppuTick = nes->masterClock;
		nes->apuPending = 0;
		nes->apuQuiet = 0;
	}
	if (hard) {
		nesMapChr(nes, 0, 0x2000, 0);
		nesSetMirroring(nes, nes->cart.mirroring);
		if (nes->cart.hasTrainer && nes->cart.wram && nes->cart.wramSize >= 0x2000) {
			memcpy(nes->cart.wram + 0x1000, nes->cart.trainer, 512);
		}
	}
	nes->board->reset(nes, hard);
	nes->ppuEveryCycle = nes->board->m2Rise != NULL;
	nes->slowCycles = nes->ppuEveryCycle || nes->board->cpuCycle || nes->exactTiming;
	nes->mapperQuietTicks = 0;
	nes->quietBudget = 0;
	cpuReset(nes, hard);
}

void nesRunFrame(struct NES* nes) {
	nes->frameDone = false;
	nes->audioCount = 0;
	while (!nes->frameDone) {
		cpuStep(nes);
		// The frame ends on the instruction during which VBL starts
		if (nes->ppuLag >= nes->ppuQuietTicks) {
			nesPpuSync(nes);
		}
	}
	nesApuSync(nes);
	apuFlushAudio(nes);
}
