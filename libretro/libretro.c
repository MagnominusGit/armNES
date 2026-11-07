/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "board.h"
#include "composite.h"
#include "options.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
static uint16_t video[COMPOSITE_WIDTH * NES_HEIGHT];
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

#define DEVICE_ID_ZAPPER RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_LIGHTGUN, 0)
#define DEVICE_ID_PADDLE RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_MOUSE, 0)
#define DEVICE_ID_POWER_PAD RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0)

// Overclocking: frames' worth of extra CPU time, negative for after the picture instead of vblank
static struct OptionChoice overclockChoices[] = {
	{ "Off", 0 },
	{ "2x (vblank)", 1 },
	{ "3x (vblank)", 2 },
	{ "4x (vblank)", 3 },
	{ "2x (after the picture)", -1 },
	{ "3x (after the picture)", -2 },
};

// Turbo: frames per press and release
static struct OptionChoice turboChoices[] = {
	{ "Off", 0 },
	{ "30 per second", 2 },
	{ "20 per second", 3 },
	{ "15 per second", 4 },
	{ "10 per second", 6 },
	{ "7.5 per second", 8 },
};

static struct OptionChoice onOffChoices[] = {
	{ "Off", 0 },
	{ "On", 1 },
};

static struct OptionChoice fourPlayerChoices[] = {
	{ "Off", 0 },
	{ "Four Score", 1 },
	{ "Famicom adapter", 2 },
};

static struct OptionChoice expansionChoices[] = {
	{ "None", EXPANSION_NONE },
	{ "Paddle", EXPANSION_PADDLE },
	{ "Power Pad", EXPANSION_MAT },
	{ "Keyboard", EXPANSION_KEYBOARD },
};

enum { AIM_LIGHT_GUN, AIM_MOUSE, AIM_STICK, AIM_STICK_PAN };

static struct OptionChoice aimChoices[] = {
	{ "Light gun", AIM_LIGHT_GUN },
	{ "Mouse", AIM_MOUSE },
	{ "Right stick (absolute)", AIM_STICK },
	{ "Right stick (pan)", AIM_STICK_PAN },
};

static struct OptionChoice filterChoices[] = {
	{ "Off", COMPOSITE_OFF },
	{ "Composite", COMPOSITE },
	{ "S-Video", COMPOSITE_SVIDEO },
};

#define VOLUME_VALUES { { "0%", NULL }, { "10%", NULL }, { "20%", NULL }, { "30%", NULL }, { "40%", NULL }, { "50%", NULL }, \
	{ "60%", NULL }, { "70%", NULL }, { "80%", NULL }, { "90%", NULL }, { "100%", NULL }, { NULL, NULL } }
#define PAN_VALUES { { "Left", NULL }, { "75% left", NULL }, { "50% left", NULL }, { "25% left", NULL }, { "Center", NULL }, \
	{ "25% right", NULL }, { "50% right", NULL }, { "75% right", NULL }, { "Right", NULL }, { NULL, NULL } }

static struct OptionChoice volumeChoices[] = {
	{ "0%", 0 }, { "10%", 10 }, { "20%", 20 }, { "30%", 30 }, { "40%", 40 }, { "50%", 50 },
	{ "60%", 60 }, { "70%", 70 }, { "80%", 80 }, { "90%", 90 }, { "100%", 100 },
};

static struct OptionChoice panChoices[] = {
	{ "Left", -100 }, { "75% left", -75 }, { "50% left", -50 }, { "25% left", -25 }, { "Center", 0 },
	{ "25% right", 25 }, { "50% right", 50 }, { "75% right", 75 }, { "Right", 100 },
};

