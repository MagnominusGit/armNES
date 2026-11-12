/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMNES_BOARD_H
#define ARMNES_BOARD_H

// What every board file shares: its registers live in nes->boardState, it maps in banks of the
// sizes the hardware uses, and the registry (list.c) finds it by iNES/NES 2.0 mapper number.

#include "nes.h"

#include <string.h>

#define STATE(type) ((struct type*) nes->boardState)

static inline void prg8(struct NES* nes, uint16_t address, int bank) {
	nesMapPrg(nes, address, 0x2000, bank);
}

static inline void prg16(struct NES* nes, uint16_t address, int bank) {
	nesMapPrg(nes, address, 0x4000, bank);
}

static inline void prg32(struct NES* nes, int bank) {
	nesMapPrg(nes, 0x8000, 0x8000, bank);
}

static inline void chr1(struct NES* nes, uint16_t address, int bank) {
	nesMapChr(nes, address, 0x400, bank);
}

static inline void chr2(struct NES* nes, uint16_t address, int bank) {
	nesMapChr(nes, address, 0x800, bank);
}

static inline void chr4(struct NES* nes, uint16_t address, int bank) {
	nesMapChr(nes, address, 0x1000, bank);
}

static inline void chr8(struct NES* nes, int bank) {
	nesMapChr(nes, 0, 0x2000, bank);
}

static inline void wram8(struct NES* nes) {
	nesMapWram(nes, 0x6000, 0x2000, 0, true);
}

// Each name table from CIRAM's two pages
static inline void mirrorPages(struct NES* nes, int a, int b, int c, int d) {
	nesSetNametable(nes, 0, nes->ppu.ntRam + a * 0x400);
	nesSetNametable(nes, 1, nes->ppu.ntRam + b * 0x400);
	nesSetNametable(nes, 2, nes->ppu.ntRam + c * 0x400);
	nesSetNametable(nes, 3, nes->ppu.ntRam + d * 0x400);
}

// Discrete boards write through the ROM: the value is what both drive on the bus
static inline uint8_t busConflict(struct NES* nes, uint16_t address, uint8_t value) {
	uint8_t* page = nes->prgPages[address >> 12];
	return page ? value & page[address & 0xFFF] : value;
}

// A cart's solder pads or DIP switches: the frontend's setting, else the one known for the dump,
// else 0. They stay put through Reset, as on the cart.
static inline uint8_t jumperSetting(const struct NES* nes) {
	return nes->jumper >= 0 ? nes->jumper : nes->cart.jumper >= 0 ? nes->cart.jumper : 0;
}

#define BOARD(var) extern const struct Board var

// basic.c
BOARD(boardNrom);
BOARD(boardUxrom);
BOARD(boardCnrom);
BOARD(boardAxrom);
BOARD(boardMmc1);
BOARD(boardMmc1Parallel);

// mmc3.c
BOARD(boardMmc3);
BOARD(boardTxsrom);
BOARD(boardTqrom);
BOARD(boardNamco108);
BOARD(boardNamco3446);
BOARD(boardNamco3433);
BOARD(boardNamco3425);
BOARD(boardNamco3453);
BOARD(boardRambo);

// asic.c
BOARD(boardMmc2);
BOARD(boardFme7);
BOARD(boardVrc24);
BOARD(boardVrc2);
BOARD(boardTc0190);
BOARD(boardSs88006);
BOARD(boardNamco163);
BOARD(boardNamco175);
BOARD(boardVrc1);
BOARD(boardVrc3);
BOARD(boardVrc6);
BOARD(boardVrc7);

// multicart.c
BOARD(boardFk23c);
BOARD(boardCoolboy);
BOARD(board8237);

// sachen.c
BOARD(boardS8259);
BOARD(boardSachenLatch);
BOARD(boardTca01);
BOARD(boardS74ls374n);

// jy.c
BOARD(boardJy);

// bandai.c
BOARD(boardBandai);
bool datachSwipe(struct NES*, const char* digits);

// mmc5.c
BOARD(boardMmc5);

// rare.c
BOARD(board127);
BOARD(boardJy043);
BOARD(boardKaraoke);
BOARD(boardQta);
BOARD(boardYuxing);
BOARD(boardVs99);
BOARD(boardKaiser7030);

// fds.c
BOARD(boardFds);
bool fdsCartLoad(struct NES*, const uint8_t* data, size_t size);
int fdsSide(struct NES*);
void fdsInsert(struct NES*, int side);
bool fdsBusy(struct NES*);

// discrete.c
const struct Board* discreteFind(int mapper, int submapper);

