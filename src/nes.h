/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMNES_NES_H
#define ARMNES_NES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The machine: CPU, PPU, APU, DMA, controllers and the cartridge's board, stepped one CPU cycle at a
// time with the PPU caught up on a shared master clock.

enum Region { REGION_NTSC, REGION_PAL, REGION_DENDY };

enum Mirroring { MIRROR_HORIZONTAL, MIRROR_VERTICAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B, MIRROR_FOUR };

enum { NES_WIDTH = 256, NES_HEIGHT = 240 };

// Interrupt sources that share the CPU's IRQ line.
enum { IRQ_FRAME = 1, IRQ_DMC = 2, IRQ_MAPPER = 4, IRQ_FDS = 8 };

struct NES;

struct Cpu {
	uint16_t pc;
	uint8_t a, x, y, s, p;
	uint64_t cycles;
	uint8_t irqLine;
	bool nmiLine, nmiPrevLine, opcodeCycle;
	bool irqSampled;
	bool needNmi, prevNeedNmi;
	bool runIrq, prevRunIrq;
	bool jammed;
	// DMA: the CPU halts on a read cycle while either unit runs; each has a halt cycle first
	bool dmcDma, dmcHalt;
	bool oamDma, oamHalt, oamFirst, oamAligned;
	uint8_t oamPage, oamIndex, oamValue;
	bool ignoreH;
	uint16_t addressBus;
	// The external data bus (open bus) and the 2A03's internal one, which $4015 reads drive alone
	uint8_t openBus, internalBus;
};

// The 2C02 as TriCNES models it: a full step every dot and a half step between, the $2007 latch
// chain, the multiplexed address bus with its octal latch, and sprite evaluation dot by dot.
struct Ppu {
	int scanline, dot;
	bool oddFrame, skippedDot;
	uint64_t frame, dots;
	bool resetting;

	bool nmiEnabled, increment32, tall, spriteTable, bgTable;
	bool showBg, showSprites, showBgLeft, showSpritesLeft, gray, emphRed, emphGreen, emphBlue;
	bool showBgDelayed, showSpritesDelayed, showBgInstant, showSpritesInstant;
	uint8_t renderingCounter;

	bool vblank, hit, hitDelayed, pendingHit, pendingHit2, overflow, overflowDelayed, canHit;
	bool vset, vsetLatch1, vsetLatch2, pendingVblank, read2002;

	uint16_t v, t;
	uint8_t fineX;
	bool w;
	uint8_t readBuffer;
	uint8_t bus;
	uint64_t busRefresh[8];
	uint16_t addressBus;
	uint8_t octalLatch;
	bool ale;

	uint8_t delay2005, value2005, delay2006;
	uint16_t value2006, value2006Before;
	bool copyV;
	uint8_t delay2001, delay2001Emphasis, delay2001Oam, value2001;
	bool wasRendering2001;

	// $2007: the read and write latch chains
	bool rd, rdSR, rdL[5], pdRb, readAle, ppuRead;
	bool wr, wrSR, wrL[5], dbPar, writeAle, tstepLatch, blnkLatch, paletteEnable, ppuWrite;
	uint8_t writeData;

	uint16_t parNt, parAt, parChr;
	uint8_t renderTemp;
	bool commitNt, commitAt, commitLo, commitHi;
	uint8_t attribute, lowPlane, highPlane, attrLatch;
	uint16_t bgL, bgH, atL, atH;

	uint8_t oam[256], oam2[32];
	uint8_t oamAddr, oam2Addr, oam2Reset;
	bool oam2Overflowed;
	uint8_t oamBufferIn, oamBuffer, oamLatch;
	uint16_t verticalOffset;
	uint8_t evalTick;
	bool nineObjects, oamAddrOverflowed, corruptedOdd, nextHasZero, hasZero;
	uint8_t spriteL[8], spriteH[8], spriteAttr[8], spriteX[8], spriteCounter[8];
	uint8_t spritePattern, spriteAttrNow;

	bool corruptOnDisable, corruptOnDisableInstant, corruptOnEnable, pendingCorrupt;
	uint8_t corruptIndex;

	uint8_t dotColor, prev1, prev2, prev3;

	// The fast model (ppu-fast.c): tile latches, and the next line's sprites as pixels
	uint8_t ntLatch, atLatch, loLatch, hiLatch;
	uint8_t lineSprites[256];
	bool lineHasSprites, lineHasZero;
	uint8_t nextCount;
	bool nextZero;
	uint8_t hitDelay;
	// The master clock of the dot being stepped
	uint64_t dotTick;
	// $2007 reads and writes going through the PPU: how many dots in (0 for none) and the address
	uint8_t vramReadStep, vramWriteStep;
	uint16_t vramReadAddress, vramWriteAddress;

	uint8_t palette[32];
	uint8_t ntRam[0x800];
};

