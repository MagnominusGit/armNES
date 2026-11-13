/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 1998 BERO
 *  Copyright (C) 2002 Xodnizel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "mapinc.h"

static void GenMMC1Power(void);
static void GenMMC1Init(CartInfo *info, int prg, int chr, int wram, int saveram);

static uint8_t submapper;
static uint8_t DRegs[4];
static uint8_t Buffer, BufferShift;

static uint32_t WRAMSIZE = 0;

static uint32_t NONSaveRAMSIZE = 0;

static void (*MMC1CHRHook4)(uint32_t A, uint8_t V);
static void (*MMC1PRGHook16)(uint32_t A, uint8_t V);

static void (*MMC1WRAMHook8)(void);

static uint8_t *WRAM = NULL;
static uint8_t *CHRRAM = NULL;
static int is155;

static uint32_t MMC1GetCHRBank (uint32_t bank) {
	if (DRegs[0] & 0x10)
		return (DRegs[1 + bank]);
	else
		return ((DRegs[1] & ~1) | bank);
}

static uint8_t MMC1WRAMEnabled(void) {
	return !(DRegs[3] & 0x10);
}

static DECLFW(MBWRAM) {
	if (MMC1WRAMEnabled() || is155)
		Page[A >> 11][A] = V;
}

static DECLFR(MAWRAM) {
	if (!MMC1WRAMEnabled() && !is155)
		return X.DB;
	return(Page[A >> 11][A]);
}

static DECLFR(SFEXPROM_readBank) {
	return DRegs[3] <<3 &0x70;
}

static void MMC1CHR(void) {
	if (MMC1WRAMHook8)
		MMC1WRAMHook8();
	else {
		if (WRAMSIZE > 8192) {
			if (WRAMSIZE > 16384)
				setprg8r(0x10, 0x6000, (DRegs[1] >> 2) & 3);
			else
				setprg8r(0x10, 0x6000, (DRegs[1] >> 3) & 1);
		}
	}
	if (MMC1CHRHook4) {
		if (DRegs[0] & 0x10) {
			MMC1CHRHook4(0x0000, DRegs[1]);
			MMC1CHRHook4(0x1000, DRegs[2]);
		} else {
			MMC1CHRHook4(0x0000, (DRegs[1] & 0xFE));
			MMC1CHRHook4(0x1000, DRegs[1] | 1);
		}
	} else {
		if (DRegs[0] & 0x10) {
			setchr4(0x0000, DRegs[1]);
			setchr4(0x1000, DRegs[2]);
		} else
			setchr8(DRegs[1] >> 1);
	}
}

static void MMC1PRG(void) {
	uint8_t offs = DRegs[1] & 0x10;
	if (MMC1PRGHook16) {
		switch (DRegs[0] & 0xC) {
		case 0xC:
			MMC1PRGHook16(0x8000, (DRegs[3] + offs));
			MMC1PRGHook16(0xC000, 0xF + offs);
			break;
		case 0x8:
			MMC1PRGHook16(0xC000, (DRegs[3] + offs));
			MMC1PRGHook16(0x8000, offs);
			break;
		case 0x0:
		case 0x4:
			MMC1PRGHook16(0x8000, ((DRegs[3] & ~1) + offs));
			MMC1PRGHook16(0xc000, ((DRegs[3] & ~1) + offs + 1));
			break;
		}
	} else {
		switch (DRegs[0] & 0xC) {
		case 0xC:
			setprg16(0x8000, (DRegs[3] + offs));
			setprg16(0xC000, 0xF + offs);
			break;
		case 0x8:
			setprg16(0xC000, (DRegs[3] + offs));
			setprg16(0x8000, offs);
			break;
		case 0x0:
		case 0x4:
			setprg16(0x8000, ((DRegs[3] & ~1) + offs));
			setprg16(0xc000, ((DRegs[3] & ~1) + offs + 1));
			break;
		}
	}
}

static void MMC1MIRROR(void) {
	if (submapper != 7)
		switch (DRegs[0] & 3) {
		case 2: setmirror(MI_V); break;
		case 3: setmirror(MI_H); break;
		case 0: setmirror(MI_0); break;
		case 1: setmirror(MI_1); break;
		}
}

