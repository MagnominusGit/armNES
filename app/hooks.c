/* Copyright (c) 2026 Magnominus
 * SPDX-License-Identifier: GPL-2.0-or-later */

// What armNES adds to the shared app (app-hooks.h has what each hook is for).

#include "app-hooks.h"
#include "frontend.h"
#include "rc_consoles.h"
#include "ui.h"

#include <string.h>

// A Disk System game is a disk image, with or without fwNES's header.
static uint32_t cheevosConsole(const uint8_t* data, size_t size) {
	bool const disk = (size >= 4 && !memcmp(data, "FDS\x1a", 4)) || (size >= 15 && !memcmp(data, "\x01*NINTENDO-HVC*", 15));
	return disk ? RC_CONSOLE_FAMICOM_DISK_SYSTEM : RC_CONSOLE_NINTENDO;
}

// Core > System, after Game on Top with no game loaded: the core started with no game, which opens the
// cheat cartridge it finds
static void startCartridge(int unused, int delta) {
	(void) unused;
	if (!delta && loadNoCartridge()) {
		uiCloseMenu();
	}
}

static void optionRows(const struct Option* option) {
	if (strcmp(option->key, "armnes_par_game") || (app.loaded && !app.noCartridge)) {
		return;
	}
	addItem("Start Cheat Cartridge", startCartridge, 0);
	helpFor("Starts the Game Genie or Pro Action Replay found in the BIOS folder or the NES games' folder, with no game: "
	        "pick one for it with Game on Top.");
}

const struct AppHooks appHooks = {
	.cheevosConsole = cheevosConsole,
	.optionRows = optionRows,
	.discMenu = "Disk Sides",
	.disc = "Side",
};
