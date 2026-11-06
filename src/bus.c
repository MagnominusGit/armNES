/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

// The CPU's address space. Reads of nothing return the last value on the data bus.

static uint8_t readPad(struct NES* nes, int port) {
	if (nes->strobe) {
		return nes->pads[port] & 1;
	}
	uint8_t bit = nes->padShift[port] & 1;
	// After eight reads an official controller returns 1s
	nes->padShift[port] = (nes->padShift[port] >> 1) | 0x80;
	return bit;
}

uint8_t busRead(struct NES* nes, uint16_t address) {
	uint8_t value;
	if (address < 0x2000) {
		value = nes->ram[address & 0x7FF];
	} else if (address < 0x4000) {
		value = ppuReadRegister(nes, address);
	} else if (address < 0x4020) {
		switch (address) {
		case 0x4015:
			// $4015 doesn't drive bit 5 and leaves the data bus as it was
			value = (apuReadStatus(nes) & ~0x20) | (nes->cpu.openBus & 0x20);
			return value;
		case 0x4016:
		case 0x4017:
			value = (readPad(nes, address - 0x4016) & 0x1F) | (nes->cpu.openBus & 0xE0);
			break;
		default:
			value = nes->cpu.openBus;
			break;
		}
	} else if (nes->readHooks & (1 << (address >> 12))) {
		value = nes->board->read(nes, address);
	} else {
		uint8_t* page = nes->prgPages[address >> 12];
		value = page ? page[address & 0xFFF] : nes->cpu.openBus;
	}
	nes->cpu.openBus = value;
	return value;
}

void busWrite(struct NES* nes, uint16_t address, uint8_t value) {
	nes->cpu.openBus = value;
	if (address < 0x2000) {
		nes->ram[address & 0x7FF] = value;
	} else if (address < 0x4000) {
		ppuWriteRegister(nes, address, value);
	} else if (address < 0x4020) {
		if (address == 0x4014) {
			nes->cpu.spritePage = value;
			nes->cpu.spriteDma = true;
			nes->cpu.dmaHalt = true;
		} else if (address == 0x4016) {
			bool strobe = value & 1;
			if (nes->strobe && !strobe) {
				nes->padShift[0] = nes->pads[0];
				nes->padShift[1] = nes->pads[1];
			}
			nes->strobe = strobe;
			if (strobe) {
				nes->padShift[0] = nes->pads[0];
				nes->padShift[1] = nes->pads[1];
			}
		} else {
			apuWrite(nes, address, value);
		}
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
