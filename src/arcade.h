/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

// A chip from a set's archive: its CRC and contents
struct ArcadeChip {
	uint32_t crc;
	const uint8_t* data;
	size_t size;
};

// A MAME VS. System or PlayChoice-10 set as an NES 2.0 image (free() it), or NULL. name is the set's
// file name without its extension, if known; parentChips gives another set's chips (a clone may keep
// only its own and need its parent's), or NULL.
uint8_t* arcadeBuild(const struct ArcadeChip* chips, int count, const char* name,
	const struct ArcadeChip* (*parentChips)(const char* set, int* count), size_t* imageSize);
