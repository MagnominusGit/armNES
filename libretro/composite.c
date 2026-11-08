/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "composite.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

// The picture as a TV decodes the console's composite signal. The PPU draws each pixel as a square
// wave between two levels, 12 samples to a cycle of the color carrier: 8 samples a pixel on NTSC, 10
// on PAL. The TV takes chroma with a filter two carrier cycles long, and luma with a notch filter, as
// TVs of the time did: the signal less its carrier band, which keeps broad detail sharp and leaves the
// fringes and dot crawl at edges (S-Video's luma has no carrier and is only smoothed). PAL mirrors the carrier's phase every other line, and the TV
// averages each line's color with the line before, which blurs color vertically and cancels hue
// errors.
//
// The decoding is linear, so each pixel's part of the output is a table indexed by its color, the
// carrier's phase where it starts and, on PAL, the line's mirroring: 11 output pixels around it, 3
// to a source pixel. Each output pixel adds up the 5 source pixels around it.

#define TAPS 11
#define BEFORE 4
#define VALUES (TAPS * 3)
// Stored with 6 zeros before and more after, so each of the 5 source pixels gives a group of 3
// output pixels 9 values in a row, read as 16 for SIMD
#define PADDED 56
#define PAD 6
// White is 4080; about four pixels add up at an output pixel, which stays within 16 bits
#define ONE 4080
// The triangle's response at the carrier: sinc(1/3) squared
#define NOTCH 0.68389f

static int16_t ntscKernel[512][3][PADDED];
static int16_t palLuma[512][6][PADDED];
static int16_t palChroma[512][6][2][PADDED];
static const int16_t blank[PADDED];
static int built = -1;

// How much of the luma triangle (8 samples wide, area 1) lies before x
static float triangleArea(float x) {
	if (x <= -4) {
		return 0;
	}
	if (x >= 4) {
		return 1;
	}
	return x < 0 ? (x + 4) * (x + 4) / 32 : 1 - (4 - x) * (4 - x) / 32;
}

static bool on(int color, int phase) {
	return (color + phase) % 12 < 6;
}

// A pixel's part of the decoded picture, as luma and chroma RGB at its 11 output pixels
static void decode(int entry, int phase, bool pal, bool mirrored, bool svideo, float luma[VALUES], float chroma[VALUES]) {
	static const float low[4] = { 0.350f, 0.518f, 0.962f, 1.550f };
	static const float high[4] = { 1.094f, 1.506f, 1.962f, 1.962f };
	const float black = 0.518f, white = 1.962f, saturation = 1.15f;
	float hue = pal ? 4.5f : 4.25f;
	int samples = pal ? 10 : 8;
	int color = entry & 15, level = (entry >> 4) & 3, emphasis = entry >> 6;
	// PAL's PPU swaps the red and green emphasis bits
	if (pal) {
		emphasis = (emphasis & 4) | (emphasis & 1) << 1 | (emphasis & 2) >> 1;
	}
	// On mirrored lines a hue n is sent as 2 - n, which the TV's flipped V decodes as n
	int sent = mirrored && color >= 1 && color <= 12 ? (14 - color) % 12 : color;
	// The signal at each of the 12 phases; S-Video's luma is its average over the cycle
	float wave[12], mean = 0;
	for (int p = 0; p < 12; p++) {
		float v = color == 0 ? high[level] : color == 13 ? low[level] : color > 13 ? low[1] : on(sent, p) ? high[level] : low[level];
		static const int emphasisColor[3] = { 12, 4, 8 };
		for (int e = 0; e < 3; e++) {
			int ec = mirrored ? (14 - emphasisColor[e]) % 12 : emphasisColor[e];
			if (color < 14 && (emphasis & (1 << e)) && on(ec, p)) {
				v *= 0.746f;
				break;
			}
		}
		wave[p] = (v - black) / (white - black);
		mean += wave[p] / 12;
	}
	float signal[10];
	for (int i = 0; i < samples; i++) {
		signal[i] = wave[(phase + i) % 12];
	}
	for (int j = 0; j < TAPS; j++) {
		float center = (j - BEFORE + 0.5f) * samples / 3;
		float y = 0, i = 0, q = 0;
		float chromaWidth = 24;
		for (int k = 0; k < samples; k++) {
			float distance = fabsf(k + 0.5f - center);
			float s = signal[k];
			if (!svideo) {
				// Luma: the signal smoothed by a triangle 8 samples wide (to the TV's bandwidth),
				// less the carrier band rebuilt from two cycles around (the notch), scaled to what
				// the smoothing leaves of the carrier so a flat color cancels. Each sample holds its
				// level for its whole length, so the filter is summed across it.
				float a = k - center, b = a + 1;
				float from = fmaxf(a, -12), to = fminf(b, 12);
				float band = to > from ? NOTCH * (sinf(3.14159265f * to / 6) - sinf(3.14159265f * from / 6)) / (12 * 3.14159265f / 6) : 0;
				y += s * (triangleArea(b) - triangleArea(a) - band);
			} else if (distance < 3) {
				y += mean / 6;
			}
			if (distance < chromaWidth / 2) {
				float c = svideo ? s - mean : s;
				float angle = 3.14159265f * ((phase + k) % 12 + hue) / 6;
				i += c * cosf(angle) / chromaWidth;
				q += c * sinf(angle) / chromaWidth;
			}
		}
		i *= saturation;
		q *= mirrored ? -saturation : saturation;
		float rgb[3] = {
			0.946882f * i + 0.623557f * q,
			-0.274788f * i - 0.635691f * q,
			-1.108545f * i + 1.709007f * q,
		};
		for (int c = 0; c < 3; c++) {
			luma[j * 3 + c] = y;
			chroma[j * 3 + c] = rgb[c];
		}
	}
}

