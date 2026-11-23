/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
// strcasestr
#define _GNU_SOURCE
#include "libretro.h"
#include "board.h"
#include "par.h"
#include "genie.h"
#include "composite.h"
#include "options.h"
#include "arcade.h"
#include "supervision/supervision.h"
#include "gamate/gamate.h"

#include <file/archive_file.h>
#include <file/file_path.h>
#include <lists/string_list.h>
#include <zlib.h>

#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// The libretro core: one NES, its picture through an NTSC palette, its sound at 48 kHz, two pads.

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static struct NES nes;
static bool loaded;
// A Watara Supervision game (src/supervision), run in place of the NES while it's loaded.
static struct Supervision supervision;
static bool svLoaded;
// A Bit Corporation Gamate game (src/gamate), likewise. Link Mode Together: a second Gamate with the
// same game on controller 2, and how they're shown (0 side by side, 1 top and bottom, 2 or 3 player 1's
// or 2's). Separate: the other player's Gamate over netplay.
static struct Gamate gamate, gamateTwo;
static bool gmLoaded, gmLinked, gmSeparate;
// The screens as shown now (GM_SCREENS_*), which R2 and L2 change, and as Screen Layout last said
enum { GM_SCREENS_SIDE, GM_SCREENS_STACKED, GM_SCREENS_ONE };
static int gmScreen, gmScreenOption = -1;
// L2 Button's fast-forward, Screen Layout Control's player (-1 Auto), Show Player Numbers, the Switch
// Controller Combo, and the console controller 1 plays
enum { GM_SWITCH_START_L2_R2, GM_SWITCH_L_R_A_B, GM_SWITCH_L3_R3, GM_SWITCH_START_L3, GM_SWITCH_START_R3, GM_SWITCH_OFF };
static bool gmL2FastForward = true, gmPlayerNumbers = true;
static int gmR2Follows = -1, gmSwitchCombo, gmControllerFocus;
// A tag of this device's own in the states it saves: one from another device's session, loaded by
// netplay, means this device joined it (so with Auto its player is 2 even where RetroArch doesn't say)
static uint32_t gmSessionTag;
static bool gmJoined;

// Pro Action Replay (see the game on top, further down)
enum { PAR_CHOICES = RETRO_NUM_CORE_OPTION_VALUES_MAX - 2, PAR_NAME = 256 };
static struct retro_core_option_v2_definition* parOption;
static char parGames[PAR_CHOICES][PAR_NAME], parLabels[PAR_CHOICES][PAR_NAME];
static size_t parGameCount;
static bool parLoaded;
// Which cheat cartridge: the Pro Action Replay or the Game Genie
static bool parIsGenie;
// The Game Genie option: plugged in between the console and the game loaded as content (parTop its
// file name), which keeps its own saves
static bool genieOver;
// Wanted, but not in the system folder: not looked for again until turned off
static bool genieMissing;
// The option as last seen: only a change to it plugs the Game Genie in or out, so one plugged in to
// match a loaded state (netplay's host) stays until the option itself changes
static bool genieOptionWas;
static const char* parName(void) {
	return parIsGenie ? "Game Genie" : "Pro Action Replay";
}
static char parFolder[4096], parSelf[PAR_NAME];
// The game on top (its file name; empty without one), its save file and that as last written
static char parTop[PAR_NAME];
// The game loaded as content (its file name; empty without one)
static char contentName[PAR_NAME];
static char parSave[4096];
static uint8_t* parSaveKept;
static size_t parSaveSize;
static unsigned parSaveFrames;
static uint8_t* parImage;
static size_t parImageSize;

// RetroArch's cheats for the game on top, for the five Cheat Code options
enum { CART_SLOTS = 5, CART_CHEATS = RETRO_NUM_CORE_OPTION_VALUES_MAX - 2 };
struct CartCheat {
	char name[96], code[128];
	int count;
	struct Cheat codes[CART_SLOTS];
};
static struct CartCheat cartCheats[CART_CHEATS];
static size_t cartCheatCount;
static struct retro_core_option_v2_definition* cartCodeOptions[CART_SLOTS];
// The codes chosen as the cartridge last started, a slot each
static char cartCodesIn[CART_SLOTS][128];
static bool parPutGame(const char* name);
static const char* parChosen(void);
static void parWriteSave(bool now);
static void setMemoryMaps(void);
static void svShowGame(void);
static void handheldCheatsApply(void);
static unsigned gamateWidth(void);
static unsigned gamateHeight(void);

// The Supervision's and Gamate's cheats: RAM bytes written before each frame
enum { HANDHELD_CHEATS = 64 };
static struct {
	uint16_t address;
	uint8_t value;
} handheldCheats[HANDHELD_CHEATS];
static int handheldCheatCount;

static uint16_t palette565[512];

#ifdef __aarch64__
#include <arm_neon.h>

// The same, split into low and high bytes, 64 entries (one emphasis) per four-register table lookup
static uint8_t palette565Low[512], palette565High[512];

// The picture's palette indices to RGB565, sixteen pixels at a time while they share their emphasis
static void convertPicture(uint16_t* out, const uint16_t* in, int count) {
	int emphasis = -1;
	uint8x16x4_t low, high;
	int i = 0;
	for (; i + 16 <= count; i += 16) {
		uint16x8_t const a = vld1q_u16(in + i), b = vld1q_u16(in + i + 8);
		int const first = (in[i] >> 6) & 7;
		uint16x8_t const mask = vdupq_n_u16(0x1C0), want = vdupq_n_u16(first << 6);
		uint16x8_t const same = vandq_u16(vceqq_u16(vandq_u16(a, mask), want), vceqq_u16(vandq_u16(b, mask), want));
		if (vminvq_u16(same) != 0xFFFF) {
			for (int k = 0; k < 16; k++) {
				out[i + k] = palette565[in[i + k] & 0x1FF];
			}
			continue;
		}
		if (first != emphasis) {
			emphasis = first;
			low = vld1q_u8_x4(palette565Low + first * 64);
			high = vld1q_u8_x4(palette565High + first * 64);
		}
		uint8x16_t const index = vandq_u8(vcombine_u8(vmovn_u16(a), vmovn_u16(b)), vdupq_n_u8(0x3F));
		uint8x16x2_t pixels = { { vqtbl4q_u8(low, index), vqtbl4q_u8(high, index) } };
		vst2q_u8((uint8_t*) (out + i), pixels);
	}
	for (; i < count; i++) {
		out[i] = palette565[in[i] & 0x1FF];
	}
}
#else
static void convertPicture(uint16_t* out, const uint16_t* in, int count) {
	for (int i = 0; i < count; i++) {
		out[i] = palette565[in[i] & 0x1FF];
	}
}
#endif
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
#define DEVICE_ID_PAD RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1)

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
enum { TURBO_FEWEST = 2, TURBO_MOST = 120 };

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

enum { AIM_LIGHT_GUN, AIM_MOUSE, AIM_STICK, AIM_STICK_PAN };
enum { SENSOR_OFF = -1, SENSOR_ACCELEROMETER, SENSOR_GYROSCOPE };

static struct OptionChoice aimChoices[] = {
	{ "Light gun", AIM_LIGHT_GUN },
	{ "Mouse", AIM_MOUSE },
	{ "Right stick (absolute)", AIM_STICK },
	{ "Right stick (pan)", AIM_STICK_PAN },
};

static struct OptionChoice sensorChoices[] = {
	{ "Off", SENSOR_OFF },
	{ "Gyroscope", SENSOR_GYROSCOPE },
	{ "Accelerometer", SENSOR_ACCELEROMETER },
};

