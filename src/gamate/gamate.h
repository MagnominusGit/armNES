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
// A frame is 72900 of the 4.433 MHz clock: 36450 CPU cycles, 243 a line (60.8093 Hz), each line sent
// to the LCD in three passes of 81
enum { GM_FRAME_CYCLES = 36450, GM_LINE_CYCLES = 243, GM_PASS_CYCLES = 81, GM_PASSES = 3 * 150 };
#define GM_FPS (4433000.0 / 72900.0)
// The sound comes out at the CPU clock / 64.
#define GM_SOUND_RATE (GM_CLOCK / 64.0)

// Buttons, as the joypad port has them (set when pressed).
enum {
	GM_UP = 1, GM_DOWN = 2, GM_LEFT = 4, GM_RIGHT = 8, GM_A = 16, GM_B = 32, GM_START = 64, GM_SELECT = 128,
};

// Bytes from the network waiting to arrive
enum { GM_INCOMING = 64 };

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
	// The cartridge: its bank at $A000 (and a 4 in 1's at $6000), and the protection's
	// serial exchange (each byte of "BIT CORPORATION" in, then one out).
	uint8_t bank, multibank;
	uint8_t protectionPassed, protectionFailed, protectionReady, protectionBit, protectionByte, protectionCount;
	struct Sunsoft5b ay;
	// The link port: the byte being sent, the bits coming in and how many, the last byte received, and
	// the bits gone out (for the network) and how many.
	uint8_t txData, rxShift, rxBits, rxData, outShift, outBits;
	// Cycles run in the frame and to the next timer IRQ (and whether it's waiting); the LCD's pass next.
	int32_t frameCycles, irqLeft, irqPending, line;
	// Cycles the CPU waits after a reset (a second Gamate switched on after the first).
	int32_t held;
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
	// The Gamate at the other end of the link cable, or NULL; and for the second of two together, how
	// many cycles after the first it's switched on (at each reset), so the two don't run in step (two
	// real consoles never do, and a game that times its link by its timer depends on it).
	struct Gamate* link;
	int32_t switchedOnLater;
	// Or the network: each byte sent, with when (in cycles, counted from load); and the bytes come in,
	// each due on this side's same count, with the last one's when it was sent and was due.
	void (*send)(void* context, uint8_t byte, uint32_t sent);
	// Called a few times a frame to take in what the network has brought; and, at the limit, to wait
	// a little for the other Gamate (false: no more waiting this frame).
	void (*poll)(void* context);
	bool (*wait)(void* context);
	// Exact timing: not to run past this cycle (as gmNow counts), where the other Gamate has got plus a
	// lead; and whether the frame was stopped there, to carry on in the next call.
	bool limited;
	uint32_t limit;
	bool midFrame;
	void* sendContext;
	struct {
		uint8_t byte;
		uint32_t due;
	} incoming[GM_INCOMING];
	int incomingCount;
	uint32_t frames, lastSent, lastDue;
	bool haveLast;
	// The frame's picture (in the four shades, 0 lightest) and sound (stereo pairs).
	uint8_t picture[GM_WIDTH * GM_HEIGHT];
	int16_t sound[2 * 640];
	int soundFrames;
};

// The BIOS and the cartridge are copied; false if they can't be used.
bool gmLoad(struct Gamate* gm, const uint8_t* bios, const uint8_t* rom, size_t size);
void gmUnload(struct Gamate* gm);
void gmReset(struct Gamate* gm);
// One frame, with the buttons held through it; false if it stopped at the link's limit (the next call
// carries on).
bool gmRunFrame(struct Gamate* gm, uint8_t buttons);
// This Gamate's time: cycles run since it was loaded.
uint32_t gmNow(const struct Gamate* gm);
// A byte from the other Gamate over the network, sent at that cycle of its count, due at that cycle
// of this one's (now, for Standard timing): it arrives then with an NMI (at once if that's past), but
// no sooner after the byte before than it was sent after it.
void gmLinkReceive(struct Gamate* gm, uint8_t byte, uint32_t sent, uint32_t due);
// A frame of two Gamates joined by the link cable, run an instruction at a time each.
void gmRunLinked(struct Gamate* first, struct Gamate* second, uint8_t buttonsFirst, uint8_t buttonsSecond);
size_t gmStateSize(void);
void gmSave(const struct Gamate* gm, void* out);
bool gmLoadState(struct Gamate* gm, const void* in, size_t size);
