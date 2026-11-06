/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <string.h>

// Save states: the machine as it is in memory, with the bank pointers written as which buffer and
// where in it, then the cartridge's work RAM and CHR RAM.

enum { STATE_MAGIC = 0x53454E41, STATE_VERSION = 1 };

struct StateHeader {
	uint32_t magic, version, size, prgCrc;
};

enum Buffer { BUF_NONE, BUF_PRG, BUF_CHR, BUF_WRAM, BUF_NT, BUF_EXTRA_NT, BUF_RAM };

static uint32_t encode(struct NES* nes, const uint8_t* p) {
	struct {
		const uint8_t* base;
		uint32_t size;
	} buffers[] = {
		{ NULL, 0 },
		{ nes->cart.prg, nes->cart.prgSize },
		{ nes->cart.chr, nes->cart.chrSize },
		{ nes->cart.wram, nes->cart.wramSize ? nes->cart.wramSize : 0x2000 },
		{ nes->ppu.ntRam, sizeof(nes->ppu.ntRam) },
		{ nes->cart.extraNt, sizeof(nes->cart.extraNt) },
		{ nes->ram, sizeof(nes->ram) },
	};
	if (!p) {
		return 0;
	}
	for (int i = 1; i < (int) (sizeof(buffers) / sizeof(buffers[0])); i++) {
		if (buffers[i].base && p >= buffers[i].base && p < buffers[i].base + buffers[i].size) {
			return (uint32_t) i << 24 | (uint32_t) (p - buffers[i].base);
		}
	}
	return 0;
}

static uint8_t* decode(struct NES* nes, uint32_t tag) {
	uint32_t offset = tag & 0xFFFFFF;
	switch (tag >> 24) {
	case BUF_PRG:
		return nes->cart.prg + offset;
	case BUF_CHR:
		return nes->cart.chr + offset;
	case BUF_WRAM:
		return nes->cart.wram + offset;
	case BUF_NT:
		return nes->ppu.ntRam + offset;
	case BUF_EXTRA_NT:
		return nes->cart.extraNt + offset;
	case BUF_RAM:
		return nes->ram + offset;
	default:
		return NULL;
	}
}

static size_t wramSize(const struct NES* nes) {
	return nes->cart.wramSize ? nes->cart.wramSize : 0x2000;
}

size_t nesStateSize(const struct NES* nes) {
	return sizeof(struct StateHeader) + sizeof(struct NES) + 32 * sizeof(uint32_t) + wramSize(nes) +
		(nes->cart.chrRam ? nes->cart.chrSize : 0);
}

bool nesSaveState(struct NES* nes, void* data, size_t size) {
	if (size < nesStateSize(nes)) {
		return false;
	}
	uint8_t* out = data;
	struct StateHeader header = { STATE_MAGIC, STATE_VERSION, (uint32_t) sizeof(struct NES), nes->cart.prgCrc };
	memcpy(out, &header, sizeof(header));
	out += sizeof(header);
	memcpy(out, nes, sizeof(struct NES));
	out += sizeof(struct NES);
	uint32_t tags[32];
	for (int i = 0; i < 16; i++) {
		tags[i] = encode(nes, nes->prgPages[i]);
		tags[16 + i] = encode(nes, nes->chrPages[i]);
	}
	memcpy(out, tags, sizeof(tags));
	out += sizeof(tags);
	memcpy(out, nes->cart.wram, wramSize(nes));
	out += wramSize(nes);
	if (nes->cart.chrRam) {
		memcpy(out, nes->cart.chr, nes->cart.chrSize);
	}
	return true;
}

bool nesLoadState(struct NES* nes, const void* data, size_t size) {
	const uint8_t* in = data;
	struct StateHeader header;
	if (size < nesStateSize(nes)) {
		return false;
	}
	memcpy(&header, in, sizeof(header));
	if (header.magic != STATE_MAGIC || header.version != STATE_VERSION || header.size != sizeof(struct NES) ||
		header.prgCrc != nes->cart.prgCrc) {
		return false;
	}
	in += sizeof(header);
	// What belongs to this run rather than to the state
	struct Cart cart = nes->cart;
	const struct Board* board = nes->board;
	uint16_t* frameBuffer = nes->frameBuffer;
	int16_t* audioOut = nes->audioOut;
	size_t audioCapacity = nes->audioCapacity;
	int sampleRate = nes->sampleRate;
	bool exactPpu = nes->exactPpu, exactTiming = nes->exactTiming;
	uint8_t pads[2] = { nes->pads[0], nes->pads[1] };

	memcpy(nes, in, sizeof(struct NES));
	in += sizeof(struct NES);
	nes->cart.prg = cart.prg;
	nes->cart.chr = cart.chr;
	nes->cart.wram = cart.wram;
	nes->board = board;
	nes->frameBuffer = frameBuffer;
	nes->audioOut = audioOut;
	nes->audioCapacity = audioCapacity;
	nes->sampleRate = sampleRate;
	nes->exactPpu = exactPpu;
	nes->exactTiming = exactTiming;
	nes->pads[0] = pads[0];
	nes->pads[1] = pads[1];
	uint32_t tags[32];
	memcpy(tags, in, sizeof(tags));
	in += sizeof(tags);
	for (int i = 0; i < 16; i++) {
		nes->prgPages[i] = decode(nes, tags[i]);
		nes->chrPages[i] = decode(nes, tags[16 + i]);
	}
	memcpy(nes->cart.wram, in, wramSize(nes));
	in += wramSize(nes);
	if (nes->cart.chrRam) {
		memcpy(nes->cart.chr, in, nes->cart.chrSize);
	}
	return true;
}