static struct OptionChoice invertChoices[] = {
	{ "Off", 0 },
	{ "Horizontal", 1 },
	{ "Vertical", 2 },
	{ "Both", 3 },
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

// A cart sound chip voice's volume and position, applied with Cart Sound Volume's
#define VOICE_OPTION(key, name) \
	{ "armnes_volume_" key, name " Volume", NULL, "One voice of the cart's sound chip, on top of Cart Sound Volume.", NULL, \
		"audio", VOLUME_VALUES, "100%" }, \
	{ "armnes_pan_" key, name " Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" }

// The voices' option keys, in enum Voice's order (the NSF's Disk System has none)
static const char* const voiceKeys[VOICE_FDS] = {
	"vrc6_pulse1",
	"vrc6_pulse2",
	"vrc6_saw",
	"vrc7_1",
	"vrc7_2",
	"vrc7_3",
	"vrc7_4",
	"vrc7_5",
	"vrc7_6",
	"5b_a",
	"5b_b",
	"5b_c",
	"n163_1",
	"n163_2",
	"n163_3",
	"n163_4",
	"n163_5",
	"n163_6",
	"n163_7",
	"n163_8",
	"mmc5_pulse1",
	"mmc5_pulse2",
	"mmc5_pcm",
};

// A cart sound chip voice's switch under Channel Selection, shown only for its chip
#define CHANNEL_OPTION(key, name) \
	{ "armnes_channel_" key, name, NULL, NULL, NULL, "audio", CHANNEL_VALUES, "On" }
#define SHOWN_VALUES { { "Shown", NULL }, { "Hidden", NULL }, { NULL, NULL } }
#define CHANNEL_VALUES { { "On", NULL }, { "Off", NULL }, { NULL, NULL } }

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
	{ "armnes_ppu", "PPU Accuracy", NULL,
		"Fast draws everything games show, skipping timing details no known game relies on. Balanced "
		"runs the picture chip dot by dot as the hardware does and passes every accuracy test, at under "
		"twice Fast's cost. Exact models it cycle for cycle, many times slower: for desktops. Takes "
		"effect when a game is loaded.",
		NULL, "system", { { "Fast", NULL }, { "Balanced", NULL }, { "Exact", NULL }, { NULL, NULL } }, "Fast" },
	{ "armnes_overclock", "Overclock", NULL,
		"Extra CPU time each frame, which removes most slowdown without changing game speed. It's added "
		"either between frames or right after the picture is drawn. A few games misbehave; for those, "
		"try the other one.",
		NULL, "system", { { "Off", NULL }, { "2x (vblank)", "2x (between frames)" }, { "3x (vblank)", "3x (between frames)" },
		{ "4x (vblank)", "4x (between frames)" }, { "2x (after the picture)", NULL }, { "3x (after the picture)", NULL },
		{ NULL, NULL } }, "Off" },
	{ "armnes_fds_fast", "Disk System Fast Loading", NULL,
		"Runs up to 16 times as fast while the disk drive reads or writes, without its sound.",
		NULL, "system", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_game_genie", "Game Genie", NULL,
		"Plugs in the Game Genie, so its code screen comes before the game. Needs [BIOS] Game Genie (USA, "
		"Europe) (Unl).unh or gamegenie.nes in the system (BIOS) folder. Cheat Code 1-3 list RetroArch's "
		"cheats for the game. Changing it restarts the console.",
		NULL, "system", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_par_game", "Game on Top", NULL,
		"With a Pro Action Replay or Game Genie loaded as the game: the game plugged into it, from the same "
		"folder (most recently played first). Changing it restarts the console.",
		NULL, "system", { { "none", "None" }, { NULL, NULL } }, "none" },
	{ "armnes_cart_code_1", "Cheat Code 1", NULL,
		"One of RetroArch's cheats for the game, entered into the cheat cartridge as if typed on its "
		"screen: up to 3 codes on the Game Genie, 5 on the Pro Action Replay. Kept for each game. Changing "
		"it restarts the console.", NULL, "system", { { "none", "None" }, { NULL, NULL } }, "none" },
	{ "armnes_cart_code_2", "Cheat Code 2", NULL,
		NULL, NULL, "system", { { "none", "None" }, { NULL, NULL } }, "none" },
	{ "armnes_cart_code_3", "Cheat Code 3", NULL,
		NULL, NULL, "system", { { "none", "None" }, { NULL, NULL } }, "none" },
	{ "armnes_cart_code_4", "Cheat Code 4", NULL,
		NULL, NULL, "system", { { "none", "None" }, { NULL, NULL } }, "none" },
	{ "armnes_cart_code_5", "Cheat Code 5", NULL,
		NULL, NULL, "system", { { "none", "None" }, { NULL, NULL } }, "none" },
	{ "armnes_gm_link", "Link Mode", NULL,
		"The Gamate's link cable, for its two-player games. Together, for netplay: both Gamates on every device, "
		"the second on controller 2 or the second player; changing it restarts them. Separate: one Gamate per "
		"device over netplay's packets, set when the game starts; over Wi-Fi most link games play badly this "
		"way. Off: one Gamate.",
		NULL, "system", { { "together", "Together" }, { "separate", "Separate" }, { "off", "Off" }, { NULL, NULL } }, "off" },
	{ "armnes_gm_link_timing", "Gamate Link Timing", NULL,
		"Standard: smooth over Wi-Fi, for games that wait for each other. Exact: in step like a real cable, "
		"slower over Wi-Fi. Both players pick the same; applies when the game starts.",
		NULL, "system", { { "standard", "Standard" }, { "exact", "Exact" }, { NULL, NULL } }, "standard" },
	{ "armnes_gm_link_screen", "Screen Layout", NULL,
		"How the screens are arranged. R2 changes it while playing.",
		NULL, "system", { { "side by side", "Side by Side" }, { "top and bottom", "Top and Bottom" },
		{ "player 1", "Player 1 Only" }, { "player 2", "Player 2 Only" }, { NULL, NULL } }, "side by side" },
	{ "armnes_gm_link_l2", "L2 Button", NULL,
		"Outside netplay, L2 fast-forwards or goes back a layout. In netplay it always goes back.",
		NULL, "system", { { "fast forward", "Fast-Forward" }, { "previous screens", "Previous Layout" }, { NULL, NULL } },
		"fast forward" },
	{ "armnes_gm_r2_follows", "Screen Layout Control", NULL,
		"In netplay, whose L2 and R2 change this device's screens. Auto: this device's player.",
		NULL, "system", { { "auto", "Auto" }, { "1", "Player 1" }, { "2", "Player 2" }, { NULL, NULL } }, "auto" },
	{ "armnes_gm_switch_controller", "Switch Controller Combo", NULL,
		"Moves controller 1 to the next console (not in netplay).",
		NULL, "system",
		{ { "start l2 r2", "Start + L2 + R2" }, { "l r a b", "L + R + A + B" }, { "l3 r3", "L3 + R3" }, { "start l3", "Start + L3" },
		{ "start r3", "Start + R3" }, { "off", "Off" }, { NULL, NULL } }, "start l2 r2" },
	{ "armnes_gm_player_numbers", "Show Player Numbers", NULL,
		"Each player's number, faintly, at the top left of their screen.",
		NULL, "system", { { "disabled", "Off" }, { "enabled", "On" }, { NULL, NULL } }, "enabled" },
	{ "armnes_sv_game", "Multicart Game", NULL,
		"The game a Supervision multicart without a menu plays. Reset moves on to the next one. Changing it "
		"restarts the console.",
		NULL, "system", { { "1", "Game 1" }, { "2", "Game 2" }, { "3", "Game 3" }, { "4", "Game 4" }, { "5", "Game 5" },
		{ "6", "Game 6" }, { "7", "Game 7" }, { "8", "Game 8" }, { NULL, NULL } }, "1" },
	{ "armnes_jumper", "Cart Jumper", NULL,
		"The cart's solder pads or DIP switches, which pick its menu, games, language or timing. Auto uses "
		"the setting known for the dump. Changing it restarts the console.",
		NULL, "system", { { "Auto", NULL }, { "0", NULL }, { "1", NULL }, { "2", NULL }, { "3", NULL }, { "4", NULL }, { "5", NULL }, { "6", NULL }, { "7", NULL }, { "8", NULL }, { "9", NULL }, { "10", NULL }, { "11", NULL }, { "12", NULL }, { "13", NULL }, { "14", NULL }, { "15", NULL }, { "16", NULL }, { "17", NULL }, { "18", NULL }, { "19", NULL }, { "20", NULL }, { "21", NULL }, { "22", NULL }, { "23", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_palette", "Palette", NULL,
		"The colors. armNES NTSC decodes the console's video signal; the others are measured or tuned. "
		"Custom loads nes.pal (or custom.pal) from the system folder.",
		NULL, "video", { { NULL, NULL } }, "armNES NTSC" },
	{ "armnes_sv_palette", "LCD Palette", NULL,
		"The Supervision's or Gamate's four shades: greys, the Gamate's green, or a Game Boy's screen.",
		NULL, "video", { { "Grey", NULL }, { "Game Boy", NULL }, { "Game Boy Pocket", NULL }, { "Game Boy Light", NULL },
		{ "Amber", NULL }, { "Gamate", NULL }, { NULL, NULL } }, "Grey" },
	{ "armnes_sv_ghosting", "LCD Ghosting", NULL,
		"Changes fade in over a few frames, as on the Supervision's and Gamate's slow LCDs. Flickering sprites "
		"need it.",
		NULL, "video", { { "disabled", "Off" }, { "25", "25%" }, { "half", "50%" }, { "75", "75%" }, { "enabled", "100%" },
		{ NULL, NULL } }, "disabled" },
	{ "armnes_filter", "NTSC/PAL Filter", NULL,
		"The picture as a TV decoded it: Composite, with its color fringes and blended dithering, or the "
		"sharper S-Video. It makes its own colors. Takes more CPU time. Not used on arcade boards, whose "
		"PPU outputs RGB.",
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
		"4:3 as a TV showed it, the console's own pixel shape (8:7, about 11:8 on PAL), or square pixels.",
		NULL, "video", { { "4:3", NULL }, { "8:7 PAR", "Console pixels (8:7)" }, { "1:1", "Square pixels" }, { NULL, NULL } }, "4:3" },
	{ "armnes_crop_vertical", "Crop Top and Bottom", NULL,
		"Pixels hidden at the top and bottom, as a TV's overscan hid them.",
		NULL, "video", { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "8" },
	{ "armnes_crop_horizontal", "Crop Sides", NULL,
		"Pixels hidden at the left and right edges.",
		NULL, "video", { { "0", NULL }, { "4", NULL }, { "8", NULL }, { "12", NULL }, { "16", NULL }, { NULL, NULL } }, "0" },
	{ "armnes_layer_selection", "Layer Selection", NULL,
		"Shows switches below that hide the background or sprites, to see one on its own: for ripping "
		"graphics or a closer look. Only the picture changes; the game plays on the same, so netplay, "
		"run-ahead and states aren't affected. Off shows everything.",
		NULL, "video", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_layer_background", "Background", NULL,
		"The scrolling background. Hidden, the backdrop color shows, and sprites drawn behind the "
		"background come to the front.", NULL, "video", SHOWN_VALUES, "Shown" },
	{ "armnes_layer_sprites_front", "Sprites in Front", NULL,
		"Sprites drawn over the background: characters, enemies, shots.", NULL, "video", SHOWN_VALUES, "Shown" },
	{ "armnes_layer_sprites_behind", "Sprites Behind the Background", NULL,
		"Sprites a game puts behind the background, showing only where it's empty.", NULL, "video", SHOWN_VALUES, "Shown" },
	{ "armnes_volume_pulse1", "Pulse 1 Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_pulse1", "Pulse 1 Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_pulse2", "Pulse 2 Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_pulse2", "Pulse 2 Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_triangle", "Triangle Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_triangle", "Triangle Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_noise", "Noise Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_noise", "Noise Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_dmc", "DMC (Samples) Volume", NULL, NULL, NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_dmc", "DMC (Samples) Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	{ "armnes_volume_cart", "Cart Sound Volume", NULL,
		"The sound chip in the cart or the Disk System.", NULL, "audio", VOLUME_VALUES, "100%" },
	{ "armnes_pan_cart", "Cart Sound Position", NULL, NULL, NULL, "audio", PAN_VALUES, "Center" },
	// Each voice of the cart's sound chip (enum Voice's order), shown only for its chip
	VOICE_OPTION("vrc6_pulse1", "VRC6 Pulse 1"),
	VOICE_OPTION("vrc6_pulse2", "VRC6 Pulse 2"),
	VOICE_OPTION("vrc6_saw", "VRC6 Sawtooth"),
	VOICE_OPTION("vrc7_1", "VRC7 FM 1"),
	VOICE_OPTION("vrc7_2", "VRC7 FM 2"),
	VOICE_OPTION("vrc7_3", "VRC7 FM 3"),
	VOICE_OPTION("vrc7_4", "VRC7 FM 4"),
	VOICE_OPTION("vrc7_5", "VRC7 FM 5"),
	VOICE_OPTION("vrc7_6", "VRC7 FM 6"),
	VOICE_OPTION("5b_a", "Sunsoft 5B Square A"),
	VOICE_OPTION("5b_b", "Sunsoft 5B Square B"),
	VOICE_OPTION("5b_c", "Sunsoft 5B Square C"),
	VOICE_OPTION("n163_1", "Namco 163 Wave 1"),
	VOICE_OPTION("n163_2", "Namco 163 Wave 2"),
	VOICE_OPTION("n163_3", "Namco 163 Wave 3"),
	VOICE_OPTION("n163_4", "Namco 163 Wave 4"),
	VOICE_OPTION("n163_5", "Namco 163 Wave 5"),
	VOICE_OPTION("n163_6", "Namco 163 Wave 6"),
	VOICE_OPTION("n163_7", "Namco 163 Wave 7"),
	VOICE_OPTION("n163_8", "Namco 163 Wave 8"),
	VOICE_OPTION("mmc5_pulse1", "MMC5 Pulse 1"),
	VOICE_OPTION("mmc5_pulse2", "MMC5 Pulse 2"),
	VOICE_OPTION("mmc5_pcm", "MMC5 PCM (Samples)"),
	{ "armnes_channel_selection", "Channel Selection", NULL,
		"Shows a switch below for each channel, to hear one or a few on their own: for ripping music or "
		"a closer listen. A channel switched off keeps its volume for when it's back on. Only the sound "
		"changes; the game plays on the same, so netplay, run-ahead and states aren't affected. Off plays "
		"everything.",
		NULL, "audio", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_channel_pulse1", "Pulse 1", NULL, NULL, NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_pulse2", "Pulse 2", NULL, NULL, NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_triangle", "Triangle", NULL, NULL, NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_noise", "Noise", NULL, NULL, NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_dmc", "DMC (Samples)", NULL, NULL, NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_cart", "Cart Sound", NULL, "The sound chip in the cart or the Disk System, all of it.",
		NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_sv_square1", "Square 1", NULL, "The Supervision's first square channel.", NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_sv_square2", "Square 2", NULL, "The Supervision's second square channel.", NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_sv_noise", "Noise", NULL, "The Supervision's noise channel.", NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_sv_samples", "Samples", NULL, "The Supervision's sample DMA.", NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_gm_a", "Channel A", NULL, "The Gamate's first square (with noise), on the left.", NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_gm_b", "Channel B", NULL, "The Gamate's second square (with noise), on the right.", NULL, "audio", CHANNEL_VALUES, "On" },
	{ "armnes_channel_gm_c", "Channel C", NULL, "The Gamate's third square (with noise), in the middle.", NULL, "audio", CHANNEL_VALUES, "On" },
	CHANNEL_OPTION("vrc6_pulse1", "VRC6 Pulse 1"),
	CHANNEL_OPTION("vrc6_pulse2", "VRC6 Pulse 2"),
	CHANNEL_OPTION("vrc6_saw", "VRC6 Sawtooth"),
	CHANNEL_OPTION("vrc7_1", "VRC7 FM 1"),
	CHANNEL_OPTION("vrc7_2", "VRC7 FM 2"),
	CHANNEL_OPTION("vrc7_3", "VRC7 FM 3"),
	CHANNEL_OPTION("vrc7_4", "VRC7 FM 4"),
	CHANNEL_OPTION("vrc7_5", "VRC7 FM 5"),
	CHANNEL_OPTION("vrc7_6", "VRC7 FM 6"),
	CHANNEL_OPTION("5b_a", "Sunsoft 5B Square A"),
	CHANNEL_OPTION("5b_b", "Sunsoft 5B Square B"),
	CHANNEL_OPTION("5b_c", "Sunsoft 5B Square C"),
	CHANNEL_OPTION("n163_1", "Namco 163 Wave 1"),
	CHANNEL_OPTION("n163_2", "Namco 163 Wave 2"),
	CHANNEL_OPTION("n163_3", "Namco 163 Wave 3"),
	CHANNEL_OPTION("n163_4", "Namco 163 Wave 4"),
	CHANNEL_OPTION("n163_5", "Namco 163 Wave 5"),
	CHANNEL_OPTION("n163_6", "Namco 163 Wave 6"),
	CHANNEL_OPTION("n163_7", "Namco 163 Wave 7"),
	CHANNEL_OPTION("n163_8", "Namco 163 Wave 8"),
	CHANNEL_OPTION("mmc5_pulse1", "MMC5 Pulse 1"),
	CHANNEL_OPTION("mmc5_pulse2", "MMC5 Pulse 2"),
	CHANNEL_OPTION("mmc5_pcm", "MMC5 PCM (Samples)"),
	{ "armnes_turbo_period", "Turbo Button Period", NULL,
		"Frames per press while a turbo button is held: X for A, Y for B.",
		NULL, "input", { { NULL, NULL } }, "4" },
	{ "armnes_opposing", "Allow Opposing Directions", NULL,
		"Left and right (or up and down) held together, which a real pad can't do. A few games glitch.",
		NULL, "input", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_left_stick_dpad", "Left Stick as D-Pad", NULL, "The left stick presses the D-pad too. Off: the game ignores the left stick.",
		NULL, "input", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "On" },
	{ "armnes_four_players", "4 Players", NULL,
		"Players 3 and 4 through the NES Four Score or the Famicom's adapter. Auto follows an NES 2.0 header.",
		NULL, "input", { { "Auto", NULL }, { "Off", NULL }, { "Four Score", NULL }, { "Famicom adapter", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_expansion", "Famicom Expansion Port", NULL,
		"The paddle follows the mouse, the Power Pad takes controller 1's buttons, and the Famicom "
		"keyboard takes your keyboard (turn on Game Focus). Auto follows an NES 2.0 header.",
		NULL, "input", { { "Auto", NULL }, { "None", NULL }, { "Paddle", NULL }, { "Power Pad", NULL },
		{ "Keyboard", NULL }, { NULL, NULL } }, "Auto" },
	{ "armnes_zapper_aim", "Zapper Input", "Input",
		"Right stick: the stick moves the crosshair, or points it where the stick points; R fires, L "
		"shoots off the screen, R3 centers. Light gun: a light gun or the mouse pointer aims. Mouse: the "
		"mouse moves a crosshair; the left button fires, the right shoots off the screen. Touching the "
		"screen always aims and fires.",
		NULL, "zapper", { { "Right stick (pan)", "Right stick (moves)" }, { "Right stick (absolute)", "Right stick (points)" },
		{ "Light gun", NULL }, { "Mouse", NULL }, { NULL, NULL } }, "Right stick (pan)" },
	{ "armnes_zapper_sensitivity", "Zapper Sensitivity", "Sensitivity",
		"How fast the crosshair moves. Holding L2 aims finer the further it's pulled.",
		NULL, "zapper", { { "25%", NULL }, { "50%", NULL }, { "75%", NULL }, { "100%", NULL }, { "150%", NULL },
		{ "200%", NULL }, { "300%", NULL }, { NULL, NULL } }, "100%" },
	{ "armnes_stick_invert", "Zapper Stick Invert", "Stick Invert",
		"Turns the right stick's aim around, side to side, up and down, or both.",
		NULL, "zapper", { { "Off", NULL }, { "Horizontal", NULL }, { "Vertical", NULL }, { "Both", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_motion_sensor", "Zapper Motion", "Motion",
		"The device's motion moves the crosshair from where the right stick aims it: turning it with the "
		"gyroscope, or tilting it with the accelerometer. R3 takes how the device is held as the middle.",
		NULL, "zapper", { { "Off", NULL }, { "Gyroscope", NULL }, { "Accelerometer", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_motion_range", "Zapper Motion Range", "Motion Range",
		"How far the motion moves the crosshair: the whole screen, to aim by motion alone, or a quarter of "
		"it either way, for fine aim on top of the stick.",
		NULL, "zapper", { { "Whole screen", NULL }, { "Fine", NULL }, { NULL, NULL } }, "Whole screen" },
	{ "armnes_motion_sensitivity", "Zapper Motion Sensitivity", "Motion Sensitivity",
		"How far the device turns or tilts to move the crosshair all the way: about 20° at 100%, less the "
		"higher it is.",
		NULL, "zapper", { { "25%", NULL }, { "50%", NULL }, { "75%", NULL }, { "100%", NULL }, { "150%", NULL },
		{ "200%", NULL }, { "300%", NULL }, { NULL, NULL } }, "100%" },
	{ "armnes_motion_invert", "Zapper Motion Invert", "Motion Invert",
		"Turns the motion's aim around, side to side, up and down, or both, for a sensor that reads one "
		"way backwards or to taste.",
		NULL, "zapper", { { "Off", NULL }, { "Horizontal", NULL }, { "Vertical", NULL }, { "Both", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_motion_rotation", "Zapper Motion Sensor Rotation", "Motion Sensor Rotation",
		"Turns the motion sensor to match the screen if aiming goes the wrong way. Usually 0° in the "
		"standalone app, often 90° in RetroArch.",
		NULL, "zapper", { { "0", "0°" }, { "90", "90°" }, { "180", "180°" }, { "270", "270°" }, { NULL, NULL } }, "0" },
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
	{ "armnes_vs_dip9", "Second Board DIP Switch 1", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip10", "Second Board DIP Switch 2", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip11", "Second Board DIP Switch 3", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip12", "Second Board DIP Switch 4", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip13", "Second Board DIP Switch 5", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip14", "Second Board DIP Switch 6", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip15", "Second Board DIP Switch 7", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ "armnes_vs_dip16", "Second Board DIP Switch 8", NULL, NULL, NULL, "vs", { { "Off", NULL }, { "On", NULL }, { NULL, NULL } }, "Off" },
	{ NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL },
};

static struct retro_core_option_v2_category categories[] = {
	{ "system", "System", "The console's region and speed, the Disk System, cart settings and cheat cartridges." },
	{ "video", "Video", "Colors, the TV filter, sprites, the picture's shape and edges, and which layers it shows." },
	{ "audio", "Audio", "Each channel's volume and where it sits between the speakers (the console mixes in mono), and which channels play." },
	{ "input", "Input", "Turbo, the D-pad, extra players and the expansion port." },
	{ "zapper", "Zapper", "How the light gun aims: the stick, a light gun, the mouse or the device's motion. Shown when a Zapper is plugged in." },
	{ "vs", "VS. System", "The arcade board's PPU, controller wiring and DIP switches (what each game's switches do is in its manual or MAME's notes). Select puts in a coin, controller 1's L3 is the service button. A DualSystem's second board takes controllers 3 and 4 (controller 3's L3 for its service button)." },
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

// Under Channel Selection, whether a channel or voice (its key) is switched off
static bool muted(const char* key) {
	char option[48];
	snprintf(option, sizeof(option), "armnes_channel_%s", key);
	return optionIs(environ_cb, "armnes_channel_selection", "On") && optionIs(environ_cb, option, "Off");
}

// A side's gain falls as the channel moves to the other side
static void readSoundOptions(void) {
	static const char* keys[MIX_CHANNELS] = { "pulse1", "pulse2", "triangle", "noise", "dmc", "cart" };
	nes.mixer.custom = false;
	for (int c = 0; c < MIX_CHANNELS; c++) {
		char key[32];
		snprintf(key, sizeof(key), "armnes_volume_%s", keys[c]);
		int volume = muted(keys[c]) ? 0 : optionChoice(environ_cb, key, volumeChoices, 11, 100);
		snprintf(key, sizeof(key), "armnes_pan_%s", keys[c]);
		int pan = optionChoice(environ_cb, key, panChoices, 9, 0);
		nes.mixer.gain[c][0] = volume / 100.0f * (pan > 0 ? 1 - pan / 100.0f : 1);
		nes.mixer.gain[c][1] = volume / 100.0f * (pan < 0 ? 1 + pan / 100.0f : 1);
		nes.mixer.custom |= volume != 100 || pan != 0;
	}
	// Each cart voice's, on top of the cart's
	float voiceGains[VOICES][2];
	bool voicesCustom = false;
	for (int v = 0; v < VOICES; v++) {
		int volume = 100, pan = 0;
		if (v < VOICE_FDS) {
			char key[48];
			snprintf(key, sizeof(key), "armnes_volume_%s", voiceKeys[v]);
			volume = muted(voiceKeys[v]) ? 0 : optionChoice(environ_cb, key, volumeChoices, 11, 100);
			snprintf(key, sizeof(key), "armnes_pan_%s", voiceKeys[v]);
			pan = optionChoice(environ_cb, key, panChoices, 9, 0);
		}
		voiceGains[v][0] = volume / 100.0f * (pan > 0 ? 1 - pan / 100.0f : 1);
		voiceGains[v][1] = volume / 100.0f * (pan < 0 ? 1 + pan / 100.0f : 1);
		voicesCustom |= volume != 100 || pan != 0;
	}
	apuSetVoiceGains(voiceGains, voicesCustom);
	nes.mixer.custom |= voicesCustom;
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
	bool const layers = optionIs(environ_cb, "armnes_layer_selection", "On");
	nesHiddenLayers = !layers ? 0 :
		(optionIs(environ_cb, "armnes_layer_background", "Hidden") ? LAYER_BACKGROUND : 0) |
		(optionIs(environ_cb, "armnes_layer_sprites_front", "Hidden") ? LAYER_SPRITES_FRONT : 0) |
		(optionIs(environ_cb, "armnes_layer_sprites_behind", "Hidden") ? LAYER_SPRITES_BEHIND : 0);
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
#ifdef __aarch64__
	for (int entry = 0; entry < 512; entry++) {
		palette565Low[entry] = (uint8_t) palette565[entry];
		palette565High[entry] = palette565[entry] >> 8;
	}
#endif
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
	for (struct retro_core_option_v2_definition* d = optionDefinitions; d->key; d++) {
		if (!strcmp(d->key, "armnes_turbo_period")) {
			static char periods[TURBO_MOST - TURBO_FEWEST + 1][4];
			static char labels[TURBO_MOST - TURBO_FEWEST + 1][12];
			d->values[0] = (struct retro_core_option_value) { "off", "Off" };
			for (int i = 0; i <= TURBO_MOST - TURBO_FEWEST; i++) {
				snprintf(periods[i], sizeof(periods[i]), "%d", TURBO_FEWEST + i);
				snprintf(labels[i], sizeof(labels[i]), "%d frames", TURBO_FEWEST + i);
				d->values[i + 1] = (struct retro_core_option_value) { periods[i], labels[i] };
			}
		}
	}
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
	for (struct retro_core_option_v2_definition* d = optionDefinitions; d->key; d++) {
		if (!strcmp(d->key, "armnes_par_game")) {
			parOption = d;
		}
		for (int i = 0; i < CART_SLOTS; i++) {
			char key[32];
			snprintf(key, sizeof(key), "armnes_cart_code_%d", i + 1);
			if (!strcmp(d->key, key)) {
				cartCodeOptions[i] = d;
			}
		}
	}
	// Without a game: the console with no cartridge (not the arcade core)
#ifdef ARMNES_ARCADE
	bool noGame = false;
#else
	bool noGame = true;
#endif
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &noGame);
#ifdef ARMNES_ARCADE
	// RetroArch 1.22.2 extracts archives despite block_extract unless asked for their path
	static const struct retro_system_content_info_override zips[] = { { "zip|7z", true, false }, { NULL, false, false } };
	cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void*) zips);
#endif
	// Auto, the frontend's default, plugs in what the game's header names
	static const struct retro_controller_description ports[] = {
		{ "Auto", RETRO_DEVICE_JOYPAD },
		{ "Controller", DEVICE_ID_PAD },
		{ "Zapper", DEVICE_ID_ZAPPER },
		{ "Paddle", DEVICE_ID_PADDLE },
		{ "Power Pad", DEVICE_ID_POWER_PAD },
		{ "None", RETRO_DEVICE_NONE },
	};
	static const struct retro_controller_description pads[] = {
		{ "Controller", RETRO_DEVICE_JOYPAD },
	};
	static const struct retro_controller_info info[] = {
		{ ports, 6 }, { ports, 6 }, { pads, 1 }, { pads, 1 }, { NULL, 0 },
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
	info->library_version = "1.4.1";
	info->need_fullpath = false;
#ifdef ARMNES_ARCADE
	// The arcade build takes archives unopened, so MAME's sets (and their parents') open here
	info->library_name = "armNES Arcade";
	info->valid_extensions = "zip|7z|nes|fds|unf|unif|nsf|sv|ws|bin";
	info->block_extract = true;
#else
	// RetroArch unpacks archives itself, so achievements identify the game inside; a frontend that
	// hands over a whole archive (an arcade set) still has it opened here
	info->library_name = "armNES";
	info->valid_extensions = "nes|fds|unf|unif|nsf|sv|ws|bin";
	info->block_extract = false;
#endif
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	memset(info, 0, sizeof(*info));
	if (svLoaded) {
		info->geometry = (struct retro_game_geometry) { SV_WIDTH, SV_HEIGHT, SV_WIDTH, SV_HEIGHT, 1.0f };
		info->timing.fps = SV_FPS;
		info->timing.sample_rate = SV_SOUND_RATE;
		return;
	}
	if (gmLoaded) {
		info->geometry = (struct retro_game_geometry) { gamateWidth(), gamateHeight(), 2 * GM_WIDTH, 2 * GM_HEIGHT,
			(float) gamateWidth() / gamateHeight() };
		info->timing.fps = GM_FPS;
		info->timing.sample_rate = GM_SOUND_RATE;
		return;
	}
	geometry(&info->geometry);
	info->timing.fps = nes.region == REGION_NTSC ? 60.0988 : (nes.region == REGION_PAL ? 50.0070 : 50.0070);
	info->timing.sample_rate = 48000;
}

static uint8_t portDevice[2];
static bool portChosen[2];

// What's plugged into a port: the frontend's choice, or with Auto (RetroArch's default) or until it
// makes one (the standalone app never does), what an NES 2.0 header names: a Zapper (a VS. System's in the first), two, a Power Pad or an
// Arkanoid paddle.
static uint8_t portType(unsigned port) {
	if (portChosen[port] || !loaded || !nes.cart.nes2) {
		return portDevice[port];
	}
	switch (nes.cart.header[15] & 0x3F) {
	case 0x07: return port ? DEVICE_PAD : DEVICE_ZAPPER;
	case 0x08: return port ? DEVICE_ZAPPER : DEVICE_PAD;
	case 0x09: return DEVICE_ZAPPER;
	case 0x0B:
	case 0x0C: return port ? DEVICE_POWER_PAD : DEVICE_PAD;
	case 0x0F: return port ? DEVICE_PADDLE : DEVICE_PAD;
	default: return portDevice[port];
	}
}

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

static bool genieOverFits(void);
static bool genieOverPut(bool on);

// Shows only the options that do something for the game loaded, with the controllers plugged in and
// the other options' values. Without a game, all of them.
static bool showOptions(void) {
	// A Supervision or Gamate game: the controller's options and its own
	if (svLoaded || gmLoaded) {
		for (const struct retro_core_option_v2_definition* d = optionDefinitions; d->key; d++) {
			bool const channel = !strncmp(d->key, svLoaded ? "armnes_channel_sv_" : "armnes_channel_gm_", 18);
			bool const show = !strcmp(d->key, "armnes_left_stick_dpad") || !strcmp(d->key, "armnes_turbo_period") ||
			                  !strcmp(d->key, "armnes_opposing") || !strcmp(d->key, "armnes_sv_palette") ||
			                  !strcmp(d->key, "armnes_sv_ghosting") || !strcmp(d->key, "armnes_channel_selection") ||
			                  (channel && optionIs(environ_cb, "armnes_channel_selection", "On")) ||
			                  (!strcmp(d->key, "armnes_sv_game") && svLoaded && supervision.games > 1) ||
			                  (!strcmp(d->key, "armnes_gm_link") && gmLoaded) ||
			                  ((!strcmp(d->key, "armnes_gm_link_screen") || !strcmp(d->key, "armnes_gm_link_l2") ||
			                    !strcmp(d->key, "armnes_gm_r2_follows") || !strcmp(d->key, "armnes_gm_switch_controller") ||
			                    !strcmp(d->key, "armnes_gm_player_numbers")) && gmLoaded && gmLinked) ||
			                  (!strcmp(d->key, "armnes_gm_link_timing") && gmLoaded && gmSeparate);
			struct retro_core_option_display display = { d->key, show };
			environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
		}
		return true;
	}
	bool const vs = !loaded || nes.cart.vs;
	bool const dual = !loaded || nes.dual;
	bool const zapper = !loaded || portType(0) == DEVICE_ZAPPER || portType(1) == DEVICE_ZAPPER;
	bool const motion = zapper && (optionIs(environ_cb, "armnes_zapper_aim", "Right stick (absolute)") ||
		optionIs(environ_cb, "armnes_zapper_aim", "Right stick (pan)"));
	bool const motionOn = motion && !optionIs(environ_cb, "armnes_motion_sensor", "Off");
	bool const layers = optionIs(environ_cb, "armnes_layer_selection", "On");
	bool const channels = optionIs(environ_cb, "armnes_channel_selection", "On");
	struct { const char* key; bool show; } const shown[] = {
		{ "armnes_fds_fast", !loaded || nes.cart.disk },
		{ "armnes_game_genie", !loaded || genieOver || genieOverFits() },
		{ "armnes_par_game", !loaded || parLoaded },
		{ "armnes_cart_code_1", !loaded || ((parLoaded || genieOver) && *parTop) },
		{ "armnes_cart_code_2", !loaded || ((parLoaded || genieOver) && *parTop) },
		{ "armnes_cart_code_3", !loaded || ((parLoaded || genieOver) && *parTop) },
		{ "armnes_cart_code_4", !loaded || (parLoaded && *parTop && !parIsGenie) },
		{ "armnes_cart_code_5", !loaded || (parLoaded && *parTop && !parIsGenie) },
		{ "armnes_jumper", !loaded || nes.hasJumper || nes.cart.jumper >= 0 },
		{ "armnes_sv_palette", !loaded },
		{ "armnes_sv_ghosting", !loaded },
		{ "armnes_sv_game", !loaded },
		{ "armnes_gm_link", !loaded },
		{ "armnes_gm_link_screen", !loaded },
		{ "armnes_gm_link_l2", !loaded },
		{ "armnes_gm_r2_follows", !loaded },
		{ "armnes_gm_switch_controller", !loaded },
		{ "armnes_gm_player_numbers", !loaded },
		{ "armnes_gm_link_timing", !loaded },
		{ "armnes_channel_sv_square1", !loaded && channels },
		{ "armnes_channel_sv_square2", !loaded && channels },
		{ "armnes_channel_sv_noise", !loaded && channels },
		{ "armnes_channel_sv_samples", !loaded && channels },
		{ "armnes_channel_gm_a", !loaded && channels },
		{ "armnes_channel_gm_b", !loaded && channels },
		{ "armnes_channel_gm_c", !loaded && channels },
		{ "armnes_palette", !optionIs(environ_cb, "armnes_filter", "Composite") && !optionIs(environ_cb, "armnes_filter", "S-Video") },
		{ "armnes_scanlines", optionIs(environ_cb, "armnes_filter", "Composite") || optionIs(environ_cb, "armnes_filter", "S-Video") },
		{ "armnes_volume_cart", !loaded || cartSound() },
		{ "armnes_pan_cart", !loaded || cartSound() },
		{ "armnes_zapper_aim", zapper },
		{ "armnes_zapper_sensitivity", zapper && !optionIs(environ_cb, "armnes_zapper_aim", "Light gun") },
		{ "armnes_stick_invert", motion },
		{ "armnes_motion_sensor", motion },
		{ "armnes_motion_invert", motionOn },
		{ "armnes_motion_range", motionOn },
		{ "armnes_motion_sensitivity", motionOn },
		{ "armnes_motion_rotation", motionOn },
		{ "armnes_vs_ppu", vs },
		{ "armnes_vs_swap", vs },
		{ "armnes_vs_screens", dual },
		{ "armnes_layer_background", layers },
		{ "armnes_layer_sprites_front", layers },
		{ "armnes_layer_sprites_behind", layers },
		{ "armnes_channel_pulse1", channels },
		{ "armnes_channel_pulse2", channels },
		{ "armnes_channel_triangle", channels },
		{ "armnes_channel_noise", channels },
		{ "armnes_channel_dmc", channels },
		{ "armnes_channel_cart", channels && (!loaded || cartSound()) },
	};
	for (size_t i = 0; i < sizeof(shown) / sizeof(*shown); i++) {
		struct retro_core_option_display display = { shown[i].key, shown[i].show };
		environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
	}
	// A sound chip's voices, for the cart or NSF that has it
	int const nsf = loaded ? nsfChips(&nes) : 0, mapper = nes.cart.mapper;
	bool const chips[] = {
		!loaded || mapper == 24 || mapper == 26 || (nsf & 1),
		!loaded || mapper == 85 || (nsf & 2),
		!loaded || mapper == 69 || (nsf & 32),
		!loaded || (mapper == 19 && nes.cart.submapper != 1 && nes.cart.submapper != 2) || (nsf & 16),
		!loaded || mapper == 5 || (nsf & 8),
	};
	for (int v = 0; v < VOICE_FDS; v++) {
		int const chip = v < VOICE_VRC7_1 ? 0 : v < VOICE_5B_A ? 1 : v < VOICE_N163_1 ? 2 : v < VOICE_MMC5_PULSE1 ? 3 : 4;
		for (int side = 0; side < 2; side++) {
			char key[48];
			snprintf(key, sizeof(key), side ? "armnes_pan_%s" : "armnes_volume_%s", voiceKeys[v]);
			struct retro_core_option_display display = { key, chips[chip] };
			environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &display);
		}
		char key[48];
		snprintf(key, sizeof(key), "armnes_channel_%s", voiceKeys[v]);
		struct retro_core_option_display display = { key, channels && chips[chip] };
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
		portChosen[port] = device != RETRO_DEVICE_JOYPAD;
		showOptions();
	}
}

void retro_reset(void) {
	if (svLoaded) {
		svReset(&supervision);
		svShowGame();
		return;
	}
	if (gmLoaded) {
		gmReset(&gamate);
		gmReset(&gamateTwo);
		return;
	}
	if (loaded) {
		if (nes.dual) {
			nesResetDual(&nes, false);
		} else {
			nesReset(&nes, false);
		}
	}
}

// A save the board keeps partly in its own RAM (batteryRam): one block for the frontend, the
// cartridge's battery RAM then the board's. What the frontend loads goes in before the first frame;
// after that the machine's copy comes back out before each.
static uint8_t* saveBlock;
static bool saveBlockIn;

static uint8_t* boardBatteryRam(size_t* size) {
	return loaded && nes.cart.battery && nes.board->batteryRam ? nes.board->batteryRam(&nes, size) : NULL;
}

static void saveBlockSync(void) {
	size_t size;
	uint8_t* ram = saveBlock ? boardBatteryRam(&size) : NULL;
	if (!ram) {
		return;
	}
	uint32_t const cart = nes.cart.batterySize;
	if (saveBlockIn) {
		memcpy(saveBlock, nes.cart.wram, cart);
		memcpy(saveBlock + cart, ram, size);
	} else {
		memcpy(nes.cart.wram, saveBlock, cart);
		memcpy(ram, saveBlock + cart, size);
		saveBlockIn = true;
	}
}

static int turboPeriod = 4;
static bool allowOpposing;
static bool leftStickDpad;
static unsigned turboFrame;
static int fourPlayers, expansion, zapperAim, zapperSpeed = 100;
static int motionSensor, motionSpeed = 100, motionRotation, stickInvert, motionInvert;
static bool motionWhole;
static int vsSwap;
static unsigned coinFrames[4];
static bool fdsFast;

// Names each button for the frontend's controls menu, the left stick as the D-pad while Left Stick as
// D-Pad is on.
static void giveDescriptors(void) {
	static const struct { unsigned id; const char* name; } buttons[] = {
		{ RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" }, { RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
		{ RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" }, { RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
		{ RETRO_DEVICE_ID_JOYPAD_B, "B" }, { RETRO_DEVICE_ID_JOYPAD_A, "A" },
		{ RETRO_DEVICE_ID_JOYPAD_SELECT, "Select (VS. System: Coin)" }, { RETRO_DEVICE_ID_JOYPAD_START, "Start" },
		{ RETRO_DEVICE_ID_JOYPAD_X, "Turbo A" }, { RETRO_DEVICE_ID_JOYPAD_Y, "Turbo B" },
	};
	static struct retro_input_descriptor descriptors[4 * 12 + 10];
	unsigned count = 0;
	for (unsigned port = 0; port < 4; port++) {
		for (unsigned i = 0; i < 10; i++) {
			descriptors[count++] = (struct retro_input_descriptor) { port, RETRO_DEVICE_JOYPAD, 0, buttons[i].id, buttons[i].name };
		}
		if (leftStickDpad) {
			descriptors[count++] = (struct retro_input_descriptor) { port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "D-Pad Left / D-Pad Right" };
			descriptors[count++] = (struct retro_input_descriptor) { port, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "D-Pad Up / D-Pad Down" };
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
}

static void readInputOptions(void) {
	const char* period = optionValue(environ_cb, "armnes_turbo_period");
	turboPeriod = period && !strcmp(period, "off") ? 0 : period ? atoi(period) : 4;
	if (turboPeriod && (turboPeriod < TURBO_FEWEST || turboPeriod > TURBO_MOST)) {
		turboPeriod = 4;
	}
	allowOpposing = optionChoice(environ_cb, "armnes_opposing", onOffChoices, 2, 0);
	bool stickDpad = optionChoice(environ_cb, "armnes_left_stick_dpad", onOffChoices, 2, 1);
	if (stickDpad != leftStickDpad) {
		leftStickDpad = stickDpad;
		giveDescriptors();
	}
	fourPlayers = optionChoice(environ_cb, "armnes_four_players", fourPlayerChoices, 4, AUTO);
	zapperAim = optionChoice(environ_cb, "armnes_zapper_aim", aimChoices, 4, AIM_STICK_PAN);
	motionRotation = optionChoice(environ_cb, "armnes_motion_rotation", rotationChoices, 4, 0);
	stickInvert = optionChoice(environ_cb, "armnes_stick_invert", invertChoices, 4, 0);
	motionInvert = optionChoice(environ_cb, "armnes_motion_invert", invertChoices, 4, 0);
	zapperSpeed = optionChoice(environ_cb, "armnes_zapper_sensitivity", sensitivityChoices, 7, 100);
	motionSensor = optionChoice(environ_cb, "armnes_motion_sensor", sensorChoices, 3, SENSOR_OFF);
	motionSpeed = optionChoice(environ_cb, "armnes_motion_sensitivity", sensitivityChoices, 7, 100);
	motionWhole = optionIs(environ_cb, "armnes_motion_range", "Whole screen");
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
	// Turbo A and B: held for the first half of each period, up to 15 frames
	if (turboPeriod && turboFrame % (unsigned) turboPeriod < (unsigned) (turboPeriod / 2 < 15 ? turboPeriod / 2 : 15)) {
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
static int sensorOn = -1;
static bool motionCentered;
static float motionX, motionY, tiltMiddleRight, tiltMiddleDown;
static int motionLastX, motionLastY;

// Turns the frontend's accelerometer or gyroscope on while a Zapper aims by the device's motion
static void useSensor(int sensor) {
	if (sensor >= 0 && !sensors.set_sensor_state && !environ_cb(RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE, &sensors)) {
		memset(&sensors, 0, sizeof(sensors));
	}
	if (!sensors.set_sensor_state || sensor == sensorOn) {
		return;
	}
	if (sensorOn >= 0) {
		sensors.set_sensor_state(0, sensorOn == SENSOR_GYROSCOPE ? RETRO_SENSOR_GYROSCOPE_DISABLE : RETRO_SENSOR_ACCELEROMETER_DISABLE, 60);
	}
	sensorOn = sensor >= 0 && sensors.set_sensor_state(0, sensor == SENSOR_GYROSCOPE ? RETRO_SENSOR_GYROSCOPE_ENABLE : RETRO_SENSOR_ACCELEROMETER_ENABLE, 60) ? sensor : -1;
	motionCentered = false;
}

// The device's motion, in right stick units to add to the stick: how far it's tilted from where it
// was held (smoothed over a few frames, as the accelerometer feels the hands too), or how far it's
// turned, since R3 was last pressed or aiming started. About 20° moves the crosshair a quarter of the
// screen at 100%, or with Whole Screen, from the middle to the edge. Nothing with no readings.
static void deviceMotion(int* x, int* y) {
	*x = *y = 0;
	if (sensorOn < 0) {
		return;
	}
	bool const gyro = sensorOn == SENSOR_GYROSCOPE;
	unsigned const first = gyro ? RETRO_SENSOR_GYROSCOPE_X : RETRO_SENSOR_ACCELEROMETER_X;
	float const sensorX = sensors.get_sensor_input(0, first);
	float const sensorY = sensors.get_sensor_input(0, first + 1);
	float const sensorZ = sensors.get_sensor_input(0, first + 2);
	if (!gyro && sensorX == 0 && sensorY == 0 && sensorZ == 0) {
		return;
	}
	// The sensor's axes turned to the screen's, for a frontend that passes them on as the device has them
	static const float turns[4][4] = { { 1, 0, 0, 1 }, { 0, -1, 1, 0 }, { -1, 0, 0, -1 }, { 0, 1, -1, 0 } };
	const float* const turn = turns[motionRotation];
	float const alongX = turn[0] * sensorX + turn[1] * sensorY, alongY = turn[2] * sensorX + turn[3] * sensorY;
	float const range = motionWhole ? 0x7FFF : 0x7FFF / 2;
	bool const center = !motionCentered || input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);
	motionCentered = true;
	if (gyro) {
		// Turning the device right moves the crosshair right, tipping its top back moves it up
		float const scale = range / (20 * 3.14159265f / 180) * motionSpeed / 100 / 60;
		if (center) {
			motionX = motionY = 0;
		}
		motionX = fmaxf(-range, fminf(range, motionX + (fabsf(alongY) > 0.003f ? alongY : 0) * scale));
		motionY = fmaxf(-range, fminf(range, motionY - (fabsf(alongX) > 0.003f ? alongX : 0) * scale));
	} else {
		// The sensor reads the push that holds the device up, so a side tipped down reads away from it
		float const right = -alongX, down = alongY;
		if (center) {
			tiltMiddleRight = right;
			tiltMiddleDown = down;
		}
		float const scale = range * 3 / 9.80665f * motionSpeed / 100;
		motionX += (fmaxf(-range, fminf(range, (right - tiltMiddleRight) * scale)) - motionX) / 3;
		motionY += (fmaxf(-range, fminf(range, (down - tiltMiddleDown) * scale)) - motionY) / 3;
	}
	*x = (int) motionX;
	*y = (int) motionY;
}

// A pan's move this frame in 256ths of a pixel: the push past the dead zone, squared, so small pushes
// aim finely and a full one crosses the screen in about a third of a second at 100%
static int panStep(int push, int speed) {
	int const dead = 0x1000, size = abs(push);
	if (size <= dead) {
		return 0;
	}
	float const amount = (float) (size - dead) / (0x7FFF - dead);
	int const step = (int) (amount * amount * 12 * 256 * speed / 25600);
	return push < 0 ? -step : step;
}

// Aim from the mouse, the stick or the device's motion: false for the light gun
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
	case AIM_STICK_PAN: {
		int x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
		int y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
		int mx, my;
		deviceMotion(&mx, &my);
		x = stickInvert & 1 ? -x : x;
		y = stickInvert & 2 ? -y : y;
		mx = motionInvert & 1 ? -mx : mx;
		my = motionInvert & 2 ? -my : my;
		if (zapperAim == AIM_STICK) {
			x = x + mx < -0x7FFF ? -0x7FFF : x + mx > 0x7FFF ? 0x7FFF : x + mx;
			y = y + my < -0x7FFF ? -0x7FFF : y + my > 0x7FFF ? 0x7FFF : y + my;
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
			// Up to 12 pixels a frame at 100%, past a small dead zone, slower the less the stick is pushed,
			// and the motion's change since the last frame
			crossX += panStep(x, speed);
			crossY += panStep(y, speed);
			crossX += (int) ((int64_t) (mx - motionLastX) * (right - left) / 0xFFFF * fine / 256);
			crossY += (int) ((int64_t) (my - motionLastY) * (bottom - top) / 0xFFFF * fine / 256);
		}
		motionLastX = mx;
		motionLastY = my;
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

// The crosshair, each of its pixels the inverse of the picture's under it (as melonDS draws its cursor)
static void drawCrosshair(int scale) {
	int cx = crossX >> 8, cy = crossY >> 8;
	for (int d = -4; d <= 4; d++) {
		int points[2][2] = { { cx + d, cy }, { cx, cy + d } };
		for (int i = d ? 0 : 1; i < 2; i++) {
			int x = points[i][0], y = points[i][1];
			if (x >= 0 && x < NES_WIDTH && y >= 0 && y < NES_HEIGHT) {
				for (int s = 0; s < scale; s++) {
					video[(y * NES_WIDTH + x) * scale + s] ^= 0xFFFF;
				}
			}
		}
	}
}

static void readDevices(void) {
	struct InputConfig* in = &nes.input;
	crossShown = false;
	in->device[0] = portType(0);
	in->device[1] = portType(1);
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
			// With a Zapper in the port, its controller still drops the coins
			bool held = (in->device[port] == DEVICE_PAD ? nes.pads[port] : readPad(port)) & 0x04;
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
	useSensor((zapperAim == AIM_STICK || zapperAim == AIM_STICK_PAN) && (in->device[0] == DEVICE_ZAPPER || in->device[1] == DEVICE_ZAPPER) ? motionSensor : SENSOR_OFF);
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

// Netplay, once it has saved or loaded a state of this game: then R2 doesn't fast-forward, as the
// other players would have to wait
static bool netplaySeen;

static void checkNetplay(void) {
	int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
	netplaySeen |= environ_cb(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context) && context == RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
}

// R2 held fast-forwards at the frontend's maximum run speed, outside netplay
static bool gamateSwitchStart(int port);

static void readFastForward(void) {
	static bool held;
	bool now = !netplaySeen && input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2) != 0;
	// Gamates together: R2 changes the screens, and L2 fast-forwards as the L2 Button option says
	if (gmLoaded && gmLinked) {
		now = false;
		for (int port = 0; port < 2; port++) {
			now |= !netplaySeen && gmL2FastForward && !gamateSwitchStart(port) &&
			       input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2) != 0;
		}
	}
	if (now == held) {
		return;
	}
	held = now;
	struct retro_fastforwarding_override override = { .ratio = -1.0f, .fastforward = now, .notification = true, .inhibit_toggle = now };
	environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &override);
}

// The Supervision's picture in a palette's four shades, which LCD Ghosting fades between: levels 0-192
// (a shade every 64). As armGBA has it, each pixel moves in linear light towards the new picture by a
// share each frame, set by the LCD's time constants to darken and to lighten (the first Game Boy's
// STN estimates, 50 and 110 ms; the Supervision's own aren't measured); 25%, 50% and 75% take that
// share of them. Its next level is read from a table, for each level now and shade wanted.
enum { SV_LEVELS = 3 * 64 + 1 };
static uint16_t svColors[SV_LEVELS];
// (Room for the widest picture: two Gamates side by side)
static uint8_t svLevel[2 * GM_WIDTH * SV_HEIGHT], svNext[SV_LEVELS][4];
static int svGhosting, svPaletteShown = -1, svGameChosen;
static bool svLevelsSet;

static void buildSupervisionFade(int strength) {
	// Each level's light, from MAME's greys
	static const int greys[4] = { 0xE0, 0xB9, 0x54, 0x12 };
	float linear[SV_LEVELS];
	for (int level = 0; level < SV_LEVELS; level++) {
		int const shade = level >> 6 < 3 ? level >> 6 : 2, within = level - shade * 64;
		float const grey = (greys[shade] * (64 - within) + greys[shade + 1] * within) / 64.0f;
		linear[level] = powf(grey / 255.0f, 2.2f);
	}
	float const frame = 1000.0f / SV_FPS;
	float const darken = 1 - expf(-frame * 4 / strength / 50), lighten = 1 - expf(-frame * 4 / strength / 110);
	for (int now = 0; now < SV_LEVELS; now++) {
		for (int shade = 0; shade < 4; shade++) {
			int const wanted = shade * 64, way = wanted > now ? 1 : -1;
			float const moved = linear[now] + (linear[wanted] - linear[now]) * (wanted > now ? darken : lighten);
			// The level nearest that light, at least one step on
			int next = now;
			while (next != wanted && fabsf(linear[next + way] - moved) <= fabsf(linear[next] - moved)) {
				next += way;
			}
			svNext[now][shade] = (uint8_t) (next == now && now != wanted ? now + way : next);
		}
	}
}

static void readSupervisionOptions(void) {
	// Lightest to darkest, as 0xRRGGBB; the first is MAME's Supervision greys, the last its Gamate
	static const uint32_t palettes[][4] = {
		{ 0xE0E0E0, 0xB9B9B9, 0x545454, 0x121212 },
		{ 0x9BBC0F, 0x8BAC0F, 0x306230, 0x0F380F },
		{ 0xC4CFA1, 0x8B956D, 0x4D533C, 0x1F1F1F },
		{ 0x00B581, 0x009A71, 0x00694A, 0x004F3B },
		{ 0xF8B800, 0xC08000, 0x704800, 0x281800 },
		{ 0x6BA64A, 0x437A63, 0x255955, 0x12424C },
	};
	static const struct OptionChoice paletteNames[] = {
		{ "Grey", 0 }, { "Game Boy", 1 }, { "Game Boy Pocket", 2 }, { "Game Boy Light", 3 }, { "Amber", 4 }, { "Gamate", 5 },
	};
	static const struct OptionChoice ghosting[] = { { "25", 1 }, { "half", 2 }, { "75", 3 }, { "enabled", 4 } };
	int const which = optionChoice(environ_cb, "armnes_sv_palette", paletteNames, 6, 0);
	if (which != svPaletteShown) {
		svPaletteShown = which;
		for (int level = 0; level < SV_LEVELS; level++) {
			int const shade = level >> 6 < 3 ? level >> 6 : 2, within = level - shade * 64;
			uint32_t const from = palettes[which][shade], to = palettes[which][shade + 1];
			int rgb[3];
			for (int c = 0; c < 3; c++) {
				int const a = (int) (from >> (16 - 8 * c) & 0xFF), b = (int) (to >> (16 - 8 * c) & 0xFF);
				rgb[c] = (a * (64 - within) + b * within + 32) / 64;
			}
			svColors[level] = (uint16_t) ((rgb[0] >> 3) << 11 | (rgb[1] >> 2) << 5 | rgb[2] >> 3);
		}
	}
	supervision.muted = (uint8_t) ((muted("sv_square1") ? SV_SQUARE1 : 0) | (muted("sv_square2") ? SV_SQUARE2 : 0) |
		(muted("sv_noise") ? SV_NOISE : 0) | (muted("sv_samples") ? SV_SAMPLES : 0));
	gamate.muted = (uint8_t) ((muted("gm_a") ? GM_CHANNEL_A : 0) | (muted("gm_b") ? GM_CHANNEL_B : 0) | (muted("gm_c") ? GM_CHANNEL_C : 0));
	// A multicart's game chosen anew: the console restarts with it
	const char* game = optionValue(environ_cb, "armnes_sv_game");
	int const chosen = game && atoi(game) > 0 ? atoi(game) : 1;
	if (svLoaded && supervision.games > 1 && chosen != svGameChosen) {
		svGameChosen = chosen;
		if ((uint32_t) (chosen - 1) % supervision.games != supervision.m.game) {
			svPlay(&supervision, (uint32_t) (chosen - 1));
			svShowGame();
		}
	}
	int const strength = optionChoice(environ_cb, "armnes_sv_ghosting", ghosting, 4, 0);
	if (strength != svGhosting) {
		svGhosting = strength;
		svLevelsSet = false;
		if (strength) {
			buildSupervisionFade(strength);
		}
	}
}

// A multicart's game playing: shown, and kept as the Multicart Game option (which then changes nothing)
static void svShowGame(void) {
	if (supervision.games < 2) {
		return;
	}
	static char text[40], value[4];
	snprintf(text, sizeof(text), "Game %u of %u", supervision.m.game + 1u, supervision.games);
	struct retro_message message = { text, 120 };
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	snprintf(value, sizeof(value), "%u", supervision.m.game + 1u);
	struct retro_variable variable = { "armnes_sv_game", value };
	if (environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &variable)) {
		svGameChosen = (int) supervision.m.game + 1;
	}
}

// The Gamate's memory for achievements, as the CPU sees it (player 1's with two together): its 1 KiB of
// RAM at $0000, mirrored to $1FFF. rcheevos has no Gamate yet; this is ready for when it does.
static void setGamateMemoryMaps(void) {
	static struct retro_memory_descriptor descriptors[1];
	static struct retro_memory_map map = { descriptors, 1 };
	descriptors[0] = (struct retro_memory_descriptor) { RETRO_MEMDESC_SYSTEM_RAM, gamate.m.ram, 0, 0x0000, 0xE000, 0x1C00, sizeof(gamate.m.ram), NULL };
	environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
	bool achievements = true;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
}

// The Supervision's memory for achievements, where rcheevos has it: RAM, registers and video RAM
static void setSupervisionMemoryMaps(void) {
	static struct retro_memory_descriptor descriptors[3];
	static struct retro_memory_map map = { descriptors, 3 };
	descriptors[0] = (struct retro_memory_descriptor) { RETRO_MEMDESC_SYSTEM_RAM, supervision.m.ram, 0, 0x0000, 0, 0, 0x2000, NULL };
	descriptors[1] = (struct retro_memory_descriptor) { 0, supervision.m.reg, 0, 0x2000, 0, 0, 0x2000, NULL };
	descriptors[2] = (struct retro_memory_descriptor) { RETRO_MEMDESC_VIDEO_RAM, supervision.m.vram, 0, 0x4000, 0, 0, 0x2000, NULL };
	environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &map);
	bool achievements = true;
	environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);
}

// The Multicart Game option's values: the cartridge's games
static void svListGames(void) {
	for (struct retro_core_option_v2_definition* d = optionDefinitions; d->key; d++) {
		if (!strcmp(d->key, "armnes_sv_game")) {
			static const char* const values[8] = { "1", "2", "3", "4", "5", "6", "7", "8" };
			static const char* const labels[8] = { "Game 1", "Game 2", "Game 3", "Game 4", "Game 5", "Game 6", "Game 7", "Game 8" };
			uint32_t const count = supervision.games > 1 && supervision.games < 8 ? supervision.games : 8;
			for (uint32_t i = 0; i < 8; i++) {
				d->values[i] = i < count ? (struct retro_core_option_value) { values[i], labels[i] } : (struct retro_core_option_value) { NULL, NULL };
			}
			optionsGive(environ_cb, &options);
			return;
		}
	}
}

// A handheld's four-shade picture in the LCD Palette, faded by LCD Ghosting. A frame the frontend
// won't show (run-ahead, fast-forward) isn't drawn, nor does it fade the LCD.
static void showLcd(const uint8_t* picture, int width, int height, void (*decorate)(void)) {
	int av = RETRO_AV_ENABLE_VIDEO;
	bool dupe = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) && !(av & RETRO_AV_ENABLE_VIDEO) &&
		environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe) && dupe) {
		video_cb(NULL, (unsigned) width, (unsigned) height, (size_t) width * sizeof(uint16_t));
		return;
	}
	int const count = width * height;
	if (!svGhosting || !svLevelsSet) {
		for (int i = 0; i < count; i++) {
			svLevel[i] = (uint8_t) (picture[i] * 64);
			video[i] = svColors[svLevel[i]];
		}
		svLevelsSet = svGhosting != 0;
	} else {
		for (int i = 0; i < count; i++) {
			svLevel[i] = svNext[svLevel[i]][picture[i]];
			video[i] = svColors[svLevel[i]];
		}
	}
	if (decorate) {
		decorate();
	}
	video_cb(video, (unsigned) width, (unsigned) height, (size_t) width * sizeof(uint16_t));
}

// A Supervision frame: the NES pad's buttons (with turbo and the left stick) as the Supervision's.
static void runSupervision(void) {
	input_poll_cb();
	turboFrame++;
	readFastForward();
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		readInputOptions();
		readSupervisionOptions();
	}
	uint8_t pad = readPad(0), buttons = 0;
	static const uint8_t to[8] = { SV_A, SV_B, SV_SELECT, SV_START, SV_UP, SV_DOWN, SV_LEFT, SV_RIGHT };
	for (int i = 0; i < 8; i++) {
		buttons |= pad >> i & 1 ? to[i] : 0;
	}
	handheldCheatsApply();
	svRunFrame(&supervision, buttons);
	if (supervision.soundFrames) {
		audio_batch_cb(supervision.sound, supervision.soundFrames);
	}
	showLcd(supervision.picture, SV_WIDTH, SV_HEIGHT, NULL);
}

// A Gamate's buttons: the NES pad's as the Gamate's, as for the Supervision, from the controller that
// plays it (controller 1 plays the console the Switch Controller Combo moved it to)
static uint8_t gamateButtons(unsigned console) {
	unsigned const port = gmLinked && !netplaySeen ? (console + 2 - (unsigned) gmControllerFocus) % 2 : console;
	uint8_t pad = readPad(port), buttons = 0;
	static const uint8_t to[8] = { GM_A, GM_B, GM_SELECT, GM_START, GM_UP, GM_DOWN, GM_LEFT, GM_RIGHT };
	for (int i = 0; i < 8; i++) {
		buttons |= pad >> i & 1 ? to[i] : 0;
	}
	return buttons;
}

// The Gamate's frame picture's size: one screen, or two side by side or top and bottom
static unsigned gamateWidth(void) {
	return gmLinked && gmScreen == GM_SCREENS_SIDE ? 2 * GM_WIDTH : GM_WIDTH;
}

static unsigned gamateHeight(void) {
	return gmLinked && gmScreen == GM_SCREENS_STACKED ? 2 * GM_HEIGHT : GM_HEIGHT;
}

// Link Mode and Screen Layout: Together in or out restarts both Gamates; the picture's shape follows.
// Separate is set when the game starts.
static void readGamateLink(bool loading) {
	bool const linked = optionIs(environ_cb, "armnes_gm_link", "together");
	static const struct OptionChoice screens[] = {
		{ "side by side", GM_SCREENS_SIDE }, { "top and bottom", GM_SCREENS_STACKED }, { "player 1", GM_SCREENS_ONE },
		{ "player 2", GM_SCREENS_ONE + 1 },
	};
	// R2 and L2 change the screens too; the option wins only when it changes
	int const chosen = optionChoice(environ_cb, "armnes_gm_link_screen", screens, 4, GM_SCREENS_SIDE);
	int const screen = chosen != gmScreenOption ? chosen : gmScreen;
	gmScreenOption = chosen;
	gmL2FastForward = !optionIs(environ_cb, "armnes_gm_link_l2", "previous screens");
	const char* follows = optionValue(environ_cb, "armnes_gm_r2_follows");
	gmR2Follows = follows && (*follows == '1' || *follows == '2') ? *follows - '1' : -1;
	gmPlayerNumbers = !optionIs(environ_cb, "armnes_gm_player_numbers", "disabled");
	static const struct OptionChoice combos[] = {
		{ "start l2 r2", GM_SWITCH_START_L2_R2 }, { "l r a b", GM_SWITCH_L_R_A_B }, { "l3 r3", GM_SWITCH_L3_R3 },
		{ "start l3", GM_SWITCH_START_L3 }, { "start r3", GM_SWITCH_START_R3 }, { "off", GM_SWITCH_OFF },
	};
	gmSwitchCombo = optionChoice(environ_cb, "armnes_gm_switch_controller", combos, 6, GM_SWITCH_START_L2_R2);
	unsigned const width = gamateWidth(), height = gamateHeight();
	if (linked != gmLinked && !loading) {
		gmReset(&gamate);
		gmReset(&gamateTwo);
		struct retro_message message = { linked ? "Link Mode Together: both Gamates restart" : "Link Mode Off: the Gamate restarts", 180 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}
	bool const shown = linked != gmLinked;
	gmLinked = linked;
	gmScreen = screen;
	bool const reshaped = gamateWidth() != width || gamateHeight() != height;
	svLevelsSet = svLevelsSet && !reshaped;
	if (!loading && reshaped) {
		struct retro_game_geometry g = { gamateWidth(), gamateHeight(), 2 * GM_WIDTH, 2 * GM_HEIGHT,
			(float) gamateWidth() / gamateHeight() };
		environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
	}
	if (shown && !loading) {
		showOptions();
	}
}

// Link Mode Separate: this Gamate's cable to the other player's over netplay's packets. The host and
// the first player to join are linked. Each side counts link time from when they paired and sends
// every byte with the time it went. With Exact timing each also says how far it has got eight times a
// frame, and neither runs more than GM_LEAD past the other: a byte from the one ahead arrives at its
// time, one from the one behind at most GM_LEAD late (its spacing kept), as the link games need. A
// side held at the limit waits within its frame for the other, slowing the game rather than
// breaking it. Standard leaves each side at its own pace, a byte arriving as soon as it comes.
static retro_netpacket_send_t gmSend;
static retro_netpacket_poll_receive_t gmPoll;
static bool gmPartnered, gmExact;
static uint16_t gmPartner;
// This Gamate's time when the two paired, and the frames run without getting past the limit
static uint32_t gmOrigin;
static int gmStalled;
enum { GM_PACKET_BYTE = 1, GM_PACKET_TIME = 2 };
enum { GM_LEAD = GM_FRAME_CYCLES };
// Frames held at the limit before running on alone (the other player paused or gone): 3 seconds
enum { GM_STALL_FRAMES = 180 };
// Waiting at the limit, up to this long a frame (ms)
enum { GM_WAIT_MS = 50 };

static void put32(uint8_t* to, uint32_t value) {
	to[0] = (uint8_t) value;
	to[1] = (uint8_t) (value >> 8);
	to[2] = (uint8_t) (value >> 16);
	to[3] = (uint8_t) (value >> 24);
}

static uint32_t get32(const uint8_t* from) {
	return (uint32_t) from[0] | (uint32_t) from[1] << 8 | (uint32_t) from[2] << 16 | (uint32_t) from[3] << 24;
}

static void gamateNetSend(void* context, uint8_t byte, uint32_t sent) {
	(void) context;
	if (gmSend && gmPartnered) {
		uint8_t packet[6] = { GM_PACKET_BYTE, byte };
		put32(packet + 2, sent - gmOrigin);
		gmSend(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, packet, sizeof(packet), gmPartner);
	}
}

static void gamateNetTime(void) {
	if (gmSend && gmPartnered && gmExact) {
		uint8_t packet[5] = { GM_PACKET_TIME };
		put32(packet + 1, gmNow(&gamate) - gmOrigin);
		gmSend(RETRO_NETPACKET_RELIABLE | RETRO_NETPACKET_FLUSH_HINT, packet, sizeof(packet), gmPartner);
	}
}

// Linked from now: link time starts here, and with Exact the limit waits for the other's first word
static void gamatePaired(void) {
	gmPartnered = true;
	gmOrigin = gmNow(&gamate);
	gmStalled = 0;
	gamate.incomingCount = 0;
	gamate.haveLast = false;
	gamate.limited = gmExact;
	gamate.limit = gmNow(&gamate) + GM_LEAD;
	gamateNetTime();
}

static void gamateUnpaired(void) {
	gmPartnered = false;
	gamate.incomingCount = 0;
	gamate.limited = false;
}

static void gamateNetPoll(void* context) {
	(void) context;
	if (gmPoll) {
		gmPoll();
	}
	gamateNetTime();
}

// At the limit: waiting for the other Gamate, up to GM_WAIT_MS in a frame
static struct timespec gmWaitStart;
static bool gmWaitStarted;

static bool gamateNetWait(void* context) {
	(void) context;
	if (!gmPoll) {
		return false;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (!gmWaitStarted) {
		gmWaitStart = now;
		gmWaitStarted = true;
	}
	long const waited = (now.tv_sec - gmWaitStart.tv_sec) * 1000000L + (now.tv_nsec - gmWaitStart.tv_nsec) / 1000;
	if (waited > GM_WAIT_MS * 1000L) {
		return false;
	}
	struct timespec const nap = { 0, 200000 };
	nanosleep(&nap, NULL);
	gmPoll();
	return true;
}

static void RETRO_CALLCONV gamateNetStart(uint16_t self, retro_netpacket_send_t send, retro_netpacket_poll_receive_t poll) {
	gmSend = send;
	gmPoll = poll;
	gmPartner = 0;
	// A player who joins is linked to the host; the host waits for the first to join
	if (self != 0) {
		gamatePaired();
	} else {
		gamateUnpaired();
	}
	struct retro_message message = { self ? "Gamate link cable: joined to the host's Gamate" : "Gamate link cable: waiting for a player to join", 240 };
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

static void RETRO_CALLCONV gamateNetReceive(const void* data, size_t size, uint16_t from) {
	const uint8_t* packet = data;
	if (!gmPartnered || from != gmPartner || !size) {
		return;
	}
	if (packet[0] == GM_PACKET_BYTE && size >= 6) {
		uint32_t const sent = gmOrigin + get32(packet + 2);
		gmLinkReceive(&gamate, packet[1], sent, gmExact ? sent : gmNow(&gamate));
	} else if (packet[0] == GM_PACKET_TIME && size >= 5 && gmExact) {
		uint32_t const limit = gmOrigin + get32(packet + 1) + GM_LEAD;
		if (!gamate.limited || (int32_t) (limit - gamate.limit) > 0) {
			gamate.limit = limit;
		}
		gamate.limited = true;
	}
}

static void RETRO_CALLCONV gamateNetStop(void) {
	gmSend = NULL;
	gmPoll = NULL;
	gamateUnpaired();
}

static bool RETRO_CALLCONV gamateNetConnected(uint16_t player) {
	if (gmPartnered) {
		return false;
	}
	gmPartner = player;
	gamatePaired();
	struct retro_message message = { "Gamate link cable: a player joined", 180 };
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	return true;
}

static void RETRO_CALLCONV gamateNetDisconnected(uint16_t player) {
	if (gmPartnered && player == gmPartner) {
		gamateUnpaired();
		struct retro_message message = { "Gamate link cable: the other player left", 180 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}
}

// Netplay's packets for Link Mode Separate, else (NULL) its usual netplay. The version names the
// timing, so RetroArch pairs only players who chose the same.
static void gamateNetplay(bool separate, bool exact) {
	static const struct retro_netpacket_callback exactCallbacks = {
		gamateNetStart, gamateNetReceive, gamateNetStop, NULL, gamateNetConnected, gamateNetDisconnected,
		"armNES Gamate link 3 exact",
	};
	static const struct retro_netpacket_callback standardCallbacks = {
		gamateNetStart, gamateNetReceive, gamateNetStop, NULL, gamateNetConnected, gamateNetDisconnected,
		"armNES Gamate link 3 standard",
	};
	gmSeparate = separate;
	gmExact = separate && exact;
	gamate.send = separate ? gamateNetSend : NULL;
	gamate.poll = separate ? gamateNetPoll : NULL;
	gamate.wait = gmExact ? gamateNetWait : NULL;
	gamate.limited = false;
	gamate.midFrame = false;
	environ_cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, !separate ? NULL : exact ? (void*) &exactCallbacks : (void*) &standardCallbacks);
}

// RetroArch after 1.22.2 says which netplay client this device is: 0 the host, then the devices that
// joined, each playing the player of its number. Not asked again in a game once it hasn't known.
#ifndef RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX
#define RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX (82 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#endif
static bool gmClientUnknown;

// This device's session tag, made once netplay is seen: the time, the processor time used and where this
// core sits in memory, which differ from one device to the next
static void gamateTagSession(void) {
	while (netplaySeen && !gmSessionTag) {
		gmSessionTag = (uint32_t) time(NULL) * 2654435761u ^ (uint32_t) clock() ^ (uint32_t) (uintptr_t) &gmSessionTag;
	}
}

// A frame RetroArch runs again and doesn't show (run-ahead): buttons aren't taken as new presses
static bool gamateRerunning(void) {
	int av = RETRO_AV_ENABLE_VIDEO;
	return environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) && !(av & RETRO_AV_ENABLE_VIDEO);
}

// The Switch Controller Combo's buttons, and whether controller 1 holds them (outside netplay)
static const struct {
	int count;
	unsigned buttons[3];
} gmCombos[] = {
	[GM_SWITCH_START_L2_R2] = { 3, { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L2, RETRO_DEVICE_ID_JOYPAD_R2 } },
	[GM_SWITCH_L_R_A_B] = { 3, { RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_A } },
	[GM_SWITCH_L3_R3] = { 2, { RETRO_DEVICE_ID_JOYPAD_L3, RETRO_DEVICE_ID_JOYPAD_R3 } },
	[GM_SWITCH_START_L3] = { 2, { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_L3 } },
	[GM_SWITCH_START_R3] = { 2, { RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_R3 } },
};

static bool gamateSwitchHeld(void) {
	if (gmSwitchCombo == GM_SWITCH_OFF || netplaySeen) {
		return false;
	}
	for (int k = 0; k < gmCombos[gmSwitchCombo].count; k++) {
		if (!input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, gmCombos[gmSwitchCombo].buttons[k])) {
			return false;
		}
	}
	// L + R + A + B: B too
	return gmSwitchCombo != GM_SWITCH_L_R_A_B || input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
}

// Controller 1 holding Start with the combo on Start + L2 + R2: its L2 and R2 only switch
static bool gamateSwitchStart(int port) {
	return gmSwitchCombo == GM_SWITCH_START_L2_R2 && port == 0 && !netplaySeen &&
	       input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
}

static void gamateNotify(const char* text) {
	struct retro_message message = { text, 120 };
	environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
}

// The Switch Controller Combo on controller 1 moves it to the next console
static void readGamateControllerSwitch(void) {
	static bool held;
	if (gamateRerunning()) {
		return;
	}
	if (netplaySeen) {
		gmControllerFocus = 0;
	}
	bool const now = gamateSwitchHeld();
	bool const pressed = now && !held;
	held = now;
	if (pressed) {
		gmControllerFocus = (gmControllerFocus + 1) % 2;
		static char name[48];
		snprintf(name, sizeof(name), "Controller 1: Player %d's Console", gmControllerFocus + 1);
		gamateNotify(name);
	}
}

// R2 moves to the next way of showing the screens, and L2 back unless it fast-forwards. In netplay
// each device shows the screens its own way, changed by one player's L2 and R2: Screen Layout
// Control's player, or with Auto this device's own, which RetroArch after 1.22.2 says; with one that
// doesn't, the host's player 1, or player 2 on a device that joined.
static void readGamateScreenKeys(void) {
	static bool held[2][2];
	if (gamateRerunning()) {
		return;
	}
	bool const netplay = netplaySeen;
	int here = gmJoined ? 1 : 0;
	unsigned client = 0;
	if (gmR2Follows >= 0) {
		here = gmR2Follows;
	} else if (netplay && !gmClientUnknown) {
		if (environ_cb(RETRO_ENVIRONMENT_GET_NETPLAY_CLIENT_INDEX, &client)) {
			here = client < 2 && !(client == 0 && gmJoined) ? (int) client : here;
		} else {
			gmClientUnknown = true;
		}
	}
	int step = 0;
	for (int port = 0; port < 2; port++) {
		if (netplay && port != here) {
			continue;
		}
		for (int k = netplay || !gmL2FastForward ? 0 : 1; k < 2; k++) {
			bool const now = !gamateSwitchStart(port) &&
			                 input_state_cb((unsigned) port, RETRO_DEVICE_JOYPAD, 0, k ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_L2);
			if (now && !held[port][k]) {
				step += k ? 1 : -1;
			}
			held[port][k] = now;
		}
	}
	if (!step) {
		return;
	}
	gmScreen = ((gmScreen + step) % 4 + 4) % 4;
	svLevelsSet = false;
	struct retro_game_geometry g = { gamateWidth(), gamateHeight(), 2 * GM_WIDTH, 2 * GM_HEIGHT, (float) gamateWidth() / gamateHeight() };
	environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &g);
	static const char* const names[4] = { "Side by Side", "Top and Bottom", "Player 1 Only", "Player 2 Only" };
	gamateNotify(names[gmScreen]);
}

// Show Player Numbers: each player's number in a small badge, faintly, at the top left of their screen,
// as armGBA draws it
static uint16_t blend565(uint16_t was, uint32_t color, int alpha) {
	int const r = (was >> 11) << 3, g = (was >> 5 & 63) << 2, b = (was & 31) << 3;
	int const cr = (int) (color >> 16 & 0xFF), cg = (int) (color >> 8 & 0xFF), cb = (int) (color & 0xFF);
	int const nr = (r * (256 - alpha) + cr * alpha) >> 8, ng = (g * (256 - alpha) + cg * alpha) >> 8, nb = (b * (256 - alpha) + cb * alpha) >> 8;
	return (uint16_t) ((nr >> 3) << 11 | (ng >> 2) << 5 | nb >> 3);
}

static void numberScreen(uint16_t* corner, int pitch, int player) {
	static const uint8_t glyphs[3][7] = {
		{ 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
		{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },
	};
	static const uint32_t colors[2] = { 0x2F6FE0, 0xD83C3C };
	uint32_t const fill = colors[player], edge = (fill >> 1) & 0x7F7F7F;
	int const width = 15, height = 11;
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			if ((x == 0 || x == width - 1) && (y == 0 || y == height - 1)) {
				continue;
			}
			bool const border = x == 0 || y == 0 || x == width - 1 || y == height - 1;
			int const gx = x - 2, gy = y - 2, glyph = gx < 5 ? 0 : 1, column = glyph ? gx - 6 : gx;
			bool const dot = !border && gy >= 0 && gy < 7 && column >= 0 && column < 5 && (gx < 5 || gx > 5) &&
			                 glyphs[glyph ? player + 1 : 0][gy] >> (4 - column) & 1;
			uint16_t* at = corner + (y + 2) * pitch + (x + 2);
			*at = dot ? blend565(*at, 0xFFFFFF, 170) : blend565(*at, border ? edge : fill, border ? 110 : 90);
		}
	}
}

static void numberGamates(void) {
	if (!gmPlayerNumbers) {
		return;
	}
	switch (gmScreen) {
	case GM_SCREENS_SIDE:
		numberScreen(video, 2 * GM_WIDTH, 0);
		numberScreen(video + GM_WIDTH, 2 * GM_WIDTH, 1);
		break;
	case GM_SCREENS_STACKED:
		numberScreen(video, GM_WIDTH, 0);
		numberScreen(video + GM_WIDTH * GM_HEIGHT, GM_WIDTH, 1);
		break;
	default:
		numberScreen(video, GM_WIDTH, gmScreen - GM_SCREENS_ONE);
		break;
	}
}

// A Gamate frame: the NES pad's buttons as the Gamate's, as for the Supervision; with the link cable,
// both Gamates, controller 2 the second's
static void runGamate(void) {
	input_poll_cb();
	turboFrame++;
	readFastForward();
	bool updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
		readInputOptions();
		readSupervisionOptions();
		readGamateLink(false);
	}
	if (gmLinked) {
		readGamateControllerSwitch();
		readGamateScreenKeys();
	}
	handheldCheatsApply();
	if (!gmLinked) {
		gmWaitStarted = false;
		bool const whole = gmRunFrame(&gamate, gamateButtons(0));
		if (gamate.soundFrames) {
			audio_batch_cb(gamate.sound, gamate.soundFrames);
		}
		// Held at Exact timing's limit: the last picture again; and if that lasts, on alone
		gmStalled = whole ? 0 : gmStalled + 1;
		if (gmStalled > GM_STALL_FRAMES) {
			gamate.limited = false;
			gmStalled = 0;
			struct retro_message message = { "Gamate link cable: the other player isn't answering", 180 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		}
		bool dupe = false;
		if (!whole && environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe) && dupe) {
			video_cb(NULL, GM_WIDTH, GM_HEIGHT, GM_WIDTH * sizeof(uint16_t));
			return;
		}
		showLcd(gamate.picture, GM_WIDTH, GM_HEIGHT, NULL);
		return;
	}
	gmRunLinked(&gamate, &gamateTwo, gamateButtons(0), gamateButtons(1));
	if (gmScreen >= GM_SCREENS_ONE) {
		const struct Gamate* shown = gmScreen == GM_SCREENS_ONE ? &gamate : &gamateTwo;
		if (shown->soundFrames) {
			audio_batch_cb(shown->sound, (size_t) shown->soundFrames);
		}
		showLcd(shown->picture, GM_WIDTH, GM_HEIGHT, numberGamates);
		return;
	}
	// Both: the sound mixed, the screens side by side or top and bottom
	static int16_t mixed[sizeof(gamate.sound) / sizeof(*gamate.sound)];
	int const frames = gamate.soundFrames < gamateTwo.soundFrames ? gamate.soundFrames : gamateTwo.soundFrames;
	for (int i = 0; i < 2 * frames; i++) {
		mixed[i] = (int16_t) ((gamate.sound[i] + gamateTwo.sound[i]) / 2);
	}
	if (frames) {
		audio_batch_cb(mixed, (size_t) frames);
	}
	static uint8_t both[2 * GM_WIDTH * GM_HEIGHT];
	if (gmScreen == GM_SCREENS_STACKED) {
		memcpy(both, gamate.picture, sizeof(gamate.picture));
		memcpy(both + GM_WIDTH * GM_HEIGHT, gamateTwo.picture, sizeof(gamateTwo.picture));
		showLcd(both, GM_WIDTH, 2 * GM_HEIGHT, numberGamates);
		return;
	}
	for (int y = 0; y < GM_HEIGHT; y++) {
		memcpy(both + y * 2 * GM_WIDTH, gamate.picture + y * GM_WIDTH, GM_WIDTH);
		memcpy(both + y * 2 * GM_WIDTH + GM_WIDTH, gamateTwo.picture + y * GM_WIDTH, GM_WIDTH);
	}
	showLcd(both, 2 * GM_WIDTH, GM_HEIGHT, numberGamates);
}

void retro_run(void) {
	if (svLoaded) {
		runSupervision();
		return;
	}
	if (gmLoaded) {
		runGamate();
		return;
	}
	saveBlockSync();
	if (parLoaded) {
		parWriteSave(false);
	}
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
	// A frame the frontend won't show (fast-forward, run-ahead) leaves out what only the picture needs,
	// unless a Zapper is looking at it
	int av = RETRO_AV_ENABLE_VIDEO;
	bool const unseen = environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE, &av) && !(av & RETRO_AV_ENABLE_VIDEO);
	nes.skipPixels = unseen && nes.input.device[0] != DEVICE_ZAPPER && nes.input.device[1] != DEVICE_ZAPPER;
	nesRunFrame(&nes);
	for (int extra = 0; fdsFast && extra < 15 && fdsBusy(&nes); extra++) {
		nesRunFrame(&nes);
	}
	nes.skipPixels = false;
	checkOptions();
	bool dupe = false;
	if (unseen && environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &dupe) && dupe) {
		if (nes.audioCount) {
			audio_batch_cb(nes.audioOut, nes.audioCount / 2);
		}
		video_cb(NULL, NES_WIDTH - 2 * cropHorizontal, NES_HEIGHT - 2 * cropVertical, NES_WIDTH * sizeof(uint16_t));
		return;
	}
	int scale = 1;
	if (filter) {
		scale = COMPOSITE_SCALE;
		compositeFrame(filter, nes.region != REGION_NTSC, nes.ppu.oddFrame, nes.frameBuffer, video);
	} else {
		convertPicture(video, nes.frameBuffer, NES_WIDTH * NES_HEIGHT);
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

// Pro Action Replay: the game plugged into it, one of the games beside it in its folder (those whose
// saves are newest first), with its own save, the one it has played on its own. The cartridge keeps
// nothing once the console is off.
static const char* saveFolder(void) {
	const char* dir = NULL;
	return environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir && *dir ? dir : NULL;
}

// The save a frontend gives the game on its own: its name with .srm, in the save folder
static void parSaveFile(const char* game, char* out, size_t size) {
	char base[PAR_NAME];
	snprintf(base, sizeof(base), "%s", game);
	char* dot = strrchr(base, '.');
	if (dot && dot != base) {
		*dot = '\0';
	}
	const char* dir = saveFolder();
	snprintf(out, size, "%s/%s.srm", dir ? dir : parFolder, base);
}

static bool parGameName(const char* name) {
	const char* dot = strrchr(name, '.');
	if (!dot || name[0] == '.' || strstr(name, "[BIOS]") || strlen(name) >= PAR_NAME || !strcmp(name, parSelf)) {
		return false;
	}
	// The cheat cartridges themselves (a Game Genie image's usual names)
	if (strcasestr(name, "game genie") || strcasestr(name, "gamegenie") || strcasestr(name, "action replay")) {
		return false;
	}
	return !strcasecmp(dot, ".nes") || !strcasecmp(dot, ".zip") || !strcasecmp(dot, ".unf") || !strcasecmp(dot, ".unif");
}

struct ParRanked {
	char name[PAR_NAME];
	long long time;
};

static int parNewestFirst(const void* a, const void* b) {
	const struct ParRanked *x = a, *y = b;
	if (x->time != y->time) {
		return x->time > y->time ? -1 : 1;
	}
	return strcasecmp(x->name, y->name);
}

// The file that keeps the Game on Top chosen, as RetroArch forgets a value its list didn't have
static void parChoiceFile(char* out, size_t size) {
	const char* dir = saveFolder();
	snprintf(out, size, "%s/armNES %s.txt", dir ? dir : parFolder, parName());
}

static void parListGames(void) {
	parGameCount = 0;
	DIR* dir = opendir(parFolder);
	struct ParRanked* all = NULL;
	size_t n = 0, room = 0;
	for (struct dirent* e; dir && (e = readdir(dir));) {
		if (!parGameName(e->d_name)) {
			continue;
		}
		if (n == room) {
			room = room ? room * 2 : 256;
			struct ParRanked* grown = realloc(all, room * sizeof(*all));
			if (!grown) {
				break;
			}
			all = grown;
		}
		char file[4096];
		struct stat st;
		snprintf(all[n].name, PAR_NAME, "%s", e->d_name);
		parSaveFile(e->d_name, file, sizeof(file));
		all[n].time = stat(file, &st) ? 0 : (long long) st.st_mtime;
		n++;
	}
	if (dir) {
		closedir(dir);
	}
	if (all) {
		qsort(all, n, sizeof(*all), parNewestFirst);
	}
	for (size_t i = 0; i < n && parGameCount < PAR_CHOICES; i++) {
		snprintf(parGames[parGameCount], PAR_NAME, "%s", all[i].name);
		snprintf(parLabels[parGameCount], PAR_NAME, "%s", all[i].name);
		char* dot = strrchr(parLabels[parGameCount], '.');
		if (dot && dot != parLabels[parGameCount]) {
			*dot = '\0';
		}
		parGameCount++;
	}
	free(all);
	if (!parOption) {
		return;
	}
	size_t v = 0;
	parOption->values[v++] = (struct retro_core_option_value) { "none", "None" };
	for (size_t i = 0; i < parGameCount; i++) {
		parOption->values[v++] = (struct retro_core_option_value) { parGames[i], parLabels[i] };
	}
	parOption->values[v] = (struct retro_core_option_value) { NULL, NULL };
	optionsGive(environ_cb, &options);
	// The choice kept from before, as the frontend may have dropped it
	char file[4096], kept[PAR_NAME] = "";
	parChoiceFile(file, sizeof(file));
	FILE* f = fopen(file, "r");
	if (f) {
		if (fgets(kept, sizeof(kept), f)) {
			kept[strcspn(kept, "\r\n")] = '\0';
		}
		fclose(f);
	}
	for (size_t i = 0; *kept && i < parGameCount; i++) {
		if (!strcmp(kept, parGames[i])) {
			struct retro_variable variable = { "armnes_par_game", parGames[i] };
			environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
		}
	}
}

// The Game on Top chosen, if it's one of those listed, else NULL
static const char* parChosen(void) {
	const char* value = optionValue(environ_cb, "armnes_par_game");
	for (size_t i = 0; value && i < parGameCount; i++) {
		if (!strcmp(value, parGames[i])) {
			return parGames[i];
		}
	}
	return NULL;
}

// The game's image (a .zip's first NES image), or NULL
static uint8_t* parReadGame(const char* name, size_t* size) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/%s", parFolder, name);
	if (path_is_compressed_file(path)) {
		struct string_list* inner = file_archive_get_file_list(path, "nes|unf|unif");
		uint8_t* data = inner && inner->size ? archiveRead(path, inner->elems[0].data, size) : NULL;
		string_list_free(inner);
		return data;
	}
	return readFile(path, size);
}

// The game on top's battery save: the cartridge's RAM, then the board's own (as the frontend has it
// for the game alone)
static size_t parSaveImage(uint8_t* out) {
	if (!nes.cart.battery || !*parTop) {
		return 0;
	}
	size_t board = 0;
	uint8_t* ram = nes.board->batteryRam ? nes.board->batteryRam(&nes, &board) : NULL;
	if (out) {
		memcpy(out, nes.cart.wram, nes.cart.batterySize);
		if (ram) {
			memcpy(out + nes.cart.batterySize, ram, board);
		}
	}
	return nes.cart.batterySize + (ram ? board : 0);
}

static void parReadSave(void) {
	free(parSaveKept);
	parSaveKept = NULL;
	parSaveSize = parSaveImage(NULL);
	if (!parSaveSize || !(parSaveKept = malloc(parSaveSize))) {
		parSaveSize = 0;
		return;
	}
	size_t size = 0;
	uint8_t* file = readFile(parSave, &size);
	if (file && size >= nes.cart.batterySize) {
		memcpy(nes.cart.wram, file, nes.cart.batterySize);
		size_t board = 0;
		uint8_t* ram = nes.board->batteryRam ? nes.board->batteryRam(&nes, &board) : NULL;
		if (ram && size >= nes.cart.batterySize + board) {
			memcpy(ram, file + nes.cart.batterySize, board);
		}
	}
	free(file);
	parSaveImage(parSaveKept);
}

// Written when it has changed (every 5 seconds, and as the game goes)
static void parWriteSave(bool now) {
	if (!parSaveSize || (!now && ++parSaveFrames < 300)) {
		return;
	}
	parSaveFrames = 0;
	uint8_t* image = malloc(parSaveSize);
	if (!image) {
		return;
	}
	parSaveImage(image);
	if (memcmp(image, parSaveKept, parSaveSize)) {
		FILE* f = fopen(parSave, "wb");
		if (f) {
			fwrite(image, 1, parSaveSize, f);
			fclose(f);
			memcpy(parSaveKept, image, parSaveSize);
		}
	}
	free(image);
}

// Cheat codes for the cheat cartridge from RetroArch's cheat database: its .cht files for the game on top
// (the same title and a region of its, the "(Action Replay)" and "(Game Genie)" files too), each cheat a
// name and its codes joined by +. The Pro Action Replay takes RAM codes ($0000-$7FFF): its own 00AAAAVV,
// AAVV for zero page, and AAAA:VV; the Game Genie ROM codes ($8000 and up): its letters, AAAA:VV and
// AAAA?CC:VV. A cheat whose codes all fit the cartridge goes in the Cheat Code options' lists.
static int cartSlots(void) {
	return parIsGenie ? 3 : 5;
}

static bool isFolder(const char* path) {
	struct stat st;
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static bool chtHasNes(const char* folder) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/Nintendo - Nintendo Entertainment System", folder);
	return isFolder(path);
}

// cheat_database_path in the retroarch.cfg in config, ~ for the home folder
static bool chtConfigured(const char* config, char* out, size_t size) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/retroarch.cfg", config);
	FILE* f = fopen(path, "r");
	if (!f) {
		return false;
	}
	char line[4096];
	bool found = false;
	while (!found && fgets(line, sizeof(line), f)) {
		const char* p = line + strspn(line, " \t");
		char* open = strncmp(p, "cheat_database_path", 19) ? NULL : strchr(p, '"');
		char* close = open ? strrchr(open + 1, '"') : NULL;
		if (!close) {
			continue;
		}
		*close = '\0';
		const char* home = getenv("HOME");
		snprintf(out, size, "%s%s", open[1] == '~' && home ? home : "", open[1] == '~' && home ? open + 2 : open + 1);
		found = *out != '\0';
	}
	fclose(f);
	return found;
}

// RetroArch's cheat database: as its configuration says, else where it keeps it by default
static bool chtFolder(char* out, size_t size) {
	const char* home = getenv("HOME");
	const char* xdg = getenv("XDG_CONFIG_HOME");
	const char* system = NULL;
	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system);
	char configs[5][4096];
	int n = 0;
	if (xdg && *xdg) {
		snprintf(configs[n++], sizeof(configs[0]), "%s/retroarch", xdg);
	}
	if (home) {
		snprintf(configs[n++], sizeof(configs[0]), "%s/.config/retroarch", home);
		snprintf(configs[n++], sizeof(configs[0]), "%s/.var/app/org.libretro.RetroArch/config/retroarch", home);
	}
	snprintf(configs[n++], sizeof(configs[0]), "/storage/.config/retroarch");
	if (system && *system && strrchr(system, '/')) {
		snprintf(configs[n], sizeof(configs[0]), "%.*s", (int) (strrchr(system, '/') - system), system);
		n++;
	}
	for (int i = 0; i < n; i++) {
		if (chtConfigured(configs[i], out, size) && chtHasNes(out)) {
			return true;
		}
	}
	static const char* const defaults[] = { "/tmp/database/cht", "/usr/share/libretro-database/cht", "/usr/share/retroarch/database/cht" };
	for (size_t i = 0; i < sizeof(defaults) / sizeof(*defaults); i++) {
		if (chtHasNes(defaults[i])) {
			snprintf(out, size, "%s", defaults[i]);
			return true;
		}
	}
	for (int i = 0; home && i < 2; i++) {
		snprintf(out, size, i ? "%s/.var/app/org.libretro.RetroArch/config/retroarch/cheats" : "%s/.config/retroarch/cheats", home);
		if (chtHasNes(out)) {
			return true;
		}
	}
	return false;
}

// A No-Intro name's title (before " (") and regions (the first group's)
static void chtName(const char* file, char* title, size_t titleSize, char* regions, size_t regionsSize) {
	snprintf(title, titleSize, "%s", file);
	char* dot = strrchr(title, '.');
	if (dot) {
		*dot = '\0';
	}
	*regions = '\0';
	char* group = strstr(title, " (");
	if (group) {
		*group = '\0';
		snprintf(regions, regionsSize, "%s", group + 2);
		regions[strcspn(regions, ")")] = '\0';
	}
}

// Whether two region lists ("USA, Europe") share one
static bool regionsShare(const char* a, const char* b) {
	if (!*a || !*b) {
		return !*a && !*b;
	}
	char copy[128];
	snprintf(copy, sizeof(copy), "%s", a);
	for (char* r = strtok(copy, ","); r; r = strtok(NULL, ",")) {
		r += strspn(r, " ");
		size_t const length = strlen(r);
		for (const char* at = strstr(b, r); at; at = strstr(at + 1, r)) {
			if ((at == b || at[-1] == ' ' || at[-1] == ',') && (at[length] == '\0' || at[length] == ',')) {
				return true;
			}
		}
	}
	return false;
}

static int hexDigits(const char* s, size_t length) {
	for (size_t i = 0; i < length; i++) {
		if (!isxdigit((unsigned char) s[i])) {
			return -1;
		}
	}
	return (int) length;
}

// A cheat's codes as the cartridge takes them; the count, or -1 if one doesn't fit or there are too many
static int cartCodesOf(const char* text, struct Cheat* out) {
	int count = 0;
	while (*text) {
		size_t const length = strcspn(text, "+ ");
		if (length) {
			struct Cheat c;
			bool ok = false;
			if (!parIsGenie && hexDigits(text, length) == 8 && !strncmp(text, "00", 2)) {
				unsigned address, value;
				sscanf(text + 2, "%4x%2x", &address, &value);
				c = (struct Cheat) { (uint16_t) address, (uint8_t) value, -1 };
				ok = true;
			} else if (!parIsGenie && hexDigits(text, length) == 4) {
				unsigned address, value;
				sscanf(text, "%2x%2x", &address, &value);
				c = (struct Cheat) { (uint16_t) address, (uint8_t) value, -1 };
				ok = true;
			} else if (nesCheatParse(text, length, &c)) {
				ok = parIsGenie ? c.address >= 0x8000 : c.address < 0x8000 && c.compare < 0;
			}
			if (!ok || count >= cartSlots()) {
				return -1;
			}
			out[count++] = c;
		}
		text += length;
		text += *text != 0;
	}
	return count ? count : -1;
}

// RetroArch's cheat names keep some HTML entities (&quot;): the characters they stand for
static void cartUnescape(char* s) {
	static const struct { const char* entity; char c; } entities[] = {
		{ "&quot;", '"' }, { "&amp;", '&' }, { "&apos;", '\'' }, { "&#39;", '\'' }, { "&lt;", '<' }, { "&gt;", '>' },
	};
	char* out = s;
	for (const char* in = s; *in;) {
		size_t i = 0;
		size_t const count = sizeof(entities) / sizeof(*entities);
		while (i < count && strncmp(in, entities[i].entity, strlen(entities[i].entity))) {
			i++;
		}
		if (i < count) {
			*out++ = entities[i].c;
			in += strlen(entities[i].entity);
		} else {
			*out++ = *in++;
		}
	}
	*out = '\0';
}

static void cartReadCht(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) {
		return;
	}
	static char names[256][96];
	char line[1024];
	while (fgets(line, sizeof(line), f) && cartCheatCount < CART_CHEATS) {
		int index = -1, used = 0;
		char field[16] = "";
		if (sscanf(line, " cheat%d_%15[a-z] =%n", &index, field, &used) < 2 || index < 0 || index >= 256) {
			continue;
		}
		char* open = strchr(line, '"');
		char* close = open ? strrchr(open + 1, '"') : NULL;
		if (!close) {
			continue;
		}
		*close = '\0';
		if (!strcmp(field, "desc")) {
			cartUnescape(open + 1);
			snprintf(names[index], sizeof(names[index]), "%s", open + 1);
			continue;
		}
		if (strcmp(field, "code")) {
			continue;
		}
		struct CartCheat* c = &cartCheats[cartCheatCount];
		c->count = cartCodesOf(open + 1, c->codes);
		if (c->count < 0 || strlen(open + 1) >= sizeof(c->code)) {
			continue;
		}
		bool known = false;
		for (size_t i = 0; i < cartCheatCount && !known; i++) {
			known = !strcmp(cartCheats[i].code, open + 1);
		}
		if (!known) {
			snprintf(c->code, sizeof(c->code), "%s", open + 1);
			snprintf(c->name, sizeof(c->name), "%s", *names[index] ? names[index] : open + 1);
			cartCheatCount++;
		}
		names[index][0] = '\0';
	}
	fclose(f);
}

// The game on top's cheats into the Cheat Code options, and the choices kept for it put back
static void cartListCheats(void) {
	cartCheatCount = 0;
	char folder[4096];
	if (*parTop && chtFolder(folder, sizeof(folder))) {
		char title[PAR_NAME], regions[128];
		chtName(parTop, title, sizeof(title), regions, sizeof(regions));
		snprintf(folder + strlen(folder), sizeof(folder) - strlen(folder), "/Nintendo - Nintendo Entertainment System");
		// The cartridge's own kind of file first
		for (int pass = 0; pass < 2; pass++) {
			DIR* dir = opendir(folder);
			for (struct dirent* e; dir && (e = readdir(dir));) {
				char t[PAR_NAME], r[128];
				chtName(e->d_name, t, sizeof(t), r, sizeof(r));
				bool const own = strstr(e->d_name, parIsGenie ? "(Game Genie)" : "(Action Replay)") != NULL;
				if (own != (pass == 0) || strcasecmp(t, title) || !regionsShare(r, regions)) {
					continue;
				}
				char path[8192];
				snprintf(path, sizeof(path), "%s/%s", folder, e->d_name);
				cartReadCht(path);
			}
			if (dir) {
				closedir(dir);
			}
		}
	}
	for (int k = 0; k < CART_SLOTS; k++) {
		if (!cartCodeOptions[k]) {
			continue;
		}
		size_t v = 0;
		cartCodeOptions[k]->values[v++] = (struct retro_core_option_value) { "none", "None" };
		for (size_t i = 0; i < cartCheatCount; i++) {
			cartCodeOptions[k]->values[v++] = (struct retro_core_option_value) { cartCheats[i].code, cartCheats[i].name };
		}
		cartCodeOptions[k]->values[v] = (struct retro_core_option_value) { NULL, NULL };
	}
	optionsGive(environ_cb, &options);
	// The codes chosen before for this game (None for those it has none for), kept beside the Game on
	// Top choice: after "Game: <its name>", or (as written before) after the choice itself
	if (!*parTop) {
		return;
	}
	const char* chosen[CART_SLOTS] = { NULL };
	char file[4096], line[300];
	parChoiceFile(file, sizeof(file));
	FILE* f = fopen(file, "r");
	bool mine = false;
	for (bool first = true; f && fgets(line, sizeof(line), f); first = false) {
		line[strcspn(line, "\r\n")] = '\0';
		if (first || !strncmp(line, "Game: ", 6)) {
			mine = !strcmp(first ? line : line + 6, parTop);
			continue;
		}
		int k = 0, used = 0;
		if (!mine || sscanf(line, "Code %d: %n", &k, &used) != 1 || !used || k < 1 || k > CART_SLOTS) {
			continue;
		}
		for (size_t i = 0; i < cartCheatCount; i++) {
			if (!strcmp(cartCheats[i].code, line + used)) {
				chosen[k - 1] = cartCheats[i].code;
			}
		}
	}
	if (f) {
		fclose(f);
	}
	for (int k = 0; k < CART_SLOTS; k++) {
		char key[32];
		snprintf(key, sizeof(key), "armnes_cart_code_%d", k + 1);
		struct retro_variable variable = { key, chosen[k] ? chosen[k] : "none" };
		environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &variable);
	}
}

