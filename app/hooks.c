/* Copyright (c) 2026 Magnominus
 * SPDX-License-Identifier: GPL-2.0-or-later */

// What armNES adds to the shared app (app-hooks.h has what each hook is for).

#include "app-hooks.h"
#include "rc_consoles.h"

#include <string.h>

// A Disk System game is a disk image, with or without fwNES's header.
static uint32_t cheevosConsole(const uint8_t* data, size_t size) {
	bool const disk = (size >= 4 && !memcmp(data, "FDS\x1a", 4)) || (size >= 15 && !memcmp(data, "\x01*NINTENDO-HVC*", 15));
	return disk ? RC_CONSOLE_FAMICOM_DISK_SYSTEM : RC_CONSOLE_NINTENDO;
}

const struct AppHooks appHooks = {
	.cheevosConsole = cheevosConsole,
	.discMenu = "Disk Sides",
	.disc = "Side",
};
