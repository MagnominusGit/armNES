/* FCEUmm - NES/Famicom Emulator
 *
 * Copyright notice for this file:
 *  Copyright (C) 2006 CaH4e3
 *  Copyright (C) 2020
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

static uint8_t *WRAM            = NULL;
static uint8_t *CHRRAM          = NULL;
static uint32_t WRAMSIZE        = 0;
static uint32_t CHRRAMSIZE      = 0;

static uint8_t fk23_regs[8]     = { 0 };
static uint8_t mmc3_regs[12]    = { 0 };
static uint8_t mmc3_ctrl        = 0;
static uint8_t mmc3_mirr        = 0;
static uint8_t mmc3_wram        = 0;
static uint8_t irq_count        = 0;
static uint8_t irq_latch        = 0;
static uint8_t irq_enabled      = 0;
static uint8_t irq_reload       = 0;
static uint8_t latch            = 0;
static uint8_t dipswitch        = 0;
static uint8_t subType          = 0;
static uint8_t jncota523        = 0;
static uint8_t dipsw_enable     = 0;
static uint8_t after_power      = 0;

extern uint32_t ROM_size;
extern uint32_t VROM_size;

static SFORMAT StateRegs[] = {
   { fk23_regs,               8, "EXPR" },
   { mmc3_regs,              12, "M3RG" },
   { &latch,                  1, "LATC" },
   { &dipswitch,              1, "DPSW" },
   { &mmc3_ctrl,              1, "M3CT" },
   { &mmc3_mirr,              1, "M3MR" },
   { &mmc3_wram,              1, "M3WR" },
   { &irq_reload,             1, "IRQR" },
   { &irq_count,              1, "IRQC" },
   { &irq_latch,              1, "IRQL" },
   { &irq_enabled,            1, "IRQA" },
   { &subType,                1, "SUBT" },
   { 0 }
};

#define INVERT_PRG          !!(mmc3_ctrl & 0x40)
#define INVERT_CHR          !!(mmc3_ctrl & 0x80)
#define WRAM_ENABLED        !!(mmc3_wram & 0x80)
#define WRAM_EXTENDED      (!!(mmc3_wram & 0x20) && subType == 2)
#define FK23_ENABLED       (!!(mmc3_wram & 0x40) || !WRAM_EXTENDED)
#define PRG_MODE              ( fk23_regs[0] & 0x07)
#define MMC3_EXTENDED       !!( fk23_regs[3] & 0x02)
#define CHR_8K_MODE         !!( fk23_regs[0] & 0x40)
#define CHR_CNROM_MODE        (~fk23_regs[0] & 0x20 && (subType == 1 || subType == 5))
#define CHR_OUTER_BANK_SIZE !!( fk23_regs[0] & 0x10)
#define CHR_MIXED           !!(WRAM_EXTENDED && mmc3_wram &0x04)

static void cwrap(uint32_t A, uint32_t V)
{
   int bank = 0;

   if (jncota523)
   {
      if (~A &0x0400) setchr2r(bank, A, V);
   }
   else
   {

      if (!VROM_size)
         bank = 0;
      else if (fk23_regs[0] &0x20 && ~fk23_regs[0] &0x40 && CHRRAMSIZE || VROM_size == 0)
         bank = 0x10;

      if (CHR_MIXED && V < 8) bank = 0x10;

      setchr1r(bank, A, V);
   }
}

static void SyncCHR(void)
{
   uint32_t outer = fk23_regs[2] | (subType == 3? (fk23_regs[6] << 8): 0);
   if (CHR_8K_MODE)
   {
      uint32_t mask = (CHR_CNROM_MODE? (CHR_OUTER_BANK_SIZE? 0x01: 0x03): 0x00);

      uint32_t bank = ((subType ==5? outer: (outer & ~mask)) | (latch & mask)) << 3;

      cwrap(0x0000, bank + 0);
      cwrap(0x0400, bank + 1);
      cwrap(0x0800, bank + 2);
      cwrap(0x0C00, bank + 3);

      cwrap(0x1000, bank + 4);
      cwrap(0x1400, bank + 5);
      cwrap(0x1800, bank + 6);
      cwrap(0x1C00, bank + 7);
   }
   else
   {
      uint32_t cbase = (INVERT_CHR? 0x1000: 0);
      uint32_t mask  = (CHR_OUTER_BANK_SIZE? 0x7F: 0xFF);
             outer = (outer << 3) & ~mask;

      if (MMC3_EXTENDED)
      {
         cwrap(cbase ^ 0x0000, mmc3_regs[0]  &mask | outer);
         cwrap(cbase ^ 0x0400, mmc3_regs[10] &mask | outer);
         cwrap(cbase ^ 0x0800, mmc3_regs[1]  &mask | outer);
         cwrap(cbase ^ 0x0c00, mmc3_regs[11] &mask | outer);

         cwrap(cbase ^ 0x1000, mmc3_regs[2]  &mask | outer);
         cwrap(cbase ^ 0x1400, mmc3_regs[3]  &mask | outer);
         cwrap(cbase ^ 0x1800, mmc3_regs[4]  &mask | outer);
         cwrap(cbase ^ 0x1c00, mmc3_regs[5]  &mask | outer);
      }
      else
      {
         cwrap(cbase ^ 0x0000,(mmc3_regs[0] & 0xFE) & mask | outer);
         cwrap(cbase ^ 0x0400,(mmc3_regs[0] | 0x01) & mask | outer);
         cwrap(cbase ^ 0x0800,(mmc3_regs[1] & 0xFE) & mask | outer);
         cwrap(cbase ^ 0x0C00,(mmc3_regs[1] | 0x01) & mask | outer);

         cwrap(cbase ^ 0x1000, mmc3_regs[2]         & mask | outer);
         cwrap(cbase ^ 0x1400, mmc3_regs[3]         & mask | outer);
         cwrap(cbase ^ 0x1800, mmc3_regs[4]         & mask | outer);
         cwrap(cbase ^ 0x1c00, mmc3_regs[5]         & mask | outer);
      }
   }
}

static void SyncPRG(void)
{
   uint32_t mask = 0x3F >> PRG_MODE;
   uint32_t prg_base = fk23_regs[1] & 0x7F;
   switch (subType)
   {
      case 1:
         if (PRG_MODE == 0 && MMC3_EXTENDED) mask = 0xFF;
	 break;
      case 2:
         prg_base |= fk23_regs[0] << 4 & 0x080 | fk23_regs[0] << 1 & 0x100 | fk23_regs[2] << 3 & 0x600 | fk23_regs[2] << 6 & 0x800;
	 break;
      case 3:
         if (PRG_MODE == 0) mask = 0xFF;
         prg_base |= fk23_regs[5] << 7;
	 break;
      case 4:
         prg_base |= fk23_regs[2] & 0x80;
	 break;
      case 5:
         prg_base = prg_base &0x1F | fk23_regs[5] <<5;
	 break;
   }

   switch (PRG_MODE)
   {
      case 0:
      case 1:
      case 2:
      {
         uint32_t cbase = (INVERT_PRG ? 0x4000 : 0);

         prg_base =(prg_base << 1) & ~mask;

         if (MMC3_EXTENDED)
         {
            setprg8(0x8000 ^ cbase, mmc3_regs[6] & mask | prg_base);
            setprg8(0xA000,         mmc3_regs[7] & mask | prg_base);
            setprg8(0xC000 ^ cbase, mmc3_regs[8] & mask | prg_base);
            setprg8(0xE000,         mmc3_regs[9] & mask | prg_base);
         }
         else
         {
            setprg8(0x8000 ^ cbase, (mmc3_regs[6] & mask) | prg_base);
            setprg8(0xA000,         (mmc3_regs[7] & mask) | prg_base);
            setprg8(0xC000 ^ cbase, (0xFE         & mask) | prg_base);
            setprg8(0xE000,         (0xFF         & mask) | prg_base);
         }
         break;
      }
      case 3:
         setprg16(0x8000, prg_base);
         setprg16(0xC000, prg_base);
         break;
      case 4:
         setprg32(0x8000, (prg_base >> 1));
         break;
      case 5:
         setprg16(0x8000, latch & 0x07 | prg_base &~0x07);
	 setprg16(0xC000,         0x07 | prg_base       );
	 break;
   }
}

static void SyncWRAM(void)
{

   if (WRAM_ENABLED || WRAM_EXTENDED)
   {
      if (WRAM_EXTENDED)
      {
         setprg8r(0x10, 0x4000, (mmc3_wram & 0x03) + 1);
         setprg8r(0x10, 0x6000, mmc3_wram & 0x03);
      }
      else
         setprg8r(0x10, 0x6000, 0);
   }
}

static void SyncMIR(void)
{
   if (jncota523)
      return;
   else
   switch (mmc3_mirr & (subType == 2? 0x03 : 0x01))
   {
      case 0: setmirror(MI_V); break;
      case 1: setmirror(MI_H); break;
      case 2: setmirror(MI_0); break;
      case 3: setmirror(MI_1); break;
   }
}

static void Sync(void)
{
   SyncPRG();
   SyncCHR();
   SyncWRAM();
   SyncMIR();
}

static DECLFW(Write4800)
{
   fk23_regs[5] = V;
   SyncPRG();
}
static DECLFW(Write5000)
{
   if (after_power && A > 0x5010 && A != 0x5FF3)
   {
      after_power = 0;
      dipsw_enable = A >= 0x5020;
   }
   if (FK23_ENABLED && (A & (0x10 << dipswitch)))
   {
      fk23_regs[A & (subType == 3? 7: 3)] = V;
      SyncPRG();
      SyncCHR();
   }
   else

      CartBW(A, V);
}

static DECLFW(Write8000)
{
   latch = V;
   if (CHR_8K_MODE && CHR_CNROM_MODE) SyncCHR();
   if (PRG_MODE == 5) SyncPRG();

   switch (A & 0xE001)
   {
      case 0x8000:
      {
         uint8_t old_ctrl;
         if (A & 2) return;
         old_ctrl = mmc3_ctrl;

         if (subType == 2)
         {
            if (V == 0x46)
               V = 0x47;
            else if (V == 0x47)
               V = 0x46;
         }

         mmc3_ctrl = V;

         if (INVERT_PRG != (old_ctrl & 0x40))
            SyncPRG();

         if (INVERT_CHR != (old_ctrl & 0x80))
            SyncCHR();

         break;
      }
      case 0x8001:
      {
         uint8_t ctrl_mask;
         if (A & 2) return;
         ctrl_mask = MMC3_EXTENDED ? 0x0F : 0x07;

         if ((mmc3_ctrl & ctrl_mask) < 12)
         {
            mmc3_regs[mmc3_ctrl & ctrl_mask] = V;

            if (((mmc3_ctrl & ctrl_mask) < 6) || ((mmc3_ctrl & ctrl_mask) >= 10))
               SyncCHR();
            else
               SyncPRG();
         }
         break;
      }
      case 0xA000:
         mmc3_mirr = V;
         SyncMIR();
         break;
      case 0xA001:

         if ((V & 0x20) == 0)
            V &= 0xC0;
         mmc3_wram = V;
         Sync();
         break;
      case 0xC000:
         irq_latch = V;
         break;
      case 0xC001:
         irq_reload = 1;
         break;
      case 0xE000:
         X6502_IRQEnd(FCEU_IQEXT);
         irq_enabled = 0;
         break;
      case 0xE001:
         irq_enabled = 1;
         break;
      default:
         break;
   }
}

static void IRQHook(void)
{
   if (!irq_count || irq_reload)
      irq_count = irq_latch;
   else
      irq_count--;

   if (!irq_count && irq_enabled)
      X6502_IRQBegin(FCEU_IQEXT);

   irq_reload = 0;
}

static void Reset(void)
{

   if (dipsw_enable) {
      dipswitch = (dipswitch + 1) & 7;
      FCEU_printf("BMCFK23C dipswitch set to $%04x\n",0x5000|0x10 << dipswitch);
   }

   fk23_regs[0]   = fk23_regs[1] = fk23_regs[2] = fk23_regs[3] = fk23_regs[4] = fk23_regs[5] = fk23_regs[6] = fk23_regs[7] = 0;
   if (subType == 1) fk23_regs[1] = 0xFF;
   mmc3_regs[0]   = 0;
   mmc3_regs[1]   = 2;
   mmc3_regs[2]   = 4;
   mmc3_regs[3]   = 5;
   mmc3_regs[4]   = 6;
   mmc3_regs[5]   = 7;
   mmc3_regs[6]   = 0;
   mmc3_regs[7]   = 1;
   mmc3_regs[8]   = ~1;
   mmc3_regs[9]   = ~0;
   mmc3_regs[10]  = ~0;
   mmc3_regs[11]  = ~0;
   mmc3_wram      = 0x80;
   mmc3_ctrl      = mmc3_mirr = irq_count = irq_latch = irq_enabled = 0;

   Sync();
}

static void Power(void)
{
   fk23_regs[0]   = fk23_regs[1] = fk23_regs[2] = fk23_regs[3] = fk23_regs[4] = fk23_regs[5] = fk23_regs[6] = fk23_regs[7] = 0;
   if (subType == 1) fk23_regs[1] = 0xFF;
   mmc3_regs[0]   = 0;
   mmc3_regs[1]   = 2;
   mmc3_regs[2]   = 4;
   mmc3_regs[3]   = 5;
   mmc3_regs[4]   = 6;
   mmc3_regs[5]   = 7;
   mmc3_regs[6]   = 0;
   mmc3_regs[7]   = 1;
   mmc3_regs[8]   = ~1;
   mmc3_regs[9]   = ~0;
   mmc3_regs[10]  = ~0;
   mmc3_regs[11]  = ~0;
   mmc3_wram      = 0x80;
   mmc3_ctrl      = mmc3_mirr = irq_count = irq_latch = irq_enabled = 0;

   Sync();

   SetReadHandler(0x8000, 0xFFFF, CartBR);
   SetWriteHandler(0x5000, 0x5FFF, Write5000);
   SetWriteHandler(0x8000, 0xFFFF, Write8000);

   if (subType == 5)
      SetWriteHandler(0x4800, 0x4FFF, Write4800);

   if (WRAMSIZE)
   {
      SetReadHandler(0x6000, 0x7FFF, CartBR);
      SetWriteHandler(0x6000, 0x7FFF, CartBW);
      FCEU_CheatAddRAM((WRAMSIZE >> 10) < 8 ? (WRAMSIZE >> 10) : 8, 0x6000, WRAM);
   }
}

static void Close(void)
{
   if (WRAM)
      FCEU_gfree(WRAM);
   WRAM = NULL;

   if (CHRRAM)
      FCEU_gfree(CHRRAM);
   CHRRAM = NULL;
}

static void StateRestore(int version)
{
   Sync();
}

static void Init(CartInfo *info)
{

   info->Power       = Power;
   info->Reset       = Reset;
   info->Close       = Close;
   GameHBIRQHook     = IRQHook;
   GameStateRestore  = StateRestore;
   AddExState(StateRegs, ~0, 0, 0);

   if (CHRRAMSIZE)
   {
      CHRRAM = (uint8_t *)FCEU_gmalloc(CHRRAMSIZE);
      SetupCartCHRMapping(0x10, CHRRAM, CHRRAMSIZE, 1);
      AddExState(CHRRAM, CHRRAMSIZE, 0, "CRAM");
   }

   if (WRAMSIZE)
   {
      WRAM = (uint8_t *)FCEU_gmalloc(WRAMSIZE);
      SetupCartPRGMapping(0x10, WRAM, WRAMSIZE, 1);
      AddExState(WRAM, WRAMSIZE, 0, "WRAM");

      if (info->battery)
      {
         info->SaveGame[0] = WRAM;
         if (info->iNES2 && info->PRGRamSaveSize)
            info->SaveGameLen[0] = info->PRGRamSaveSize;
         else
            info->SaveGameLen[0] = WRAMSIZE;
      }
   }

}

void Mapper176_Init(CartInfo *info) {
   dipsw_enable = 0;
   jncota523 = 0;
   if (info->iNES2)
   {
      subType = info->submapper;
      after_power = subType != 2;
      CHRRAMSIZE = info->CHRRamSize + info->CHRRamSaveSize;
      WRAMSIZE = info->PRGRamSize + info->PRGRamSaveSize;
   }
   else
   {

      if (info->battery)
      {
         subType = 2;
         after_power = 0;
         WRAMSIZE = 32 * 1024;
      }
      else
      {

         WRAMSIZE = 8 * 1024;

         subType = (ROM_size ==128 && VROM_size ==256 ||
                    ROM_size ==128 && VROM_size ==128 ||
                    ROM_size ==128 && VROM_size ==64  ||
                    ROM_size ==128 && VROM_size ==0   ||
                    ROM_size ==64  && VROM_size ==64)?
                    0: 1;

         after_power = 1;
      }
   }
   Init(info);
}

void BMCFK23C_Init(CartInfo *info)
{
   if (!UNIFchrrama)
   {

      if ((ROM_size * 16) == 2048 && (VROM_size * 8) == 512)
         CHRRAMSIZE = 8 * 1024;
   }
   WRAMSIZE = 8 * 1024;

   dipsw_enable = 0;
   after_power = 1;
   jncota523 = 0;
   subType =ROM_size *16 >=4096? 2: ROM_size == 64 && VROM_size == 128? 1: 0;
   if (subType == 2)
      CHRRAMSIZE = 256 * 1024;

   Init(info);
}

void BMCFK23CA_Init(CartInfo *info)
{
   WRAMSIZE = 8 * 1024;

   dipsw_enable = 0;
   after_power = 1;
   jncota523 = 0;
   subType =ROM_size *16 >=2048? 2: 1;
   if (subType == 2)
      CHRRAMSIZE = 256 * 1024;

   Init(info);
}

void Super24_Init(CartInfo *info)
{
   CHRRAMSIZE = 8 * 1024;
   dipsw_enable = 0;
   after_power = 0;
   jncota523 = 0;
   subType = 0;
   Init(info);
}

void WAIXINGFS005_Init(CartInfo *info)
{
   CHRRAMSIZE = 8 * 1024;
   WRAMSIZE = 32 * 1024;
   dipsw_enable = 0;
   after_power = 0;
   jncota523 = 0;
   subType = 2;
   Init(info);
}

void Mapper523_Init(CartInfo *info)
{
   WRAMSIZE = 8 * 1024;
   dipsw_enable = 0;
   after_power = 0;
   jncota523 = 1;
   subType = 1;
   Init(info);
}