// The chosen cheats' codes to the cartridge, in order, as far as its slots go
static void cartImport(void) {
	struct Cheat codes[CART_SLOTS];
	uint8_t table[CART_SLOTS][4];
	int n = 0;
	for (int k = 0; k < CART_SLOTS; k++) {
		char key[32];
		snprintf(key, sizeof(key), "armnes_cart_code_%d", k + 1);
		const char* value = k < cartSlots() ? optionValue(environ_cb, key) : NULL;
		*cartCodesIn[k] = '\0';
		for (size_t i = 0; value && i < cartCheatCount; i++) {
			const struct CartCheat* c = &cartCheats[i];
			if (strcmp(value, c->code) || n + c->count > cartSlots()) {
				continue;
			}
			snprintf(cartCodesIn[k], sizeof(cartCodesIn[k]), "%s", c->code);
			for (int j = 0; j < c->count; j++, n++) {
				codes[n] = c->codes[j];
				table[n][0] = 0;
				table[n][1] = (uint8_t) (c->codes[j].address >> 8);
				table[n][2] = (uint8_t) c->codes[j].address;
				table[n][3] = c->codes[j].value;
			}
			break;
		}
	}
	genieImport(codes, parIsGenie ? n : 0);
	parImport(table, parIsGenie ? 0 : n);
}

