/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include "libretro.h"

// What's plugged in besides standard controllers: a Four Score or Famicom 4-player adapter, the Zapper,
// the paddle, the Power Pad, and on the Famicom's expansion port the paddle, the Famicom's Power
// Pad, the keyboard; and the microphone on the Famicom's second pad. The
// standard pads' serial reports go through the APU's shift registers (apu.c); this gives their
// reports and the other bits of $4016 and $4017.

static uint8_t padBits(uint8_t pad) {
	// Shifted out from bit 7: A first
	uint8_t out = 0;
	for (int i = 0; i < 8; i++) {
		if (pad & (1 << i)) {
			out |= 0x80 >> i;
		}
	}
	return out;
}

// A port's serial report as the APU's shift register holds it, first bit at the top: a pad's 8 bits
// then 1s, or with the Four Score players 1 and 3 (2 and 4), its signature, then 1s
uint32_t inputReport(const struct NES* nes, int port) {
	const struct InputConfig* in = &nes->input;
	if (in->device[port] != DEVICE_PAD) {
		return 0;
	}
	uint8_t pad = nes->pads[port];
	// Two VS. boards wire the pads' fourth bit high (behaviour per Mesen)
	if (nes->cart.vsHardware == 4 || nes->cart.vsHardware == 6) {
		pad |= 0x08;
	}
	if (in->fourScore) {
		uint8_t signature = port ? 0x20 : 0x10;
		return (uint32_t) padBits(pad) << 24 | padBits(in->pads[port]) << 16 | signature << 8 | 0xFF;
	}
	return (uint32_t) padBits(pad) << 24 | 0xFFFFFF;
}

// The protection chips on three VS. boards: two give a sequence of bytes from $5E01 after a $5E00
// read (behaviour per Mesen), the third answers at four addresses, two ways that a read of $5567
// switches between (behaviour per FCEUmm)
uint8_t vsProtectionRead(struct NES* nes, uint16_t address) {
	static const uint8_t sequences[2][32] = {
		{ 0x00, 0x00, 0x00, 0x00, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x6F, 0x00, 0x00, 0x00, 0x00, 0x94 },
		{ 0xFF, 0xBF, 0xB7, 0x97, 0x97, 0x17, 0x57, 0x4F, 0x6F, 0x6B, 0xEB, 0xA9, 0xB1, 0x90, 0x94, 0x14,
		  0x56, 0x4E, 0x6F, 0x6B, 0xEB, 0xA9, 0xB1, 0x90, 0xD4, 0x5C, 0x3E, 0x26, 0x87, 0x83, 0x13, 0x00 },
	};
	uint8_t* counter = &nes->inputState.vsCounter;
	if (nes->cart.vsHardware == 3) {
		switch (address) {
		case 0x54FF:
			return 0x05;
		case 0x5678:
			return *counter ? 0x00 : 0x01;
		case 0x578F:
			return *counter ? 0xD1 : 0x89;
		case 0x5567:
			*counter ^= 1;
			return *counter ? 0x37 : 0x3E;
		}
	} else if (address == 0x5E00) {
		*counter = 0;
		return 0;
	} else if (address == 0x5E01) {
		return sequences[nes->cart.vsHardware - 1][(*counter)++ & 31];
	}
	return nes->cpu.openBus;
}

// The paddle's knob as its counter reads it: 9 bits, the 8-bit range per version
static uint16_t knob(const struct NES* nes, bool famicom) {
	int position = nes->inputState.paddle;
	int value = famicom ? 0x4D + position * (0xF2 - 0x4D) / 255 : 0x62 + position * 0xA0 / 255;
	return (uint16_t) ((value & 0xFF) << 1);
}