struct Pulse {
	uint8_t duty, dutyStep;
	bool lengthHalt, constant;
	uint8_t volume;
	uint8_t lengthCounter;
	uint16_t timerPeriod, timer;
	bool sweepEnabled, sweepNegate, sweepReload;
	uint8_t sweepPeriod, sweepShift, sweepCounter;
	bool envelopeStart;
	uint8_t envelopeDivider, envelopeDecay;
	bool second;
	bool enabled;
};

struct Triangle {
	bool control;
	uint8_t linearReload, linearCounter;
	bool linearReloadFlag;
	uint8_t lengthCounter;
	uint16_t timerPeriod, timer;
	uint8_t step;
	bool enabled;
};

struct Noise {
	bool lengthHalt, constant;
	uint8_t volume;
	uint8_t lengthCounter;
	bool mode;
	uint16_t timerPeriod, timer;
	uint16_t shift;
	bool envelopeStart;
	uint8_t envelopeDivider, envelopeDecay;
	bool enabled;
};

struct Dmc {
	bool irqEnabled, loop;
	uint8_t rateIndex;
	uint16_t lfsr;
	uint8_t output;
	uint16_t sampleAddress, sampleLength;
	uint16_t currentAddress, bytesRemaining;
	uint8_t buffer, shifter, bitsRemaining;
	bool silent;
	bool enabled, enabledPending;
	uint8_t enableDelay;
	uint8_t loadDelay;
	uint8_t cannotRun;
	bool implicitAbort, setImplicitAbort;
};

struct Apu {
	struct Pulse pulse[2];
	struct Triangle triangle;
	struct Noise noise;
	struct Dmc dmc;
	bool putCycle;
	bool fiveStep, irqInhibit;
	uint16_t frameCounter;
	uint8_t frameReset;
	bool quarterClock, halfClock;
	bool frameFlag, clearingFrameFlag;
	bool dmcFlag;
	// The IRQ level detector the frame counter and DMC drive, latched onto the IRQ line once a cycle
	bool irqDetector;
	bool lengthReload[4], lengthReloadAny, haltPending;
	uint8_t lengthReloadValue[4];
	uint8_t registers[0x10];
	// Controllers: the strobe latches on the APU's get-to-put edge; reads shift two cycles later
	bool strobing, strobed;
	uint8_t padShift[2];
	uint8_t padShiftDelay[2];
	uint64_t cycles;
	int32_t expansion;
	// The mix only changes when a channel's output can
	bool mixDirty;
	int32_t lastMix;
};

// A board: which parts of the cartridge the CPU and PPU see, and its registers.
struct Board {
	const char* name;
	void (*reset)(struct NES*, bool hard);
	// Writes to $4020-$FFFF, after any RAM page there has taken the value
	void (*write)(struct NES*, uint16_t address, uint8_t value);
	// Reads from pages set in readHooks (4 KB each)
	uint8_t (*read)(struct NES*, uint16_t address);
	// Every PPU bus address the PPU puts out, for boards set to watch it
	void (*ppuBus)(struct NES*, uint16_t address);
	// Reads of the PPU's pattern and name tables, for boards that change data as it's fetched
	uint8_t (*ppuRead)(struct NES*, uint16_t address);
	void (*cpuCycle)(struct NES*);
	// M2 rising, partway through each CPU cycle
	void (*m2Rise)(struct NES*);
	// PPU dots during which the board's IRQ line can't change on its own
	int (*irqQuietDots)(struct NES*);
	// The board's own sound, mixed with the APU's
	int (*audio)(struct NES*);
	// Restore pointers from registers after a state load
	void (*sync)(struct NES*);
	size_t stateSize;
};

struct Cart {
	uint8_t* prg;
	uint32_t prgSize;
	uint8_t* chr;
	uint32_t chrSize;
	bool chrRam;
	uint8_t* wram;
	uint32_t wramSize;
	uint32_t batterySize;
	bool battery;
	int mapper, submapper;
	enum Mirroring mirroring;
	enum Region region;
	uint32_t prgCrc;
	uint8_t trainer[512];
	bool hasTrainer;
	uint8_t extraNt[0x800];
};

struct NES {
	struct Cpu cpu;
	struct Ppu ppu;
	struct Apu apu;
	struct Cart cart;
	const struct Board* board;
	uint8_t boardState[1024];

