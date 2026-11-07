/* Copyright (c) 2026 Magnominus
 * The mapping functions are FCEUmm's cart.c (Copyright (C) 2002 Xodnizel), under the GNU General
 * Public License, version 2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

#include <stdlib.h>

#include "fceu-types.h"
#include "fceu.h"
#include "cart.h"
#include "ppu.h"
#include "x6502.h"
#include "state.h"
#include "fceu-memory.h"
#include "sound.h"
#include "git.h"
#include "ines.h"
#include "unif.h"
#include "fds_apu.h"

// FCEUmm's board files on armNES, for the mappers armNES has no board of its own for. They bank into
// FCEUmm's Page and VPage tables, which are copied into the NES's page tables after each call into
// a board. Their state is FCEUmm's globals, so there is one of these at a time.

static struct NES* nes;
static CartInfo info;
static bool loaded, dirty, handlersDirty;
static uint16_t handlerPages;

uint8_t* fceuRam;
uint8_t* fceuNtaram;
X6502 X;
uint32_t timestamp, sound_timestamp, soundtsoffs;
uint64_t timestampbase;
int scanline;
uint32_t RefreshAddr;
uint8_t PPU[4];
uint8_t PAL;
unsigned dendy;
int MMC5Hack, PEC586Hack;
void (*GameHBIRQHook)(void), (*GameHBIRQHook2)(void);
void FP_FASTAPASS(1) (*PPU_hook)(uint32_t A);
void FP_FASTAPASS(1) (*MapIRQHook)(int a);
void (*GameStateRestore)(int version);
static FCEUGI gameInfo;
FCEUGI* GameInfo = &gameInfo;
uint8_t* vnapage[4];
uint8_t PPUNTARAM, PPUCHRRAM;
readfunc ARead[0x10000];
writefunc BWrite[0x10000];
uint8_t *ROM, *VROM, *UNIFchrrama, *trainerpoo, *MiscROM;
uint32_t ROM_size, VROM_size;
EXPSOUND GameExpSound;
iNES_HEADER head;

void FCEUPPU_LineUpdate(void) {
	dirty = true;
}

// From FCEUmm's cart.c

uint8_t *Page[32], *VPage[8];
uint8_t **VPageR = VPage;
uint8_t *MMC5SPRVPage[8];
uint8_t *MMC5BGVPage[8];

static uint8_t PRGIsRAM[32];

static int CHRram[32];
static int PRGram[32];

uint8_t *PRGptr[32];
uint8_t *CHRptr[32];

uint32_t PRGsize[32];
uint32_t CHRsize[32];

uint32_t PRGmask2[32];
uint32_t PRGmask4[32];
uint32_t PRGmask8[32];
uint32_t PRGmask16[32];
uint32_t PRGmask32[32];

uint32_t CHRmask1[32];
uint32_t CHRmask2[32];
uint32_t CHRmask4[32];
uint32_t CHRmask8[32];

static INLINE void setpageptr(int s, uint32_t A, uint8_t *p, int ram) {
	uint32_t AB = A >> 11;
	int x;

	dirty = true;
	if (p)
		for (x = (s >> 1) - 1; x >= 0; x--) {
			PRGIsRAM[AB + x] = ram;
			Page[AB + x] = p - A;
		}
	else
		for (x = (s >> 1) - 1; x >= 0; x--) {
			PRGIsRAM[AB + x] = 0;
			Page[AB + x] = 0;
		}
}

static uint8_t nothing[8192];
void ResetCartMapping(void) {
	int x;

	for (x = 0; x < 32; x++) {
		Page[x] = nothing - x * 2048;
		PRGptr[x] = CHRptr[x] = 0;
		PRGsize[x] = CHRsize[x] = 0;
	}
	for (x = 0; x < 8; x++) {
		MMC5SPRVPage[x] = MMC5BGVPage[x] = VPageR[x] = nothing - 0x400 * x;
	}
}

void SetupCartPRGMapping(int chip, uint8_t *p, uint32_t size, int ram) {
	PRGptr[chip] = p;
	PRGsize[chip] = size;

	PRGmask2[chip] = (size >> 11) - 1;
	PRGmask4[chip] = (size >> 12) - 1;
	PRGmask8[chip] = (size >> 13) - 1;
	PRGmask16[chip] = (size >> 14) - 1;
	PRGmask32[chip] = (size >> 15) - 1;

	PRGram[chip] = ram ? 1 : 0;
}

void SetupCartCHRMapping(int chip, uint8_t *p, uint32_t size, int ram) {
	CHRptr[chip] = p;
	CHRsize[chip] = size;

	CHRmask1[chip] = (size >> 10) - 1;
	CHRmask2[chip] = (size >> 11) - 1;
	CHRmask4[chip] = (size >> 12) - 1;
	CHRmask8[chip] = (size >> 13) - 1;

	CHRram[chip] = ram;
}

DECLFR(CartBR) {
	return Page[A >> 11][A];
}

DECLFW(CartBW) {
	if (PRGIsRAM[A >> 11] && Page[A >> 11])
		Page[A >> 11][A] = V;
}

DECLFR(CartBROB) {
	if (!Page[A >> 11])
		return(X.DB);
	else
		return Page[A >> 11][A];
}

void FASTAPASS(3) setprg2r(int r, uint32_t A, uint32_t V) {
	if (!PRGptr[r] || PRGsize[r] < 2048) {
		setpageptr(2, A, NULL, PRGram[r]);
		return;
	}
	V &= PRGmask2[r];
	setpageptr(2, A, &PRGptr[r][V << 11], PRGram[r]);
}

void FASTAPASS(2) setprg2(uint32_t A, uint32_t V) {
	setprg2r(0, A, V);
}

void FASTAPASS(3) setprg4r(int r, uint32_t A, uint32_t V) {
	if (!PRGptr[r] || PRGsize[r] < 4096) {
		if (PRGptr[r] && PRGsize[r] >= 2048) {
			uint32_t mask2 = PRGmask2[r];
			uint32_t VA = V << 1;
			int x;
			for (x = 0; x < 2; x++)
				setpageptr(2, A + (x << 11), &PRGptr[r][((VA + x) & mask2) << 11], PRGram[r]);
		} else {
			setpageptr(2, A,            NULL, PRGram[r]);
			setpageptr(2, A + 0x800,    NULL, PRGram[r]);
		}
		return;
	}
	V &= PRGmask4[r];
	setpageptr(4, A, &PRGptr[r][V << 12], PRGram[r]);
}

void FASTAPASS(2) setprg4(uint32_t A, uint32_t V) {
	setprg4r(0, A, V);
}

void FASTAPASS(3) setprg8r(int r, uint32_t A, uint32_t V) {
	if (PRGsize[r] >= 8192) {
		V &= PRGmask8[r];
		setpageptr(8, A, PRGptr[r] ? (&PRGptr[r][V << 13]) : 0, PRGram[r]);
	} else {
		uint32_t VA = V << 2;
		int x;
		for (x = 0; x < 4; x++)
			setpageptr(2, A + (x << 11), PRGptr[r] ? (&PRGptr[r][((VA + x) & PRGmask2[r]) << 11]) : 0, PRGram[r]);
	}
}

void FASTAPASS(2) setprg8(uint32_t A, uint32_t V) {
	setprg8r(0, A, V);
}

void FASTAPASS(3) setprg16r(int r, uint32_t A, uint32_t V) {
	if (PRGsize[r] >= 16384) {
		V &= PRGmask16[r];
		setpageptr(16, A, PRGptr[r] ? (&PRGptr[r][V << 14]) : 0, PRGram[r]);
	} else {
		uint32_t VA = V << 3;
		int x;

		for (x = 0; x < 8; x++)
			setpageptr(2, A + (x << 11), PRGptr[r] ? (&PRGptr[r][((VA + x) & PRGmask2[r]) << 11]) : 0, PRGram[r]);
	}
}

void FASTAPASS(2) setprg16(uint32_t A, uint32_t V) {
	setprg16r(0, A, V);
}

void FASTAPASS(3) setprg32r(int r, uint32_t A, uint32_t V) {
	if (PRGsize[r] >= 32768) {
		V &= PRGmask32[r];
		setpageptr(32, A, PRGptr[r] ? (&PRGptr[r][V << 15]) : 0, PRGram[r]);
	} else {
		uint32_t VA = V << 4;
		int x;

		for (x = 0; x < 16; x++)
			setpageptr(2, A + (x << 11), PRGptr[r] ? (&PRGptr[r][((VA + x) & PRGmask2[r]) << 11]) : 0, PRGram[r]);
	}
}

void FASTAPASS(2) setprg32(uint32_t A, uint32_t V) {
	setprg32r(0, A, V);
}

void FASTAPASS(3) setchr1r(int r, uint32_t A, uint32_t V) {
	if (!CHRptr[r]) return;
	if (CHRsize[r] < 1024) return;
	FCEUPPU_LineUpdate();
	V &= CHRmask1[r];
	if (CHRram[r])
		PPUCHRRAM |= (1 << (A >> 10));
	else
		PPUCHRRAM &= ~(1 << (A >> 10));
	VPageR[(A) >> 10] = &CHRptr[r][(V) << 10] - (A);
}

void FASTAPASS(3) setchr2r(int r, uint32_t A, uint32_t V) {
	if (!CHRptr[r]) return;
	if (CHRsize[r] < 2048) return;
	FCEUPPU_LineUpdate();
	V &= CHRmask2[r];
	VPageR[(A) >> 10] = VPageR[((A) >> 10) + 1] = &CHRptr[r][(V) << 11] - (A);
	if (CHRram[r])
		PPUCHRRAM |= (3 << (A >> 10));
	else
		PPUCHRRAM &= ~(3 << (A >> 10));
}

void FASTAPASS(3) setchr4r(int r, uint32_t A, uint32_t V) {
	if (!CHRptr[r]) return;
	if (CHRsize[r] < 4096) return;
	FCEUPPU_LineUpdate();
	V &= CHRmask4[r];
	VPageR[(A) >> 10] = VPageR[((A) >> 10) + 1] =
							VPageR[((A) >> 10) + 2] = VPageR[((A) >> 10) + 3] = &CHRptr[r][(V) << 12] - (A);
	if (CHRram[r])
		PPUCHRRAM |= (15 << (A >> 10));
	else
		PPUCHRRAM &= ~(15 << (A >> 10));
}

void FASTAPASS(2) setchr8r(int r, uint32_t V) {
	int x;

	if (!CHRptr[r]) return;
	FCEUPPU_LineUpdate();
	if (CHRsize[r] < 8192) {
		V = 0;
	} else {
		V &= CHRmask8[r];
	}
	for (x = 7; x >= 0; x--)
		VPageR[x] = &CHRptr[r][V << 13];
	if (CHRram[r])
		PPUCHRRAM |= (255);
	else
		PPUCHRRAM = 0;
}

void FASTAPASS(2) setchr1(uint32_t A, uint32_t V) {
	setchr1r(0, A, V);
}

void FASTAPASS(2) setchr2(uint32_t A, uint32_t V) {
	setchr2r(0, A, V);
}

void FASTAPASS(2) setchr4(uint32_t A, uint32_t V) {
	setchr4r(0, A, V);
}

void FASTAPASS(1) setchr8(uint32_t V) {
	setchr8r(0, V);
}

void FASTAPASS(3) setntamem(uint8_t * p, int ram, uint32_t b) {
	FCEUPPU_LineUpdate();
	vnapage[b] = p;
	PPUNTARAM &= ~(1 << b);
	if (ram)
		PPUNTARAM |= 1 << b;
}

static int mirrorhard = 0;
void setmirrorw(int a, int b, int c, int d) {
	FCEUPPU_LineUpdate();
	vnapage[0] = NTARAM + a * 0x400;
	vnapage[1] = NTARAM + b * 0x400;
	vnapage[2] = NTARAM + c * 0x400;
	vnapage[3] = NTARAM + d * 0x400;
}

void FASTAPASS(1) setmirror(int t) {
	FCEUPPU_LineUpdate();
	if (!mirrorhard) {
		switch (t) {
		case MI_H:
			vnapage[0] = vnapage[1] = NTARAM; vnapage[2] = vnapage[3] = NTARAM + 0x400;
			break;
		case MI_V:
			vnapage[0] = vnapage[2] = NTARAM; vnapage[1] = vnapage[3] = NTARAM + 0x400;
			break;
		case MI_0:
			vnapage[0] = vnapage[1] = vnapage[2] = vnapage[3] = NTARAM;
			break;
		case MI_1:
			vnapage[0] = vnapage[1] = vnapage[2] = vnapage[3] = NTARAM + 0x400;
			break;
		}
		PPUNTARAM = 0xF;
	}
}

void SetupCartMirroring(int m, int hard, uint8_t *extra) {
	if (m < 4) {
		mirrorhard = 0;
		setmirror(m);
	} else {
		vnapage[0] = NTARAM;
		vnapage[1] = NTARAM + 0x400;
		vnapage[2] = extra;
		vnapage[3] = extra + 0x400;
		PPUNTARAM = 0xF;
	}
	mirrorhard = hard;
}

// The rest of FCEUmm's core that boards call

static DECLFW(BNull) {
}

static DECLFR(ANull) {
	return X.DB;
}

static DECLFR(ARAM) {
	return RAM[A & 0x7FF];
}

static DECLFW(BRAM) {
	RAM[A & 0x7FF] = V;
}

readfunc FASTAPASS(1) GetReadHandler(int32_t a) {
	return ARead[a];
}

writefunc FASTAPASS(1) GetWriteHandler(int32_t a) {
	return BWrite[a];
}

void FASTAPASS(3) SetReadHandler(int32_t start, int32_t end, readfunc func) {
	if (!func) {
		func = ANull;
	}
	for (int32_t x = start; x <= end; x++) {
		ARead[x] = func;
	}
	for (int32_t p = start >> 12; p <= end >> 12; p++) {
		handlerPages |= 1 << p;
	}
	handlersDirty = dirty = true;
}

void FASTAPASS(3) SetWriteHandler(int32_t start, int32_t end, writefunc func) {
	if (!func) {
		func = BNull;
	}
	for (int32_t x = start; x <= end; x++) {
		BWrite[x] = func;
	}
}

void FASTAPASS(1) X6502_IRQBegin(int w) {
	X.IRQlow |= w;
	nesSetIrq(nes, IRQ_MAPPER, true);
}

void FASTAPASS(1) X6502_IRQEnd(int w) {
	X.IRQlow &= ~w;
	nesSetIrq(nes, IRQ_MAPPER, X.IRQlow & (FCEU_IQEXT | FCEU_IQEXT2));
}

uint8_t FASTAPASS(1) X6502_DMR(uint32_t A) {
	return A < 0x2000 ? RAM[A & 0x7FF] : ARead[A](A);
}

void FASTAPASS(2) X6502_DMW(uint32_t A, uint8_t V) {
	if (A < 0x2000) {
		RAM[A & 0x7FF] = V;
	} else {
		BWrite[A](A, V);
	}
}

void* FCEU_gmalloc(uint32_t size) {
	return calloc(1, size);
}

void* FCEU_malloc(uint32_t size) {
	return calloc(1, size);
}

void FCEU_gfree(void* ptr) {
	free(ptr);
}

void FCEU_free(void* ptr) {
	FCEU_gfree(ptr);
}

void FCEU_printf(const char* format, ...) {
}

void FCEU_PrintError(const char* format, ...) {
}

void FCEU_CheatAddRAM(int s, uint32_t A, uint8_t* p) {
}

// Boards ask for their jumper as they power on
uint8_t FCEU_Jumper(void) {
	nes->hasJumper = true;
	return jumperSetting(nes);
}

void FCEU_MemoryRand(uint8_t* ptr, uint32_t size) {
	memset(ptr, 0, size);
}

void FCEU_MemoryRand_Reseed(uint32_t seed) {
}

uint32_t uppow2(uint32_t n) {
	uint32_t p = 1;
	while (p < n) {
		p <<= 1;
	}
	return p;
}

// FDS sound on the boards that carry it isn't played yet
void FDSSoundPower(void) {
}

void FDSSoundReset(void) {
}

DECLFW(FDSSoundWrite) {
}

// Save states: FCEUmm's state list, as one block

static struct {
	void* v;
	uint32_t s;
} exState[64];
static int exCount;

void ResetExState(void (*PreSave)(void), void (*PostSave)(void)) {
	exCount = 0;
}

void AddExState(void* v, uint32_t s, int type, char* desc) {
	if (!v || exCount == 64) {
		return;
	}
	if (s == ~0u) {
		for (SFORMAT* f = v; f->v; f++) {
			AddExState(f->v, f->s, 0, NULL);
		}
		return;
	}
	exState[exCount].v = v;
	exState[exCount].s = s & FCEUSTATE_SIZE_MASK;
	exCount++;
}

static size_t fceuSaveExtra(struct NES* n, uint8_t* out) {
	size_t size = sizeof(X.IRQlow);
	if (out) {
		memcpy(out, &X.IRQlow, sizeof(X.IRQlow));
	}
	for (int i = 0; i < exCount; i++) {
		if (out) {
			memcpy(out + size, exState[i].v, exState[i].s);
		}
		size += exState[i].s;
	}
	return size;
}

// Copies the banks from FCEUmm's tables into the NES's
static void syncPages(void) {
	static readfunc pageRead[16];
	dirty = false;
	if (handlersDirty) {
		for (int p = 4; p < 16; p++) {
			if (!(handlerPages & (1 << p))) {
				continue;
			}
			uint32_t start = p == 4 ? 0x4020 : p << 12;
			readfunc f = ARead[start];
			for (uint32_t a = start + 1; a < (uint32_t) (p + 1) << 12 && f; a++) {
				if (ARead[a] != f) {
					f = NULL;
				}
			}
			pageRead[p] = f;
		}
		handlerPages = 0;
		handlersDirty = false;
	}
	uint16_t hooks = 0;
	for (int p = 4; p < 16; p++) {
		readfunc f = pageRead[p];
		uint8_t* page = Page[p * 2];
		nes->prgPages[p] = NULL;
		nes->prgWritable[p] = false;
		if ((f == CartBR || f == CartBROB) && page == Page[p * 2 + 1]) {
			nes->prgPages[p] = page ? page + (p << 12) : NULL;
		} else if (f != ANull) {
			hooks |= 1 << p;
		}
	}
	nes->readHooks = hooks;
	for (int i = 0; i < 8; i++) {
		nes->chrPages[i] = VPage[i] + (i << 10);
		nes->chrWritable[i] = (PPUCHRRAM >> i) & 1;
	}
	for (int i = 0; i < 4; i++) {
		nes->chrPages[8 + i] = nes->chrPages[12 + i] = vnapage[i];
		nes->chrWritable[8 + i] = nes->chrWritable[12 + i] = (PPUNTARAM >> i) & 1;
	}
}

static void fceuLoadExtra(struct NES* n, const uint8_t* in) {
	nes = n;
	memcpy(&X.IRQlow, in, sizeof(X.IRQlow));
	size_t size = sizeof(X.IRQlow);
	for (int i = 0; i < exCount; i++) {
		memcpy(exState[i].v, in + size, exState[i].s);
		size += exState[i].s;
	}
	if (GameStateRestore) {
		GameStateRestore(1);
	}
	syncPages();
}

static inline void enter(struct NES* n) {
	nes = n;
	timestamp = (uint32_t) n->cpu.cycles;
	scanline = n->ppu.scanline;
	RefreshAddr = n->ppu.v;
}

// Boards that switch between chips switch hooks too
static void syncHooks(void) {
	const struct Board* board = MapIRQHook ? &boardFceuCycle : &boardFceu;
	nes->watchPpuBus = PPU_hook != NULL;
	if (nes->board != board) {
		nes->board = board;
		nes->slowCycles = nes->ppuEveryCycle || board->cpuCycle || nes->exactTiming;
	}
}

static inline void leave(void) {
	if (dirty) {
		syncPages();
	}
	if ((nes->board == &boardFceuCycle) != (MapIRQHook != NULL) || nes->watchPpuBus != (PPU_hook != NULL)) {
		syncHooks();
	}
}

static void fceuWrite(struct NES* n, uint16_t address, uint8_t value) {
	enter(n);
	X.DB = value;
	BWrite[address](address, value);
	leave();
}

static uint8_t fceuRead(struct NES* n, uint16_t address) {
	if (address < 0x4020) {
		return n->cpu.openBus;
	}
	enter(n);
	X.DB = n->cpu.openBus;
	uint8_t value = ARead[address](address);
	leave();
	return value;
}

static void fceuCycle(struct NES* n) {
	enter(n);
	if (MapIRQHook) {
		MapIRQHook(1);
	}
	leave();
}

// FCEUmm's renderer shows its boards the name table and pattern fetches, not the attribute ones,
// which come with A13 still high from the name table's: name table addresses go to the board as
// A13 rises
static bool busA13;

static void fceuPpuBus(struct NES* n, uint16_t address) {
	bool a13 = (address & 0x3000) == 0x2000;
	bool was = busA13;
	busA13 = a13;
	if (a13 && was) {
		return;
	}
	if (PPU_hook) {
		enter(n);
		PPU_hook(address);
		leave();
	}
}

// FCEUmm clocks these in hblank of each rendered line and the pre-render line: here, at the start
// of the next
static void fceuScanline(struct NES* n, int line) {
	struct Ppu* ppu = &n->ppu;
	if (!(ppu->showBg || ppu->showSprites)) {
		return;
	}
	enter(n);
	if (GameHBIRQHook && !(ppu->spriteTable && ppu->bgTable && !ppu->tall)) {
		GameHBIRQHook();
	}
	if (GameHBIRQHook2) {
		GameHBIRQHook2();
	}
	leave();
}

static int fceuIrqQuietDots(struct NES* n) {
	if (PPU_hook) {
		return 8;
	}
	if (GameHBIRQHook || GameHBIRQHook2) {
		return n->ppu.dot < 340 ? 341 - n->ppu.dot : 1;
	}
	return 1 << 20;
}

static void fceuSync(struct NES* n) {
	nes = n;
	syncPages();
}

static const struct {
	int mapper;
	void (*init)(CartInfo*);
} mappers[] = {
#include "mappers.inc"
};

static void (*findInit(int mapper))(CartInfo*) {
	for (size_t i = 0; i < sizeof(mappers) / sizeof(mappers[0]); i++) {
		if (mappers[i].mapper == mapper) {
			return mappers[i].init;
		}
	}
	return NULL;
}

static uint8_t* pow2Copy(const uint8_t* data, uint32_t size, uint32_t* allocated) {
	uint32_t p = uppow2(size);
	uint8_t* copy = malloc(p);
	memset(copy, 0xFF, p);
	memcpy(copy, data, size);
	*allocated = p;
	return copy;
}

static void unload(void) {
	if (!loaded) {
		return;
	}
	if (info.Close) {
		info.Close();
	}
	free(ROM);
	free(VROM);
	ROM = VROM = NULL;
	loaded = false;
}

static void power(struct NES* n) {
	unload();
	struct Cart* cart = &n->cart;
	const uint8_t* h = cart->header;
	memset(&info, 0, sizeof(info));
	memset(&X, 0, sizeof(X));
	GameHBIRQHook = GameHBIRQHook2 = NULL;
	PPU_hook = NULL;
	MapIRQHook = NULL;
	GameStateRestore = NULL;
	memset(&GameExpSound, 0, sizeof(GameExpSound));
	trainerpoo = cart->hasTrainer ? cart->trainer : NULL;
	MiscROM = cart->misc;
	PAL = cart->region == REGION_PAL;
	dendy = cart->region == REGION_DENDY;
	fceuRam = n->ram;
	fceuNtaram = n->ppu.ntRam;

	memcpy(&head, h, sizeof(head));
	info.iNES2 = cart->nes2;
	info.mapper = cart->mapper;
	info.submapper = cart->submapper;
	info.mirror = cart->mirroring == MIRROR_FOUR ? 2 : cart->mirroring == MIRROR_VERTICAL;
	info.battery = cart->battery;
	info.region = cart->region == REGION_PAL ? 1 : cart->region == REGION_DENDY ? 3 : 0;
	info.miscROMSize = cart->miscSize;
	info.miscROMNumber = h[14] & 3;
	info.PRGRomSize = cart->prgSize;
	info.CHRRomSize = cart->chrRam ? 0 : cart->chrSize;
	if (info.iNES2) {
		info.PRGRamSize = cart->wramSize - cart->batterySize;
		info.PRGRamSaveSize = cart->batterySize;
		info.CHRRamSize = cart->chrRam ? cart->chrSize : cart->vramSize;
	}
	memset(&gameInfo, 0, sizeof(gameInfo));

	ResetCartMapping();
	ResetExState(NULL, NULL);
	SetReadHandler(0, 0xFFFF, NULL);
	SetWriteHandler(0, 0xFFFF, NULL);
	SetReadHandler(0, 0x1FFF, ARAM);
	SetWriteHandler(0, 0x1FFF, BRAM);
	uint32_t size;
	ROM = pow2Copy(cart->prg, cart->prgSize, &size);
	ROM_size = cart->prgSize >> 14;
	SetupCartPRGMapping(0, ROM, size, 0);
	if (info.CHRRomSize) {
		VROM = pow2Copy(cart->chr, cart->chrSize, &size);
		VROM_size = cart->chrSize >> 13;
		SetupCartCHRMapping(0, VROM, size, 0);
	} else {
		uint32_t chrRam;
		if (info.iNES2) {
			chrRam = info.CHRRamSize + info.CHRRamSaveSize;
			if (!chrRam) {
				chrRam = info.CHRRamSize = 0x10000;
			}
		} else {
			switch (cart->mapper) {
			case 13:
				chrRam = 0x4000;
				break;
			case 6:
			case 28:
			case 29:
			case 30:
			case 45:
			case 96:
			case 513:
				chrRam = 0x8000;
				break;
			case 176:
				chrRam = 0x20000;
				break;
			case 268:
				chrRam = 0x40000;
				break;
			default:
				chrRam = 0x2000;
				break;
			}
			info.CHRRamSize = chrRam;
		}
		VROM = calloc(1, chrRam);
		VROM_size = 0;
		UNIFchrrama = VROM;
		SetupCartCHRMapping(0, VROM, chrRam, 1);
		AddExState(VROM, chrRam, 0, "CHRR");
	}
	if (cart->mirroring == MIRROR_FOUR) {
		SetupCartMirroring(4, 1, cart->extraNt);
		AddExState(cart->extraNt, sizeof(cart->extraNt), 0, "EXNR");
	} else {
		SetupCartMirroring(info.mirror, 0, NULL);
	}

	findInit(cart->mapper)(&info);
	loaded = true;
	if (info.Power) {
		info.Power();
	}
	if (trainerpoo) {
		for (int x = 0; x < 512; x++) {
			X6502_DMW(0x7000 + x, trainerpoo[x]);
		}
	}

	// The board's battery RAM becomes the cartridge's, keeping what was there
	if (info.SaveGame[0] && info.SaveGameLen[0] && info.SaveGame[0] != cart->wram) {
		uint32_t keep = info.SaveGameLen[0] < cart->wramSize ? info.SaveGameLen[0] : cart->wramSize;
		memcpy(info.SaveGame[0], cart->wram, keep);
		free(cart->wram);
		// Some boards' buffers are static or the CHR RAM, so the board keeps them
		cart->wram = info.SaveGame[0];
		cart->wramBorrowed = true;
		cart->wramSize = cart->batterySize = info.SaveGameLen[0];
	}
}

static void fceuReset(struct NES* n, bool hard) {
	nes = n;
	if (hard || !loaded) {
		power(n);
	} else if (info.Reset) {
		info.Reset();
	}
	n->board = MapIRQHook ? &boardFceuCycle : &boardFceu;
	n->watchPpuBus = PPU_hook != NULL;
	handlerPages = 0xFFF0;
	handlersDirty = true;
	syncPages();
}

const struct Board boardFceu = {
	.name = "FCEUmm", .reset = fceuReset, .write = fceuWrite, .read = fceuRead, .ppuBus = fceuPpuBus,
	.irqQuietDots = fceuIrqQuietDots, .scanline = fceuScanline, .sync = fceuSync, .saveExtra = fceuSaveExtra,
	.loadExtra = fceuLoadExtra,
};

const struct Board boardFceuCycle = {
	.name = "FCEUmm", .reset = fceuReset, .write = fceuWrite, .read = fceuRead, .ppuBus = fceuPpuBus,
	.cpuCycle = fceuCycle, .irqQuietDots = fceuIrqQuietDots, .scanline = fceuScanline, .sync = fceuSync,
	.saveExtra = fceuSaveExtra, .loadExtra = fceuLoadExtra,
};

const struct Board* fceuFind(int mapper) {
	return findInit(mapper) ? &boardFceu : NULL;
}