// The strobe latching the controllers: the serial devices' reports with them
void inputLatch(struct NES* nes) {
	const struct InputConfig* in = &nes->input;
	struct InputState* st = &nes->inputState;
	for (int port = 0; port < 2; port++) {
		// Players 3 and 4 on D1, as Famicom adapters give them
		st->hori[port] = (uint32_t) padBits(in->pads[port]) << 24 | 0xFFFFFF;
		if (in->device[port] == DEVICE_PADDLE) {
			st->knob[port] = knob(nes, false);
		}
		if (in->device[port] == DEVICE_POWER_PAD) {
			// Buttons 2, 1, 5, 9, 6, 10, 11, 7 and 4, 3, 12, 8, first at the top
			static const uint8_t low[8] = { 2, 1, 5, 9, 6, 10, 11, 7 }, high[4] = { 4, 3, 12, 8 };
			st->powerLow[port] = st->powerHigh[port] = 0;
			for (int i = 0; i < 8; i++) {
				st->powerLow[port] |= ((in->powerPad >> (low[i] - 1)) & 1) << (7 - i);
			}
			for (int i = 0; i < 4; i++) {
				st->powerHigh[port] |= ((in->powerPad >> (high[i] - 1)) & 1) << (7 - i);
			}
			st->powerHigh[port] |= 0x0F;
		}
	}
	if (in->expansion == EXPANSION_PADDLE) {
		st->knob[1] = knob(nes, true);
	}
}

// The Zapper's photodiode: light while the beam is within about 20 lines past a bright pixel at the
// aim (white and light colors; it sees nothing off the screen)
static bool zapperLight(struct NES* nes) {
	const struct InputConfig* in = &nes->input;
	if (in->zapperX < 0 || in->zapperY < 0) {
		return false;
	}
	nesPpuSync(nes);
	int line = nes->ppu.scanline, dot = nes->ppu.dot;
	int y = in->zapperY, x = in->zapperX;
	if (line < y || line - y >= 20 || (line == y && dot <= x)) {
		return false;
	}
	for (int dy = -2; dy <= 2; dy++) {
		for (int dx = -2; dx <= 2; dx++) {
			int px = x + dx, py = y + dy;
			// Pixels the beam hasn't drawn yet this frame still hold the last one's
			if (px < 0 || px > 255 || py < 0 || py > 239 || py > line || (py == line && px + 5 > dot)) {
				continue;
			}
			uint8_t color = nes->frameBuffer[py * NES_WIDTH + px] & 0x3F;
			if (color >= 0x20 && (color & 0x0F) <= 0x0C) {
				return true;
			}
		}
	}
	return false;
}

