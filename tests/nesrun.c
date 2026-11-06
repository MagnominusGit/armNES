/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
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

// A palette from the NTSC signal levels (Bisqwit's method), good enough to look at frames
static void paletteRgb(int index, uint8_t rgb[3]) {
	static const float low[4] = { 0.350f, 0.518f, 0.962f, 1.550f };
	static const float high[4] = { 1.094f, 1.506f, 1.962f, 1.962f };
	const float black = 0.518f, white = 1.962f;
	int color = index & 15, level = (index >> 4) & 3;
	if (color >= 14) {
		rgb[0] = rgb[1] = rgb[2] = 0;
		return;
	}
	float y = 0, i = 0, q = 0;
	for (int p = 0; p < 12; p++) {
		bool on = (color + p) % 12 < 6;
		float v = color == 0 ? high[level] : color == 13 ? low[level] : on ? high[level] : low[level];
		v = (v - black) / (white - black);
		y += v / 12;
		i += v * cosf(3.14159265f * (p + 3) / 6) / 12;
		q += v * sinf(3.14159265f * (p + 3) / 6) / 12;
	}
	float out[3] = { y + 0.946882f * i + 0.623557f * q, y - 0.274788f * i - 0.635691f * q,
		y - 1.108545f * i + 1.709007f * q };
	for (int c = 0; c < 3; c++) {
		float v = out[c] < 0 ? 0 : out[c] > 1 ? 1 : out[c];
		rgb[c] = (uint8_t) (255 * powf(v, 1 / 2.2f) + 0.5f);
	}
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
	nes.exactPpu = getenv("NESRUN_EXACT") != NULL;
	if (!nesLoad(&nes, data, size)) {
		printf("UNSUPPORTED mapper %d.%d\n", nes.cart.mapper, nes.cart.submapper);
		return 3;
	}
	// NESRUN_INFO: the header as read, then stop
	if (getenv("NESRUN_INFO")) {
		printf("INFO %d.%d mirror=%d battery=%d prg=%u chr=%u%s wram=%u vram=%u region=%d\n", nes.cart.mapper, nes.cart.submapper,
			nes.cart.mirroring, nes.cart.battery, nes.cart.prgSize, nes.cart.chrSize, nes.cart.chrRam ? "ram" : "",
			nes.cart.wramSize, nes.cart.vramSize, nes.cart.region);
		return 0;
	}
	int resetAt = -1;
	bool seenRunning = false;
	// NESRUN_PRESS="frame:mask[:frames],...": hold pad 1 buttons (bit 0 A ... bit 7 Right)
	const char* press = getenv("NESRUN_PRESS");
	// NESRUN_WAV=file: the sound, 48 kHz 16-bit stereo
	FILE* wav = getenv("NESRUN_WAV") ? fopen(getenv("NESRUN_WAV"), "wb") : NULL;
	uint32_t wavBytes = 0;
	if (wav) {
		fwrite("RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data\0\0\0\0", 1, 44, wav);
	}
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
		if (wav) {
			wavBytes += fwrite(nes.audioOut, 2, nes.audioCount, wav) * 2;
		}
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
	const char* ppm = getenv("NESRUN_PPM");
	if (ppm) {
		FILE* f = fopen(ppm, "wb");
		if (f) {
			fprintf(f, "P6 256 240 255\n");
			for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) {
				uint8_t rgb[3];
				paletteRgb(nes.frameBuffer[i] & 0x3F, rgb);
				fwrite(rgb, 1, 3, f);
			}
			fclose(f);
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
	if (wav) {
		uint32_t riff = wavBytes + 36;
		fseek(wav, 4, SEEK_SET);
		fwrite(&riff, 4, 1, wav);
		fseek(wav, 40, SEEK_SET);
		fwrite(&wavBytes, 4, 1, wav);
		fclose(wav);
	}
	uint32_t hash = crc32(0, (uint8_t*) nes.frameBuffer, NES_WIDTH * NES_HEIGHT * 2);
	bool seen[512] = { false };
	int colors = 0;
	for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) {
		if (!seen[nes.frameBuffer[i] & 0x1FF]) {
			seen[nes.frameBuffer[i] & 0x1FF] = true;
			colors++;
		}
	}
	printf("FRAME %08x pc=%04x colors=%d%s\n", hash, nes.cpu.pc, colors, nes.cpu.jammed ? " jammed" : "");
	return 4;
}
