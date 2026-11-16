/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMNES_PAR_H
#define ARMNES_PAR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct NES;

// Datel's Pro Action Replay, plugged in between the console and the game (par.c).
//
// Its 32 KB ROM and 8 KB of CHR ROM take the game's place until its menu starts the game, and its 4 KB
// of RAM sits at $5000-$5FFF throughout. Starting the game, the menu writes an NMI handler there that
// pokes each code's value into RAM ($500C: PHP, PHA, LDA #value / STA address for up to five codes,
// PLA, PLP, JMP ($FFFA)) and the handler's address at $500A-$500B, then runs a stub at $5000 that
// writes $FFF8 (its ROM steps aside) and jumps through the game's reset vector. From then on the
// console's NMI vector fetch gets $500A-$500B, so the codes go in every frame before the game's own
// NMI; any other read of the vector is the game's. Reset brings its menu back, with its RAM as it was
// (the trainer compares the RAM it copies there as it starts; the codes stay while "WHB" is at $5034).

// Whether the ROM image (with its header) is one of the three dumps: v1.0, v1.1 and v1.2 (Europe).
bool parIdentify(const uint8_t* data, size_t size);
// Plugs the Pro Action Replay in (image as parIdentify took it) between the console and the game
// loaded in nes; the next reset starts its menu. Without image, unplugs it.
void parAttach(struct NES*, const uint8_t* image, size_t size);
bool parPresent(void);
// Its 4 KB of RAM ($5000-$5FFF), or NULL without it
uint8_t* parMemory(void);
// Whether its menu has the console, rather than the game
bool parInMenu(void);

// For the console (nes.c, bus.c, cpu.c, state.c)
// A reset: the menu back over the game's pages (as the reset sequence fetches its vector)
void parReset(struct NES*, bool hard);
// The game's pages back while its pages are laid over them, as a reset or a state save needs; and the
// overlay put back after
void parUnmap(struct NES*);
void parMap(struct NES*);
// A write to $4020-$FFFF, before the game's board sees it: true when the Pro Action Replay keeps it
bool parWrite(struct NES*, uint16_t address, uint8_t value);
// While its menu runs: writes to $8000-$FFFF are its (parWrite); while the game runs: the NMI vector
// it gives in place of the game's
extern bool parMenuWrites, parHooksNmi;
uint16_t parNmiVector(void);
// Its part of a save state: the menu flag and its RAM
// Codes for its table ($5037: up to five, each 00, address high, address low, value), in place as it
// powers on, as if typed and kept (with "WHB" at $5034, its mark that the table holds codes); count 0
// for none. Kept until changed.
void parImport(const uint8_t codes[][4], int count);
size_t parStateSize(void);
void parSaveState(uint8_t* out);
void parLoadState(struct NES*, const uint8_t* in);

#endif
