/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// nesrun <rom> [frames]: runs a test ROM headless. ROMs that report through $6000 (blargg's
// protocol) print their result; others print a hash of the last frame.

static uint8_t* readFile(const char* path, size_t* size) {
	FILE* f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = malloc(*size);
	if (fread(data, 1, *size, f) != *size) {
		free(data);
		data = NULL;
	}
	fclose(f);
	return data;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: nesrun <rom> [frames]\n");
		return 2;
	}
	size_t size;
	uint8_t* data = readFile(argv[1], &size);
	if (!data) {
		fprintf(stderr, "can't read %s\n", argv[1]);
		return 2;
	}
	int frames = argc > 2 ? atoi(argv[2]) : 3600;
	static struct NES nes;
	nesInit(&nes);
	static int16_t audio[8192];
	nes.audioOut = audio;
	nes.audioCapacity = 8192;
	if (!nesLoad(&nes, data, size)) {
		printf("UNSUPPORTED mapper %d.%d\n", nes.cart.mapper, nes.cart.submapper);
		return 3;
	}
	int resetAt = -1;
	bool seenRunning = false;
	// NESRUN_PRESS="frame:mask[:frames],...": hold pad 1 buttons (bit 0 A ... bit 7 Right)
	const char* press = getenv("NESRUN_PRESS");
	for (int frame = 0; frame < frames; frame++) {
		nes.pads[0] = 0;
		for (const char* p = press; p && *p;) {
			int at = 0, mask = 0, hold = 4;
			int n = sscanf(p, "%d:%i:%d", &at, &mask, &hold);
			if (n >= 2 && frame >= at && frame < at + hold) {
				nes.pads[0] |= mask;
			}
			p = strchr(p, ',');
			p = p ? p + 1 : NULL;
		}
		nesRunFrame(&nes);
		uint8_t* w = nes.cart.wram;
		if (!w || memcmp(w + 1, "\xDE\xB0\x61", 3)) {
			continue;
		}
		uint8_t status = w[0];
		if (status == 0x80) {
			seenRunning = true;
		} else if (status == 0x81) {
			// The test asks for a reset after a short wait
			if (resetAt < 0) {
				resetAt = frame + 6;
			} else if (frame >= resetAt) {
				nesReset(&nes, false);
				resetAt = -1;
			}
		} else if (seenRunning || status < 0x80) {
			printf("%s %d: %s\n", status ? "FAIL" : "PASS", status, (char*) w + 4);
			return status ? 1 : 0;
		}
	}
	const char* dump = getenv("NESRUN_RAM");
	if (dump) {
		FILE* f = fopen(dump, "wb");
		if (f) {
			fwrite(nes.ram, 1, sizeof(nes.ram), f);
			fclose(f);
		}
	}
	// Text tests write ASCII tile numbers into the first name table
	if (getenv("NESRUN_TEXT")) {
		for (int row = 0; row < 30; row++) {
			char line[33];
			int n = 0;
			for (int col = 0; col < 32; col++) {
				uint8_t c = nes.chrPages[8][row * 32 + col];
				line[n++] = c >= 0x20 && c < 0x7F ? c : ' ';
			}
			while (n && line[n - 1] == ' ') {
				n--;
			}
			line[n] = 0;
			if (n) {
				printf("%s\n", line);
			}
		}
	}
	uint32_t hash = crc32(0, (uint8_t*) nes.frameBuffer, NES_WIDTH * NES_HEIGHT * 2);
	printf("FRAME %08x pc=%04x%s\n", hash, nes.cpu.pc, nes.cpu.jammed ? " jammed" : "");
	return 4;
}