// Whether a Cheat Code option differs from what the cartridge started with
static bool cartCodesChanged(void) {
	for (int k = 0; k < cartSlots(); k++) {
		char key[32];
		snprintf(key, sizeof(key), "armnes_cart_code_%d", k + 1);
		const char* value = optionValue(environ_cb, key);
		bool const chosen = value && strcmp(value, "none");
		if (strcmp(chosen ? value : "", cartCodesIn[k])) {
			for (size_t i = 0; chosen && i < cartCheatCount; i++) {
				if (!strcmp(value, cartCheats[i].code)) {
					return true;
				}
			}
			if (!chosen && *cartCodesIn[k]) {
				return true;
			}
		}
	}
	return false;
}

// The codes put in for this game, kept for next time with those of the other games (the most recent
// first, up to CART_GAMES); and the Game on Top choice, on the first line (onTop, or as it was)
enum { CART_GAMES = 1000 };

static void cartKeepChoices(const char* onTop) {
	if (!*parTop) {
		return;
	}
	char file[4096];
	parChoiceFile(file, sizeof(file));
	size_t size = 0;
	uint8_t* read = readFile(file, &size);
	char* old = malloc(size + 1);
	if (old) {
		if (read) {
			memcpy(old, read, size);
		}
		old[size] = '\0';
	}
	free(read);
	FILE* f = fopen(file, "w");
	if (!f) {
		free(old);
		return;
	}
	// The first line, then this game's codes
	char* rest = old ? old + strcspn(old, "\n") : NULL;
	char const keep = rest ? *rest : '\0';
	if (rest) {
		*rest = '\0';
	}
	fprintf(f, "%s\n", onTop ? onTop : old ? old : "");
	int games = 0;
	bool any = false;
	for (int k = 0; k < CART_SLOTS; k++) {
		any = any || *cartCodesIn[k];
	}
	if (any) {
		fprintf(f, "Game: %s\n", parTop);
		for (int k = 0; k < CART_SLOTS; k++) {
			if (*cartCodesIn[k]) {
				fprintf(f, "Code %d: %s\n", k + 1, cartCodesIn[k]);
			}
		}
		games++;
	}
	// The other games' (codes written before under the first line are its game's)
	const char* game = old ? old : "";
	bool copying = old && strcmp(old, parTop) && ++games <= CART_GAMES;
	bool header = false;
	for (char* line = rest && keep ? rest + 1 : NULL; line && *line;) {
		char* end = line + strcspn(line, "\n");
		char const after = *end;
		*end = '\0';
		if (!strncmp(line, "Game: ", 6)) {
			game = line + 6;
			copying = strcmp(game, parTop) && ++games <= CART_GAMES;
			header = false;
		} else if (copying && !strncmp(line, "Code ", 5)) {
			if (!header) {
				fprintf(f, "Game: %s\n", game);
				header = true;
			}
			fprintf(f, "%s\n", line);
		}
		line = after ? end + 1 : NULL;
	}
	fclose(f);
	free(old);
}

