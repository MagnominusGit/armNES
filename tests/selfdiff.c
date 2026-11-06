/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// selfdiff <rom> [frames]: runs the game on the exact model (every master clock, half-dot PPU) and
// on the fast one, and reports the first frame where what the game shows, plays or computes differs.

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

static const char* differs(struct NES* a, struct NES* b) {
	nesPpuSync(b);
	// Line 0 is left out: on odd frames the hardware draws a sprite shifter artifact there, in the
	// overscan, which only the exact model keeps
	if (memcmp(a->frameBuffer + NES_WIDTH, b->frameBuffer + NES_WIDTH, NES_WIDTH * (NES_HEIGHT - 1) * 2)) {
		return "frame";
	}
	if (memcmp(a->ram, b->ram, sizeof(a->ram))) {
		return "ram";
	}
	if (a->cpu.pc != b->cpu.pc || a->cpu.a != b->cpu.a || a->cpu.x != b->cpu.x || a->cpu.y != b->cpu.y ||
		a->cpu.s != b->cpu.s || a->cpu.p != b->cpu.p || a->cpu.cycles != b->cpu.cycles) {
		return "cpu";
	}
	if (a->audioCount != b->audioCount || memcmp(a->audioOut, b->audioOut, a->audioCount * 2)) {
		return "audio";
	}
	if (memcmp(a->boardState, b->boardState, sizeof(a->boardState))) {
		for (size_t i = 0; i < sizeof(a->boardState); i++) {
			if (a->boardState[i] != b->boardState[i]) {
				printf("  board byte %zu: %02x/%02x\n", i, a->boardState[i], b->boardState[i]);
			}
		}
		return "board";
	}
	return NULL;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: selfdiff <rom> [frames]\n");
		return 2;
	}
	size_t size;
	uint8_t* data = readFile(argv[1], &size);
	if (!data) {
		return 2;
	}
	int frames = argc > 2 ? atoi(argv[2]) : 1800;
	static struct NES a, b;
	static int16_t audioA[8192], audioB[8192];
	nesInit(&a);
	nesInit(&b);
	a.audioOut = audioA;
	b.audioOut = audioB;
	a.audioCapacity = b.audioCapacity = 8192;
	a.exactPpu = true;
	a.exactTiming = true;
	if (!nesLoad(&a, data, size) || !nesLoad(&b, data, size)) {
		printf("UNSUPPORTED\n");
		return 3;
	}
	// A fixed button script: Start, then A and directions in turn
	for (int frame = 0; frame < frames; frame++) {
		uint8_t pad = 0;
		int phase = frame % 240;
		if (frame % 600 < 8) {
			pad = 0x08;
		} else if (phase < 60) {
			pad = 0x80;
		} else if (phase < 90) {
			pad = 0x81;
		} else if (phase < 150) {
			pad = 0x40 | ((frame / 7) & 1 ? 0x02 : 0);
		} else if (phase < 200) {
			pad = 0x20;
		}
		a.pads[0] = b.pads[0] = pad;
		nesRunFrame(&a);
		nesRunFrame(&b);
		const char* what = differs(&a, &b);
		if (what) {
			printf("DIFF frame %d: %s (exact line %d dot %d, fast line %d dot %d) cycles %llu/%llu pc %04x/%04x", frame,
				what, a.ppu.scanline, a.ppu.dot, b.ppu.scanline, b.ppu.dot, (unsigned long long) a.cpu.cycles,
				(unsigned long long) b.cpu.cycles, a.cpu.pc, b.cpu.pc);
			for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) {
				if (a.frameBuffer[i] != b.frameBuffer[i]) {
					int n = 0;
					for (int j = i; j < NES_WIDTH * NES_HEIGHT; j++) {
						n += a.frameBuffer[j] != b.frameBuffer[j];
					}
					printf(" first pixel %d,%d (%03x/%03x), %d differ", i % NES_WIDTH, i / NES_WIDTH, a.frameBuffer[i],
						b.frameBuffer[i], n);
					break;
				}
			}
			printf("\n  regs a %02x/%02x x %02x/%02x y %02x/%02x p %02x/%02x s %02x/%02x\n", a.cpu.a, b.cpu.a, a.cpu.x,
				b.cpu.x, a.cpu.y, b.cpu.y, a.cpu.p, b.cpu.p, a.cpu.s, b.cpu.s);
			if (getenv("SELFDIFF_OAM2")) {
				printf("  oam2 exact:");
				for (int i = 0; i < 32; i++) {
					printf(" %02x", a.ppu.oam2[i]);
				}
				printf("\n  oam2 fast: ");
				for (int i = 0; i < 32; i++) {
					printf(" %02x", b.ppu.oam2[i]);
				}
				printf(" count %d\n  oam:", b.ppu.nextCount);
				for (int i = 0; i < 32; i++) {
					printf(" %02x", a.ppu.oam[i]);
				}
				printf("\n");
			}
			const char* dump = getenv("SELFDIFF_DUMP");
			if (dump) {
				FILE* f = fopen(dump, "wb");
				fwrite(a.frameBuffer, 2, NES_WIDTH * NES_HEIGHT, f);
				fwrite(b.frameBuffer, 2, NES_WIDTH * NES_HEIGHT, f);
				fclose(f);
			}
			if (getenv("SELFDIFF_LINES")) {
				for (int y = 0; y < NES_HEIGHT; y++) {
					int n = 0, shifted = 0;
					for (int x = 0; x < NES_WIDTH; x++) {
						n += a.frameBuffer[y * 256 + x] != b.frameBuffer[y * 256 + x];
						shifted += x < 255 && a.frameBuffer[y * 256 + x] != b.frameBuffer[y * 256 + x + 1];
					}
					if (n) {
						printf("  line %d: %d differ, %d vs shifted\n", y, n, shifted);
					}
				}
			}
			return 1;
		}
	}
	printf("SAME %d frames\n", frames);
	return 0;
}
