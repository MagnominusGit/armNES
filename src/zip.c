/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "zip.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// .zip archives in memory: the central directory's entries, stored or deflated

static uint32_t le16(const uint8_t* p) {
	return p[0] | p[1] << 8;
}

static uint32_t le32(const uint8_t* p) {
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24;
}

bool zipIs(const uint8_t* data, size_t size) {
	return size >= 22 && le32(data) == 0x04034B50;
}

int zipList(const uint8_t* zip, size_t size, struct ZipEntry* out, int max) {
	// The end record, within its comment's reach of the end
	size_t end = size >= 22 ? size - 22 : 0;
	while (end > 0 && size - end < 22 + 0xFFFF && le32(zip + end) != 0x06054B50) {
		end--;
	}
	if (size < 22 || le32(zip + end) != 0x06054B50) {
		return 0;
	}
	int entries = (int) le16(zip + end + 10);
	size_t at = le32(zip + end + 16);
	int count = 0;
	for (int i = 0; i < entries && count < max; i++) {
		if (at + 46 > size || le32(zip + at) != 0x02014B50) {
			break;
		}
		const uint8_t* h = zip + at;
		size_t nameLength = le16(h + 28);
		if (at + 46 + nameLength > size) {
			break;
		}
		struct ZipEntry* e = &out[count++];
		e->method = (uint16_t) le16(h + 10);
		e->crc = le32(h + 16);
		e->packed = le32(h + 20);
		e->size = le32(h + 24);
		e->offset = le32(h + 42);
		size_t n = nameLength < sizeof(e->name) - 1 ? nameLength : sizeof(e->name) - 1;
		memcpy(e->name, h + 46, n);
		e->name[n] = 0;
		at += 46 + nameLength + le16(h + 30) + le16(h + 32);
	}
	return count;
}

uint8_t* zipRead(const uint8_t* zip, size_t size, const struct ZipEntry* e) {
	if ((size_t) e->offset + 30 > size || le32(zip + e->offset) != 0x04034B50) {
		return NULL;
	}
	size_t data = e->offset + 30 + le16(zip + e->offset + 26) + le16(zip + e->offset + 28);
	if (data + e->packed > size) {
		return NULL;
	}
	uint8_t* out = malloc(e->size ? e->size : 1);
	if (!out) {
		return NULL;
	}
	if (e->method == 0 && e->packed == e->size) {
		memcpy(out, zip + data, e->size);
	} else if (e->method == 8) {
		z_stream s;
		memset(&s, 0, sizeof(s));
		s.next_in = (Bytef*) (zip + data);
		s.avail_in = e->packed;
		s.next_out = out;
		s.avail_out = e->size;
		bool ok = inflateInit2(&s, -15) == Z_OK && inflate(&s, Z_FINISH) == Z_STREAM_END && s.total_out == e->size;
		inflateEnd(&s);
		if (!ok) {
			free(out);
			return NULL;
		}
	} else {
		free(out);
		return NULL;
	}
	if ((uint32_t) crc32(0, out, e->size) != e->crc) {
		free(out);
		return NULL;
	}
	return out;
}
