/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// The WDC 65C02 (with the Rockwell bit instructions, WAI and STP), as the Watara Supervision has it:
// whole instructions at a time, each with its cycle count. Memory is read and written through the
// callbacks.
struct W65c02 {
	uint16_t pc;
	uint8_t a, x, y, s, p;
	// An NMI's edge seen and not yet taken; the IRQ line as it is now.
	bool nmi, irq;
	// After WAI, until an interrupt; after STP, until reset.
	bool waiting, stopped;
	uint8_t (*read)(void* context, uint16_t address);
	void (*write)(void* context, uint16_t address, uint8_t data);
	void* context;
};

void w65c02Reset(struct W65c02* cpu);
// One instruction, or the interrupt taken before it; the cycles it took (at least 1, waiting too).
int w65c02Step(struct W65c02* cpu);
