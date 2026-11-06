/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2022 Cluster
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

static uint8_t latch_out, latch_in;

static void INX_007T_Sync() {
	setprg32(0x8000, (latch_in & 7) | (latch_out << 3));
	setmirror(MI_0 + ((latch_in >> 4) & 1));
	setchr8(0);
}

static void StateRestore(int version) {
	INX_007T_Sync();
}

static DECLFW(INX_007T_WriteLow)
{
	latch_out = V;
	INX_007T_Sync();
}

static DECLFW(INX_007T_WriteHi)
{
	latch_in = V;
	INX_007T_Sync();
}

static void INX_007T_Power(void) {
	latch_in = latch_out = 0;
	INX_007T_Sync();
	SetReadHandler(0x8000, 0xFFFF, CartBR);
	SetWriteHandler(0x5000,0x5FFF, INX_007T_WriteLow);
	SetWriteHandler(0x8000, 0xFFFF, INX_007T_WriteHi);
}

static void INX_007T_Reset(void) {
	latch_in = latch_out = 0;
	INX_007T_Sync();
}

void INX_007T_Init(CartInfo *info) {
	info->Power = INX_007T_Power;
	info->Reset = INX_007T_Reset;
	GameStateRestore = StateRestore;
	SetupCartMirroring(MI_0, 0, NULL);
	AddExState(&latch_out, 1, 0, "LATO");
	AddExState(&latch_in, 1, 0, "LATI");
}
