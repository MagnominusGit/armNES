/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2011 CaH4e3
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
 *
 */

#include "mapinc.h"
#include "../fds_apu.h"

static uint8_t reg, mirr;
static uint8_t prg;

static SFORMAT StateRegs[] =
{
	{ &reg, 1, "REG" },
	{ &mirr, 1, "MIRR" },
	{ 0 }
};

static void Sync(void) {
	setprg8(0x6000, reg);
	setprg32(0x8000, prg);
	setchr8(0);
	setmirror(mirr);
}

static DECLFW(AC08Mirr) {
	mirr = ((V & 8) >> 3) ^ 1;
	Sync();
}

static DECLFW(AC08Write) {
	if (A == 0x8001)
		reg = (V >> 1) & 0xf;
	else
		reg = V & 0xf;
	Sync();
}

static void AC08Power(void) {
	FDSSoundPower();
	reg = 0;
	Sync();
	SetReadHandler(0x6000, 0xFFFF, CartBR);
	SetWriteHandler(0x4025, 0x4025, AC08Mirr);
	SetWriteHandler(0x8000, 0xFFFF, AC08Write);
}

static void StateRestore(int version) {
	Sync();
}

void AC08_Init(CartInfo *info) {
	prg = (info->PRGRomSize / 16384) & 0x0F ? 4 : 7;
	info->Power = AC08Power;
	GameStateRestore = StateRestore;
	AddExState(&StateRegs, ~0, 0, 0);
}