// Bits 1-4 of a $4016 ($4017) read; a read also clocks the devices with serial reports
uint8_t inputBits(struct NES* nes, int port) {
	const struct InputConfig* in = &nes->input;
	struct InputState* st = &nes->inputState;
	uint8_t bits = 0;
	switch (in->device[port]) {
	case DEVICE_ZAPPER:
		bits |= (zapperLight(nes) ? 0 : 0x08) | (in->zapperTrigger ? 0x10 : 0);
		break;
	case DEVICE_PADDLE:
		// Inverted, most significant bit first; the button straight through
		bits |= ((~st->knob[port] >> 8) & 1) << 4 | (in->paddleButton ? 0x08 : 0);
		st->knob[port] = (uint16_t) ((st->knob[port] << 1) & 0x1FF);
		break;
	case DEVICE_POWER_PAD:
		bits |= (st->powerLow[port] >> 7) << 3 | (st->powerHigh[port] >> 7) << 4;
		st->powerLow[port] = (uint8_t) (st->powerLow[port] << 1 | 1);
		st->powerHigh[port] = (uint8_t) (st->powerHigh[port] << 1 | 1);
		break;
	}
	switch (in->expansion) {
	case EXPANSION_HORI:
		bits |= (st->hori[port] >> 31) << 1;
		st->hori[port] = st->hori[port] << 1 | 1;
		break;
	case EXPANSION_PADDLE:
		if (port) {
			bits |= ((~st->knob[1] >> 8) & 1) << 1;
			st->knob[1] = (uint16_t) ((st->knob[1] << 1) & 0x1FF);
		} else {
			bits |= in->paddleButton ? 0x02 : 0;
		}
		break;
	case EXPANSION_MAT:
		if (port) {
			// The selected rows' buttons, by column, low when pressed
			uint8_t columns = 0;
			for (int row = 0; row < 3; row++) {
				if (!(st->lastWrite & (4 >> row))) {
					columns |= (in->powerPad >> (row * 4)) & 0x0F;
				}
			}
			for (int column = 0; column < 4; column++) {
				bits |= ((columns >> column) & 1 ? 0 : 1) << (4 - column);
			}
		}
		break;
	case EXPANSION_KEYBOARD:
		if (port && st->keyboardEnable) {
			bits |= 0x1E;
			if (st->keyboardRow < 9) {
				static const uint16_t matrix[9][8] = {
					{ RETROK_RIGHTBRACKET, RETROK_LEFTBRACKET, RETROK_RETURN, RETROK_F8, RETROK_END, RETROK_BACKSLASH,
						RETROK_RSHIFT, RETROK_RALT },
					{ RETROK_SEMICOLON, RETROK_QUOTE, RETROK_BACKQUOTE, RETROK_F7, RETROK_EQUALS, RETROK_MINUS,
						RETROK_SLASH, RETROK_RCTRL },
					{ RETROK_k, RETROK_l, RETROK_o, RETROK_F6, RETROK_0, RETROK_p, RETROK_COMMA, RETROK_PERIOD },
					{ RETROK_j, RETROK_u, RETROK_i, RETROK_F5, RETROK_8, RETROK_9, RETROK_n, RETROK_m },
					{ RETROK_h, RETROK_g, RETROK_y, RETROK_F4, RETROK_6, RETROK_7, RETROK_v, RETROK_b },
					{ RETROK_d, RETROK_r, RETROK_t, RETROK_F3, RETROK_4, RETROK_5, RETROK_c, RETROK_f },
					{ RETROK_a, RETROK_s, RETROK_w, RETROK_F2, RETROK_3, RETROK_e, RETROK_z, RETROK_x },
					{ RETROK_LCTRL, RETROK_q, RETROK_ESCAPE, RETROK_F1, RETROK_2, RETROK_1, RETROK_LALT, RETROK_LSHIFT },
					{ RETROK_LEFT, RETROK_RIGHT, RETROK_UP, RETROK_HOME, RETROK_INSERT, RETROK_BACKSPACE, RETROK_SPACE,
						RETROK_DOWN },
				};
				const uint16_t* keys = &matrix[st->keyboardRow][st->keyboardColumn ? 4 : 0];
				for (int i = 0; i < 4; i++) {
					uint16_t key = keys[i];
					if ((nes->keys[key >> 3] >> (key & 7)) & 1) {
						bits &= ~(0x10 >> i);
					}
				}
			}
		}
		break;
	}
	// The Famicom's second pad has a microphone
	if (!port && in->mic) {
		bits |= 0x04;
	}
	// The VS. System's cabinet: service button, DIP switches 1-2 and coins on $4016, switches 3-8
	// on $4017
	if (nes->cart.vs) {
		if (port) {
			bits |= in->vsDip & 0xFC;
		} else {
			bits |= in->vsService << 2 | (in->vsDip & 3) << 3 | in->vsCoin[0] << 5 | in->vsCoin[1] << 6 | nes->vsSub << 7;
		}
	}
	return bits;
}

// A $4016 write: the keyboard's row and column, the Famicom Power Pad's rows, a VS. System's banks
void inputWrite(struct NES* nes, uint8_t value) {
	struct InputState* st = &nes->inputState;
	if (nes->board->out) {
		nes->board->out(nes, value);
	}
	// A VS. DualSystem: bit 1 low raises the other board's IRQ, and the main board's gives the shared
	// RAM to the second (behaviour per Mesen)
	if (nes->dual && ((st->lastWrite ^ value) & 2)) {
		nesSetIrq(nes->dual, IRQ_DUAL, !(value & 2));
		nes->dual->quietBudget = nes->dual->quietGranted = 0;
		if (!nes->vsSub) {
			nes->vsRamAccess = value & 2;
			nes->dual->vsRamAccess = !(value & 2);
		}
	}
	if (nes->input.expansion == EXPANSION_KEYBOARD) {
		if (value & 1) {
			st->keyboardRow = 0;
			st->keyboardColumn = false;
		} else if (st->keyboardColumn && !(value & 2)) {
			st->keyboardRow = (uint8_t) ((st->keyboardRow + 1) % 10);
		}
		st->keyboardColumn = value & 2;
		st->keyboardEnable = value & 4;
	}
	st->lastWrite = value;
}
