/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2020 NewRisingSun
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
#include "mmc3.h"

static uint8_t reverseCHR_A18_A19;
static uint8_t invertC000;
static uint8_t SL0;
static uint8_t submapper;

static uint8_t getMMC3Bank (int bank) {
	if (~bank &1 && MMC3_cmd &0x40) bank ^= 2;
	return bank &2? 0xFE | bank &1: DRegBuf[6 | bank &1];
}

static void wrapPRG (uint32_t A, uint8_t V) {
	int prgAND = EXPREGS[0] &0x40? 0x0F: 0x1F;
	int prgOR  = (EXPREGS[0] <<4 &0x70 | (EXPREGS[0] ^0x20) <<3 &0x180) &~prgAND;

	if (submapper == 1) prgOR = prgOR &0x7F | prgOR >>1 &0x80;

	if (submapper == 2) prgOR = prgOR &0x7F | EXPREGS[1] <<5 &0x80;

	if (submapper == 3 && EXPREGS[0] &0x04) {
		prgAND &= ~0x02;
		prgOR |= EXPREGS[0] &0x20? 0x00: 0x02;
	}

	if (submapper == 5) prgOR |= EXPREGS[4] <<9;

	if (submapper == 6) {
		prgOR = EXPREGS[0] <<4 &0x10 | EXPREGS[4] <<5 &0x20 | EXPREGS[4] <<3 &0x40 | EXPREGS[4] <<3 &0x80 | EXPREGS[4] <<3 &0x700 | EXPREGS[0] <<7 &0x800;
		prgOR &= ~prgAND;
	} else
	if (submapper == 7) {
		prgOR = EXPREGS[0] <<4 &0x10 | EXPREGS[4] <<5 &0x20 | EXPREGS[4] <<2 &0x40 | EXPREGS[4] <<4 &0x80 | EXPREGS[4] <<3 &0x700 | EXPREGS[0] <<7 &0x800;
		prgOR &= ~prgAND;
	}

	for (A = 0; A < 4; A++) {

		V = getMMC3Bank(A &((EXPREGS[3] &0x0D) == 0x0D? 2: EXPREGS[3] &0x01? 0: 3));

		if (EXPREGS[3] &0x08) switch(EXPREGS[3] &0x03) {
			case 0: V = V &3 | V <<1 &~3; break;
			case 1: V = A &3 | V <<1 &~1; break;
			case 2: V = V &3 | V <<2 &~3; break;
			case 3: V = A &3 | V <<2 &~3; break;
		} else
		if (EXPREGS[3] &0x01) {
			V = A &1 | V &~1;
			if (EXPREGS[3] &0x02) V = A &2 | V &~2;
		}
		setprg8(0x8000 +A*0x2000, V &prgAND | prgOR &~prgAND);
	}
	mwrap(A000B);
}

static void wrapCHR (uint32_t A, uint8_t V) {
	int chrAND = EXPREGS[0] &0x80? 0x7F: 0xFF;
	int chrOR;
	if (reverseCHR_A18_A19)
		chrOR = (EXPREGS[0] <<4 &0x080 | (EXPREGS[0] ^0x20) <<3 &0x100 | EXPREGS[0] <<5 &0x200) &~chrAND;
	else
	if (submapper == 4)
		chrOR = (EXPREGS[0] <<4 &0x080 | EXPREGS[0] <<7 &0x100 | ~EXPREGS[0] <<4 &0x200 | EXPREGS[0] <<6 &0x400) &~chrAND;
	else
		chrOR = ((EXPREGS[0] ^0x20) <<4 &0x380 | EXPREGS[0] <<8 &0x400) &~chrAND;

	if (submapper == 3 && EXPREGS[0] &0x04) {
		chrAND &= ~0x10;
		chrOR |= EXPREGS[0] &0x20? 0x00: 0x10;
	}
	if (EXPREGS[3] &0x10)
		setchr8(EXPREGS[2] &(chrAND >>3) | (chrOR &~chrAND) >>3);
	else
		setchr1(A, (V & chrAND) | chrOR);
}