static uint64_t lreset;
static DECLFW(MMC1_write) {
	int n = (A >> 13) - 4;

	if ((timestampbase + timestamp) < (lreset + 2))
		return;

	if (V & 0x80) {
		DRegs[0] |= 0xC;
		BufferShift = Buffer = 0;
		MMC1PRG();
		lreset = timestampbase + timestamp;
		return;
	}

	Buffer |= (V & 1) << (BufferShift++);

	if (BufferShift == 5) {

		DRegs[n] = Buffer;
		BufferShift = Buffer = 0;
		switch (n) {
		case 0: MMC1MIRROR(); MMC1CHR(); MMC1PRG(); break;
		case 1: MMC1CHR(); MMC1PRG(); break;
		case 2: MMC1CHR(); break;
		case 3: MMC1PRG(); break;
		}
	}
}

static void MMC1_Restore(int version) {
	MMC1MIRROR();
	MMC1CHR();
	MMC1PRG();
	/* armNES: the timestamps carry on through a load, so lreset stays as saved */
}

static void MMC1CMReset(void) {
	int i;

	for (i = 0; i < 4; i++)
		DRegs[i] = 0;
	Buffer = BufferShift = 0;
	DRegs[0] = 0x0C;

	DRegs[1] = 0;
	DRegs[2] = 0;
	DRegs[3] = 0;

	MMC1MIRROR();
	MMC1CHR();
	MMC1PRG();
}

static int DetectMMC1WRAMSize(CartInfo *info, int *saveRAM) {
	int workRAM = 8;
	if (info->iNES2) {
		workRAM = (info->PRGRamSize + info->PRGRamSaveSize) / 1024;
		*saveRAM = info->PRGRamSaveSize / 1024;

		if (workRAM > 0 && workRAM < 8)
			workRAM = 8;
		if (workRAM > 32)
			workRAM = 32;
		if (*saveRAM > 0 && *saveRAM < 8)
			*saveRAM = 8;
		if (*saveRAM > 32)
			*saveRAM = 32;

		if (*saveRAM > workRAM) {
			*saveRAM = workRAM;
			workRAM = 0;
		}
	} else if (info->battery) {
		*saveRAM = 8;
	}
	if (workRAM > 8)
		FCEU_printf(" >8KB external WRAM present.  Use NES 2.0 if you hack the ROM image.\n");
	return workRAM;
}

static uint32_t NWCIRQCount;
static uint8_t NWCRec;
static uint32_t nwcdip = 0x4;

static void NWCIRQHook(int a) {
	if (!(NWCRec & 0x10)) {
		NWCIRQCount += a;
		if (NWCIRQCount >= nwcdip) {
			NWCIRQCount = 0;
			X6502_IRQBegin(FCEU_IQEXT);
		}
	}
}

static void NWCCHRHook(uint32_t A, uint8_t V) {
	if ((V & 0x10)) {
		NWCIRQCount = 0;
		X6502_IRQEnd(FCEU_IQEXT);
	}

	NWCRec = V;
	if (V & 0x08)
		MMC1PRG();
	else
		setprg32(0x8000, (V >> 1) & 3);
}

static void NWCPRGHook(uint32_t A, uint8_t V) {
	if (NWCRec & 0x8)
		setprg16(A, 8 | (V & 0x7));
	else
		setprg32(0x8000, (NWCRec >> 1) & 3);
}

static void NWCPower(void) {
	GenMMC1Power();
	setchr8r(0, 0);
	nwcdip = 0x20000000 | ((uint32_t)GameInfo->cspecial << 25);
}

static void NWCReset(void) {
	nwcdip = 0x20000000 | ((uint32_t)GameInfo->cspecial << 25);
	MMC1CMReset();
}

void Mapper105_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 256, 8, 0);
	MMC1CHRHook4 = NWCCHRHook;
	MMC1PRGHook16 = NWCPRGHook;
	MapIRQHook = NWCIRQHook;
	info->Power = NWCPower;
	info->Reset = NWCReset;
}