// The chosen game under the Pro Action Replay, the console started again with its menu; or none
static bool parPutGame(const char* name) {
	parWriteSave(true);
	free(parSaveKept);
	parSaveKept = NULL;
	parSaveSize = 0;
	*parTop = '\0';
	parAttach(&nes, NULL, 0);
	genieAttach(&nes, NULL, 0);
	size_t size = 0;
	uint8_t* data = name ? parReadGame(name, &size) : NULL;
	cartFree(&nes);
	bool ok = data && nesLoad(&nes, data, size);
	free(data);
	if (!ok) {
		// The cartridge alone: its menu, and nothing to start
		nesLoad(&nes, parImage, parImageSize);
		cartListCheats();
		return false;
	}
	snprintf(parTop, sizeof(parTop), "%s", name);
	// The game's cheats to choose from, and those chosen, in the cartridge as it powers on
	cartListCheats();
	cartImport();
	if (parIsGenie) {
		genieAttach(&nes, parImage, parImageSize);
	} else {
		parAttach(&nes, parImage, parImageSize);
	}
	nesReset(&nes, true);
	parSaveFile(name, parSave, sizeof(parSave));
	parReadSave();
	cartKeepChoices(name);
	return true;
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
		const char* top = parLoaded ? parChosen() : NULL;
		bool const genieWanted = optionIs(environ_cb, "armnes_game_genie", "On");
		bool const genieChanged = genieWanted != genieOptionWas;
		genieOptionWas = genieWanted;
		genieMissing = genieMissing && genieWanted;
		if (genieChanged && genieWanted != genieOver && !genieMissing && genieOverFits()) {
			if (genieOverPut(genieWanted) || !genieWanted) {
				struct retro_message shown = { genieWanted ? "Game Genie on: the console restarts with its code screen" :
					"Game Genie off: the console restarts", 240 };
				environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
			}
			showOptions();
		} else if (genieOver && *parTop && cartCodesChanged()) {
			cartImport();
			cartKeepChoices(NULL);
			nesReset(&nes, true);
			struct retro_message shown = { "Game Genie: the console restarts with the codes chosen", 240 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
		} else if (parLoaded && *parTop && top && !strcmp(top, parTop) && cartCodesChanged()) {
			parPutGame(top);
			static char text[200];
			snprintf(text, sizeof(text), "%s: the console restarts with the codes chosen", parName());
			struct retro_message shown = { text, 240 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
		} else if (parLoaded && strcmp(top ? top : "", parTop)) {
			enum Region before = nes.region;
			char message[300];
			if (parPutGame(top)) {
				const char* dot = strrchr(parTop, '.');
				snprintf(message, sizeof(message), "%s: %.*s on top", parName(), dot ? (int) (dot - parTop) : (int) strlen(parTop), parTop);
			} else {
				snprintf(message, sizeof(message), "%s: no game on top", parName());
			}
			struct retro_message shown = { message, 240 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &shown);
			setMemoryMaps();
			if (nes.region != before) {
				struct retro_system_av_info av;
				retro_get_system_av_info(&av);
				environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av);
			}
		}
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

// A cheat cartridge's image from a file (in a .zip, its first image), the headerless Game Genie made whole
static uint8_t* cartridgeRead(const char* path, size_t* size) {
	uint8_t* data = NULL;
	if (path_is_compressed_file(path)) {
		struct string_list* inner = file_archive_get_file_list(path, "nes|unh");
		data = inner && inner->size ? archiveRead(path, inner->elems[0].data, size) : NULL;
		string_list_free(inner);
	} else {
		data = readFile(path, size);
	}
	uint8_t* whole = data ? genieImageFrom(data, *size, size) : NULL;
	if (whole) {
		free(data);
		data = whole;
	}
	if (data && !genieIdentify(data, *size)) {
		free(data);
		data = NULL;
	}
	return data;
}

// The Gamate's BIOS (4 KiB) from the system folder: either of its two versions, by MAME's names (loose
// or in its gamate.zip) or No-Intro's (loose or each in its own zip); NULL without it
static uint8_t* gamateBios(void) {
	const char* system = NULL;
	if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system) || !system) {
		return NULL;
	}
	// Each file, and the archive it may be in
	static const char* const places[][2] = {
		{ "gamate_bios_umc.bin", "gamate.zip" },
		{ "gamate_bios_bit.bin", "gamate.zip" },
		{ "[BIOS] Gamate (World).bin", "[BIOS] Gamate (World).zip" },
		{ "[BIOS] Gamate (World) (Rev 1).bin", "[BIOS] Gamate (World) (Rev 1).zip" },
	};
	char path[4096];
	for (size_t i = 0; i < sizeof(places) / sizeof(*places); i++) {
		size_t size = 0;
		snprintf(path, sizeof(path), "%s/%s", system, places[i][0]);
		uint8_t* data = path_is_valid(path) ? readFile(path, &size) : NULL;
		if (!data || size != GM_BIOS_SIZE) {
			free(data);
			snprintf(path, sizeof(path), "%s/%s", system, places[i][1]);
			data = path_is_valid(path) ? archiveRead(path, places[i][0], &size) : NULL;
		}
		if (data && size == GM_BIOS_SIZE) {
			return data;
		}
		free(data);
	}
	return NULL;
}

