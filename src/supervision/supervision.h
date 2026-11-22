/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "w65c02.h"

#include <stddef.h>
#include <stdint.h>

// The Watara Supervision: a 65C02 at 4 MHz, 8 KiB of RAM, a 160x160 LCD of four shades drawn from
// 8 KiB of video RAM, two square channels, a noise channel and a 4-bit sample DMA, a timer, and a
// cartridge of up to 512 KiB banked 16 KiB at a time. After MAME's driver (Peter Trauner, GPL-2.0+).

enum { SV_WIDTH = 160, SV_HEIGHT = 160, SV_CLOCK = 4000000, SV_FPS = 61 };
// The sound comes out at the clock / 128.
enum { SV_SOUND_RATE = SV_CLOCK / 128 };

// The sound channels, to mute.
enum { SV_SQUARE1 = 1, SV_SQUARE2 = 2, SV_NOISE = 4, SV_SAMPLES = 8 };

// Buttons, as the joypad port has them (set when pressed).
enum {
	SV_RIGHT = 1, SV_LEFT = 2, SV_DOWN = 4, SV_UP = 8, SV_B = 16, SV_A = 32, SV_SELECT = 64, SV_START = 128,
};

struct SvSquare {
	uint8_t reg[4];
	uint8_t on, waveform, volume;
	uint16_t size, pos, count;
};

struct SvNoise {
	uint8_t reg[3];
	uint8_t on, play, left, right, wide, volume, value;
	uint16_t count, state, period, timer;
};

struct SvDma {
	uint8_t reg[5];
	uint8_t on, left, right;
	uint16_t start, period, timer;
	uint32_t bank, size, pos;
};

// All of the machine a state holds.
struct SvMachine {
	uint16_t pc;
	uint8_t a, x, y, s, p;
	uint8_t nmi, irq, waiting, stopped;
	uint8_t ram[0x2000], vram[0x2000], reg[0x2000];
	uint8_t timerOn, timerShot, dmaFinished, buttons;
	// The game of a multicart playing.
	uint8_t game;
	int32_t timerLeft;
	// Cycles run in the frame, and towards the next sound tick; the frames' leftover clocks (of 61ths).
	int32_t frameCycles, soundCycles, frameRemainder;
	// Clocks the CPU is held for (by the video DMA).
	int32_t stall;
	struct SvSquare square[2];
	struct SvNoise noise;
	struct SvDma dma;
	// The sound tick being summed for the next sample.
	int32_t sumLeft, sumRight, summed;
};

struct Supervision {
	struct SvMachine m;
	struct W65c02 cpu;
	uint8_t* rom;
	uint32_t romSize, banks;
	// Games in the cartridge (1 unless a multicart).
	uint32_t games;
	// Sound channels left out of the sound (SV_SQUARE1...), the machine running on the same.
	uint8_t muted;
	// The frame's picture (in the four shades, 0 lightest) and sound (stereo pairs).
	uint8_t picture[SV_WIDTH * SV_HEIGHT];
	int16_t sound[2 * (SV_SOUND_RATE / SV_FPS + 64)];
	int soundFrames;
};

// The cartridge is copied; false if it can't be used.
bool svLoad(struct Supervision* sv, const uint8_t* rom, size_t size);
void svUnload(struct Supervision* sv);
// A multicart moves to its next game.
void svReset(struct Supervision* sv);
// Starts a multicart's game (from 0).
void svPlay(struct Supervision* sv, uint32_t game);
// One frame, with the buttons held through it.
void svRunFrame(struct Supervision* sv, uint8_t buttons);
size_t svStateSize(void);
void svSave(const struct Supervision* sv, void* out);
bool svLoadState(struct Supervision* sv, const void* in, size_t size);