static void GenMMC1Power(void) {
	lreset = 0;
	SetWriteHandler(0x8000, 0xFFFF, MMC1_write);
	SetReadHandler(0x8000, 0xFFFF, CartBR);

	if (WRAMSIZE) {
		FCEU_CheatAddRAM(8, 0x6000, WRAM);

		if (NONSaveRAMSIZE)
			FCEU_dwmemset(WRAM, 0, NONSaveRAMSIZE);

		SetReadHandler(0x6000, 0x7FFF, MAWRAM);
		SetWriteHandler(0x6000, 0x7FFF, MBWRAM);
		setprg8r(0x10, 0x6000, 0);
	} else
	if (submapper == 8)
		SetReadHandler(0x6000, 0x6FFF, SFEXPROM_readBank);

	MMC1CMReset();
}

static void GenMMC1Close(void) {
	if (CHRRAM)
		FCEU_gfree(CHRRAM);
	if (WRAM)
		FCEU_gfree(WRAM);
	CHRRAM = WRAM = NULL;
}

static void GenMMC1Init(CartInfo *info, int prg, int chr, int wram, int saveram) {
	submapper = info->submapper;
	is155 = 0;

	info->Close = GenMMC1Close;
	MMC1PRGHook16 = MMC1CHRHook4 = 0;
	MMC1WRAMHook8 = 0;
	WRAMSIZE = wram * 1024;
	NONSaveRAMSIZE = (wram - saveram) * 1024;
	PRGmask16[0] &= (prg >> 14) - 1;
	CHRmask4[0] &= (chr >> 12) - 1;
	CHRmask8[0] &= (chr >> 13) - 1;

	if (WRAMSIZE) {
		WRAM = (uint8_t*)FCEU_gmalloc(WRAMSIZE);
		SetupCartPRGMapping(0x10, WRAM, WRAMSIZE, 1);
		AddExState(WRAM, WRAMSIZE, 0, "WRAM");
		if (saveram) {
			info->SaveGame[0] = WRAM + NONSaveRAMSIZE;
			info->SaveGameLen[0] = saveram * 1024;
		}
	}
	if (!chr) {
		CHRRAM = (uint8_t*)FCEU_gmalloc(8192);
		SetupCartCHRMapping(0, CHRRAM, 8192, 1);
		AddExState(CHRRAM, 8192, 0, "CHRR");
	}
	AddExState(DRegs, 4, 0, "DREG");

	info->Power = GenMMC1Power;
	GameStateRestore = MMC1_Restore;
	AddExState(&lreset, 8, 1, "LRST");
	AddExState(&Buffer, 1, 1, "BFFR");
	AddExState(&BufferShift, 1, 1, "BFRS");
}

void Mapper1_Init(CartInfo *info) {
	int bs = info->battery ? 8 : 0;
	int ws = DetectMMC1WRAMSize(info, &bs);
	GenMMC1Init(info, 512, 256, ws, bs);
}

void Mapper155_Init(CartInfo *info) {
	GenMMC1Init(info, 512, 256, 8, info->battery ? 8 : 0);
	is155 = 1;
}

void SAROM_Init(CartInfo *info) {
	GenMMC1Init(info, 128, 64, 8, info->battery ? 8 : 0);
}

void SBROM_Init(CartInfo *info) {
	GenMMC1Init(info, 128, 64, 0, 0);
}

void SCROM_Init(CartInfo *info) {
	GenMMC1Init(info, 128, 128, 0, 0);
}

void SEROM_Init(CartInfo *info) {
	GenMMC1Init(info, 32, 64, 0, 0);
}

void SGROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 0, 0, 0);
}

void SKROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 64, 8, info->battery ? 8 : 0);
}

void SLROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 128, 0, 0);
}

void SL1ROM_Init(CartInfo *info) {
	GenMMC1Init(info, 128, 128, 0, 0);
}

static void SL2ROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 256, 0, 0);
}

static void SFROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 256, 0, 0);
}

static void SHROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 256, 0, 0);
}

void SNROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 0, 8, info->battery ? 8 : 0);
}

void SOROM_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 0, 16, info->battery ? 8 : 0);
}

static uint8_t reg, lock;

static void FARIDSLROM8IN1PRGHook(uint32_t A, uint8_t V) {
	setprg16(A, (V & 0x07) | (reg << 3));
}

