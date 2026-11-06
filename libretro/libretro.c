/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "board.h"
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

static struct OptionChoice jumperChoices[] = {
	{ "Auto", -1 },
	{ "0", 0 },
	{ "1", 1 },
	{ "2", 2 },
	{ "3", 3 },
	{ "4", 4 },
	{ "5", 5 },
	{ "6", 6 },
	{ "7", 7 },
	{ "8", 8 },
	{ "9", 9 },
	{ "10", 10 },
	{ "11", 11 },
	{ "12", 12 },
	{ "13", 13 },
	{ "14", 14 },
	{ "15", 15 },
	{ "16", 16 },
	{ "17", 17 },
	{ "18", 18 },
	{ "19", 19 },
	{ "20", 20 },
	{ "21", 21 },
	{ "22", 22 },
	{ "23", 23 },
};

enum { ASPECT_TV, ASPECT_PAR, ASPECT_SQUARE };

static struct OptionChoice aspectChoices[] = {
	{ "4:3", ASPECT_TV },
	{ "8:7 PAR", ASPECT_PAR },
	{ "1:1", ASPECT_SQUARE },
};

static struct OptionChoice cropChoices[] = {
	{ "0", 0 },
	{ "4", 4 },
	{ "8", 8 },
	{ "12", 12 },
	{ "16", 16 },
};

