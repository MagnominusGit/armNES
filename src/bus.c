/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

// The CPU's address space. Reads of nothing return the last value on the data bus.

uint8_t busRead(struct NES* nes, uint16_t address) {
	struct Cpu* cpu = &nes->cpu;
	uint8_t value;
	bool driven = true;
	if (address < 0x2000) {
		value = nes->ram[address & 0x7FF];
	} else if (address < 0x4000) {
		if ((address & 7) != 2 || nes->exactPpu || nes->dotPpu || !fastStatusAhead(nes, &value)) {
			nesPpuSync(nes);
			value = ppuReadRegister(nes, address);
			if ((address & 7) == 7 && nes->board->irqQuietDots) {
				nes->mapperQuietTicks = 0;
			}
		}
		// The RC2C05s give an ID in $2002's low bits
		if (__builtin_expect(nes->cart.vs, 0) && (address & 7) == 2 && nes->cart.vsPpu >= 8 && nes->cart.vsPpu <= 12) {
			static const uint8_t ids[5] = { 0x1B, 0x3D, 0x1C, 0x1B, 0x00 };
			value = (value & 0xC0) | ids[nes->cart.vsPpu - 8];
		}
	} else if (nes->readHooks & (1 << (address >> 12))) {
		if (nes->vsShared && (address & 0xE000) == 0x6000) {
			value = nes->vsRamAccess ? nes->vsShared[address & 0x7FF] : cpu->openBus;
			driven = nes->vsRamAccess;
		} else {
			nesPpuSync(nes);
			value = nes->board->read(nes, address);
			// The read may have acknowledged the board's IRQ
			nes->mapperQuietTicks = 0;
		}
	} else {
		uint8_t* page = address >= 0x4020 ? nes->prgPages[address >> 12] : NULL;
		if (page) {
			value = page[address & 0xFFF];
		} else if (__builtin_expect(nes->cart.vsHardware - 1u < 3u, 0) && address < 0x6000) {
			value = vsProtectionRead(nes, address);
		} else {
			value = cpu->openBus;
			driven = false;
		}
	}
	if (__builtin_expect(nes->cheatPages, 0) && (nes->cheatPages & (1 << (address >> 12)))) {
		value = nesCheatRead(nes, address, value);
	}
	cpu->openBus = value;
	// The APU's registers answer by the CPU's address bus, which a DMA leaves where the CPU halted
	if ((cpu->addressBus & 0xFFE0) == 0x4000) {
		nesApuSync(nes);
		value = apuConflictRead(nes, value, address, driven);
		// A $4015 read clears the frame IRQ a cycle or two later
		nes->apuQuiet = 0;
		return value;
	}
	cpu->internalBus = value;
	return value;
}

void busWrite(struct NES* nes, uint16_t address, uint8_t value) {
	uint8_t before = nes->cpu.openBus;
	nes->cpu.openBus = value;
	nes->cpu.internalBus = value;
	if (address < 0x2000) {
		nes->ram[address & 0x7FF] = value;
	} else if (address < 0x4000) {
		// The RC2C05s swap $2000 and $2001
		if (__builtin_expect(nes->cart.vs, 0) && nes->cart.vsPpu >= 8 && nes->cart.vsPpu <= 12 && (address & 6) == 0) {
			address ^= 1;
		}
		if ((address & 7) != 4 || nes->exactPpu || nes->dotPpu || !fastOamWriteAhead(nes, value)) {
			nesPpuSync(nes);
			ppuWriteRegister(nes, address, value, before);
			// The board's IRQ bound depends on what the PPU is about to put on its bus
			if (nes->board->irqQuietDots) {
				nes->mapperQuietTicks = 0;
			}
		}
	} else if (address < 0x4020) {
		nesApuSync(nes);
		apuWrite(nes, address, value);
		nes->apuQuiet = apuQuietCycles(nes);
	} else {
		int page = address >> 12;
		if (nes->board->write) {
			nesPpuSync(nes);
		}
		if (nes->prgWritable[page] && nes->prgPages[page]) {
			nes->prgPages[page][address & 0xFFF] = value;
		} else if (__builtin_expect(nes->vsShared != NULL, 0) && (address & 0xE000) == 0x6000 && nes->vsRamAccess) {
			nes->vsShared[address & 0x7FF] = value;
		}
		if (nes->board->write) {
			nes->board->write(nes, address, value);
			// The write may have armed the board's IRQ
			nes->mapperQuietTicks = 0;
		}
	}
}