static struct OptionChoice spriteChoices[] = {
	{ "8 per line", 0 },
	{ "No limit", 1 },
};

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
	{ "armnes_palette", "Palette", NULL,
		"The colors. armNES NTSC decodes the console's video signal; the others are measured or tuned "
		"palettes; Custom loads nes.pal (or custom.pal) from the system folder.",
		NULL, NULL, { { NULL, NULL } }, "armNES NTSC" },
	{ "armnes_filter", "NTSC/PAL Filter", NULL,
		"The picture as a TV decodes the console's video signal, NTSC or PAL by the region: Composite "
		"with its color fringes and the blending some games' dithering relied on, S-Video sharper. It "
		"makes its own colors, so the palette doesn't apply. Takes more CPU time.",
		NULL, NULL, { { "Off", NULL }, { "Composite", NULL }, { "S-Video", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_fds_fast", "Disk System Fast Loading", NULL,
		"Runs the Disk System up to 16 times as fast while the drive reads or writes the disk, showing only "
		"every 16th frame and dropping its sound.",
		NULL, NULL, { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
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
	{ "armnes_overclock", "Overclock", NULL,
		"Gives the CPU extra time each frame, which removes most slowdown. The picture, sound and game "
		"speed stay as they are. Not accurate: a few games misbehave, mostly with the extra time in "
		"vblank; try the other placement for those.",
		NULL, NULL, { { "Off", NULL }, { "2x (vblank)", NULL }, { "3x (vblank)", NULL }, { "4x (vblank)", NULL },
		{ "2x (after the picture)", NULL }, { "3x (after the picture)", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_sprite_limit", "Sprite Limit", NULL,
		"The console shows 8 sprites on a line and drops the rest, which games turn into flicker. No limit "
		"shows them all; games still see the console's limit.",
		NULL, NULL, { { "8 per line", NULL }, { "No limit", NULL }, { NULL, NULL } }, "8 per line" },
	{ "armnes_turbo", "Turbo Speed", NULL,
		"How fast the turbo buttons (X for A, Y for B) press and release.",
		NULL, NULL, { { "Off", NULL }, { "30 per second", NULL }, { "20 per second", NULL }, { "15 per second", NULL },
		{ "10 per second", NULL }, { "7.5 per second", NULL }, { NULL, NULL } }, "15 per second" },
	{ "armnes_opposing", "Allow Opposing Directions", NULL,
		"Lets left and right (or up and down) be held together, which a pad can't do. A few games glitch "
		"with it; some speedrun tricks use it.",
		NULL, NULL, { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_four_players", "4 Players", NULL,
		"Players 3 and 4 on controllers 3 and 4, through the NES Four Score or the Famicom's adapter on "
		"the expansion port. Games read one or the other.",
		NULL, NULL, { { "Off", NULL }, { "Four Score", NULL }, { "Famicom adapter", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_expansion", "Famicom Expansion Port", NULL,
		"A device in the Famicom's expansion port. The paddle follows the mouse; the Power Pad (the "
		"Famicom's, read by rows) takes controller 1's buttons; the keyboard takes the keyboard (turn on "
		"Game Focus).",
		NULL, NULL, { { "None", NULL }, { "Paddle", NULL }, { "Power Pad", NULL },
		{ "Keyboard", NULL }, { NULL, NULL } }, "None" },
	{ "armnes_zapper_aim", "Zapper Aim", NULL,
		"How the Zapper aims. Light gun points where the light gun (or the mouse pointer) is. Mouse moves "
		"a crosshair; the left button fires. Right stick (absolute) puts the crosshair where the stick "
		"points; Right stick (pan) moves it. With the stick, controller 1's R or R2 fires. Touching the "
		"screen aims and fires there with any of them.",
		NULL, NULL, { { "Light gun", NULL }, { "Mouse", NULL }, { "Right stick (absolute)", NULL },
		{ "Right stick (pan)", NULL }, { NULL, NULL } }, "Light gun" },
	{ "armnes_jumper", "Cart Jumper", NULL,
		"Some carts have solder pads or DIP switches that pick their menu, the games listed, a language or "
		"the video timing. Auto uses the setting known for the dump, or 0. Changing it restarts the game, "
		"as it would on the cart. Other carts ignore it.",
		NULL, NULL, { { "Auto", NULL }, { "0", NULL }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", NULL }, { "11", NULL }, { "12", NULL }, { "13", NULL }, { "14", NULL }, { "15", NULL }, { "16", NULL }, { "17", NULL }, { "18", NULL }, { "19", NULL }, { "20", NULL }, { "21", NULL }, { "22", NULL }, { "23", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_volume_pulse1", "Sound > Pulse 1 Volume", "Pulse 1 Volume", NULL, NULL, "sound", VOLUME_VALUES, "100%" },
	{ "armnes_pan_pulse1", "Sound > Pulse 1 Position", "Pulse 1 Position", NULL, NULL, "sound", PAN_VALUES, "Center" },
	{ "armnes_volume_pulse2", "Sound > Pulse 2 Volume", "Pulse 2 Volume", NULL, NULL, "sound", VOLUME_VALUES, "100%" },
	{ "armnes_pan_pulse2", "Sound > Pulse 2 Position", "Pulse 2 Position", NULL, NULL, "sound", PAN_VALUES, "Center" },
	{ "armnes_volume_triangle", "Sound > Triangle Volume", "Triangle Volume", NULL, NULL, "sound", VOLUME_VALUES, "100%" },
	{ "armnes_pan_triangle", "Sound > Triangle Position", "Triangle Position", NULL, NULL, "sound", PAN_VALUES, "Center" },
	{ "armnes_volume_noise", "Sound > Noise Volume", "Noise Volume", NULL, NULL, "sound", VOLUME_VALUES, "100%" },
	{ "armnes_pan_noise", "Sound > Noise Position", "Noise Position", NULL, NULL, "sound", PAN_VALUES, "Center" },
	{ "armnes_volume_dmc", "Sound > DMC Volume", "DMC Volume", NULL, NULL, "sound", VOLUME_VALUES, "100%" },
	{ "armnes_pan_dmc", "Sound > DMC Position", "DMC Position", NULL, NULL, "sound", PAN_VALUES, "Center" },
	{ "armnes_volume_cart", "Sound > Cart Sound Volume", "Cart Sound Volume", NULL, NULL, "sound", VOLUME_VALUES, "100%" },
	{ "armnes_pan_cart", "Sound > Cart Sound Position", "Cart Sound Position", NULL, NULL, "sound", PAN_VALUES, "Center" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_option_v2_category categories[] = {
	{ "sound", "Sound", "Each channel's volume and where it sits between the speakers. The console mixes in mono." },
	{ NULL, NULL, NULL },
};

static struct retro_core_options_v2 options = { categories, optionDefinitions };

static void checkOptions(void);

// The picture: its shape and how much of the edges is cropped
static int aspect, cropVertical, cropHorizontal, filter;

// Applied at once: the line count follows the region (262 lines NTSC, 312 PAL and Dendy)
static void readSpeedOptions(void) {
	int overclock = optionChoice(environ_cb, "armnes_overclock", overclockChoices, 6, 0);
	int frames = overclock < 0 ? -overclock : overclock;
	nes.overclockLines = frames * (nes.region == REGION_NTSC ? 262 : 312);
	nes.overclockPostRender = overclock < 0;
	nes.noSpriteLimit = optionChoice(environ_cb, "armnes_sprite_limit", spriteChoices, 2, 0);
}

// A side's gain falls as the channel moves to the other side
static void readSoundOptions(void) {
	static const char* keys[MIX_CHANNELS] = { "pulse1", "pulse2", "triangle", "noise", "dmc", "cart" };
	nes.mixer.custom = false;
	for (int c = 0; c < MIX_CHANNELS; c++) {
		char key[32];
		snprintf(key, sizeof(key), "armnes_volume_%s", keys[c]);
		int volume = optionChoice(environ_cb, key, volumeChoices, 11, 100);
		snprintf(key, sizeof(key), "armnes_pan_%s", keys[c]);
		int pan = optionChoice(environ_cb, key, panChoices, 9, 0);
		nes.mixer.gain[c][0] = volume / 100.0f * (pan > 0 ? 1 - pan / 100.0f : 1);
		nes.mixer.gain[c][1] = volume / 100.0f * (pan < 0 ? 1 + pan / 100.0f : 1);
		nes.mixer.custom |= volume != 100 || pan != 0;
	}
	nes.apu.stereo = nes.mixer.custom;
	nes.apu.mixDirty = true;
}

static void readVideoOptions(void) {
	aspect = optionChoice(environ_cb, "armnes_aspect", aspectChoices, 3, ASPECT_TV);
	cropVertical = optionChoice(environ_cb, "armnes_crop_vertical", cropChoices, 5, 0);
	cropHorizontal = optionChoice(environ_cb, "armnes_crop_horizontal", cropChoices, 5, 0);
	filter = optionChoice(environ_cb, "armnes_filter", filterChoices, 3, COMPOSITE_OFF);
}

static void geometry(struct retro_game_geometry* g) {
	unsigned width = NES_WIDTH - 2 * cropHorizontal, height = NES_HEIGHT - 2 * cropVertical;
	// NTSC pixels are 8:7; PAL's are 2950000:2128137
	float pixel = nes.region == REGION_NTSC ? 8.0f / 7.0f : 2950000.0f / 2128137.0f;
	g->base_width = filter ? width * COMPOSITE_SCALE : width;
	g->base_height = height;
	g->max_width = COMPOSITE_WIDTH;
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

// Palettes from FCEUmm's set, and one loaded from the system folder (64 colors, or 512 with the
// emphasis combinations)
static const struct {
	const char* name;
	uint32_t rgb[64];
} palettes[] = {
#include "palettes.inc"
};

enum { PALETTE_NTSC = 0, PALETTE_CUSTOM = 1 + sizeof(palettes) / sizeof(palettes[0]) };

static uint32_t customPalette[512];
static int customColors;

static bool loadCustomPalette(void) {
	const char* dir = NULL;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) || !dir) {
		return false;
	}
	static const char* names[] = { "nes.pal", "custom.pal" };
	for (int i = 0; i < 2; i++) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		FILE* f = fopen(path, "rb");
		if (!f) {
			continue;
		}
		uint8_t data[1536];
		size_t size = fread(data, 1, sizeof(data), f);
		fclose(f);
		if (size != 192 && size != 1536) {
			continue;
		}
		customColors = (int) size / 3;
		for (int c = 0; c < customColors; c++) {
			customPalette[c] = data[c * 3] << 16 | data[c * 3 + 1] << 8 | data[c * 3 + 2];
		}
		return true;
	}
	return false;
}

// The NTSC signal's levels decoded to RGB (Bisqwit's method), or a palette's colors, with the
// emphasis bits dimming the other two channels (red and green swapped on PAL and Dendy)
static void buildPalette(int which) {
	static const float low[4] = { 0.350f, 0.518f, 0.962f, 1.550f };
	static const float high[4] = { 1.094f, 1.506f, 1.962f, 1.962f };
	const float black = 0.518f, white = 1.962f, hue = 4.25f, saturation = 1.15f;
	for (int entry = 0; entry < 512; entry++) {
		int color = entry & 15, level = (entry >> 4) & 3;
		float rgb[3] = { 0, 0, 0 };
		const uint32_t* table = which > PALETTE_NTSC && which < PALETTE_CUSTOM ? palettes[which - 1].rgb : NULL;
		if (which == PALETTE_CUSTOM && customColors) {
			table = customPalette;
		}
		if (table) {
			uint32_t c = table[customColors == 512 && table == customPalette ? entry : entry & 0x3F];
			rgb[0] = (c >> 16) / 255.0f;
			rgb[1] = ((c >> 8) & 0xFF) / 255.0f;
			rgb[2] = (c & 0xFF) / 255.0f;
		} else if (color < 14) {
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
		if (nes.region != REGION_NTSC) {
			emphasis = (emphasis & 4) | (emphasis & 1) << 1 | (emphasis & 2) >> 1;
		}
		if (emphasis && !(table == customPalette && customColors == 512)) {
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

static struct OptionChoice paletteChoices[PALETTE_CUSTOM + 1];

static int palette = -1, paletteRegion = -1;

static void readPaletteOption(void) {
	int which = optionChoice(environ_cb, "armnes_palette", paletteChoices, PALETTE_CUSTOM + 1, PALETTE_NTSC);
	if (which == PALETTE_CUSTOM && !loadCustomPalette()) {
		struct retro_message message = { "armNES: no nes.pal or custom.pal (192 or 1536 bytes) in the system folder", 300 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		which = PALETTE_NTSC;
	}
	if (which != palette || (int) nes.region != paletteRegion) {
		palette = which;
		paletteRegion = nes.region;
		buildPalette(which);
	}
}

void retro_set_environment(retro_environment_t cb) {
	environ_cb = cb;
	// The palette option's values: ours, the table's, and the system folder's
	for (struct retro_core_option_v2_definition* d = optionDefinitions; d->key; d++) {
		if (!strcmp(d->key, "armnes_palette")) {
			paletteChoices[0] = (struct OptionChoice) { "armNES NTSC", PALETTE_NTSC };
			for (int i = 1; i < PALETTE_CUSTOM; i++) {
				paletteChoices[i] = (struct OptionChoice) { palettes[i - 1].name, i };
			}
			paletteChoices[PALETTE_CUSTOM] = (struct OptionChoice) { "Custom", PALETTE_CUSTOM };
			for (int i = 0; i <= PALETTE_CUSTOM; i++) {
				d->values[i].value = paletteChoices[i].value;
				d->values[i].label = NULL;
			}
			d->values[PALETTE_CUSTOM + 1].value = NULL;
		}
	}
	bool noGame = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
	static const struct retro_controller_description ports[] = {
		{ "Controller", RETRO_DEVICE_JOYPAD },
		{ "Zapper", DEVICE_ID_ZAPPER },
		{ "Paddle", DEVICE_ID_PADDLE },
		{ "Power Pad", DEVICE_ID_POWER_PAD },
		{ "None", RETRO_DEVICE_NONE },
	};
	static const struct retro_controller_description pads[] = {
		{ "Controller", RETRO_DEVICE_JOYPAD },
	};
	static const struct retro_controller_info info[] = {
		{ ports, 5 }, { ports, 5 }, { pads, 1 }, { pads, 1 }, { NULL, 0 },
	};
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*) info);
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
	buildPalette(PALETTE_NTSC);
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
	info->valid_extensions = "nes|fds|unf|unif|nsf";
	info->need_fullpath = false;
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	geometry(&info->geometry);
	info->timing.fps = nes.region == REGION_NTSC ? 60.0988 : (nes.region == REGION_PAL ? 50.0070 : 50.0070);
	info->timing.sample_rate = 48000;
}

static uint8_t portDevice[2];

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (port < 2) {
		switch (device) {
		case DEVICE_ID_ZAPPER: portDevice[port] = DEVICE_ZAPPER; break;
		case DEVICE_ID_PADDLE: portDevice[port] = DEVICE_PADDLE; break;
		case DEVICE_ID_POWER_PAD: portDevice[port] = DEVICE_POWER_PAD; break;
		case RETRO_DEVICE_NONE: portDevice[port] = DEVICE_NONE; break;
		default: portDevice[port] = DEVICE_PAD; break;
		}
	}
}

void retro_reset(void) {
	if (loaded) {
		nesReset(&nes, false);
	}
}

static int turboPeriod = 4;
static bool allowOpposing;
static unsigned turboFrame;
static int fourPlayers, expansion, zapperAim;
static bool fdsFast;

static void readInputOptions(void) {
	turboPeriod = optionChoice(environ_cb, "armnes_turbo", turboChoices, 6, 4);
	allowOpposing = optionChoice(environ_cb, "armnes_opposing", onOffChoices, 2, 0);
	fourPlayers = optionChoice(environ_cb, "armnes_four_players", fourPlayerChoices, 3, 0);
	zapperAim = optionChoice(environ_cb, "armnes_zapper_aim", aimChoices, 4, AIM_LIGHT_GUN);
	fdsFast = optionChoice(environ_cb, "armnes_fds_fast", onOffChoices, 2, 0);
	expansion = optionChoice(environ_cb, "armnes_expansion", expansionChoices, 4, EXPANSION_NONE);
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
	// Turbo A and B: held for the first half of each period
	if (turboPeriod && turboFrame % (unsigned) turboPeriod < (unsigned) turboPeriod / 2) {
		if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X)) {
			pad |= 0x01;
		}
		if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y)) {
			pad |= 0x02;
		}
	}
	// A pad can't press opposite directions at once
	if (!allowOpposing) {
		if ((pad & 0x30) == 0x30) {
			pad &= ~0x30;
		}
		if ((pad & 0xC0) == 0xC0) {
			pad &= ~0xC0;
		}
	}
	return pad;
}

// The Power Pads' 12 buttons, 1 to 12, from a controller's
static const unsigned matButtons[12] = {
	RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A,
	RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_RIGHT, RETRO_DEVICE_ID_JOYPAD_DOWN,
	RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
};

static uint16_t readMat(unsigned port) {
	uint16_t buttons = 0;
	for (int i = 0; i < 12; i++) {
		if (input_state_cb(port, RETRO_DEVICE_JOYPAD, 0, matButtons[i])) {
			buttons |= 1 << i;
		}
	}
	return buttons;
}

static int touchX, touchY, touchFrames;

// The crosshair the mouse and stick move, in 256ths of a pixel, and whether it's drawn
static int crossX = 128 << 8, crossY = 120 << 8;
static bool crossShown;

// Aim from the mouse or the stick: false for the light gun
static bool aimCrosshair(unsigned port, bool* trigger, bool* away) {
	int left = cropHorizontal << 8, right = (NES_WIDTH - 1 - cropHorizontal) << 8;
	int top = cropVertical << 8, bottom = (NES_HEIGHT - 1 - cropVertical) << 8;
	switch (zapperAim) {
	case AIM_MOUSE:
		// Half a pixel a count, from this port's mouse and the first one
		for (unsigned p = 0; p <= port; p += port ? port : 1) {
			crossX += input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X) * 128;
			crossY += input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y) * 128;
			*trigger |= input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
			*away |= input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
		}
		break;
	case AIM_STICK:
	case AIM_STICK_PAN: {
		int x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
		int y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
		if (zapperAim == AIM_STICK) {
			crossX = left + (x + 0x8000) * ((right - left) >> 8) / 0xFFFF * 256;
			crossY = top + (y + 0x8000) * ((bottom - top) >> 8) / 0xFFFF * 256;
		} else {
			// Up to 4 pixels a frame, past a small dead zone
			crossX += abs(x) > 0x1000 ? x / 32 : 0;
			crossY += abs(y) > 0x1000 ? y / 32 : 0;
		}
		*trigger |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R) ||
			input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
		break;
	}
	default:
		return false;
	}
	crossX = crossX < left ? left : crossX > right ? right : crossX;
	crossY = crossY < top ? top : crossY > bottom ? bottom : crossY;
	return true;
}

static void drawCrosshair(int scale) {
	int cx = crossX >> 8, cy = crossY >> 8;
	for (int pass = 0; pass < 2; pass++) {
		for (int d = -4; d <= 4; d++) {
			for (int w = -1; w <= 1; w++) {
				if (pass && (w || d == 0 || abs(d) == 4)) {
					continue;
				}
				int points[2][2] = { { cx + d, cy + w }, { cx + w, cy + d } };
				for (int i = 0; i < 2; i++) {
					int x = points[i][0], y = points[i][1];
					if (x >= 0 && x < NES_WIDTH && y >= 0 && y < NES_HEIGHT) {
						for (int s = 0; s < scale; s++) {
							video[(y * NES_WIDTH + x) * scale + s] = pass ? 0xFFFF : 0x0000;
						}
					}
				}
			}
		}
	}
}

static void readDevices(void) {
	struct InputConfig* in = &nes.input;
	crossShown = false;
	in->device[0] = portDevice[0];
	in->device[1] = portDevice[1];
	in->fourScore = fourPlayers == 1;
	in->expansion = fourPlayers == 2 ? EXPANSION_HORI : (uint8_t) expansion;
	for (unsigned port = 0; port < 2; port++) {
		nes.pads[port] = in->device[port] == DEVICE_PAD ? readPad(port) : 0;
		in->pads[port] = fourPlayers ? readPad(port + 2) : 0;
	}
	in->mic = in->device[1] == DEVICE_PAD && input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
	in->powerPad = 0;
	for (unsigned port = 0; port < 2; port++) {
		switch (in->device[port]) {
		case DEVICE_ZAPPER: {
			// The aim on the shown picture; off it, or with the reload button, at nothing. A touch aims
			// and fires there, and the aim stays a moment after it, while the game looks for the light.
			int x = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X);
			int y = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y);
			bool away = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_IS_OFFSCREEN) ||
				input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD);
			in->zapperTrigger = input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_TRIGGER) ||
				input_state_cb(port, RETRO_DEVICE_LIGHTGUN, 0, RETRO_DEVICE_ID_LIGHTGUN_RELOAD);
			bool trigger = false, offscreen = false;
			crossShown = aimCrosshair(port, &trigger, &offscreen);
			if (crossShown) {
				in->zapperTrigger = trigger || offscreen;
				away = offscreen;
			}
			if (input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED)) {
				touchX = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
				touchY = input_state_cb(port, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
				touchFrames = 30;
				in->zapperTrigger = true;
			}
			int width = NES_WIDTH - 2 * cropHorizontal, height = NES_HEIGHT - 2 * cropVertical;
			if (touchFrames) {
				touchFrames--;
				crossShown = false;
				in->zapperX = (int16_t) (cropHorizontal + (touchX + 0x7FFF) * width / 0xFFFF);
				in->zapperY = (int16_t) (cropVertical + (touchY + 0x7FFF) * height / 0xFFFF);
			} else if (crossShown) {
				in->zapperX = away ? -1 : (int16_t) (crossX >> 8);
				in->zapperY = away ? -1 : (int16_t) (crossY >> 8);
			} else {
				in->zapperX = away ? -1 : (int16_t) (cropHorizontal + (x + 0x7FFF) * width / 0xFFFF);
				in->zapperY = away ? -1 : (int16_t) (cropVertical + (y + 0x7FFF) * height / 0xFFFF);
			}
			break;
		}
		case DEVICE_POWER_PAD:
			in->powerPad = readMat(port);
			break;
		}
	}
	// The paddle's knob follows the mouse across its range
	unsigned mousePort = in->device[0] == DEVICE_PADDLE ? 0 : in->device[1] == DEVICE_PADDLE ? 1 :
		in->expansion == EXPANSION_PADDLE ? 0 : 4;
	if (mousePort < 4) {
		int position = nes.inputState.paddle + input_state_cb(mousePort, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
		nes.inputState.paddle = (uint8_t) (position < 0 ? 0 : position > 255 ? 255 : position);
		in->paddleButton = input_state_cb(mousePort, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
	}
	if (in->expansion == EXPANSION_MAT) {
		in->powerPad = readMat(0);
	}
}

// NSF files: the tune's names and which song plays, picked with the pad
static const struct {
	uint8_t width;
	uint16_t rows[12];
} font[] = {
#include "font.inc"
};

static bool playingNsf;
static unsigned songFrames;
static uint8_t lastNsfPad;

static int textWidth(const char* text) {
	int width = 0;
	for (; *text; text++) {
		int c = (unsigned char) *text;
		width += font[c >= 32 && c < 127 ? c - 32 : '?' - 32].width + 1;
	}
	return width;
}

// A line centered at y; characters outside ASCII show as ?
static void drawText(int y, const char* text, uint16_t color) {
	int x = (NES_WIDTH - textWidth(text)) / 2;
	x = x < 4 ? 4 : x;
	for (; *text; text++) {
		int c = (unsigned char) *text;
		c = c >= 32 && c < 127 ? c - 32 : '?' - 32;
		for (int row = 0; row < 12; row++) {
			for (int bit = 0; bit < font[c].width; bit++) {
				int px = x + bit;
				if ((font[c].rows[row] >> bit) & 1 && px < NES_WIDTH - 4) {
					video[(y + row) * NES_WIDTH + px] = color;
				}
			}
		}
		x += font[c].width + 1;
	}
}

static void runNsf(void) {
	struct NsfInfo info;
	nsfInfo(&nes, &info);
	// Right and left change the song, A starts it again
	uint8_t pad = nes.pads[0], pressed = pad & ~lastNsfPad;
	lastNsfPad = pad;
	int song = info.song;
	if (pressed & 0x80) {
		song = (song + 1) % info.songs;
	} else if (pressed & 0x40) {
		song = (song + info.songs - 1) % info.songs;
	}
	if (song != info.song || (pressed & 0x01)) {
		nsfSelect(&nes, song);
		songFrames = 0;
		info.song = song;
	}
	nes.pads[0] = 0;
	nesRunFrame(&nes);
	checkOptions();
	songFrames++;
	memset(video, 0, NES_WIDTH * NES_HEIGHT * sizeof(video[0]));
	char line[64];
	drawText(40, info.title, 0xFFFF);
	drawText(60, info.artist, 0xC618);
	drawText(76, info.copyright, 0xC618);
	snprintf(line, sizeof(line), "Song %d of %d", info.song + 1, info.songs);
	drawText(112, line, 0xFFFF);
	unsigned seconds = songFrames / (nes.region == REGION_NTSC ? 60 : 50);
	snprintf(line, sizeof(line), "%u:%02u", seconds / 60, seconds % 60);
	drawText(128, line, 0xC618);
	drawText(184, "Left / Right: song    A: from the start", 0x8410);
	video_cb(video, NES_WIDTH, NES_HEIGHT, NES_WIDTH * sizeof(uint16_t));
	if (nes.audioCount) {
		audio_batch_cb(nes.audioOut, nes.audioCount / 2);
	}
}

void retro_run(void) {
	input_poll_cb();
	turboFrame++;
	readDevices();
	if (playingNsf) {
		runNsf();
		return;
	}
	if (nes.board == &boardYuxing || nes.input.expansion == EXPANSION_KEYBOARD) {
		memset(nes.keys, 0, sizeof(nes.keys));
		for (unsigned key = 1; key < RETROK_LAST; key++) {
			if (input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, key)) {
				nes.keys[key >> 3] |= 1 << (key & 7);
			}
		}
	}
	nesRunFrame(&nes);
	for (int extra = 0; fdsFast && extra < 15 && fdsBusy(&nes); extra++) {
		nesRunFrame(&nes);
	}
	checkOptions();
	int scale = 1;
	if (filter) {
		scale = COMPOSITE_SCALE;
		compositeFrame(filter, nes.region != REGION_NTSC, nes.ppu.oddFrame, nes.frameBuffer, video);
	} else {
		for (int i = 0; i < NES_WIDTH * NES_HEIGHT; i++) {
			video[i] = palette565[nes.frameBuffer[i] & 0x1FF];
		}
	}
	if (crossShown) {
		drawCrosshair(scale);
	}
	video_cb(video + (cropVertical * NES_WIDTH + cropHorizontal) * scale, (NES_WIDTH - 2 * cropHorizontal) * scale,
		NES_HEIGHT - 2 * cropVertical, NES_WIDTH * scale * sizeof(uint16_t));
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
		int oldAspect = aspect, oldVertical = cropVertical, oldHorizontal = cropHorizontal, oldFilter = filter;
		readVideoOptions();
		readSpeedOptions();
		readPaletteOption();
		readInputOptions();
		readSoundOptions();
		if (aspect != oldAspect || cropVertical != oldVertical || cropHorizontal != oldHorizontal || filter != oldFilter) {
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

// Where the console's RAM and the cart's work RAM sit, for achievements and cheat searches
static void setMemoryMaps(void) {
	static struct retro_memory_descriptor descriptors[2];
	static struct retro_memory_map map;
	unsigned count = 0;
	descriptors[count++] = (struct retro_memory_descriptor) { RETRO_MEMDESC_SYSTEM_RAM, nes.ram, 0, 0x0000, 0, 0, sizeof(nes.ram), NULL };
	if (nes.cart.wram) {
		// $6000-$7FFF, or the Disk System's $6000-$DFFF
		size_t length = nes.cart.disk ? 0x8000 : 0x2000;
		length = nes.cart.wramSize < length ? nes.cart.wramSize : length;
		descriptors[count++] = (struct retro_memory_descriptor) {
			nes.cart.battery ? RETRO_MEMDESC_SAVE_RAM : 0, nes.cart.wram, 0, 0x6000, 0, 0, length, NULL };
	}
	map.descriptors = descriptors;
	map.num_descriptors = count;
	environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
	bool achievements = true;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
}

bool retro_load_game(const struct retro_game_info* game) {
	if (!game || !game->data) {
		return false;
	}
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		return false;
	}
	static const struct { unsigned id; const char* name; } buttons[] = {
		{ RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" }, { RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
		{ RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" }, { RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
		{ RETRO_DEVICE_ID_JOYPAD_B, "B" }, { RETRO_DEVICE_ID_JOYPAD_A, "A" },
		{ RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" }, { RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ RETRO_DEVICE_ID_JOYPAD_X, "Turbo A" }, { RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B" },
	};
	static struct retro_input_descriptor descriptors[4 * 10 + 6];
	unsigned count = 0;
	for (unsigned port = 0; port < 4; port++) {
		for (unsigned i = 0; i < 10; i++) {
			descriptors[count++] = (struct retro_input_descriptor) { port, RETRO_DEVICE_JOYPAD, 0, buttons[i].id, buttons[i].name };
		}
	}
	descriptors[count++] = (struct retro_input_descriptor) { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Microphone" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Zapper Aim X" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Zapper Aim Y" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Zapper Trigger" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Zapper Trigger" };
	descriptors[count] = (struct retro_input_descriptor) { 0, 0, 0, 0, NULL };
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
	readSpeedOptions();
	readPaletteOption();
	readInputOptions();
	readSoundOptions();
	setMemoryMaps();
	playingNsf = nes.board == &boardNsf;
	songFrames = 0;
	lastNsfPad = 0;
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
	nesCheatClear(&nes);
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	(void) index;
	if (enabled && code && !nesCheatAdd(&nes, code)) {
		struct retro_message message = { "armNES: a cheat code wasn't understood", 180 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}
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