static void FARIDSLROM8IN1CHRHook(uint32_t A, uint8_t V) {
	setchr4(A, (V & 0x1F) | (reg << 5));
}

static DECLFW(FARIDSLROM8IN1Write) {
	if (MMC1WRAMEnabled() && !lock) {
		lock = (V & 0x08) >> 3;
		reg = (V & 0xF0) >> 4;
		MMC1MIRROR();
		MMC1CHR();
		MMC1PRG();
	}
}

static void FARIDSLROM8IN1Power(void) {
	reg = lock = 0;
	GenMMC1Power();
	SetWriteHandler(0x6000, 0x7FFF, FARIDSLROM8IN1Write);
}

static void FARIDSLROM8IN1Reset(void) {
	reg = lock = 0;
	MMC1CMReset();
}

void FARIDSLROM8IN1_Init(CartInfo *info) {
	GenMMC1Init(info, 1024, 256, 8, 0);
	MMC1CHRHook4 = FARIDSLROM8IN1CHRHook;
	MMC1PRGHook16 = FARIDSLROM8IN1PRGHook;
	info->Power = FARIDSLROM8IN1Power;
	info->Reset = FARIDSLROM8IN1Reset;
	AddExState(&lock, 1, 0, "LOCK");
	AddExState(&reg, 1, 0, "REG6");
}

static uint8_t game = 0;
static void M374PRG(uint32_t A, uint8_t V) {
	setprg16(A, (V & 0x07) | (game << 3));
}

static void M374CHR(uint32_t A, uint8_t V) {
	setchr4(A, (V & 0x1F) | (game << 5));
}

static void M374Reset(void) {
	game = (game + 1) & 3;
	MMC1CMReset();
}

void Mapper374_Init(CartInfo *info) {
	GenMMC1Init(info, 128, 128, 0, 0);
	MMC1CHRHook4 = M374CHR;
	MMC1PRGHook16 = M374PRG;
	info->Reset = M374Reset;
	AddExState(&game, 1, 0, "GAME");
}

static uint8_t mode;
static uint8_t latch;

static void M297PRG(uint32_t A, uint8_t V) {
	setprg16(A, (V & 0x07) | ((mode & 1) << 3));
}

static void M297CHR(uint32_t A, uint8_t V) {
	setchr4(A, (V & 0x1F) | ((mode & 1) << 5));
}

static void Sync(void) {
	if (mode & 1) {

		MMC1PRG();
		MMC1CHR();
		MMC1MIRROR();
	} else {

		setprg16(0x8000, ((mode & 2) << 1) | ((latch >> 4) & 3));
		setprg16(0xC000, ((mode & 2) << 1) | 3);
		setchr8(latch & 0xF);
		setmirror(1);
	}
}

static DECLFW(M297Mode) {
	if (A & 0x100) {
		mode = V;
		Sync();
	}
}

static DECLFW(M297Latch) {
	if (mode & 1) {
		MMC1_write(A, V);
	} else {
		latch = V;
		Sync();
	}
}

static void M297Power(void) {
	latch = 0;
	mode = 0;
	Sync();
	GenMMC1Power();
	SetWriteHandler(0x4120, 0x4120, M297Mode);
	SetWriteHandler(0x8000, 0xFFFF, M297Latch);
}

static void M297StateRestore(int version) {
	Sync();
}

void Mapper297_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 256, 0, 0);
	info->Power = M297Power;
	MMC1CHRHook4 = M297CHR;
	MMC1PRGHook16 = M297PRG;
	GameStateRestore = M297StateRestore;
	AddExState(&latch, 1, 0, "LATC");
	AddExState(&mode, 1, 0, "MODE");
}

static uint8_t outerBank;
static uint8_t bits;
static uint8_t shift;

static void M543PRG16(uint32_t A, uint8_t V) {
	setprg16(A, (V & 0x0F) | (outerBank << 4));
}

static void M543CHR4(uint32_t A, uint8_t V) {
	setchr4(A, (V & 7));
}

static void M543WRAM8(void) {
	uint32_t wramBank;
	if (outerBank & 2)
		wramBank = 4 | ((outerBank >> 1) & 2) | (outerBank & 1) ;
	else
		wramBank = ((outerBank << 1) & 2) | ((MMC1GetCHRBank(0) >> 3) & 1);
	setprg8r(0x10, 0x6000, wramBank);
}

