/* FCE Ultra - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2002 Xodnizel
 *  Copyright (C) 2005 CaH4e3
 *  Copyright (C) 2019 Libretro Team
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

static uint8_t *WRAM;
static uint32_t WRAMSIZE;
static uint8_t reg[4];
static SFORMAT StateRegs[] =
{
        { reg, 4, "REGS" },
        { 0 }
};

static void sync()
{
   setprg32(0x8000, reg[2] <<4 | reg[0] &0x0C
                  | ( reg[3] &0x04                ?         0x00 : 0x02)
		  | ( reg[3] &0x04                ?(reg[0] &0x02): 0x00)
		  | (                 reg[3] &0x01?         0x00 : reg[1] >>1 &0x01)
		  | (~reg[3] &0x04 && reg[3] &0x01?         0x01 : 0x00)
		  | ( reg[3] &0x04 && reg[3] &0x01?(reg[0] &0x01): 0x00)
   );
   setprg8r(0x10, 0x6000, 0);
   if (~reg[0] &0x80)
      setchr8(0);
}

static void hblank(void) {
   if (reg[0] &0x80 && scanline <239)
   {
      setchr4(0x0000, scanline >=127? 1: 0);
      setchr4(0x1000, scanline >=127? 1: 0);
   }
   else
      setchr8(0);
}

static DECLFR(readReg)
{
   return 0x00;
}

static DECLFW(writeReg)
{
   reg[A >>8 &3] = V;
   sync();
}

static void power(void)
{
   memset(reg, 0, sizeof(reg));
   sync();
   SetReadHandler (0x5000, 0x57FF, readReg);
   SetWriteHandler(0x5000, 0x57FF, writeReg);
   SetReadHandler (0x6000, 0xFFFF, CartBR);
   SetWriteHandler(0x6000, 0x7FFF, CartBW);
}

static void reset(void)
{
   memset(reg, 0, sizeof(reg));
   sync();
}

static void close(void)
{
   if (WRAM)
      FCEU_gfree(WRAM);
   WRAM = NULL;
}

static void StateRestore(int version)
{
   sync();
}

void Mapper162_Init (CartInfo *info)
{
   info->Power   = power;
   info->Reset   = reset;
   info->Close   = close;
   GameHBIRQHook = hblank;

   GameStateRestore = StateRestore;
   AddExState(StateRegs, ~0, 0, 0);

   WRAMSIZE = CartInfo_PRGRAM_bytes(info, 8192);
   WRAM = (uint8_t*) FCEU_gmalloc(WRAMSIZE);
   SetupCartPRGMapping(0x10, WRAM, WRAMSIZE, 1);
   AddExState(WRAM, WRAMSIZE, 0, "WRAM");
   FCEU_CheatAddRAM((WRAMSIZE >> 10) < 8 ? (WRAMSIZE >> 10) : 8, 0x6000, WRAM);

   if (info->battery) {
      info->SaveGame[0] = WRAM;
      info->SaveGameLen[0] = WRAMSIZE;
   }
}
