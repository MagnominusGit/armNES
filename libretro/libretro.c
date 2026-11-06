/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "nes.h"
#include "options.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// The libretro core: one NES, its picture through an NTSC palette, its sound at 48 kHz, two pads.

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static struct NES nes;
static bool loaded;
static uint16_t palette565[512];
static uint16_t video[NES_WIDTH * NES_HEIGHT];
static int16_t audio[8192];

static struct OptionChoice regionChoices[] = {
	{ "Auto", -1 },
	{ "NTSC", REGION_NTSC },
	{ "PAL", REGION_PAL },
	{ "Dendy", REGION_DENDY },
};

static struct retro_core_option_v2_definition optionDefinitions[] = {
	{ "armnes_region", "Region", NULL, "The console the game runs on. Auto takes it from the game's header.", NULL, NULL,
		{ { "Auto", NULL }, { "NTSC", NULL }, { "PAL", NULL }, { "Dendy", NULL }, { NULL, NULL } }, "Auto" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_options_v2 options = { NULL, optionDefinitions };

// The NTSC signal's levels decoded to RGB (Bisqwit's method), with the emphasis bits dimming the
// other two channels
static void buildPalette(void) {
	static const float low[4] = { 0.350f, 0.518f, 0.962f, 1.550f };
	static const float high[4] = { 1.094f, 1.506f, 1.962f, 1.962f };
	const float black = 0.518f, white = 1.962f, hue = 4.25f, saturation = 1.15f;
	for (int entry = 0; entry < 512; entry++) {
		int color = entry & 15, level = (entry >> 4) & 3;
		float rgb[3] = { 0, 0, 0 };
		if (color < 14) {
			float y = 0, i = 0, q = 0;
			for (int p = 0; p < 12; p++) {
				bool on = (color + p) % 12 < 6;
				float v = color == 0 ? high[level] : color == 13 ? low[level] : on ? high[level] : low[level];
				v = (v - black) / (white - black);
				y += v / 12;
				i += v * cosf(3.14159265f * (p + hue) / 6) / 12;
				q += v * sinf(3.14159265f * (p + hue) / 6) / 12;
			}
			i *= saturation;
			q *= saturation;
			rgb[0] = y + 0.946882f * i + 0.623557f * q;
			rgb[1] = y - 0.274788f * i - 0.635691f * q;
			rgb[2] = y - 1.108545f * i + 1.709007f * q;
		}
		int emphasis = entry >> 6;
		if (emphasis) {
			for (int c = 0; c < 3; c++) {
				if (!(emphasis & (1 << c))) {
					rgb[c] *= 0.816f;
				}
			}
		}
		int r = (int) (31 * fminf(fmaxf(rgb[0], 0), 1) + 0.5f);
		int g = (int) (63 * fminf(fmaxf(rgb[1], 0), 1) + 0.5f);
		int b = (int) (31 * fminf(fmaxf(rgb[2], 0), 1) + 0.5f);
		palette565[entry] = r << 11 | g << 5 | b;
	}
}

void retro_set_environment(retro_environment_t cb) {
	environ_cb = cb;
	bool noGame = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
	optionsGive(cb, &options);
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
	video_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
	(void) cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
	audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb) {
	input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb) {
	input_state_cb = cb;
}

void retro_init(void) {
	buildPalette();
}

void retro_deinit(void) {
}

unsigned retro_api_version(void) {
	return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info* info) {
	memset(info, 0, sizeof(*info));
	info->library_name = "armNES";
	info->library_version = "0.1";
	info->valid_extensions = "nes";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = NES_WIDTH;
	info->geometry.base_height = NES_HEIGHT;
	info->geometry.max_width = NES_WIDTH;
	info->geometry.max_height = NES_HEIGHT;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = nes.region == REGION_NTSC ? 60.0988 : (nes.region == REGION_PAL ? 50.0070 : 50.0070);
	info->timing.sample_rate = 48000;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	(void) port;
	(void) device;
}

void retro_reset(void) {
	if (loaded) {
		nesReset(&nes, false);
	}
}

static uint8_t readPad(unsigned port) {
	// NES bits: A, B, Select, Start, Up, Down, Left, Right
	static const unsigned ids[8] = {
		RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
		RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN, RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
	};
	uint8_t pad = 0;
	for (int i = 0; i < 8; i++) {
		if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, ids[i])) {
			pad |= 1 << i;
		}
	}
	// A pad can't press opposite directions at once
	if ((pad & 0x30) == 0x30) {
		pad &= ~0x30;
	}
	if ((pad & 0xC0) == 0xC0) {
		pad &= ~0xC0;
	}
	return pad;
}

void retro_run(void) {
	input_poll_cb();
	nes.pads[0] = readPad(0);
	nes.pads[1] = readPad(1);
	nesRunFrame(&nes);
	for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) {
		video[i] = palette565[nes.frameBuffer[i] & 0x1FF];
	}
	video_cb(video, NES_WIDTH, NES_HEIGHT, NES_WIDTH * sizeof(uint16_t));
	if (nes.audioCount) {
		audio_batch_cb(nes.audioOut, nes.audioCount / 2);
	}
}

bool retro_load_game(const struct retro_game_info* game) {
	if (!game || !game->data) {
		return false;
	}
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		return false;
	}
	static const struct retro_input_descriptor descriptors[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
		{ 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ 0, 0, 0, 0, NULL },
	};
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*) descriptors);

	nesInit(&nes);
	nes.audioOut = audio;
	nes.audioCapacity = sizeof(audio) / sizeof(audio[0]);
	if (!nesLoad(&nes, game->data, game->size)) {
		if (nes.cart.mapper || nes.cart.prgSize) {
			struct retro_message message = { "armNES: this game's board isn't supported yet", 360 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		}
		return false;
	}
	int region = optionChoice(environ_cb, "armnes_region", regionChoices, 4, -1);
	if (region >= 0 && region != (int) nes.region) {
		nesSetRegion(&nes, region);
		nesReset(&nes, true);
	}
	loaded = true;
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num) {
	(void) type;
	(void) info;
	(void) num;
	return false;
}

void retro_unload_game(void) {
	if (loaded) {
		nesFree(&nes);
		loaded = false;
	}
}

unsigned retro_get_region(void) {
	return nes.region == REGION_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

size_t retro_serialize_size(void) {
	return loaded ? nesStateSize(&nes) : 0;
}

bool retro_serialize(void* data, size_t size) {
	return loaded && nesSaveState(&nes, data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	return loaded && nesLoadState(&nes, data, size);
}

void retro_cheat_reset(void) {
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	(void) index;
	(void) enabled;
	(void) code;
}

void* retro_get_memory_data(unsigned id) {
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return loaded && nes.cart.battery ? nes.cart.wram : NULL;
	case RETRO_MEMORY_SYSTEM_RAM:
		return nes.ram;
	default:
		return NULL;
	}
}

size_t retro_get_memory_size(unsigned id) {
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		return loaded && nes.cart.battery ? nes.cart.batterySize : 0;
	case RETRO_MEMORY_SYSTEM_RAM:
		return sizeof(nes.ram);
	default:
		return 0;
	}
}