static void store(int16_t* out, const float* a, const float* b) {
	for (int v = 0; v < VALUES; v++) {
		out[PAD + v] = (int16_t) lrintf((a[v] + (b ? b[v] : 0)) * ONE);
	}
}

static void build(int mode) {
	bool svideo = mode == COMPOSITE_SVIDEO;
	float luma[VALUES], chroma[VALUES];
	for (int entry = 0; entry < 512; entry++) {
		for (int p = 0; p < 3; p++) {
			decode(entry, p * 4, false, false, svideo, luma, chroma);
			store(ntscKernel[entry][p], luma, chroma);
		}
		for (int p = 0; p < 6; p++) {
			for (int m = 0; m < 2; m++) {
				decode(entry, p * 2, true, m, svideo, luma, chroma);
				store(palChroma[entry][p][m], chroma, NULL);
			}
			store(palLuma[entry][p], luma, NULL);
		}
	}
	built = mode;
}

// A group of 3 output pixels: the 9 values from the source pixels 2 after it to 2 before it, and 7
// more that the next group overwrites
static inline void gather(const int16_t* const* k, int16_t* out) {
	for (int v = 0; v < 16; v++) {
		out[v] = (int16_t) (k[4][v] + k[3][9 + v] + k[2][18 + v] + k[1][27 + v] + k[0][36 + v]);
	}
}

// A line's values clamped to black and white, then packed into pixels
static void pack(int16_t* values, uint16_t* row) {
	for (int v = 0; v < COMPOSITE_WIDTH * 3; v++) {
		int16_t c = values[v];
		values[v] = c < 0 ? 0 : c > 4095 ? 4095 : c;
	}
	for (int x = 0; x < COMPOSITE_WIDTH; x++) {
		const int16_t* a = &values[x * 3];
		row[x] = (uint16_t) ((a[0] >> 7) << 11 | (a[1] >> 6) << 5 | a[2] >> 7);
	}
}

void compositeFrame(int mode, bool pal, unsigned frame, const uint16_t* in, uint16_t* out) {
	if (built != mode) {
		build(mode);
	}
	// Each source pixel's table, with 2 blank pixels at either end
	static const int16_t* luma[NES_WIDTH + 4];
	static const int16_t* chroma[NES_WIDTH + 4];
	static int16_t chromaLines[2][COMPOSITE_WIDTH * 3 + 16], values[COMPOSITE_WIDTH * 3 + 16];
	luma[0] = luma[1] = luma[NES_WIDTH + 2] = luma[NES_WIDTH + 3] = blank;
	chroma[0] = chroma[1] = chroma[NES_WIDTH + 2] = chroma[NES_WIDTH + 3] = blank;
	for (int line = 0; line < NES_HEIGHT; line++) {
		const uint16_t* pixels = &in[line * NES_WIDTH];
		uint16_t* row = &out[line * COMPOSITE_WIDTH];
		if (!pal) {
			// 341 dots of 8 samples move the carrier 4 samples a line; the frame's skipped dot every
			// other frame moves it back
			int phase = ((line + (frame & 1)) % 3) * 4;
			for (int x = 0; x < NES_WIDTH; x++) {
				luma[x + 2] = ntscKernel[pixels[x] & 0x1FF][phase / 4];
				phase = (phase + 8) % 12;
			}
			for (int x = 0; x < NES_WIDTH; x++) {
				gather(&luma[x], &values[x * 9]);
			}
			pack(values, row);
		} else {
			// 341 dots of 10 samples move it 2 samples a line
			int phase = (line * 2) % 12, mirrored = line & 1;
			for (int x = 0; x < NES_WIDTH; x++) {
				int entry = pixels[x] & 0x1FF;
				luma[x + 2] = palLuma[entry][phase / 2];
				chroma[x + 2] = palChroma[entry][phase / 2][mirrored];
				phase = (phase + 10) % 12;
			}
			int16_t* current = chromaLines[line & 1];
			const int16_t* previous = line ? chromaLines[(line & 1) ^ 1] : current;
			for (int x = 0; x < NES_WIDTH; x++) {
				gather(&chroma[x], &current[x * 9]);
			}
			for (int x = 0; x < NES_WIDTH; x++) {
				gather(&luma[x], &values[x * 9]);
			}
			for (int v = 0; v < COMPOSITE_WIDTH * 3; v++) {
				values[v] = (int16_t) (values[v] + ((current[v] + previous[v]) >> 1));
			}
			pack(values, row);
		}
	}
}
