/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef ARMNES_GENIE_H
#define ARMNES_GENIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct NES;
struct Cheat;

// The Game Genie (Camerica, sold by Galoob), plugged in between the console and the game (genie.c),
// as NESdev's Game Genie page describes it.
//
// At power-on its 4 KB ROM is at $C000-$FFFF (four times; $8000-$BFFF is open bus) and gates make its
// pattern tables, the game's cart still giving the nametables. Its code screen writes up to three codes
// to $8001-$800C (address, compare, replace) and $8000: with bit 0 set it steps aside for the game
// until the power goes, and from then on each enabled code replaces the CPU's reads of its address in
// $8000-$FFFF (only when the game's byte there matches, with compare on). A code at $C020-$FFFF reads as
// its value ANDed with whatever the cart has at the address less $8000, a bus conflict some codes need.

// Whether the ROM image (with its header) is the Game Genie's: its 4 KB program, alone or repeated to
// 16 KB, then its pattern tables
bool genieIdentify(const uint8_t* data, size_t size);
// No-Intro's headerless dump, [BIOS] Game Genie (USA, Europe) (Unl).unh (its 4 KB program alone), made
// into the image genieIdentify takes: the program repeated to 16 KB and the pattern tables its gates
// make. NULL (and data left alone) for anything else; the image is malloc'd.
uint8_t* genieImageFrom(const uint8_t* data, size_t size, size_t* imageSize);
// Plugs it in (image as genieIdentify took it) between the console and the game loaded in nes; the next
// power-on starts its code screen. Without image, unplugs it.
void genieAttach(struct NES*, const uint8_t* image, size_t size);
bool geniePresent(void);
// Whether its code screen has the console, rather than the game
bool genieInMenu(void);

// For the console (nes.c, bus.c, state.c), as for the Pro Action Replay
void genieReset(struct NES*, bool hard);
void genieUnmap(struct NES*);
void genieMap(struct NES*);
extern bool genieMenuWrites;
bool genieWrite(struct NES*, uint16_t address, uint8_t value);
// A read of a page with a code (cheatPages): the byte the CPU gets
uint8_t genieRead(const struct NES*, uint16_t address, uint8_t value);
// The pages its codes need watched, put back in cheatPages after the frontend's cheats change
void genieCheatPages(struct NES*);
// Codes for the slots its code screen leaves empty, taken as it steps aside for the game (RetroArch's
// cheat database, through the frontend); count 0 for none. Kept until changed.
void genieImport(const struct Cheat* codes, int count);
size_t genieStateSize(void);
void genieSaveState(uint8_t* out);
void genieLoadState(struct NES*, const uint8_t* in);

#endif
