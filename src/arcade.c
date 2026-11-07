/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "arcade.h"

#include <stdlib.h>
#include <string.h>

// MAME's VS. System and PlayChoice-10 sets: each set's chips, found by CRC among its archive's (and its
// parent's), put together as an NES 2.0 image

struct ArcadePiece {
	uint32_t crc, from, size, to;
};

struct ArcadeSet {
	const char* name;
	const char* parent;
	uint8_t header[16];
	uint32_t size;
	uint16_t first, count;
};

#include "arcade-sets.inc"

static const struct ArcadeChip* findChip(const struct ArcadeChip* chips, int count, uint32_t crc) {
	for (int i = 0; i < count; i++) {
		if (chips[i].crc == crc) {
			return &chips[i];
		}
	}
	return NULL;
}

// How many of a set's pieces these chips have
static int found(const struct ArcadeSet* set, const struct ArcadeChip* chips, int count) {
	int n = 0;
	for (int i = 0; i < set->count; i++) {
		n += findChip(chips, count, arcadePieces[set->first + i].crc) != NULL;
	}
	return n;
}

uint8_t* arcadeBuild(const struct ArcadeChip* chips, int count, const char* name,
	const struct ArcadeChip* (*parentChips)(const char* set, int* count), size_t* imageSize) {
	// The set by its name, else the one most of whose chips are here
	const struct ArcadeSet* set = NULL;
	int best = 0;
	for (size_t i = 0; i < sizeof(arcadeSets) / sizeof(arcadeSets[0]); i++) {
		const struct ArcadeSet* s = &arcadeSets[i];
		if (name && !strcmp(name, s->name)) {
			set = s;
			break;
		}
		int n = found(s, chips, count);
		if (n > best) {
			best = n;
			set = s;
		}
	}
	if (!set) {
		return NULL;
	}
	const struct ArcadeChip* parent = NULL;
	int parentCount = 0;
	if (found(set, chips, count) < set->count && set->parent && parentChips) {
		parent = parentChips(set->parent, &parentCount);
	}
	uint8_t* image = calloc(1, 16 + set->size);
	for (int i = 0; image && i < set->count; i++) {
		const struct ArcadePiece* p = &arcadePieces[set->first + i];
		const struct ArcadeChip* chip = findChip(chips, count, p->crc);
		chip = chip ? chip : findChip(parent, parentCount, p->crc);
		if (!chip || p->from + p->size > chip->size) {
			free(image);
			return NULL;
		}
		memcpy(image + 16 + p->to, chip->data + p->from, p->size);
	}
	if (image) {
		memcpy(image, set->header, 16);
		*imageSize = 16 + set->size;
	}
	return image;
}