static struct retro_core_option_v2_definition optionDefinitions[] = {
	{ "armnes_region", "Region", NULL, "The console the game runs on. Auto takes it from the game's header.", NULL, NULL,
		{ { "Auto", NULL }, { "NTSC", NULL }, { "PAL", NULL }, { "Dendy", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_aspect", "Aspect Ratio", NULL,
		"4:3 is the picture as a TV showed it. 8:7 PAR keeps the console's pixel shape (8:7 on NTSC, about "
		"11:8 on PAL). 1:1 shows square pixels.",
		NULL, NULL, { { "4:3", NULL }, { "8:7 PAR", NULL }, { "1:1", NULL }, { NULL, NULL } }, "4:3" },
	{ "armnes_crop_vertical", "Crop Top and Bottom", NULL,
		"Pixels hidden at the top and at the bottom, as a TV's overscan hid them.",
		NULL, NULL, { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "0" },
	{ "armnes_crop_horizontal", "Crop Sides", NULL,
		"Pixels hidden at the left and at the right edge.",
		NULL, NULL, { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "0" },
	{ "armnes_jumper", "Cart Jumper", NULL,
		"Some carts have solder pads or DIP switches that pick their menu, the games listed, a language or "
		"the video timing. Auto uses the setting known for the dump, or 0. Changing it restarts the game, "
		"as it would on the cart. Other carts ignore it.",
		NULL, NULL, { { "Auto", NULL }, { "0", NULL }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", NULL }, { "11", NULL }, { "12", NULL }, { "13", NULL }, { "14", NULL }, { "15", NULL }, { "16", NULL }, { "17", NULL }, { "18", NULL }, { "19", NULL }, { "20", NULL }, { "21", NULL }, { "22", NULL }, { "23", NULL }, { NULL, NULL } }, "Auto" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_options_v2 options = { NULL, optionDefinitions };

static void checkOptions(void);

// The picture: its shape and how much of the edges is cropped
static int aspect, cropVertical, cropHorizontal;

static void readVideoOptions(void) {
	aspect = optionChoice(environ_cb, "armnes_aspect", aspectChoices, 3, ASPECT_TV);
	cropVertical = optionChoice(environ_cb, "armnes_crop_vertical", cropChoices, 5, 0);
	cropHorizontal = optionChoice(environ_cb, "armnes_crop_horizontal", cropChoices, 5, 0);
}

static void geometry(struct retro_game_geometry* g) {
	unsigned width = NES_WIDTH - 2 * cropHorizontal, height = NES_HEIGHT - 2 * cropVertical;
	// NTSC pixels are 8:7; PAL's are 2950000:2128137
	float pixel = nes.region == REGION_NTSC ? 8.0f / 7.0f : 2950000.0f / 2128137.0f;
	g->base_width = width;
	g->base_height = height;
	g->max_width = NES_WIDTH;
	g->max_height = NES_HEIGHT;
	switch (aspect) {
	case ASPECT_PAR:
		g->aspect_ratio = width * pixel / height;
		break;
	case ASPECT_SQUARE:
		g->aspect_ratio = (float) width / height;
		break;
	default:
		// The TV's 4:3 for the whole picture, cropped in proportion
		g->aspect_ratio = (4.0f / 3.0f) * (width / (float) NES_WIDTH) / (height / (float) NES_HEIGHT);
		break;
	}
}

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
	info->valid_extensions = "nes|fds|unf|unif";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	geometry(&info->geometry);
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
	if (nes.board == &boardYuxing) {
		memset(nes.keys, 0, sizeof(nes.keys));
		for (unsigned key = 1; key < RETROK_LAST; key++) {
			if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, key)) {
				nes.keys[key >> 3] |= 1 << (key & 7);
			}
		}
	}
	nesRunFrame(&nes);
	checkOptions();
	for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) {
		video[i] = palette565[nes.frameBuffer[i] & 0x1FF];
	}
	video_cb(video + cropVertical * NES_WIDTH + cropHorizontal, NES_WIDTH - 2 * cropHorizontal,
		NES_HEIGHT - 2 * cropVertical, NES_WIDTH * sizeof(uint16_t));
	if (nes.audioCount) {
		audio_batch_cb(nes.audioOut, nes.audioCount / 2);
	}
}

// The Disk System's BIOS, from the frontend's system folder; some dumps carry a 16-byte header
static uint8_t fdsBios[0x2000];

static bool loadFdsBios(void) {
	const char* dir = NULL;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir) {
		return false;
	}
	static const char* names[] = { "disksys.rom", "Disksys.rom", "DISKSYS.ROM" };
	for (int i = 0; i < 3; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		FILE* f = fopen(path, "rb");
		if (!f) {
			continue;
		}
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		bool ok = size >= 0x2000 && fseek(f, size - 0x2000, SEEK_SET) == 0 && fread(fdsBios, 1, 0x2000, f) == 0x2000;
		fclose(f);
		if (ok) {
			return true;
		}
	}
	return false;
}

static bool isDisk(const uint8_t* data, size_t size) {
	return (size >= 4 && !memcmp(data, "FDS\x1A", 4)) || (size >= 15 && data[0] == 1 && !memcmp(data + 1, "*NINTENDO-HVC*", 14));
}

// Disk sides for the frontend's disk menu; the core also turns to the side the BIOS asks for
static unsigned diskIndex;

static bool setEjectState(bool ejected) {
	if (ejected) {
		int side = fdsSide(&nes);
		diskIndex = side >= 0 ? (unsigned) side : diskIndex;
	}
	fdsInsert(&nes, ejected ? -1 : (int) diskIndex);
	return true;
}

static bool getEjectState(void) {
	return fdsSide(&nes) < 0;
}

static unsigned getImageIndex(void) {
	int side = fdsSide(&nes);
	return side >= 0 ? (unsigned) side : diskIndex;
}

static bool setImageIndex(unsigned index) {
	if (index >= (unsigned) nes.cart.diskSides) {
		return false;
	}
	diskIndex = index;
	if (fdsSide(&nes) >= 0) {
		fdsInsert(&nes, (int) index);
	}
	return true;
}

static unsigned getNumImages(void) {
	return nes.cart.diskSides;
}

static struct retro_disk_control_callback diskControl = {
	setEjectState, getEjectState, getImageIndex, setImageIndex, getNumImages, NULL, NULL,
};

// Options changed while running: the picture's shape at once; a cart's solder pads or DIP switches
// restart the game. Carts with them say so once the game looks at them.
static bool jumperNoticed;

static void checkOptions(void) {
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		int oldAspect = aspect, oldVertical = cropVertical, oldHorizontal = cropHorizontal;
		readVideoOptions();
		if (aspect != oldAspect || cropVertical != oldVertical || cropHorizontal != oldHorizontal) {
			struct retro_game_geometry g;
			geometry(&g);
			environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
		}
		int jumper = optionChoice(environ_cb, "armnes_jumper", jumperChoices, 25, -1);
		if (jumper != nes.jumper) {
			nes.jumper = jumper;
			if (nes.hasJumper) {
				nesReset(&nes, true);
			}
		}
	}
	if (nes.hasJumper && !jumperNoticed) {
		jumperNoticed = true;
		char text[200];
		snprintf(text, sizeof(text), "This cart has solder pads or DIP switches, set to %d%s. Core Options > Cart Jumper changes them.",
			jumperSetting(&nes), nes.jumper < 0 && nes.cart.jumper >= 0 ? " for this dump" : "");
		struct retro_message message = { text, 480 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
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
	if (isDisk(game->data, game->size)) {
		if (!loadFdsBios()) {
			struct retro_message message = { "armNES: Disk System games need disksys.rom in the system folder", 360 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
			return false;
		}
		nes.fdsBios = fdsBios;
		diskIndex = 0;
		environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &diskControl);
	}
	nes.jumper = optionChoice(environ_cb, "armnes_jumper", jumperChoices, 25, -1);
	jumperNoticed = false;
	if (!nesLoad(&nes, game->data, game->size)) {
		if (nes.cart.mapper || nes.cart.prgSize) {
			struct retro_message message = { "armNES: this game's board isn't supported yet", 360 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		}
		return false;
	}
	readVideoOptions();
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
		// A disk's save is the disk
		if (loaded && nes.cart.disk) {
			return nes.cart.disk;
		}
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
		if (loaded && nes.cart.disk) {
			return (size_t) nes.cart.diskSides * nes.cart.diskSideSize;
		}
		return loaded && nes.cart.battery ? nes.cart.batterySize : 0;
	case RETRO_MEMORY_SYSTEM_RAM:
		return sizeof(nes.ram);
	default:
		return 0;
	}
}
