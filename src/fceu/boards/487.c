/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file: 2025 NewRisingSun
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

static uint8_t reg[2];
static uint8_t submapper;

static SFORMAT StateRegs[] ={
	{ reg, 2, "REGS" },
	{ 0 }
};

static void Sync(void) {
	int prg =(reg[1] &0x40? (reg[0] >>3 &1): (reg[1] &0x01)) |
		  reg[1] &0x3E;
	int chr = reg[0] &0x03 |
		 (reg[1] &0x40? (reg[0] &0x04): (reg[1] <<2 &4)) |
		  reg[1] <<2 &0xF8;
	if (prg &0x30) prg -=0x10;
	if (chr &0xC0) chr -=0x40;
	setprg32(0x8000, prg);
	setchr8(chr);
	setmirror(reg[1] &0x80? MI_H: MI_V);
}

static DECLFW(WriteNINA_Prototype) {
	if (A &0x200)
		reg[1] =V;
	else
	if (A &0x100)
		reg[0] =V;
	Sync();
}

static DECLFW(WriteNINA08) {
	if (A &0x100) {
		if (A &0x080)
			reg[1] =V;
		else
		if (~reg[1] &0x20)
			reg[0] =V;
		Sync();
	}
	Sync();
}

static DECLFW(WriteColorDreams) {
	if (reg[1] &0x20) {
		reg[0] =V <<3 &8 | V >>4 &7;
		Sync();
	}
}

static void Power(void) {
	reg[0] = 0;
	reg[1] = 0;
	Sync();
	SetWriteHandler(0x8000, 0xFFFF, WriteColorDreams);
	SetWriteHandler(0x4100, 0x7FFF, submapper == 1? WriteNINA_Prototype: WriteNINA08);
	SetReadHandler(0x8000, 0xFFFF, CartBR);
}

static void StateRestore(int version) {
	Sync();
}

static void Reset(void) {
	RAM[0x000] =0;
	reg[0] = 0;
	reg[1] = 0;
	Sync();
}

void Mapper487_Init(CartInfo *info) {
	submapper = info->submapper;
	info->Power = Power;
	info->Reset = Reset;
	AddExState(&StateRegs, ~0, 0, 0);
	GameStateRestore = StateRestore;
}