// nsf.c: music files, played by a driver of armNES's own, as a mapper number past NES 2.0's
#define NSF_MAPPER 0x1000
BOARD(boardNsf);
bool nsfCartLoad(struct NES*, const uint8_t* data, size_t size);
struct NsfInfo {
	const char *title, *artist, *copyright;
	int songs, song;
};
bool nsfInfo(struct NES*, struct NsfInfo* info);
void nsfSelect(struct NES*, int song);

// The sound chips on carts, for their boards and for NSF files

// asic.c: Sunsoft 5B, an AY-3-8910's three square channels, noise and envelope
struct Sunsoft5b {
	uint8_t address, regs[16];
	uint16_t toneTimer[3];
	bool toneOut[3];
	uint8_t noiseTimer;
	uint32_t noiseShift;
	uint32_t envelopeTimer;
	uint8_t envelopeStep;
	bool envelopeHolding;
	uint8_t divider;
};
void sunsoft5bWrite(struct Sunsoft5b*, uint8_t value);
void sunsoft5bCycle(struct Sunsoft5b*);
int sunsoft5bOutput(const struct Sunsoft5b*);

// asic.c: VRC6, two pulses and a sawtooth at $9000-$B002
struct Vrc6Pulse {
	uint8_t control;
	uint16_t period, timer;
	uint8_t step;
	bool enabled;
};
struct Vrc6Sound {
	struct Vrc6Pulse pulse[2];
	uint8_t sawRate, sawStep, sawAccumulator;
	uint16_t sawPeriod, sawTimer;
	bool sawEnabled;
};
void vrc6SoundWrite(struct Vrc6Sound*, int channel, int reg, uint8_t value);
void vrc6SoundCycle(struct Vrc6Sound*);
int vrc6SoundOutput(const struct Vrc6Sound*);

// asic.c: VRC7's FM synthesizer, through emu2413; one at a time
struct Vrc7Sound {
	uint8_t address;
	uint8_t clock;
	int16_t sample;
};
void vrc7SoundReset(struct Vrc7Sound*, bool hard);
// The synthesizer's own state, saved to data or loaded from it; with data NULL, only measured
size_t vrc7SoundState(uint8_t* data, bool load);
void vrc7SoundWrite(struct Vrc7Sound*, uint8_t value);
void vrc7SoundCycle(struct Vrc7Sound*, bool silenced);
int vrc7SoundOutput(const struct Vrc7Sound*);

// asic.c: Namco 163, up to 8 wavetable channels in 128 bytes of RAM
struct Namco163Sound {
	uint8_t address;
	uint8_t ram[128];
	int8_t output[8];
	uint8_t channel, tick;
};
uint8_t namco163SoundRead(struct Namco163Sound*);
void namco163SoundWrite(struct Namco163Sound*, uint8_t value);
void namco163SoundCycle(struct Namco163Sound*, bool disabled);
int namco163SoundOutput(const struct Namco163Sound*);

// mmc5.c: MMC5's two pulses and PCM at $5000-$5015
struct Mmc5Pulse {
	uint8_t control, length, envelopeDivider, envelopeDecay, step;
	uint16_t period, timer;
	bool envelopeStart, enabled;
};
struct Mmc5Sound {
	struct Mmc5Pulse pulse[2];
	uint8_t pcm;
	uint16_t frameTimer;
	bool odd;
};
void mmc5SoundWrite(struct Mmc5Sound*, uint16_t address, uint8_t value);
uint8_t mmc5SoundStatus(const struct Mmc5Sound*);
void mmc5SoundCycle(struct Mmc5Sound*);
int mmc5SoundOutput(const struct Mmc5Sound*);

// fds.c: the Disk System's wavetable channel with its modulator at $4040-$408A
struct FdsSound {
	uint8_t wave[64], modTable[64];
	uint8_t volume, volumeSpeed, modGain, modSpeed, masterSpeed, masterVolume;
	bool volumeIncrease, volumeDirect, modIncrease, modDirect;
	uint16_t pitch, modPitch;
	bool waveHalt, envelopeHalt, modHalt, waveWrite;
	int8_t modCounter;
	uint8_t modPosition, waveOutput;
	uint32_t wavePhase, modPhase;
	int32_t volumeTimer, modTimer;
	int32_t filtered;
};
void fdsSoundReset(struct FdsSound*);
void fdsSoundWrite(struct FdsSound*, uint16_t address, uint8_t value);
uint8_t fdsSoundRead(const struct FdsSound*, uint16_t address, uint8_t open);
void fdsSoundCycle(struct FdsSound*);

// fceu/bridge.c: FCEUmm's boards, for the rest
BOARD(boardFceu);
BOARD(boardFceuCycle);
const struct Board* fceuFind(int mapper);

#endif
