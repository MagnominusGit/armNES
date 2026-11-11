/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"
#include "board.h"
#include "composite.h"
#include "options.h"
#include "arcade.h"

#include <file/archive_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <zlib.h>

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
static uint16_t video[2 * COMPOSITE_WIDTH * NES_HEIGHT];
static int16_t audio[8192];

// A VS. DualSystem's second board, the RAM the two share, and its sound
static struct NES sub;
static uint8_t vsShared[0x800];
static int16_t subAudio[8192];
static uint16_t subPicture[NES_WIDTH * NES_HEIGHT];


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

#define AUTO (-1)

static struct OptionChoice autoOnOffChoices[] = {
	{ "Auto", AUTO },
	{ "Off", 0 },
	{ "On", 1 },
};

static struct OptionChoice fourPlayerChoices[] = {
	{ "Auto", AUTO },
	{ "Off", 0 },
	{ "Four Score", 1 },
	{ "Famicom adapter", 2 },
};

static struct OptionChoice expansionChoices[] = {
	{ "Auto", AUTO },
	{ "None", EXPANSION_NONE },
	{ "Paddle", EXPANSION_PADDLE },
	{ "Power Pad", EXPANSION_MAT },
	{ "Keyboard", EXPANSION_KEYBOARD },
};

enum { AIM_LIGHT_GUN, AIM_MOUSE, AIM_STICK, AIM_STICK_PAN, AIM_TILT };

static struct OptionChoice aimChoices[] = {
	{ "Light gun", AIM_LIGHT_GUN },
	{ "Mouse", AIM_MOUSE },
	{ "Right stick (absolute)", AIM_STICK },
	{ "Right stick (pan)", AIM_STICK_PAN },
	{ "Device tilt", AIM_TILT },
};

static struct OptionChoice rotationChoices[] = {
	{ "0", 0 },
	{ "90", 1 },
	{ "180", 2 },
	{ "270", 3 },
};

static struct OptionChoice sensitivityChoices[] = {
	{ "25%", 25 },
	{ "50%", 50 },
	{ "75%", 75 },
	{ "100%", 100 },
	{ "150%", 150 },
	{ "200%", 200 },
	{ "300%", 300 },
};

static struct OptionChoice filterChoices[] = {
	{ "Off", COMPOSITE_OFF },
	{ "Composite", COMPOSITE },
	{ "S-Video", COMPOSITE_SVIDEO },
};

