/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "zip.h"

#include <stdlib.h>
#include <string.h>

// MAME's VS. System and PlayChoice-10 sets: each set's chips, found by CRC in its .zip (and its
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

enum { MAX_CHIPS = 64 };

struct Archive {
	const uint8_t* data;
	size_t size;
	struct ZipEntry entries[MAX_CHIPS];
	int count;
};

static const struct ZipEntry* findChip(const struct Archive* a, uint32_t crc) {
	for (int i = 0; a && i < a->count; i++) {
		if (a->entries[i].crc == crc) {
			return &a->entries[i];
		}
	}
	return NULL;
}

// How many of a set's pieces this archive has
static int found(const struct ArcadeSet* set, const struct Archive* a) {
	int n = 0;
	for (int i = 0; i < set->count; i++) {
		n += findChip(a, arcadePieces[set->first + i].crc) != NULL;
	}
	return n;
}

uint8_t* arcadeBuild(const uint8_t* zip, size_t size, const char* name,
	uint8_t* (*loadSet)(const char* set, size_t* size), size_t* imageSize) {
	static struct Archive own, parent;
	own.data = zip;
	own.size = size;
	own.count = zipList(zip, size, own.entries, MAX_CHIPS);
	// The set by its name, else the one most of whose chips are here
	const struct ArcadeSet* set = NULL;
	int best = 0;
	for (size_t i = 0; i < sizeof(arcadeSets) / sizeof(arcadeSets[0]); i++) {
		const struct ArcadeSet* s = &arcadeSets[i];
		int n = found(s, &own);
		if (name && !strcmp(name, s->name)) {
			set = s;
			break;
		}
		if (n > best) {
			best = n;
			set = s;
		}
	}
	if (!set) {
		return NULL;
	}
	uint8_t* parentData = NULL;
	parent.count = 0;
	if (found(set, &own) < set->count && set->parent && loadSet) {
		size_t parentSize = 0;
		parentData = loadSet(set->parent, &parentSize);
		if (parentData) {
			parent.data = parentData;
			parent.size = parentSize;
			parent.count = zipList(parentData, parentSize, parent.entries, MAX_CHIPS);
		}
	}
	uint8_t* image = calloc(1, 16 + set->size);
	for (int i = 0; image && i < set->count; i++) {
		const struct ArcadePiece* p = &arcadePieces[set->first + i];
		const struct Archive* a = findChip(&own, p->crc) ? &own : &parent;
		const struct ZipEntry* e = findChip(a, p->crc);
		uint8_t* chip = e && p->from + p->size <= e->size ? zipRead(a->data, a->size, e) : NULL;
		if (!chip) {
			free(image);
			image = NULL;
			break;
		}
		memcpy(image + 16 + p->to, chip + p->from, p->size);
		free(chip);
	}
	free(parentData);
	if (image) {
		memcpy(image, set->header, 16);
		*imageSize = 16 + set->size;
	}
	return image;
}