static DECLFW(M543Write) {
	bits |= ((V >> 3) & 1) << shift++;
	if (shift == 4) {
		outerBank = bits;
		bits = shift = 0;
		MMC1PRG();
		MMC1CHR();
	}
}

static void M543Reset(void) {
	bits = 0;
	shift = 0;
	outerBank = 0;
	MMC1CMReset();
	RAM[0] =0;
}

static void M543Power(void) {
	bits = 0;
	shift = 0;
	outerBank = 0;
	GenMMC1Power();
	SetWriteHandler(0x5000, 0x5FFF, M543Write);
	RAM[0] =0;
}

void Mapper543_Init(CartInfo *info) {

	GenMMC1Init(info, 2048, 32, 64, info->battery ? 64 : 0);
	info->Power = M543Power;
	info->Reset = M543Reset;
	MMC1CHRHook4 = M543CHR4;
	MMC1PRGHook16 = M543PRG16;
	MMC1WRAMHook8 = M543WRAM8;
	AddExState(&bits, 1, 0, "BITS");
	AddExState(&shift, 1, 0, "SHFT");
	AddExState(&outerBank, 1, 0, "BANK");
}

static uint8_t latch;
static uint8_t outerBank;

static void M550PRG16(uint32_t A, uint8_t V) {
	if ((outerBank & 6) == 6)
		setprg16(A, (V & 7) | (outerBank << 2));
	else
		setprg32(0x8000, (latch >> 4) | (outerBank << 1));
}

static void M550CHR4(uint32_t A, uint8_t V) {
	if ((outerBank & 6) == 6)
		setchr4(A, (V & 7) | ((outerBank << 2) & 0x18));
	else
		setchr8((latch & 3) | ((outerBank << 1) & 0x0C));
}

static DECLFW(M550Write7) {
	if (!(outerBank & 8)) {
		outerBank = A & 15;
		MMC1PRG();
		MMC1CHR();
	}
}

static DECLFW(M550Write8) {
	latch = V;
	if ((outerBank & 6) == 6)
		MMC1_write(A, V);
	MMC1PRG();
	MMC1CHR();
}

static void M550Reset(void) {
	latch = 0;
	outerBank = 0;
	MMC1CMReset();
}

static void M550Power(void) {
	latch = 0;
	outerBank = 0;
	GenMMC1Power();
	SetWriteHandler(0x7000, 0x7FFF, M550Write7);
	SetWriteHandler(0x8000, 0xFFFF, M550Write8);
}

void Mapper550_Init(CartInfo *info) {
	GenMMC1Init(info, 512, 128, 8, 0);
	info->Power = M550Power;
	info->Reset = M550Reset;
	MMC1CHRHook4 = M550CHR4;
	MMC1PRGHook16 = M550PRG16;
	AddExState(&latch, 1, 0, "LATC");
	AddExState(&outerBank, 1, 0, "BANK");
}

static uint8_t outerBank;

static void M404PRG16(uint32_t A, uint8_t V) {
	uint8_t mask = outerBank & 0x40 ? 0x07 : 0x0F;
	setprg16(A, (V & mask) | (outerBank << 3) & ~mask);
}

static void M404CHR4(uint32_t A, uint8_t V) {
	setchr4(A, (V & 0x1F) | outerBank << 5);
}

static DECLFW(M404Write) {
	if (!(outerBank & 0x80)) {
		outerBank = V;
		MMC1PRG();
		MMC1CHR();
	}
}

static void M404Reset(void) {
	outerBank = 0;
	MMC1CMReset();
}

static void M404Power(void) {
	outerBank = 0;
	GenMMC1Power();
	SetWriteHandler(0x6000, 0x7FFF, M404Write);
}

void Mapper404_Init(CartInfo *info) {
	GenMMC1Init(info, 256, 256, 0, 0);
	info->Power = M404Power;
	info->Reset = M404Reset;
	MMC1CHRHook4 = M404CHR4;
	MMC1PRGHook16 = M404PRG16;
	AddExState(&outerBank, 1, 0, "BANK");
}