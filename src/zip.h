/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ZipEntry {
	char name[256];
	uint32_t crc, size, packed, offset;
	uint16_t method;
};

bool zipIs(const uint8_t* data, size_t size);
// Up to max entries; how many there are
int zipList(const uint8_t* zip, size_t size, struct ZipEntry* out, int max);
// An entry's contents, checked against its CRC (free() them), or NULL
uint8_t* zipRead(const uint8_t* zip, size_t size, const struct ZipEntry*);

// arcade.c: a MAME VS. System or PlayChoice-10 set as an NES 2.0 image (free() it), or NULL. name is
// the set's file name without .zip, if known; loadSet reads another set's .zip (the parent of a
// clone that keeps only its own chips) and returns it to be freed, or NULL.
uint8_t* arcadeBuild(const uint8_t* zip, size_t size, const char* name,
	uint8_t* (*loadSet)(const char* set, size_t* size), size_t* imageSize);