static void wrapMirroring (uint8_t V) {
	A000B = V;
	if (EXPREGS[3] &0x20) {
		if (DRegBuf[6] &0x10)
			setmirror(MI_1);
		else
			setmirror(MI_0);
	} else
	if (EXPREGS[1] &0x02) {
		switch(A000B &3) {
		case 0: setmirror(MI_V); break;
		case 1: setmirror(MI_H); break;
		case 2: setmirror(MI_0); break;
		case 3: setmirror(MI_1); break;
		}
	} else
		setmirror(A000B &1? MI_H: MI_V);

}

static DECLFW (writeWRAM) {
	CartBW(A, V);

	if (submapper == 5 && !(A &3) && V &0x20 && EXPREGS[0] &0x04 && ~V &0x04) EXPREGS[4] = EXPREGS[4] <<1 | EXPREGS[0] >>1 &0x01;

	if ((submapper == 6 || submapper == 7) && !(A &3) && ~EXPREGS[0] &0x04 && V &0x04) EXPREGS[4] = V &~0x06;

	if ((A &3) == 2) {
		int latchMask = 0xFF &~(EXPREGS[2] &0x80? 0xF0: 0x00) &~(EXPREGS [2] >>3 &0x0E);
		EXPREGS[2] = EXPREGS[2] &~latchMask | V &latchMask;
		FixMMC3CHR(MMC3_cmd);
	} else
	if (~EXPREGS[3] &0x80) {

		EXPREGS[A &3] = V;
		FixMMC3PRG(MMC3_cmd);
		FixMMC3CHR(MMC3_cmd);
		mwrap(A000B);
	}
}

static DECLFW (MMC3_CMDWriteA) {
	if ((A &0xE001) == 0x8000) {
		MMC3_cmd = V;
		FixMMC3PRG(MMC3_cmd);
		FixMMC3CHR(MMC3_cmd);
	} else
		MMC3_CMDWrite(A, V);
}

static DECLFW (writeCart) {
	if ((EXPREGS[3] &0x09) == 0x09)
		MMC3_CMDWriteA(0x8000 | (EXPREGS[3] &0x08? 1: A) &1, V);
	else
	if (A >= 0xC000)
		MMC3_IRQWrite(A, V ^(invertC000? 0xFF: 0x00));
	else
		MMC3_CMDWriteA(A, V);
}

static DECLFR (readPRG) {
	if (EXPREGS[1] &1) A = A &~1 | SL0 &1;
	return CartBR(A);
}

static void reset (void) {
	SL0++;
	EXPREGS[0] = EXPREGS[1] = EXPREGS[2] = EXPREGS[3] = EXPREGS[4] = 0;
	MMC3RegReset();
}

static void power (void) {
	SL0 = 0;
	EXPREGS[0] = EXPREGS[1] = EXPREGS[2] = EXPREGS[3] = EXPREGS[4] = 0;
	GenMMC3Power();
	SetWriteHandler(0x6000, 0x7FFF, writeWRAM);
	SetWriteHandler(0x8000, 0xFFFF, writeCart);
	SetReadHandler(0x8000, 0xFFFF, readPRG);
}

static void init (CartInfo *info) {
	GenMMC3_Init(info, 512, 256, 8, info->battery);
	cwrap = wrapCHR;
	pwrap = wrapPRG;
	mwrap = wrapMirroring;
	submapper = info->submapper;

	info->Power = power;
	info->Reset = reset;

	AddExState(EXPREGS, submapper < 5? 4: 5, 0, "EXPR");
	AddExState(&SL0, 1, 0, "DPSW");
}

void Mapper126_Init (CartInfo *info) {
	reverseCHR_A18_A19 = 1;
	invertC000 = 0;
	init(info);
}

void Mapper422_Init (CartInfo *info) {
	reverseCHR_A18_A19 = 0;
	invertC000 = 0;
	init(info);
}

void Mapper534_Init (CartInfo *info) {
	reverseCHR_A18_A19 = 0;
	invertC000 = 1;
	init(info);
}