// How much darker the rows between the picture's lines are, in 256ths kept
static struct OptionChoice scanlineChoices[] = {
	{ "Off", 256 },
	{ "Light", 192 },
	{ "Medium", 128 },
	{ "Dark", 64 },
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

static struct OptionChoice vsPpuChoices[] = {
	{ "Auto", -1 }, { "RP2C03", 0 }, { "RP2C04-0001", 2 }, { "RP2C04-0002", 3 }, { "RP2C04-0003", 4 }, { "RP2C04-0004", 5 },
	{ "RC2C05-01", 8 }, { "RC2C05-02", 9 }, { "RC2C05-03", 10 }, { "RC2C05-04", 11 }, { "RC2C05-05", 12 },
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

// Keys and values match saved configurations; labels are what the menu shows. showOptions hides
// the options that do nothing for the game loaded or with the others' values.
static struct retro_core_option_v2_definition optionDefinitions[] = {
	{ "armnes_region", "Region", NULL, "The console the game runs on. Auto reads it from the game's header.", NULL, "system",
		{ { "Auto", NULL }, { "NTSC", NULL }, { "PAL", NULL }, { "Dendy", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_overclock", "Overclock", NULL,
		"Extra CPU time each frame, which removes most slowdown without changing game speed. A few games "
		"misbehave; try the other placement for those.",
		NULL, "system", { { "Off", NULL }, { "2x (vblank)", NULL }, { "3x (vblank)", NULL }, { "4x (vblank)", NULL },
		{ "2x (after the picture)", NULL }, { "3x (after the picture)", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_fds_fast", "Disk System Fast Loading", NULL,
		"Runs up to 16 times as fast while the disk drive reads or writes, without its sound.",
		NULL, "system", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_jumper", "Cart Jumper", NULL,
		"The cart's solder pads or DIP switches, which pick its menu, games, language or timing. Auto uses "
		"the setting known for the dump. Changing it restarts the game.",
		NULL, "system", { { "Auto", NULL }, { "0", NULL }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", NULL }, { "11", NULL }, { "12", NULL }, { "13", NULL }, { "14", NULL }, { "15", NULL }, { "16", NULL }, { "17", NULL }, { "18", NULL }, { "19", NULL }, { "20", NULL }, { "21", NULL }, { "22", NULL }, { "23", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_palette", "Palette", NULL,
		"The colors. armNES NTSC decodes the console's video signal; the others are measured or tuned. "
		"Custom loads nes.pal (or custom.pal) from the system folder.",
		NULL, "video", { { NULL, NULL } }, "armNES NTSC" },
	{ "armnes_filter", "NTSC/PAL Filter", NULL,
		"The picture as a TV decoded it: Composite, with its color fringes and blended dithering, or the "
		"sharper S-Video. It makes its own colors. Takes more CPU time. Off for arcade boards.",
		NULL, "video", { { "Off", NULL }, { "Composite", NULL }, { "S-Video", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_scanlines", "Scanlines", NULL,
		"Dark rows between the filtered picture's lines, as a CRT's beam left them. Best with integer scaling "
		"or a smooth filter in the frontend.",
		NULL, "video", { { "Off", NULL }, { "Light", NULL }, { "Medium", NULL }, { "Dark", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_sprite_limit", "Sprite Limit", NULL,
		"The console shows 8 sprites on a line, which games turn into flicker. No limit shows them all; "
		"games still see the console's limit.",
		NULL, "video", { { "8 per line", NULL }, { "No limit", NULL }, { NULL, NULL } }, "8 per line" },
	{ "armnes_aspect", "Aspect Ratio", NULL,
		"4:3 as a TV showed it, 8:7 PAR the console's pixel shape (about 11:8 on PAL), or square pixels.",
		NULL, "video", { { "4:3", NULL }, { "8:7 PAR", NULL }, { "1:1", NULL }, { NULL, NULL } }, "4:3" },
	{ "armnes_crop_vertical", "Crop Top and Bottom", NULL,
		"Pixels hidden at the top and bottom, as a TV's overscan hid them.",
		NULL, "video", { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "8" },
	{ "armnes_crop_horizontal", "Crop Sides", NULL,
		"Pixels hidden at the left and right edges.",
		NULL, "video", { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "0" },
	{ "armnes_volume_pulse1", "Pulse 1 Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_pulse1", "Pulse 1 Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_pulse2", "Pulse 2 Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_pulse2", "Pulse 2 Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_triangle", "Triangle Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_triangle", "Triangle Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_noise", "Noise Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_noise", "Noise Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_dmc", "DMC Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_dmc", "DMC Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_cart", "Cart Sound Volume", NULL,
		"The sound chip in the cart or the Disk System.", NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_cart", "Cart Sound Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_turbo", "Turbo Speed", NULL,
		"How fast the turbo buttons (X for A, Y for B) press and release.",
		NULL, "input", { { "Off", NULL }, { "30 per second", NULL }, { "20 per second", NULL }, { "15 per second", NULL },
		{ "10 per second", NULL }, { "7.5 per second", NULL }, { NULL, NULL } }, "15 per second" },
	{ "armnes_opposing", "Allow Opposing Directions", NULL,
		"Left and right (or up and down) held together, which a real pad can't do. A few games glitch.",
		NULL, "input", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_left_stick_dpad", "Left Stick as D-Pad", NULL, "The left stick presses the D-pad too.",
		NULL, "input", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "On" },
	{ "armnes_four_players", "4 Players", NULL,
		"Players 3 and 4 through the NES Four Score or the Famicom's adapter. Auto follows an NES 2.0 header.",
		NULL, "input", { { "Auto", NULL }, { "Off", NULL }, { "Four Score", NULL }, { "Famicom adapter", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_expansion", "Famicom Expansion Port", NULL,
		"The paddle follows the mouse, the Power Pad takes controller 1's buttons, the keyboard the "
		"keyboard (turn on Game Focus). Auto follows an NES 2.0 header.",
		NULL, "input", { { "Auto", NULL }, { "None", NULL }, { "Paddle", NULL }, { "Power Pad", NULL },
		{ "Keyboard", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_zapper_aim", "Zapper Aim", NULL,
		"A light gun or mouse pointer, a mouse-moved crosshair (left button fires, right off the screen), "
		"the right stick pointing (absolute) or moving (pan) it, or tilting the device (R fires, L off the "
		"screen). R3 centers the crosshair, and with tilt, takes how the device is held as the middle. "
		"Touching the screen always aims and fires.",
		NULL, "input", { { "Light gun", NULL }, { "Mouse", NULL }, { "Right stick (absolute)", NULL },
		{ "Right stick (pan)", NULL }, { "Device tilt", NULL }, { NULL, NULL } }, "Light gun" },
	{ "armnes_zapper_sensitivity", "Zapper Sensitivity", NULL,
		"How fast the crosshair moves. Holding L2 aims finer the further it's pulled.",
		NULL, "input", { { "25%", NULL }, { "50%", NULL }, { "75%", NULL }, { "100%", NULL }, { "150%", NULL },
		{ "200%", NULL }, { "300%", NULL }, { NULL, NULL } }, "100%" },
	{ "armnes_motion_rotation", "Device Tilt Sensor Rotation", NULL,
		"Turns the motion sensor to match the screen if tilting goes the wrong way. Usually 0° in the "
		"standalone app, often 90° in RetroArch.",
		NULL, "input", { { "0", "0°" }, { "90", "90°" }, { "180", "180°" }, { "270", "270°" }, { NULL, NULL } }, "0" },
	{ "armnes_vs_ppu", "PPU", NULL, "The board's PPU, which sets its colors. Auto follows an NES 2.0 header, or the RP2C03.",
		NULL, "vs", { { "Auto", NULL }, { "RP2C03", NULL }, { "RP2C04-0001", NULL }, { "RP2C04-0002", NULL }, { "RP2C04-0003", NULL },
		{ "RP2C04-0004", NULL }, { "RC2C05-01", NULL }, { "RC2C05-02", NULL }, { "RC2C05-03", NULL }, { "RC2C05-04", NULL },
		{ "RC2C05-05", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_vs_swap", "Swap Controllers", NULL,
		"Some cabinets wire player 1 to the second port. Auto follows an NES 2.0 header.",
		NULL, "vs", { { "Auto", NULL }, { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_vs_dip1", "DIP Switch 1", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip2", "DIP Switch 2", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip3", "DIP Switch 3", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip4", "DIP Switch 4", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip5", "DIP Switch 5", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip6", "DIP Switch 6", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip7", "DIP Switch 7", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip8", "DIP Switch 8", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_screens", "DualSystem Screens", NULL,
		"A DualSystem cabinet's two screens side by side, or one of them.",
		NULL, "vs", { { "Both", NULL }, { "Main board", NULL }, { "Second board", NULL }, { NULL, NULL } }, "Both" },
	{ "armnes_vs_dip9", "DualSystem DIP Switch 9", NULL, "The second board's switches 1 to 8.", NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip10", "DualSystem DIP Switch 10", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip11", "DualSystem DIP Switch 11", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip12", "DualSystem DIP Switch 12", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip13", "DualSystem DIP Switch 13", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip14", "DualSystem DIP Switch 14", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip15", "DualSystem DIP Switch 15", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip16", "DualSystem DIP Switch 16", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_option_v2_category categories[] = {
	{ "system", "System", "The console's region and speed, the Disk System and cart settings." },
	{ "video", "Video", "Colors, the TV filter, sprites and the picture's shape and edges." },
	{ "audio", "Audio", "Each channel's volume and where it sits between the speakers. The console mixes in mono." },
	{ "input", "Input", "Turbo, extra players, the expansion port and the Zapper." },
	{ "vs", "VS. System", "The arcade board's PPU, controller wiring and DIP switches. Select puts in a coin, controller 1's L3 is the service button. A DualSystem's second board takes controllers 3 and 4 (controller 3's L3 for its service button)." },
	{ NULL, NULL, NULL },
};

static struct retro_core_options_v2 options = { categories, optionDefinitions };

static void checkOptions(void);
static bool showOptions(void);
static void swapDisk(void);

// The picture: its shape and how much of the edges is cropped
static int aspect, cropVertical, cropHorizontal, filter, scanlines = 256;

enum { SCREENS_BOTH, SCREENS_MAIN, SCREENS_SECOND };
static int screens;

static struct OptionChoice screenChoices[] = {
	{ "Both", SCREENS_BOTH },
	{ "Main board", SCREENS_MAIN },
	{ "Second board", SCREENS_SECOND },
};

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
	cropVertical = optionChoice(environ_cb, "armnes_crop_vertical", cropChoices, 5, 8);
	cropHorizontal = optionChoice(environ_cb, "armnes_crop_horizontal", cropChoices, 5, 0);
	filter = nes.cart.vs || nes.cart.playchoice ? COMPOSITE_OFF : optionChoice(environ_cb, "armnes_filter", filterChoices, 3, COMPOSITE_OFF);
	scanlines = optionChoice(environ_cb, "armnes_scanlines", scanlineChoices, 4, 256);
	screens = optionChoice(environ_cb, "armnes_vs_screens", screenChoices, 3, SCREENS_BOTH);
}

static void geometry(struct retro_game_geometry* g) {
	unsigned width = NES_WIDTH - 2 * cropHorizontal, height = NES_HEIGHT - 2 * cropVertical;
	// NTSC pixels are 8:7; PAL's are 2950000:2128137
	float pixel = nes.region == REGION_NTSC ? 8.0f / 7.0f : 2950000.0f / 2128137.0f;
	unsigned count = nes.dual && screens == SCREENS_BOTH && nes.cart.vsHardware != 6 ? 2 : 1;
	g->base_width = (filter ? width * COMPOSITE_SCALE : width) * count;
	// The filtered picture has two rows to a line, the second for the scanline
	g->base_height = filter ? 2 * height : height;
	g->max_width = 2 * COMPOSITE_WIDTH;
	g->max_height = 2 * NES_HEIGHT;
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
	g->aspect_ratio *= count;
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
static const uint16_t vsPalettes[5][64] = {
#include "vs-palettes.inc"
};

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
		if (nes.cart.vs || nes.cart.playchoice) {
			// The RGB PPUs' own colors; emphasis turns a channel fully on
			int ppu = nes.cart.vsPpu >= 2 && nes.cart.vsPpu <= 5 ? nes.cart.vsPpu - 1 : 0;
			uint16_t c = vsPalettes[ppu][entry & 0x3F];
			for (int i = 0; i < 3; i++) {
				rgb[i] = (entry & (0x40 << i)) ? 1 : ((c >> (6 - 3 * i)) & 7) / 7.0f;
			}
		} else if (table) {
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
		int emphasis = nes.cart.vs ? 0 : entry >> 6;
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

static int palette = -1, paletteRegion = -1, paletteVs = -1;

static void readPaletteOption(void) {
	int which = optionChoice(environ_cb, "armnes_palette", paletteChoices, PALETTE_CUSTOM + 1, PALETTE_NTSC);
	if (which == PALETTE_CUSTOM && !loadCustomPalette()) {
		struct retro_message message = { "armNES: no nes.pal or custom.pal (192 or 1536 bytes) in the system folder", 300 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		which = PALETTE_NTSC;
	}
	int vs = nes.cart.vs || nes.cart.playchoice ? nes.cart.vsPpu : -1;
	if (which != palette || (int) nes.region != paletteRegion || vs != paletteVs) {
		palette = which;
		paletteRegion = nes.region;
		paletteVs = vs;
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
#ifdef ARMNES_ARCADE
	// RetroArch 1.22.2 extracts archives despite block_extract unless asked for their path
	static const struct retro_system_content_info_override zips[] = { { "zip|7z", true, false }, { NULL, false, false } };
	cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*) zips);
#endif
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
	static struct retro_core_options_update_display_callback update = { showOptions };
	cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK, &update);
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
	info->library_version = "1.0";
	info->need_fullpath = false;
#ifdef ARMNES_ARCADE
	// The arcade build takes archives unopened, so MAME's sets (and their parents') open here
	info->library_name = "armNES Arcade";
	info->valid_extensions = "zip|7z|nes|fds|unf|unif|nsf";
	info->block_extract = true;
#else
	// RetroArch unpacks archives itself, so achievements identify the game inside; a frontend that
	// hands over a whole archive (an arcade set) still has it opened here
	info->library_name = "armNES";
	info->valid_extensions = "nes|fds|unf|unif|nsf";
	info->block_extract = false;
#endif
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	geometry(&info->geometry);
	info->timing.fps = nes.region == REGION_NTSC ? 60.0988 : (nes.region == REGION_PAL ? 50.0070 : 50.0070);
	info->timing.sample_rate = 48000;
}

static uint8_t portDevice[2];

// Whether the cart has its own sound chip: the Disk System's, MMC5's, Namco 163's, VRC6's, Sunsoft's
// or VRC7's, or an NSF's, which may name any of them.
static bool cartSound(void) {
	static const int mappers[] = { 5, 19, 20, 24, 26, 69, 85 };
	if (nes.cart.disk || nes.board == &boardNsf) {
		return true;
	}
	for (size_t i = 0; i < sizeof(mappers) / sizeof(*mappers); i++) {
		if (nes.cart.mapper == mappers[i]) {
			return true;
		}
	}
	return false;
}

// Shows only the options that do something for the game loaded, with the controllers plugged in and
// the other options' values. Without a game, all of them.
static bool showOptions(void) {
	bool const vs = !loaded || nes.cart.vs;
	bool const dual = !loaded || nes.dual;
	bool const zapper = !loaded || portDevice[0] == DEVICE_ZAPPER || portDevice[1] == DEVICE_ZAPPER;
	struct { const char* key; bool show; } const shown[] = {
		{ "armnes_fds_fast", !loaded || nes.cart.disk },
		{ "armnes_jumper", !loaded || nes.hasJumper || nes.cart.jumper >= 0 },
		{ "armnes_palette", !optionIs(environ_cb, "armnes_filter", "Composite") && !optionIs(environ_cb, "armnes_filter", "S-Video") },
		{ "armnes_scanlines", optionIs(environ_cb, "armnes_filter", "Composite") || optionIs(environ_cb, "armnes_filter", "S-Video") },
		{ "armnes_volume_cart", !loaded || cartSound() },
		{ "armnes_pan_cart", !loaded || cartSound() },
		{ "armnes_zapper_aim", zapper },
		{ "armnes_zapper_sensitivity", zapper && !optionIs(environ_cb, "armnes_zapper_aim", "Light gun") },
		{ "armnes_motion_rotation", zapper && optionIs(environ_cb, "armnes_zapper_aim", "Device tilt") },
		{ "armnes_vs_ppu", vs },
		{ "armnes_vs_swap", vs },
		{ "armnes_vs_screens", dual },
	};
	for (size_t i = 0; i < sizeof(shown) / sizeof(*shown); i++) {
		struct retro_core_option_display display = { shown[i].key, shown[i].show };
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
	}
	for (int i = 1; i <= 16; i++) {
		char key[32];
		snprintf(key, sizeof(key), "armnes_vs_dip%d", i);
		struct retro_core_option_display display = { key, i <= 8 ? vs : dual };
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
	}
	return true;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (port < 2) {
		switch (device) {
		case DEVICE_ID_ZAPPER: portDevice[port] = DEVICE_ZAPPER; break;
		case DEVICE_ID_PADDLE: portDevice[port] = DEVICE_PADDLE; break;
		case DEVICE_ID_POWER_PAD: portDevice[port] = DEVICE_POWER_PAD; break;
		case RETRO_DEVICE_NONE: portDevice[port] = DEVICE_NONE; break;
		default: portDevice[port] = DEVICE_PAD; break;
		}
		showOptions();
	}
}

void retro_reset(void) {
	if (loaded) {
		if (nes.dual) {
			nesResetDual(&nes, false);
		} else {
			nesReset(&nes, false);
		}
	}
}

static int turboPeriod = 4;
static bool allowOpposing;
static bool leftStickDpad;
static unsigned turboFrame;
static int fourPlayers, expansion, zapperAim, zapperSpeed = 100, motionRotation;
static int vsSwap;
static unsigned coinFrames[4];
static bool fdsFast;

static void readInputOptions(void) {
	turboPeriod = optionChoice(environ_cb, "armnes_turbo", turboChoices, 6, 4);
	allowOpposing = optionChoice(environ_cb, "armnes_opposing", onOffChoices, 2, 0);
	leftStickDpad = optionChoice(environ_cb, "armnes_left_stick_dpad", onOffChoices, 2, 1);
	fourPlayers = optionChoice(environ_cb, "armnes_four_players", fourPlayerChoices, 4, AUTO);
	zapperAim = optionChoice(environ_cb, "armnes_zapper_aim", aimChoices, 5, AIM_LIGHT_GUN);
	motionRotation = optionChoice(environ_cb, "armnes_motion_rotation", rotationChoices, 4, 0);
	zapperSpeed = optionChoice(environ_cb, "armnes_zapper_sensitivity", sensitivityChoices, 7, 100);
	fdsFast = optionChoice(environ_cb, "armnes_fds_fast", onOffChoices, 2, 0);
	vsSwap = optionChoice(environ_cb, "armnes_vs_swap", autoOnOffChoices, 3, AUTO);
	for (int board = 0; board < 2; board++) {
		struct InputConfig* in = board ? &sub.input : &nes.input;
		in->vsDip = 0;
		for (int i = 0; i < 8; i++) {
			char key[24];
			snprintf(key, sizeof(key), "armnes_vs_dip%d", board * 8 + i + 1);
			in->vsDip |= optionChoice(environ_cb, key, onOffChoices, 2, 0) << i;
		}
	}
	expansion = optionChoice(environ_cb, "armnes_expansion", expansionChoices, 5, AUTO);
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
	if (leftStickDpad) {
		int x = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
		int y = input_state_cb(port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
		pad |= (y < -0x4000 ? 0x10 : 0) | (y > 0x4000 ? 0x20 : 0) | (x < -0x4000 ? 0x40 : 0) | (x > 0x4000 ? 0x80 : 0);
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
static int stickX = -1, stickY;

// Fine aim: the crosshair's speed in 256ths, from all of it down to a quarter as L2 is pulled
static int fineAim(void) {
	int pull = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2);
	if (!pull && input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2)) {
		pull = 0x7FFF;
	}
	return 256 - pull * 192 / 0x7FFF;
}

static struct retro_sensor_interface sensors;
static bool accelerometerOn, tiltCentered;
static float tiltRight, tiltDown, tiltMiddleRight, tiltMiddleDown;

// Turns the frontend's accelerometer on while a Zapper aims by tilt
static void useAccelerometer(bool on) {
	if (on && !sensors.set_sensor_state && !environ_cb(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensors)) {
		memset(&sensors, 0, sizeof(sensors));
	}
	if (sensors.set_sensor_state && on != accelerometerOn) {
		accelerometerOn = sensors.set_sensor_state(0, on ? RETRO_SENSOR_ACCELEROMETER_ENABLE : RETRO_SENSOR_ACCELEROMETER_DISABLE, 60) && on;
		tiltCentered = false;
	}
}

// The device's tilt as a right stick, from where it was held when R3 was last pressed (or tilt
// aiming started): gravity along the screen's right and down, smoothed over a few frames, with a
// third of a g reaching the edge at 100%. False with no readings, for the right stick to stand in.
static bool deviceTilt(int* x, int* y) {
	if (!accelerometerOn) {
		return false;
	}
	float const sensorX = sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_X);
	float const sensorY = sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Y);
	float const out = sensors.get_sensor_input(0, RETRO_SENSOR_ACCELEROMETER_Z);
	if (sensorX == 0 && sensorY == 0 && out == 0) {
		return false;
	}
	// The sensor's axes turned to the screen's, for a frontend that passes them on as the device has them
	static const float turns[4][4] = { { 1, 0, 0, 1 }, { 0, -1, 1, 0 }, { -1, 0, 0, -1 }, { 0, 1, -1, 0 } };
	const float* const turn = turns[motionRotation];
	// The sensor reads the push that holds the device up, so a side tipped down reads away from it
	float const right = -(turn[0] * sensorX + turn[1] * sensorY), down = turn[2] * sensorX + turn[3] * sensorY;
	if (!tiltCentered || input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3)) {
		tiltRight = tiltMiddleRight = right;
		tiltDown = tiltMiddleDown = down;
		tiltCentered = true;
	}
	tiltRight += (right - tiltRight) / 3;
	tiltDown += (down - tiltDown) / 3;
	float const scale = 0x7FFF * 3 / 9.80665f * zapperSpeed / 100;
	*x = (int) fmaxf(-0x7FFF, fminf(0x7FFF, (tiltRight - tiltMiddleRight) * scale));
	*y = (int) fmaxf(-0x7FFF, fminf(0x7FFF, (tiltDown - tiltMiddleDown) * scale));
	return true;
}

// Aim from the mouse, the stick or the device's tilt: false for the light gun
static bool aimCrosshair(unsigned port, bool* trigger, bool* away) {
	int left = cropHorizontal << 8, right = (NES_WIDTH - 1 - cropHorizontal) << 8;
	int top = cropVertical << 8, bottom = (NES_HEIGHT - 1 - cropVertical) << 8;
	int fine = fineAim(), speed = zapperSpeed * fine;
	switch (zapperAim) {
	case AIM_MOUSE:
		// Half a pixel a count at 100%, from this port's mouse and the first one
		for (unsigned p = 0; p <= port; p += port ? port : 1) {
			crossX += input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X) * 128 * speed / 25600;
			crossY += input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y) * 128 * speed / 25600;
			*trigger |= input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) != 0;
			*away |= input_state_cb(p, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT) != 0;
		}
		break;
	case AIM_STICK:
	case AIM_STICK_PAN:
	case AIM_TILT: {
		int x, y;
		if (zapperAim != AIM_TILT || !deviceTilt(&x, &y)) {
			x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
			y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
		}
		if (zapperAim != AIM_STICK_PAN) {
			// With fine aim, the stick's moves count for less from where the crosshair is
			int sx = left + (x + 0x8000) * ((right - left) >> 8) / 0xFFFF * 256;
			int sy = top + (y + 0x8000) * ((bottom - top) >> 8) / 0xFFFF * 256;
			if (fine < 256 && stickX >= 0) {
				crossX += (sx - stickX) * fine / 256;
				crossY += (sy - stickY) * fine / 256;
			} else {
				crossX = sx;
				crossY = sy;
			}
			stickX = sx;
			stickY = sy;
		} else {
			// Up to 4 pixels a frame at 100%, past a small dead zone
			crossX += abs(x) > 0x1000 ? x / 32 * speed / 25600 : 0;
			crossY += abs(y) > 0x1000 ? y / 32 * speed / 25600 : 0;
		}
		*trigger |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R) != 0;
		*away |= input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L) != 0;
		break;
	}
	default:
		return false;
	}
	if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3)) {
		crossX = (left + right) / 2;
		crossY = (top + bottom) / 2;
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
	// Auto: what an NES 2.0 header's default expansion device names
	uint8_t named = nes.cart.nes2 ? nes.cart.header[15] & 0x3F : 0;
	int players = fourPlayers != AUTO ? fourPlayers : named == 0x02 ? 1 : (named == 0x03 || named == 0x06) ? 2 : 0;
	int port = expansion;
	if (port == AUTO) {
		port = named == 0x10 || named == 0x11 ? EXPANSION_PADDLE : named == 0x0D || named == 0x0E ? EXPANSION_MAT
			: named == 0x23 ? EXPANSION_KEYBOARD : EXPANSION_NONE;
	}
	in->fourScore = players == 1;
	in->expansion = players == 2 ? EXPANSION_HORI : (uint8_t) port;
	for (unsigned port = 0; port < 2; port++) {
		nes.pads[port] = in->device[port] == DEVICE_PAD ? readPad(port) : 0;
		in->pads[port] = fourPlayers ? readPad(port + 2) : 0;
	}
	in->mic = in->device[1] == DEVICE_PAD && input_state_cb(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
	if (nes.cart.vs) {
		// Select drops a coin in that player's slot, held a few frames as a coin takes to fall. The
		// cabinet's start buttons report where a pad's Select does.
		for (unsigned port = 0; port < 2; port++) {
			bool held = nes.pads[port] & 0x04;
			coinFrames[port] = held ? coinFrames[port] + 1 : 0;
			in->vsCoin[port] = held && coinFrames[port] <= 4;
			nes.pads[port] = (nes.pads[port] & ~0x0C) | (nes.pads[port] & 0x08) >> 1;
		}
		in->vsService = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
		in->mic = false;
		// A DualSystem's second board: controllers 3 and 4
		for (unsigned port = 0; nes.dual && port < 2; port++) {
			uint8_t pad = readPad(port + 2);
			bool held = pad & 0x04;
			coinFrames[port + 2] = held ? coinFrames[port + 2] + 1 : 0;
			sub.input.device[port] = DEVICE_PAD;
			sub.input.vsCoin[port] = held && coinFrames[port + 2] <= 4;
			sub.pads[port] = (pad & ~0x0C) | (pad & 0x08) >> 1;
			sub.input.vsService = input_state_cb(2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
		}
		if (vsSwap == AUTO ? named == 0x05 : vsSwap) {
			uint8_t pad = nes.pads[0];
			nes.pads[0] = nes.pads[1];
			nes.pads[1] = pad;
		}
	}
	in->powerPad = 0;
	useAccelerometer(zapperAim == AIM_TILT && (in->device[0] == DEVICE_ZAPPER || in->device[1] == DEVICE_ZAPPER));
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

// L3: in a Disk System game, the disk swap; on the Datach's reader, a swipe of the next code from
// datach.txt in the system folder, one to a line, or any valid one, as games take a product's barcode
static unsigned barcodeLine;
static bool lastSwipe;

static void swipeBarcode(void) {
	bool held = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
	bool pressed = held && !lastSwipe;
	lastSwipe = held;
	if (pressed && nes.cart.diskSides) {
		swapDisk();
	}
	if (!pressed || nes.cart.mapper != 157) {
		return;
	}
	char code[32] = "";
	const char* dir = NULL;
	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/datach.txt", dir);
		FILE* f = fopen(path, "r");
		if (f) {
			// The next line with a code, back to the first after the last
			char line[64], first[32] = "";
			unsigned lines = 0;
			while (fgets(line, sizeof(line), f)) {
				if (line[0] >= '0' && line[0] <= '9') {
					if (!lines) {
						snprintf(first, sizeof(first), "%s", line);
					}
					if (lines++ == barcodeLine) {
						snprintf(code, sizeof(code), "%s", line);
					}
				}
			}
			fclose(f);
			if (lines && !code[0]) {
				memcpy(code, first, sizeof(first));
				barcodeLine = 0;
			}
			barcodeLine++;
		}
	}
	if (!code[0]) {
		static uint32_t seed = 1;
		seed = seed * 1103515245 + 12345 + turboFrame;
		for (int i = 0; i < 12; i++) {
			code[i] = (char) ('0' + (seed >> (i + 8)) % 10);
			seed = seed * 1103515245 + 12345;
		}
		code[12] = 0;
	}
	char text[80];
	code[strcspn(code, "\r\n")] = 0;
	snprintf(text, sizeof(text), datachSwipe(&nes, code) ? "Barcode %s swiped" : "%s isn't an EAN-13 or EAN-8 barcode", code);
	struct retro_message message = { text, 180 };
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

// A DualSystem: both boards' pictures side by side, or one, and their sound mixed
static void runDual(void) {
	static uint16_t screen[COMPOSITE_WIDTH * NES_HEIGHT];
	nesRunFrameDual(&nes, subPicture);
	checkOptions();
	int scale = filter ? COMPOSITE_SCALE : 1;
	int width = (NES_WIDTH - 2 * cropHorizontal) * scale, height = NES_HEIGHT - 2 * cropVertical;
	// The protected DualSystem's second board shows nothing of the game
	int shown = nes.cart.vsHardware == 6 ? SCREENS_MAIN : screens;
	int count = shown == SCREENS_BOTH ? 2 : 1;
	for (int i = 0; i < count; i++) {
		const struct NES* board = (i || shown == SCREENS_SECOND) ? &sub : &nes;
		const uint16_t* picture = board == &sub ? subPicture : nes.frameBuffer;
		if (filter) {
			compositeFrame(filter, false, board->ppu.oddFrame, picture, screen);
		} else {
			for (int p = 0; p < NES_WIDTH * NES_HEIGHT; p++) {
				screen[p] = palette565[picture[p] & 0x1FF];
			}
		}
		for (int y = 0; y < height; y++) {
			memcpy(video + (y * count + i) * width, screen + ((y + cropVertical) * NES_WIDTH + cropHorizontal) * scale,
				width * sizeof(uint16_t));
		}
	}
	video_cb(video, width * count, height, width * count * sizeof(uint16_t));
	size_t samples = nes.audioCount < sub.audioCount ? nes.audioCount : sub.audioCount;
	for (size_t i = 0; i < samples; i++) {
		nes.audioOut[i] = (int16_t) ((nes.audioOut[i] + sub.audioOut[i]) / 2);
	}
	if (samples) {
		audio_batch_cb(nes.audioOut, samples / 2);
	}
}

// R2 held fast-forwards at the frontend's maximum run speed
static void readFastForward(void) {
	static bool held;
	bool now = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) != 0;
	if (now == held) {
		return;
	}
	held = now;
	struct retro_fastforwarding_override override = { .ratio = -1.0f, .fastforward = now, .notification = true, .inhibit_toggle = now };
	environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &override);
}

void retro_run(void) {
	input_poll_cb();
	turboFrame++;
	readDevices();
	readFastForward();
	swipeBarcode();
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
	if (nes.dual) {
		runDual();
		return;
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
	if (filter) {
		// Each line twice, the second dimmed as the scanline
		static uint16_t tall[2 * COMPOSITE_WIDTH * NES_HEIGHT];
		for (int y = 0; y < NES_HEIGHT; y++) {
			const uint16_t* line = &video[y * COMPOSITE_WIDTH];
			uint16_t* top = &tall[2 * y * COMPOSITE_WIDTH];
			uint16_t* bottom = top + COMPOSITE_WIDTH;
			memcpy(top, line, COMPOSITE_WIDTH * sizeof(uint16_t));
			for (int x = 0; x < COMPOSITE_WIDTH; x++) {
				unsigned p = line[x];
				bottom[x] = (uint16_t) (((p >> 11) * scanlines >> 8) << 11 | (((p >> 5) & 63) * scanlines >> 8) << 5 | (p & 31) * scanlines >> 8);
			}
		}
		video_cb(tall + (2 * cropVertical * NES_WIDTH + cropHorizontal) * COMPOSITE_SCALE, (NES_WIDTH - 2 * cropHorizontal) * COMPOSITE_SCALE,
			2 * (NES_HEIGHT - 2 * cropVertical), COMPOSITE_WIDTH * sizeof(uint16_t));
	} else {
		video_cb(video + cropVertical * NES_WIDTH + cropHorizontal, NES_WIDTH - 2 * cropHorizontal,
			NES_HEIGHT - 2 * cropVertical, NES_WIDTH * sizeof(uint16_t));
	}
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

// A .zip or .7z: the game file in it, or a MAME VS. System or PlayChoice-10 set, whose clones may keep
// only their own chips and need the parent set's archive from the same folder. Read with RetroArch's
// own archive code, as armGBA does; frontends hand archives over by their path (see
// retro_set_environment).
static uint8_t* unpacked;
static char romDir[1024];

static uint8_t* readFile(const char* path, size_t* size) {
	FILE* f = path ? fopen(path, "rb") : NULL;
	if (!f) {
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	long length = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t* data = length > 0 ? malloc((size_t) length) : NULL;
	if (data && fread(data, 1, (size_t) length, f) != (size_t) length) {
		free(data);
		data = NULL;
	}
	fclose(f);
	*size = (size_t) length;
	return data;
}

// One file from an archive
static void* archiveRead(const char* archive, const char* name, size_t* size) {
	char inner[4096];
	snprintf(inner, sizeof(inner), "%s#%s", archive, name);
	void* data = NULL;
	int64_t length = 0;
	if (!file_archive_compressed_read(inner, &data, NULL, &length) || length <= 0) {
		free(data);
		return NULL;
	}
	*size = (size_t) length;
	return data;
}

// Every file in an archive, as chips by their CRC
struct Chips {
	struct ArcadeChip* chips;
	int count;
};

static void readChips(const char* archive, struct Chips* out) {
	out->chips = NULL;
	out->count = 0;
	struct string_list* entries = file_archive_get_file_list(archive, NULL);
	if (!entries) {
		return;
	}
	out->chips = calloc(entries->size + 1, sizeof(struct ArcadeChip));
	for (size_t i = 0; out->chips && i < entries->size; i++) {
		size_t size = 0;
		uint8_t* data = archiveRead(archive, entries->elems[i].data, &size);
		if (data) {
			out->chips[out->count++] = (struct ArcadeChip) { (uint32_t) crc32(0, data, (uInt) size), data, size };
		}
	}
	string_list_free(entries);
}

static void freeChips(struct Chips* c) {
	for (int i = 0; i < c->count; i++) {
		free((void*) c->chips[i].data);
	}
	free(c->chips);
	c->chips = NULL;
	c->count = 0;
}

static struct Chips parent;

static const struct ArcadeChip* parentChips(const char* set, int* count) {
	static const char* const kinds[] = { "zip", "7z" };
	for (int i = 0; i < 2 && !parent.count; i++) {
		char path[1100];
		snprintf(path, sizeof(path), "%s%s.%s", romDir, set, kinds[i]);
		readChips(path, &parent);
	}
	*count = parent.count;
	return parent.chips;
}

static bool unpackArchive(const char* path, const uint8_t** data, size_t* size) {
	struct string_list* games = file_archive_get_file_list(path, "nes|fds|unf|unif|nsf");
	if (games && games->size) {
		unpacked = archiveRead(path, games->elems[0].data, size);
		string_list_free(games);
		*data = unpacked;
		return unpacked != NULL;
	}
	string_list_free(games);
	char name[256];
	const char* base = strrchr(path, '/');
	const char* backslash = strrchr(path, '\\');
	base = backslash && (!base || backslash > base) ? backslash : base;
	base = base ? base + 1 : path;
	snprintf(romDir, sizeof(romDir), "%.*s", (int) (base - path), path);
	snprintf(name, sizeof(name), "%s", base);
	char* dot = strrchr(name, '.');
	if (dot) {
		*dot = 0;
	}
	struct Chips own;
	readChips(path, &own);
	unpacked = arcadeBuild(own.chips, own.count, name, parentChips, size);
	freeChips(&own);
	freeChips(&parent);
	*data = unpacked;
	return unpacked != NULL;
}

static bool unpack(const struct retro_game_info* game, const uint8_t** data, size_t* size) {
	if (game->path && path_is_compressed_file(game->path)) {
		return unpackArchive(game->path, data, size);
	}
	*data = game->data;
	*size = game->size;
	if (!game->data) {
		unpacked = readFile(game->path, size);
		*data = unpacked;
	}
	return *data != NULL;
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

// L3 in a Disk System game: takes the disk out, then puts the next side in
static void swapDisk(void) {
	char text[40];
	if (fdsSide(&nes) >= 0) {
		setEjectState(true);
		snprintf(text, sizeof(text), "Disk out");
	} else {
		diskIndex = (diskIndex + 1) % (unsigned) nes.cart.diskSides;
		setEjectState(false);
		if (nes.cart.diskSides > 2) {
			snprintf(text, sizeof(text), "Disk %u side %c in", diskIndex / 2 + 1, 'A' + diskIndex % 2);
		} else {
			snprintf(text, sizeof(text), "Side %c in", 'A' + diskIndex % 2);
		}
	}
	struct retro_message message = { text, 120 };
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

// Options changed while running: the picture's shape at once; a cart's solder pads or DIP switches
// restart the game. Carts with them say so once the game looks at them.
static bool jumperNoticed;

static void checkOptions(void) {
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		int oldAspect = aspect, oldVertical = cropVertical, oldHorizontal = cropHorizontal, oldFilter = filter, oldScreens = screens;
		readVideoOptions();
		readSpeedOptions();
		readPaletteOption();
		readInputOptions();
		readSoundOptions();
		if (aspect != oldAspect || cropVertical != oldVertical || cropHorizontal != oldHorizontal || filter != oldFilter ||
			screens != oldScreens) {
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
		showOptions();
		char text[200];
		snprintf(text, sizeof(text), "This cart has solder pads or DIP switches, set to %d%s. Core Options > System > Cart Jumper changes them.",
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
	if (!game || (!game->data && !game->path)) {
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
		{ RETRO_DEVICE_ID_JOYPAD_SELECT, "Select (VS. System: Coin)" }, { RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ RETRO_DEVICE_ID_JOYPAD_X, "Turbo A" }, { RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B" },
	};
	static struct retro_input_descriptor descriptors[4 * 10 + 10];
	unsigned count = 0;
	for (unsigned port = 0; port < 4; port++) {
		for (unsigned i = 0; i < 10; i++) {
			descriptors[count++] = (struct retro_input_descriptor) { port, RETRO_DEVICE_JOYPAD, 0, buttons[i].id, buttons[i].name };
		}
	}
	descriptors[count++] = (struct retro_input_descriptor) { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Microphone" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "Disk System: Swap Disk; VS. System: Service; Datach: Swipe Barcode" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Zapper Aim X" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Zapper Aim Y" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "Zapper Trigger" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "Zapper Trigger Off Screen" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "Zapper: Center Crosshair" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "Fast-Forward" };
	descriptors[count++] = (struct retro_input_descriptor) { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "Zapper Fine Aim" };
	descriptors[count] = (struct retro_input_descriptor) { 0, 0, 0, 0, NULL };
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*) descriptors);

	nesInit(&nes);
	nes.audioOut = audio;
	nes.audioCapacity = sizeof(audio) / sizeof(audio[0]);
	const uint8_t* data;
	size_t size;
	free(unpacked);
	unpacked = NULL;
	if (!unpack(game, &data, &size)) {
		struct retro_message message = { "armNES: no game in this .zip, or not all of a MAME set's chips", 360 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		return false;
	}
	if (isDisk(data, size)) {
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
	nesInit(&sub);
	sub.audioOut = subAudio;
	sub.audioCapacity = sizeof(subAudio) / sizeof(subAudio[0]);
	memset(subPicture, 0, sizeof(subPicture));
	if (!nesLoadDual(&nes, &sub, vsShared, data, size)) {
		nesFree(&sub);
		if (!nesLoad(&nes, data, size)) {
			if (nes.cart.mapper || nes.cart.prgSize) {
				struct retro_message message = { "armNES: this game's board isn't supported yet", 360 };
				environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
			}
			return false;
		}
	}
	readVideoOptions();
	int vsPpu = optionChoice(environ_cb, "armnes_vs_ppu", vsPpuChoices, 11, -1);
	if (nes.cart.vs && vsPpu >= 0) {
		nes.cart.vsPpu = sub.cart.vsPpu = (uint8_t) vsPpu;
	}
	int region = optionChoice(environ_cb, "armnes_region", regionChoices, 4, -1);
	if (region >= 0 && region != (int) nes.region) {
		nesSetRegion(&nes, region);
		if (nes.dual) {
			nesSetRegion(&sub, region);
			nesResetDual(&nes, true);
		} else {
			nesReset(&nes, true);
		}
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
	showOptions();
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num) {
	(void) type;
	(void) info;
	(void) num;
	return false;
}

void retro_unload_game(void) {
	useAccelerometer(false);
	if (loaded) {
		if (nes.dual) {
			nesFree(&sub);
		}
		nesFree(&nes);
		free(unpacked);
		unpacked = NULL;
		loaded = false;
		showOptions();
	}
}

unsigned retro_get_region(void) {
	return nes.region == REGION_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

// A DualSystem's state: both boards', then the RAM they share, the second's last picture and the one
// it's drawing (its frames run out of step with the main board's)
size_t retro_serialize_size(void) {
	if (!loaded) {
		return 0;
	}
	return nes.dual ? nesStateSize(&nes) + nesStateSize(&sub) + sizeof(vsShared) + 2 * sizeof(subPicture) : nesStateSize(&nes);
}

bool retro_serialize(void* data, size_t size) {
	if (!loaded || size < retro_serialize_size()) {
		return false;
	}
	if (!nes.dual) {
		return nesSaveState(&nes, data, size);
	}
	size_t first = nesStateSize(&nes), second = nesStateSize(&sub);
	memcpy((uint8_t*) data + first + second, vsShared, sizeof(vsShared));
	memcpy((uint8_t*) data + first + second + sizeof(vsShared), subPicture, sizeof(subPicture));
	memcpy((uint8_t*) data + first + second + sizeof(vsShared) + sizeof(subPicture), sub.frameBuffer, sizeof(subPicture));
	return nesSaveState(&nes, data, first) && nesSaveState(&sub, (uint8_t*) data + first, second);
}

bool retro_unserialize(const void* data, size_t size) {
	if (!loaded || size < retro_serialize_size()) {
		return false;
	}
	if (!nes.dual) {
		return nesLoadState(&nes, data, size);
	}
	size_t first = nesStateSize(&nes), second = nesStateSize(&sub);
	if (!nesLoadState(&nes, data, first) || !nesLoadState(&sub, (const uint8_t*) data + first, second)) {
		return false;
	}
	memcpy(vsShared, (const uint8_t*) data + first + second, sizeof(vsShared));
	memcpy(subPicture, (const uint8_t*) data + first + second + sizeof(vsShared), sizeof(subPicture));
	memcpy(sub.frameBuffer, (const uint8_t*) data + first + second + sizeof(vsShared) + sizeof(subPicture), sizeof(subPicture));
	return true;
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