// The Game Genie in the system folder, or NULL
static uint8_t* genieFind(size_t* size) {
	static const char* const names[] = {
		"[BIOS] Game Genie (USA, Europe) (Unl).unh", "[BIOS] Game Genie (USA, Europe) (Unl).zip", "gamegenie.nes",
		"Nintendo - Nintendo Entertainment System/gamegenie.nes", "Game Genie (USA, Europe) (Unl).unh",
	};
	const char* system = NULL;
	environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system);
	for (size_t i = 0; system && *system && i < sizeof(names) / sizeof(*names); i++) {
		char path[8192];
		snprintf(path, sizeof(path), "%s/%s", system, names[i]);
		uint8_t* image = cartridgeRead(path, size);
		if (image) {
			return image;
		}
	}
	return NULL;
}

// Whether the Game Genie fits what's loaded: a cart (or none), not a disk, an NSF, a VS. System game or
// a cheat cartridge loaded as the game
static bool genieOverFits(void) {
	return loaded && !parLoaded && !nes.dual && !nes.cart.disk && !nes.cart.vs && nes.board != &boardNsf;
}

// The Game Genie option: in, with the codes chosen from RetroArch's cheats for the game; or out. The
// console restarts either way. False when it's wanted but not in the system folder.
static bool genieOverPut(bool on) {
	if (!on) {
		if (genieOver) {
			genieAttach(&nes, NULL, 0);
			genieOver = parIsGenie = false;
			*parTop = '\0';
			cartListCheats();
			nesReset(&nes, true);
		}
		return true;
	}
	size_t size = 0;
	uint8_t* image = genieFind(&size);
	genieMissing = !image;
	if (!image) {
		struct retro_message message = { "armNES: no Game Genie in the system folder ([BIOS] Game Genie (USA, Europe) (Unl).unh "
			"or gamegenie.nes)", 480 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		return false;
	}
	genieOver = parIsGenie = true;
	snprintf(parTop, sizeof(parTop), "%s", contentName);
	cartListCheats();
	cartImport();
	cartKeepChoices(NULL);
	genieAttach(&nes, image, size);
	free(image);
	nesReset(&nes, true);
	return true;
}

// A state says whether the Game Genie was plugged in, and netplay's host sends its own as a player joins
// (and again if they drift apart): loading one plugs this side's Game Genie in or out to match, leaving
// the option as it was. Each side needs it in its own system folder. Not for a cheat cartridge loaded as the
// game, whose state is its own.
static void genieFollowState(const uint8_t* data, size_t size) {
	size_t const need = nesStateSize(&nes);
	bool const wanted = size >= need && data[need - genieStateSize()] != 0;
	if (wanted == genieOver || parLoaded || (wanted && (genieMissing || !genieOverFits()))) {
		return;
	}
	if (genieOverPut(wanted)) {
		struct retro_message message = { wanted ? "Game Genie on, as the state has it" : "Game Genie off, as the state has it", 240 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}
}

// The console with no cartridge (started with no game): NROM whose program spins at $C000, the screen
// left off
static const uint8_t* noCartridge(size_t* size) {
	enum { PRG = 0x4000, SIZE = 16 + PRG + 0x2000 };
	static uint8_t image[SIZE];
	static const uint8_t header[16] = { 'N', 'E', 'S', 0x1A, 1, 1 };
	memcpy(image, header, sizeof(header));
	uint8_t* prg = image + 16;
	prg[0] = 0x4C; // JMP $C000
	prg[1] = 0x00;
	prg[2] = 0xC0;
	for (int v = PRG - 6; v < PRG; v += 2) {
		prg[v] = 0x00;
		prg[v + 1] = 0xC0;
	}
	*size = SIZE;
	return image;
}

bool retro_load_game(const struct retro_game_info* game) {
	netplaySeen = false;
	bool const empty = !game || (!game->data && !game->path);
	genieMissing = false;
	const char* contentPath = empty || !game->path ? "" : game->path;
	snprintf(contentName, sizeof(contentName), "%s", strrchr(contentPath, '/') ? strrchr(contentPath, '/') + 1 : contentPath);
	enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format)) {
		return false;
	}
	giveDescriptors();

	// A Supervision game, by its extension (.sv, or .ws as MAME also takes)
	svLoaded = false;
	gmLoaded = false;
	handheldCheatCount = 0;
	const char* dot = strrchr(contentName, '.');
	if (!empty && dot && (!strcasecmp(dot, ".sv") || !strcasecmp(dot, ".ws"))) {
		const uint8_t* data;
		size_t size;
		free(unpacked);
		unpacked = NULL;
		if (!unpack(game, &data, &size) || !svLoad(&supervision, data, size)) {
			struct retro_message message = { "armNES: this Supervision game couldn't be loaded", 360 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
			return false;
		}
		svLoaded = true;
		svLevelsSet = false;
		svGameChosen = 0;
		if (supervision.games > 1) {
			svListGames();
		}
		setSupervisionMemoryMaps();
		readInputOptions();
		readSupervisionOptions();
		showOptions();
		return true;
	}

	// A Gamate game, by its extension (.bin, as No-Intro and MAME's lists have them), with the BIOS
	if (!empty && dot && !strcasecmp(dot, ".bin")) {
		const uint8_t* data;
		size_t size;
		free(unpacked);
		unpacked = NULL;
		uint8_t* bios = gamateBios();
		if (!bios) {
			struct retro_message message = { "armNES: the Gamate needs its BIOS in the system folder: gamate_bios_umc.bin, "
				"gamate.zip or [BIOS] Gamate (World).bin", 480 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
			return false;
		}
		// The second Gamate switched on 65536 cycles (1.8 frames) after the first: half the BIOS's
		// 8-tick wait, which a game's link handshake can have to fall within either way
		gamateTwo.switchedOnLater = 65536;
		bool const ok = unpack(game, &data, &size) && gmLoad(&gamate, bios, data, size) && gmLoad(&gamateTwo, bios, data, size);
		free(bios);
		if (!ok) {
			struct retro_message message = { "armNES: this Gamate game couldn't be loaded", 360 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
			return false;
		}
		gmLoaded = true;
		setGamateMemoryMaps();
		svLevelsSet = false;
		gmLinked = false;
		gmClientUnknown = false;
		gmScreenOption = -1;
		gmControllerFocus = 0;
		gmSessionTag = 0;
		gmJoined = false;
		readGamateLink(true);
		gamateNetplay(optionIs(environ_cb, "armnes_gm_link", "separate"), optionIs(environ_cb, "armnes_gm_link_timing", "exact"));
		readInputOptions();
		readSupervisionOptions();
		showOptions();
		return true;
	}

	nesInit(&nes);
	nes.audioOut = audio;
	nes.audioCapacity = sizeof(audio) / sizeof(audio[0]);
	const uint8_t* data;
	size_t size;
	free(unpacked);
	unpacked = NULL;
	if (empty) {
		data = noCartridge(&size);
	} else if (!unpack(game, &data, &size)) {
		struct retro_message message = { "armNES: no game in this .zip, or not all of a MAME set's chips", 360 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		return false;
	}
	// No-Intro's headerless Game Genie, made whole
	size_t wholeSize = 0;
	uint8_t* whole = genieImageFrom(data, size, &wholeSize);
	if (whole) {
		free(unpacked);
		unpacked = whole;
		data = whole;
		size = wholeSize;
	}
	// A cheat cartridge (a Pro Action Replay or a Game Genie): the game on top is one beside it
	parAttach(&nes, NULL, 0);
	genieAttach(&nes, NULL, 0);
	parIsGenie = genieIdentify(data, size);
	parLoaded = parIsGenie || parIdentify(data, size);
	*parTop = '\0';
	// The game's folder: the games for Game on Top, and where the cheat choices go without a save folder
	const char* slash = strrchr(contentPath, '/');
	snprintf(parFolder, sizeof(parFolder), "%.*s", slash ? (int) (slash - contentPath) : 1, slash ? contentPath : ".");
	if (parLoaded) {
		free(parImage);
		parImageSize = 0;
		if ((parImage = malloc(size))) {
			memcpy(parImage, data, size);
			parImageSize = size;
		}
		snprintf(parSelf, sizeof(parSelf), "%s", contentName);
		parListGames();
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
	nes.exactPpu = optionIs(environ_cb, "armnes_ppu", "Exact");
	nes.dotPpu = optionIs(environ_cb, "armnes_ppu", "Balanced");
	nesInit(&sub);
	sub.exactPpu = nes.exactPpu;
	sub.dotPpu = nes.dotPpu;
	sub.audioOut = subAudio;
	sub.audioCapacity = sizeof(subAudio) / sizeof(subAudio[0]);
	memset(subPicture, 0, sizeof(subPicture));
	if (parLoaded) {
		nesFree(&sub);
		if (!parPutGame(parChosen())) {
			static char text[200];
			snprintf(text, sizeof(text), "%s: pick the game plugged into it in Core Options > System > Game on Top", parName());
			struct retro_message message = { text, 480 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		}
		if (!nes.board) {
			return false;
		}
	} else if (!nesLoadDual(&nes, &sub, vsShared, data, size)) {
		nesFree(&sub);
		if (!nesLoad(&nes, data, size)) {
			if (nes.cart.mapper || nes.cart.prgSize) {
				struct retro_message message = { "armNES: this game's board isn't supported yet", 360 };
				environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
			}
			return false;
		}
	}
	loaded = true;
	genieOptionWas = optionIs(environ_cb, "armnes_game_genie", "On");
	if (genieOptionWas && genieOverFits()) {
		genieOverPut(true);
	} else if (!parLoaded) {
		// The Cheat Code options empty, with no cheat cartridge
		*parTop = '\0';
		cartListCheats();
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
	size_t boardRam;
	// The game on top keeps its save itself (parWriteSave)
	if (!parLoaded && boardBatteryRam(&boardRam) && (saveBlock = malloc(nes.cart.batterySize + boardRam))) {
		saveBlockIn = true;
		saveBlockSync();
		saveBlockIn = false;
	}
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
	if (gmLoaded) {
		gamateNetplay(false, false);
		gmUnload(&gamate);
		gmUnload(&gamateTwo);
		gmLoaded = false;
		free(unpacked);
		unpacked = NULL;
		showOptions();
		return;
	}
	if (svLoaded) {
		svUnload(&supervision);
		svLoaded = false;
		free(unpacked);
		unpacked = NULL;
		showOptions();
		return;
	}
	useSensor(-1);
	if (parLoaded) {
		parWriteSave(true);
	}
	if (parLoaded || genieOver) {
		parAttach(&nes, NULL, 0);
		genieAttach(&nes, NULL, 0);
		parLoaded = genieOver = parIsGenie = false;
		*parTop = '\0';
	}
	if (loaded) {
		if (nes.dual) {
			nesFree(&sub);
		}
		nesFree(&nes);
		free(unpacked);
		unpacked = NULL;
		free(saveBlock);
		saveBlock = NULL;
		loaded = false;
		showOptions();
	}
}

unsigned retro_get_region(void) {
	if (svLoaded || gmLoaded) {
		return RETRO_REGION_NTSC;
	}
	return nes.region == REGION_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

// A DualSystem's state: both boards', then the RAM they share, the second's last picture and the one
// it's drawing (its frames run out of step with the main board's)
size_t retro_serialize_size(void) {
	if (svLoaded) {
		return svStateSize();
	}
	if (gmLoaded) {
		return 2 * gmStateSize() + sizeof(gmSessionTag);
	}
	if (!loaded) {
		return 0;
	}
	return nes.dual ? nesStateSize(&nes) + nesStateSize(&sub) + sizeof(vsShared) + 2 * sizeof(subPicture) : nesStateSize(&nes);
}

bool retro_serialize(void* data, size_t size) {
	checkNetplay();
	if (svLoaded) {
		if (size < svStateSize()) {
			return false;
		}
		svSave(&supervision, data);
		return true;
	}
	if (gmLoaded) {
		if (size < 2 * gmStateSize() + sizeof(gmSessionTag)) {
			return false;
		}
		gamateTagSession();
		gmSave(&gamate, data);
		gmSave(&gamateTwo, (uint8_t*) data + gmStateSize());
		memcpy((uint8_t*) data + 2 * gmStateSize(), &gmSessionTag, sizeof(gmSessionTag));
		return true;
	}
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
	checkNetplay();
	if (svLoaded) {
		return svLoadState(&supervision, data, size);
	}
	if (gmLoaded) {
		if (size < 2 * gmStateSize() + sizeof(gmSessionTag) || !gmLoadState(&gamate, data, gmStateSize()) ||
			!gmLoadState(&gamateTwo, (const uint8_t*) data + gmStateSize(), gmStateSize())) {
			return false;
		}
		// A state of another device's session, loaded by netplay: this device joined it. One loaded by
		// hand in netplay keeps this device's own tag.
		uint32_t tag;
		memcpy(&tag, (const uint8_t*) data + 2 * gmStateSize(), sizeof(tag));
		int context = RETRO_SAVESTATE_CONTEXT_NORMAL;
		bool const byNetplay = environ_cb(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &context) && context == RETRO_SAVESTATE_CONTEXT_ROLLBACK_NETPLAY;
		if (!netplaySeen || byNetplay) {
			gamateTagSession();
			gmJoined |= netplaySeen && tag != gmSessionTag;
			gmSessionTag = tag;
		}
		return true;
	}
	if (!loaded) {
		return false;
	}
	// One board's state checks its own size (1.1's are a little shorter). A state loaded before the
	// first frame keeps its own save RAM, as it does in one block; one refused leaves the save alone.
	if (!nes.dual) {
		genieFollowState(data, size);
		bool const ok = nesLoadState(&nes, data, size);
		saveBlockIn |= ok;
		return ok;
	}
	if (size < retro_serialize_size()) {
		return false;
	}
	saveBlockIn = true;
	size_t first = nesStateSize(&nes), second = nesStateSize(&sub);
	if (!nesLoadState(&nes, data, first) || !nesLoadState(&sub, (const uint8_t*) data + first, second)) {
		return false;
	}
	memcpy(vsShared, (const uint8_t*) data + first + second, sizeof(vsShared));
	memcpy(subPicture, (const uint8_t*) data + first + second + sizeof(vsShared), sizeof(subPicture));
	memcpy(sub.frameBuffer, (const uint8_t*) data + first + second + sizeof(vsShared) + sizeof(subPicture), sizeof(subPicture));
	return true;
}

// A code's writes, each "AAAA:VV" (or with a space or =, or "AAAAVV" alone), several joined by +; false
// if one isn't understood or there's no room
static bool handheldCheatAdd(const char* code) {
	while (*code) {
		char* end;
		unsigned long address = strtoul(code, &end, 16), value;
		if (end - code == 6 && address <= 0xffffff) {
			value = address & 0xff;
			address >>= 8;
		} else if (end - code >= 1 && end - code <= 4 && (*end == ':' || *end == ' ' || *end == '=')) {
			const char* digits = end + 1;
			value = strtoul(digits, &end, 16);
			if (end == digits || end - digits > 2) {
				return false;
			}
		} else {
			return false;
		}
		if (address > 0xffff || handheldCheatCount == HANDHELD_CHEATS) {
			return false;
		}
		handheldCheats[handheldCheatCount].address = (uint16_t) address;
		handheldCheats[handheldCheatCount++].value = (uint8_t) value;
		while (*end == ' ' || *end == '+') {
			end++;
		}
		code = end;
	}
	return true;
}

// Before a frame: the cheats into RAM, the only memory they can change
static void handheldCheatsApply(void) {
	for (int i = 0; i < handheldCheatCount; i++) {
		uint16_t const a = handheldCheats[i].address;
		if (a < 0x2000) {
			if (svLoaded) {
				supervision.m.ram[a] = handheldCheats[i].value;
			} else {
				gamate.m.ram[a & 0x3ff] = gamateTwo.m.ram[a & 0x3ff] = handheldCheats[i].value;
			}
		}
	}
}

void retro_cheat_reset(void) {
	if (svLoaded || gmLoaded) {
		handheldCheatCount = 0;
		return;
	}
	nesCheatClear(&nes);
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	(void) index;
	if (svLoaded || gmLoaded) {
		if (enabled && code && !handheldCheatAdd(code)) {
			struct retro_message message = { "armNES: a cheat code wasn't understood (AAAA:VV writes VV to RAM at AAAA)", 240 };
			environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
		}
		return;
	}
	if (enabled && code && !nesCheatAdd(&nes, code)) {
		struct retro_message message = { "armNES: a cheat code wasn't understood", 180 };
		environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &message);
	}
}

void* retro_get_memory_data(unsigned id) {
	if (svLoaded) {
		return id == RETRO_MEMORY_SYSTEM_RAM ? supervision.m.ram : id == RETRO_MEMORY_VIDEO_RAM ? supervision.m.vram : NULL;
	}
	if (gmLoaded) {
		return id == RETRO_MEMORY_SYSTEM_RAM ? gamate.m.ram : id == RETRO_MEMORY_VIDEO_RAM ? gamate.m.vram : NULL;
	}
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		// The game on top under a Pro Action Replay keeps its own save
		if (parLoaded) {
			return NULL;
		}
		// A disk's save is the disk
		if (loaded && nes.cart.disk) {
			return nes.cart.disk;
		}
		if (saveBlock) {
			return saveBlock;
		}
		return loaded && nes.cart.battery ? nes.cart.wram : NULL;
	case RETRO_MEMORY_SYSTEM_RAM:
		return nes.ram;
	default:
		return NULL;
	}
}

size_t retro_get_memory_size(unsigned id) {
	if (svLoaded) {
		return id == RETRO_MEMORY_SYSTEM_RAM ? sizeof(supervision.m.ram) : id == RETRO_MEMORY_VIDEO_RAM ? sizeof(supervision.m.vram) : 0;
	}
	if (gmLoaded) {
		return id == RETRO_MEMORY_SYSTEM_RAM ? sizeof(gamate.m.ram) : id == RETRO_MEMORY_VIDEO_RAM ? sizeof(gamate.m.vram) : 0;
	}
	switch (id) {
	case RETRO_MEMORY_SAVE_RAM:
		if (parLoaded) {
			return 0;
		}
		if (loaded && nes.cart.disk) {
			return (size_t) nes.cart.diskSides * nes.cart.diskSideSize;
		}
		size_t boardRam = 0;
		if (saveBlock) {
			boardBatteryRam(&boardRam);
		}
		return loaded && nes.cart.battery ? nes.cart.batterySize + boardRam : 0;
	case RETRO_MEMORY_SYSTEM_RAM:
		return sizeof(nes.ram);
	default:
		return 0;
	}
}
