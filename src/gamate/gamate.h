/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "../boards/board.h"
#include "../supervision/w65c02.h"

#include <stddef.h>
#include <stdint.h>

// The Bit Corporation Gamate: a 65C02 at 2.2165 MHz with a 4 KiB BIOS, 1 KiB of RAM, a 160x150 LCD of
// four shades drawn from two 8 KiB bit planes, an AY-3-8910's sound (three squares, noise and an
// envelope, in stereo), and cartridges of up to 512 KiB, each with the BIOS's protection check.
// After MAME's driver (Peter Trauner, GPL-2.0+) and its video, cartridge and protection devices
// (David Haywood, Peter Wilhelmsen, Kevin Horton; BSD-3-Clause).

enum { GM_WIDTH = 160, GM_HEIGHT = 150, GM_CLOCK = 2216500, GM_BIOS_SIZE = 0x1000 };
// A frame is 72900 of the 4.433 MHz clock: 36450 CPU cycles, 243 a line (60.8093 Hz)
enum { GM_FRAME_CYCLES = 36450, GM_LINE_CYCLES = 243 };
#define GM_FPS (4433000.0 / 72900.0)
// The sound comes out at the CPU clock / 64.
#define GM_SOUND_RATE (GM_CLOCK / 64.0)

// Buttons, as the joypad port has them (set when pressed).
enum {
	GM_UP = 1, GM_DOWN = 2, GM_LEFT = 4, GM_RIGHT = 8, GM_A = 16, GM_B = 32, GM_START = 64, GM_SELECT = 128,
};

// The sound channels, to mute: the AY's three squares (noise mixes into them) as A, B and C.
enum { GM_CHANNEL_A = 1, GM_CHANNEL_B = 2, GM_CHANNEL_C = 4 };

// All of the machine a state holds.
struct GmMachine {
	uint16_t pc;
	uint8_t a, x, y, s, p;
	uint8_t nmi, irq, waiting, stopped;
	uint8_t ram[0x400];
	// The two bit planes, interleaved: each address's plane 0 byte, then its plane 1 byte.
	uint8_t vram[0x4000];
	uint16_t vramAddress;
	uint8_t plane, scrollX, scrollY, lcd, buttons;
	// The cartridge: its bank at $A000 (and a 4 in 1's at $6000), the card flag, and the protection's
	// serial exchange (each byte of "BIT CORPORATION" in, then one out).
	uint8_t bank, multibank, cardAvailable;
	uint8_t protectionPassed, protectionFailed, protectionReady, protectionBit, protectionByte, protectionCount;
	struct Sunsoft5b ay;
	// Cycles run in the frame, to the next timer IRQ, and to the IRQ line dropping; the line drawn next.
	int32_t frameCycles, irqLeft, irqHeld, line;
	// The sound: cycles towards the next tick and sample, and the ticks being summed.
	int32_t soundCycles, sumLeft, sumRight, summed;
};

struct Gamate {
	struct GmMachine m;
	struct W65c02 cpu;
	uint8_t* rom;
	uint32_t romSize;
	// 0 plain, 1 banked, 2 the 4 in 1
	int type;
	uint8_t bios[GM_BIOS_SIZE];
	// Sound channels left out of the sound, the machine running on the same.
	uint8_t muted;
	// The frame's picture (in the four shades, 0 lightest) and sound (stereo pairs).
	uint8_t picture[GM_WIDTH * GM_HEIGHT];
	int16_t sound[2 * 640];
	int soundFrames;
};

// The BIOS and the cartridge are copied; false if they can't be used.
bool gmLoad(struct Gamate* gm, const uint8_t* bios, const uint8_t* rom, size_t size);
void gmUnload(struct Gamate* gm);
void gmReset(struct Gamate* gm);
// One frame, with the buttons held through it.
void gmRunFrame(struct Gamate* gm, uint8_t buttons);
size_t gmStateSize(void);
void gmSave(const struct Gamate* gm, void* out);
bool gmLoadState(struct Gamate* gm, const void* in, size_t size);