	uint8_t ram[0x800];
	enum Region region;
	uint64_t masterClock;
	// Master clock phases: the CPU's (cpuPeriod long) and the PPU's (ppuPeriod long). The PPU runs
	// behind the CPU by ppuLag master clocks and catches up when something could observe it, with
	// ppuCpuClock the CPU's phase at the clock the PPU is on.
	int cpuClock, ppuClock, ppuCpuClock;
	int cpuPeriod, ppuPeriod;
	int ppuLag, ppuQuietTicks, mapperQuietTicks;
	// Something needs every CPU cycle stepped in full: exact timing or a board hook
	bool slowCycles;
	// APU cycles the CPU has passed but the APU hasn't run, and how many can pass before it must
	int apuPending, apuQuiet;
	bool ppuEveryCycle;
	// The master clock of this CPU cycle's access, for boards that count M2 from the PPU's side, and
	// the next master clock the PPU hasn't run
	uint64_t slotTick, ppuTick;
	// Every master clock in order, as the reference for the catch-up (tests/selfdiff.c)
	bool exactTiming;
	// The half-dot PPU (ppu.c) instead of the fast one (ppu-fast.c)
	bool exactPpu;

	// $4020-$FFFF in 4 KB pages
	uint8_t* prgPages[16];
	bool prgWritable[16];
	uint16_t readHooks;
	// $0000-$3FFF on the PPU bus in 1 KB pages
	uint8_t* chrPages[16];
	bool chrWritable[16];
	bool watchPpuBus;
	bool ppuReadHook;

	// Controllers: buttons pressed, bit 0 A to bit 7 Right
	uint8_t pads[2];

	uint16_t* frameBuffer;
	bool frameDone;

	// Audio: samples at the CPU's rate are filtered down to sampleRate
	int16_t* audioOut;
	size_t audioCount, audioCapacity;
	int sampleRate;
	int64_t audioPhase;
	int32_t audioAccum, audioAccumCount;
	// The mix's level from audioCycle on, and where it changes after that, in APU cycles; turned into
	// samples by apuFlushAudio()
	uint64_t audioCycle;
	int32_t audioLevel;
	uint32_t audioChangeCount;
	struct AudioChange {
		uint64_t cycle;
		int32_t level;
	} audioChanges[8192];
	int32_t hpPrev, hpOut, lpOut;
};

// cart.c
bool cartLoad(struct NES*, const uint8_t* data, size_t size);
void cartFree(struct NES*);

// nes.c
void nesInit(struct NES*);
bool nesLoad(struct NES*, const uint8_t* data, size_t size);
void nesReset(struct NES*, bool hard);
void nesRunFrame(struct NES*);
void nesFree(struct NES*);
void nesSetRegion(struct NES*, enum Region);

// state.c
size_t nesStateSize(const struct NES*);
bool nesSaveState(struct NES*, void* data, size_t size);
bool nesLoadState(struct NES*, const void* data, size_t size);
void nesSetMirroring(struct NES*, enum Mirroring);
void nesSetNametable(struct NES*, int index, uint8_t* page);
// Banks in units of size bytes, counted from the start of PRG ROM, CHR (ROM or RAM) or work RAM
void nesMapPrg(struct NES*, uint16_t address, uint32_t size, int bank);
void nesMapWram(struct NES*, uint16_t address, uint32_t size, int bank, bool writable);
void nesUnmapPrg(struct NES*, uint16_t address, uint32_t size);
void nesMapChr(struct NES*, uint16_t address, uint32_t size, int bank);
void nesSetIrq(struct NES*, int source, bool on);

// cpu.c
void nesTick(struct NES*);
void nesPpuSync(struct NES*);
void nesApuSync(struct NES*);
void nesPpuSyncAhead(struct NES*, int clocks);
void cpuReset(struct NES*, bool hard);
void cpuStep(struct NES*);
uint8_t cpuPeek(struct NES*, uint16_t address);

// bus.c
uint8_t busRead(struct NES*, uint16_t address);
void busWrite(struct NES*, uint16_t address, uint8_t value);

// ppu.c
void ppuReset(struct NES*, bool hard);
void ppuStep(struct NES*);
void ppuHalfStep(struct NES*);
uint8_t ppuReadRegister(struct NES*, uint16_t address);
void ppuWriteRegister(struct NES*, uint16_t address, uint8_t value, uint8_t busBefore);
bool ppuNmiOutput(const struct NES*);
int ppuQuietDots(const struct NES*);

// ppu-fast.c
void fastRun(struct NES*, int dots);
uint8_t fastReadRegister(struct NES*, uint16_t address);
void fastWriteRegister(struct NES*, uint16_t address, uint8_t value, uint8_t busBefore);
uint8_t ppuBusRead(struct NES*, uint16_t address);

// apu.c
void apuReset(struct NES*, bool hard);
void apuCycle(struct NES*);
void apuRun(struct NES*, int cycles);
int apuQuietCycles(struct NES*);
void apuFlushAudio(struct NES*);
uint8_t apuReadStatus(struct NES*);
void apuWrite(struct NES*, uint16_t address, uint8_t value);
void apuDmcDmaGet(struct NES*);
uint8_t apuConflictRead(struct NES*, uint8_t value, uint16_t address, bool driven);
void apuLatchIrq(struct NES*);

// boards.c
const struct Board* boardFind(int mapper, int submapper);

#endif
