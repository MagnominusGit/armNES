/* Copyright (c) 2026 Magnominus
 * Register behaviour after FCEUmm's bandai.c (Copyright (C) 2007 CaH4e3, Copyright (C) 2011 FCEUX
 * team), under the GNU General Public License, version 2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

// Bandai FCG and LZ93D50 (16, 153, 157, 159): eight 1 KB CHR banks, a 16 KB PRG bank, a 16-bit
// down-counting IRQ, and a serial EEPROM (24C02, or 24C01 on 159) whose contents are the save.

// The EEPROM, on SCL and SDA from register $D; it answers on bit 4 of reads from $6000-$7FFF
enum { EE_IDLE, EE_CHIP, EE_ADDRESS, EE_WRITE, EE_READ, EE_SEND_ACK, EE_WAIT_ACK };

struct Eeprom {
	uint8_t mode, next, data, address, count;
	bool scl, sda, out, read;
};

struct Bandai {
	uint8_t regs[10];
	uint16_t latch;
	int32_t counter;
	bool irqEnabled;
	struct Eeprom eeprom;
};

#define BD STATE(Bandai)

static bool is24c01(const struct NES* nes) {
	return nes->cart.mapper == 159;
}

static uint8_t* eepromData(struct NES* nes) {
	return nes->cart.wram;
}

static void eepromWrite(struct NES* nes, bool scl, bool sda) {
	struct Eeprom* e = &BD->eeprom;
	bool c01 = is24c01(nes);
	uint8_t mask = c01 ? 0x7F : 0xFF;
	if (e->scl && scl && e->sda && !sda) {
		// Start
		e->mode = c01 ? EE_ADDRESS : EE_CHIP;
		e->count = 0;
		e->data = 0;
		e->out = true;
	} else if (e->scl && scl && !e->sda && sda) {
		// Stop
		e->mode = EE_IDLE;
		e->out = true;
	} else if (!e->scl && scl) {
		// SCL rising: a bit in or out
		switch (e->mode) {
		case EE_CHIP:
		case EE_ADDRESS:
		case EE_WRITE:
			if (c01) {
				e->data |= sda << e->count;
			} else {
				e->data = (e->data << 1) | sda;
			}
			if (++e->count < 8) {
				break;
			}
			e->count = 0;
			if (e->mode == EE_CHIP) {
				if ((e->data & 0xF0) == 0xA0) {
					e->read = e->data & 1;
					e->next = e->read ? EE_READ : EE_ADDRESS;
					e->mode = EE_SEND_ACK;
				} else {
					e->mode = EE_IDLE;
				}
			} else if (e->mode == EE_ADDRESS) {
				if (c01) {
					e->address = e->data & 0x7F;
					e->read = e->data & 0x80;
					e->next = e->read ? EE_READ : EE_WRITE;
				} else {
					e->address = e->data;
					e->next = EE_WRITE;
				}
				e->mode = EE_SEND_ACK;
			} else {
				eepromData(nes)[e->address & mask] = e->data;
				e->address = (e->address + 1) & mask;
				e->next = EE_WRITE;
				e->mode = EE_SEND_ACK;
			}
			e->data = 0;
			break;
		case EE_READ:
			if (++e->count == 8) {
				e->count = 0;
				e->address = (e->address + 1) & mask;
				e->mode = EE_WAIT_ACK;
			}
			break;
		case EE_SEND_ACK:
			e->mode = e->next;
			e->count = 0;
			e->data = 0;
			break;
		case EE_WAIT_ACK:
			e->mode = sda ? EE_IDLE : EE_READ;
			e->count = 0;
			break;
		}
	} else if (e->scl && !scl) {
		// SCL falling: what the chip drives next
		if (e->mode == EE_SEND_ACK) {
			e->out = false;
		} else if (e->mode == EE_READ) {
			uint8_t value = eepromData(nes)[e->address & mask];
			e->out = c01 ? (value >> e->count) & 1 : (value >> (7 - e->count)) & 1;
		} else {
			e->out = true;
		}
	}
	e->scl = scl;
	e->sda = sda;
}

static void bandaiSync(struct NES* nes) {
	struct Bandai* b = BD;
	if (nes->cart.mapper == 153) {
		int base = (b->regs[0] & 1) << 4;
		chr8(nes, 0);
		prg16(nes, 0x8000, (b->regs[8] & 0x0F) | base);
		prg16(nes, 0xC000, 0x0F | base);
	} else {
		for (int i = 0; i < 8; i++) {
			chr1(nes, i * 0x400, b->regs[i]);
		}
		prg16(nes, 0x8000, b->regs[8]);
		prg16(nes, 0xC000, -1);
	}
	static const enum Mirroring mirrors[4] = { MIRROR_VERTICAL, MIRROR_HORIZONTAL, MIRROR_SCREEN_A, MIRROR_SCREEN_B };
	nesSetMirroring(nes, mirrors[b->regs[9] & 3]);
}

static void bandaiWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Bandai* b = BD;
	if (address < 0x6000 || (nes->cart.mapper == 153 && address < 0x8000)) {
		return;
	}
	int reg = address & 0x0F;
	if (reg < 0x0A) {
		b->regs[reg] = value;
		bandaiSync(nes);
		return;
	}
	switch (reg) {
	case 0x0A:
		nesSetIrq(nes, IRQ_MAPPER, false);
		b->irqEnabled = value & 1;
		b->counter = b->latch;
		break;
	case 0x0B:
	case 0x0C:
		if (reg == 0x0B) {
			b->latch = (b->latch & 0xFF00) | value;
		} else {
			b->latch = (b->latch & 0x00FF) | (value << 8);
		}
		// The earlier FCG chips have no latch: these write the counter
		if (nes->cart.submapper != 5) {
			b->counter = b->latch;
		}
		break;
	case 0x0D:
		if (nes->cart.mapper != 153) {
			eepromWrite(nes, (value >> 5) & 1, (value >> 6) & 1);
		}
		break;
	}
}

static uint8_t bandaiRead(struct NES* nes, uint16_t address) {
	return (nes->cpu.openBus & 0xEF) | (BD->eeprom.out << 4);
}

static void bandaiCycle(struct NES* nes) {
	struct Bandai* b = BD;
	if (b->irqEnabled && --b->counter < 0) {
		nesSetIrq(nes, IRQ_MAPPER, true);
		b->irqEnabled = false;
		b->counter = -1;
	}
}

static void bandaiReset(struct NES* nes, bool hard) {
	struct Bandai* b = BD;
	if (hard) {
		memset(b, 0, sizeof(*b));
		b->eeprom.out = true;
		if (nes->cart.mapper != 153) {
			// The EEPROM is the save
			nes->cart.battery = true;
			nes->cart.batterySize = is24c01(nes) ? 128 : 256;
		}
	}
	if (nes->cart.mapper == 153) {
		wram8(nes);
	} else {
		nesUnmapPrg(nes, 0x6000, 0x2000);
		nes->readHooks |= 1 << 6 | 1 << 7;
	}
	bandaiSync(nes);
}

const struct Board boardBandai = {
	.name = "Bandai FCG", .reset = bandaiReset, .write = bandaiWrite, .read = bandaiRead, .cpuCycle = bandaiCycle,
	.sync = bandaiSync, .stateSize = sizeof(struct Bandai),
};
