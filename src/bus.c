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
		value = ppuReadRegister(nes, address);
	} else if (nes->readHooks & (1 << (address >> 12))) {
		value = nes->board->read(nes, address);
	} else {
		uint8_t* page = address >= 0x4020 ? nes->prgPages[address >> 12] : NULL;
		if (page) {
			value = page[address & 0xFFF];
		} else {
			value = cpu->openBus;
			driven = false;
		}
	}
	cpu->openBus = value;
	// The APU's registers answer by the CPU's address bus, which a DMA leaves where the CPU halted
	if ((cpu->addressBus & 0xFFE0) == 0x4000) {
		return apuConflictRead(nes, value, address, driven);
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
		ppuWriteRegister(nes, address, value, before);
	} else if (address < 0x4020) {
		apuWrite(nes, address, value);
	} else {
		int page = address >> 12;
		if (nes->prgWritable[page] && nes->prgPages[page]) {
			nes->prgPages[page][address & 0xFFF] = value;
		}
		if (nes->board->write) {
			nes->board->write(nes, address, value);
		}
	}
}
