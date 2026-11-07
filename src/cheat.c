/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// Cheats: Game Genie codes and raw ones. Codes at $8000 and up change what the CPU reads there (when
// the ROM holds the compare value, for codes that have one); codes below hold a RAM byte at a value,
// set again every frame.

static int genieDigit(char c) {
	static const char letters[] = "APZLGITYEOXUKSVN";
	const char* p = strchr(letters, toupper((unsigned char) c));
	return c && p ? (int) (p - letters) : -1;
}

static bool parseGenie(const char* code, size_t length, struct Cheat* cheat) {
	int n[8];
	if (length != 6 && length != 8) {
		return false;
	}
	for (size_t i = 0; i < length; i++) {
		if ((n[i] = genieDigit(code[i])) < 0) {
			return false;
		}
	}
	cheat->address = 0x8000 | ((n[3] & 7) << 12) | ((n[5] & 7) << 8) | ((n[4] & 8) << 8) | ((n[2] & 7) << 4) |
		((n[1] & 8) << 4) | (n[4] & 7) | (n[3] & 8);
	cheat->value = ((n[1] & 7) << 4) | ((n[0] & 8) << 4) | (n[0] & 7) | ((length == 6 ? n[5] : n[7]) & 8);
	cheat->compare = length == 8 ? ((n[7] & 7) << 4) | ((n[6] & 8) << 4) | (n[6] & 7) | (n[5] & 8) : -1;
	return true;
}

// AAAA:VV or AAAA?CC:VV, in hex
static bool parseRaw(const char* code, size_t length, struct Cheat* cheat) {
	char text[16];
	if (length >= sizeof(text)) {
		return false;
	}
	memcpy(text, code, length);
	text[length] = 0;
	unsigned address, value, compare;
	char tail;
	if (sscanf(text, "%4x?%2x:%2x%c", &address, &compare, &value, &tail) == 3) {
		cheat->compare = (int16_t) compare;
	} else if (sscanf(text, "%4x:%2x%c", &address, &value, &tail) == 2) {
		cheat->compare = -1;
	} else {
		return false;
	}
	cheat->address = address;
	cheat->value = value;
	return true;
}

void nesCheatClear(struct NES* nes) {
	nes->cheatCount = 0;
	nes->cheatPages = 0;
}

// One or more codes, joined by '+', spaces or commas; false if any can't be read
bool nesCheatAdd(struct NES* nes, const char* codes) {
	bool ok = true;
	while (*codes) {
		size_t length = strcspn(codes, "+ ,;\t\r\n");
		struct Cheat cheat;
		if (length) {
			if ((parseGenie(codes, length, &cheat) || parseRaw(codes, length, &cheat)) && nes->cheatCount < MAX_CHEATS) {
				nes->cheats[nes->cheatCount++] = cheat;
				if (cheat.address >= 0x8000) {
					nes->cheatPages |= 1 << (cheat.address >> 12);
				}
			} else {
				ok = false;
			}
		}
		codes += length;
		codes += *codes != 0;
	}
	return ok;
}

uint8_t nesCheatRead(const struct NES* nes, uint16_t address, uint8_t value) {
	for (int i = 0; i < nes->cheatCount; i++) {
		const struct Cheat* c = &nes->cheats[i];
		if (c->address == address && (c->compare < 0 || c->compare == value)) {
			return c->value;
		}
	}
	return value;
}

void nesCheatFrame(struct NES* nes) {
	for (int i = 0; i < nes->cheatCount; i++) {
		const struct Cheat* c = &nes->cheats[i];
		if (c->address < 0x2000) {
			nes->ram[c->address & 0x7FF] = c->value;
		} else if (c->address < 0x8000 && c->address >= 0x4020) {
			uint8_t* page = nes->prgPages[c->address >> 12];
			if (page && nes->prgWritable[c->address >> 12]) {
				page[c->address & 0xFFF] = c->value;
			}
		}
	}
}
