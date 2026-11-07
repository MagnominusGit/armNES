/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"

#include <dlfcn.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

// coretest <core.so> <rom>: loads the game through the libretro API, runs it, and checks that a
// save state taken and loaded again replays the same picture and sound.

static uint32_t videoHash, audioHash;
static enum retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned frameWidth, frameHeight;
static size_t audioFrames;

static struct retro_disk_control_callback disk;

// CORETEST_LOG: the core's log lines on stderr
static void logLine(enum retro_log_level level, const char* format, ...) {
	(void) level;
	va_list args;
	va_start(args, format);
	vfprintf(stderr, format, args);
	va_end(args);
}

static bool environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		if (!getenv("CORETEST_LOG")) {
			return false;
		}
		((struct retro_log_callback*) data)->log = logLine;
		return true;
	case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE:
		disk = *(const struct retro_disk_control_callback*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixelFormat = *(enum retro_pixel_format*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		fprintf(stderr, "MESSAGE: %s\n", ((const struct retro_message*) data)->msg);
		return true;
	// CORETEST_SYSTEM=<dir>: where cores find BIOS files
	// CORETEST_SAVE=<dir>: where cores keep saves (the system folder if not given)
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char**) data = getenv("CORETEST_SAVE") ? getenv("CORETEST_SAVE") : getenv("CORETEST_SYSTEM");
		return *(const char**) data != NULL;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char**) data = getenv("CORETEST_SYSTEM");
		return *(const char**) data != NULL;
	// CORETEST_VARS="key=value,...": core options
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = data;
		static char value[64];
		const char* vars = getenv("CORETEST_VARS");
		v->value = NULL;
		size_t keyLength = strlen(v->key);
		for (const char* p = vars; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : NULL) {
			if (!strncmp(p, v->key, keyLength) && p[keyLength] == '=') {
				const char* start = p + keyLength + 1;
				size_t length = strcspn(start, ",");
				length = length < sizeof(value) - 1 ? length : sizeof(value) - 1;
				memcpy(value, start, length);
				value[length] = 0;
				v->value = value;
				return true;
			}
		}
		return false;
	}
	default:
		return false;
	}
}

static unsigned frame;

// CORETEST_PPM=<file>: the picture at frame 599 (or CORETEST_FRAME), for comparing cores by eye
static void dumpFrame(const void* data, unsigned width, unsigned height, size_t pitch) {
	const char* path = getenv("CORETEST_PPM");
	FILE* f = path ? fopen(path, "wb") : NULL;
	if (!f) {
		return;
	}
	fprintf(f, "P6\n%u %u\n255\n", width, height);
	for (unsigned y = 0; y < height; y++) {
		const uint8_t* line = (const uint8_t*) data + y * pitch;
		for (unsigned x = 0; x < width; x++) {
			uint8_t rgb[3];
			if (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t p = ((const uint32_t*) line)[x];
				rgb[0] = p >> 16;
				rgb[1] = p >> 8;
				rgb[2] = p;
			} else {
				uint16_t p = ((const uint16_t*) line)[x];
				bool rgb565 = pixelFormat == RETRO_PIXEL_FORMAT_RGB565;
				rgb[0] = (rgb565 ? p >> 11 : (p >> 10) & 0x1F) << 3;
				rgb[1] = rgb565 ? ((p >> 5) & 0x3F) << 2 : ((p >> 5) & 0x1F) << 3;
				rgb[2] = (p & 0x1F) << 3;
			}
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static void video(const void* data, unsigned width, unsigned height, size_t pitch) {
	static int dumpAt = -1;
	if (dumpAt < 0) {
		dumpAt = getenv("CORETEST_FRAME") ? atoi(getenv("CORETEST_FRAME")) : 599;
	}
	if (frame == (unsigned) dumpAt && data) {
		dumpFrame(data, width, height, pitch);
	}
	frameWidth = width;
	frameHeight = height;
	for (unsigned y = 0; y < height; y++) {
		videoHash = crc32(videoHash, (const uint8_t*) data + y * pitch, width * (pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2));
	}
}

// CORETEST_PCM=<file>: the sound up to the save state, as 16-bit stereo
static bool dumpingSound;

static size_t audioBatch(const int16_t* data, size_t frames) {
	static FILE* pcm;
	static bool opened;
	if (!opened) {
		opened = true;
		pcm = getenv("CORETEST_PCM") ? fopen(getenv("CORETEST_PCM"), "wb") : NULL;
	}
	if (pcm && dumpingSound) {
		fwrite(data, 4, frames, pcm);
	}
	audioHash = crc32(audioHash, (const uint8_t*) data, frames * 4);
	audioFrames += frames;
	return frames;
}

static void audioSample(int16_t l, int16_t r) {
	(void) l;
	(void) r;
}

static void inputPoll(void) {
}

// CORETEST_PADS=n: controllers 2 to n each hold a button: B, Select, Up
// CORETEST_AIM="x,y": a light gun held there (-32767 to 32767), trigger twice a second
// CORETEST_MOUSE="n[,m]": the mouse moves n a frame, right for 2 seconds then left, and m down; left
// button twice a second
static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	if (device == RETRO_DEVICE_LIGHTGUN && getenv("CORETEST_AIM")) {
		int x = 0, y = 0;
		sscanf(getenv("CORETEST_AIM"), "%d,%d", &x, &y);
		return id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_X ? x : id == RETRO_DEVICE_ID_LIGHTGUN_SCREEN_Y ? y :
			id == RETRO_DEVICE_ID_LIGHTGUN_TRIGGER ? frame % 30 < 3 : 0;
	}
	// CORETEST_TOUCH="x,y": a touch there for 2 frames, twice a second
	if (device == RETRO_DEVICE_POINTER && getenv("CORETEST_TOUCH")) {
		int x = 0, y = 0;
		sscanf(getenv("CORETEST_TOUCH"), "%d,%d", &x, &y);
		return id == RETRO_DEVICE_ID_POINTER_X ? x : id == RETRO_DEVICE_ID_POINTER_Y ? y :
			id == RETRO_DEVICE_ID_POINTER_PRESSED ? frame % 30 < 2 : 0;
	}
	if (device == RETRO_DEVICE_MOUSE && getenv("CORETEST_MOUSE")) {
		int step = 0, down = 0;
		sscanf(getenv("CORETEST_MOUSE"), "%d,%d", &step, &down);
		return id == RETRO_DEVICE_ID_MOUSE_X ? (frame % 240 < 120 ? step : -step) : id == RETRO_DEVICE_ID_MOUSE_Y ? down :
			id == RETRO_DEVICE_ID_MOUSE_LEFT ? frame % 30 < 3 : 0;
	}
	// CORETEST_STICK="x,y": the right stick held there, R pressed twice a second
	if (getenv("CORETEST_STICK") && device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
		int x = 0, y = 0;
		sscanf(getenv("CORETEST_STICK"), "%d,%d", &x, &y);
		return id == RETRO_DEVICE_ID_ANALOG_X ? x : y;
	}
	if (getenv("CORETEST_STICK") && device == RETRO_DEVICE_JOYPAD && port == 0 && id == RETRO_DEVICE_ID_JOYPAD_R) {
		return frame % 30 < 3;
	}
	// CORETEST_TYPE="text": typed from frame 300, a key every 10 frames; | is Return
	if (device == RETRO_DEVICE_KEYBOARD && getenv("CORETEST_TYPE")) {
		const char* text = getenv("CORETEST_TYPE");
		unsigned i = (frame - 300) / 10;
		if (frame < 300 || i >= strlen(text) || (frame - 300) % 10 >= 5) {
			return 0;
		}
		char c = (char) tolower((unsigned char) text[i]);
		return id == (c == '|' ? RETROK_RETURN : (unsigned) c);
	}
	if (device != RETRO_DEVICE_JOYPAD) {
		return 0;
	}
	if (port && getenv("CORETEST_PADS") && port < (unsigned) atoi(getenv("CORETEST_PADS"))) {
		static const unsigned held[4] = { 0, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_UP };
		return port < 4 && id == held[port];
	}
	// CORETEST_IDLE: controller 1 left alone
	if (getenv("CORETEST_IDLE")) {
		return 0;
	}
	// CORETEST_L=frame: L pressed for 5 frames from then
	if (getenv("CORETEST_L") && port == 0 && id == RETRO_DEVICE_ID_JOYPAD_L) {
		unsigned at = (unsigned) atoi(getenv("CORETEST_L"));
		return frame >= at && frame < at + 5;
	}
	// CORETEST_SELECT: Select pressed a second before each Start
	if (getenv("CORETEST_SELECT") && port == 0 && id == RETRO_DEVICE_ID_JOYPAD_SELECT) {
		return frame % 300 >= 240 && frame % 300 < 246;
	}
	return port == 0 && ((id == RETRO_DEVICE_ID_JOYPAD_START && frame % 300 < 6) ||
		(id == RETRO_DEVICE_ID_JOYPAD_RIGHT && frame % 240 < 120) || (id == RETRO_DEVICE_ID_JOYPAD_A && frame % 60 < 20));
}

#define SYM(name) typeof(&name) p_##name = (typeof(&name)) dlsym(core, #name)

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: coretest <core.so> <rom>\n");
		return 2;
	}
	void* core = dlopen(argv[1], RTLD_NOW);
	if (!core) {
		fprintf(stderr, "%s\n", dlerror());
		return 2;
	}
	SYM(retro_set_environment);
	SYM(retro_set_video_refresh);
	SYM(retro_set_audio_sample);
	SYM(retro_set_audio_sample_batch);
	SYM(retro_set_input_poll);
	SYM(retro_set_input_state);
	SYM(retro_init);
	SYM(retro_load_game);
	SYM(retro_run);
	SYM(retro_serialize_size);
	SYM(retro_serialize);
	SYM(retro_unserialize);
	SYM(retro_get_system_av_info);
	SYM(retro_unload_game);
	SYM(retro_deinit);
	SYM(retro_set_controller_port_device);
	SYM(retro_get_memory_data);
	// Some cores set up in retro_init what the other callbacks use
	p_retro_set_environment(environment);
	p_retro_init();
	p_retro_set_video_refresh(video);
	p_retro_set_audio_sample(audioSample);
	p_retro_set_audio_sample_batch(audioBatch);
	p_retro_set_input_poll(inputPoll);
	p_retro_set_input_state(inputState);

	FILE* f = fopen(argv[2], "rb");
	if (!f) {
		return 2;
	}
	fseek(f, 0, SEEK_END);
	size_t size = ftell(f);
	fseek(f, 0, SEEK_SET);
	void* data = malloc(size);
	if (fread(data, 1, size, f) != size) {
		return 2;
	}
	fclose(f);
	struct retro_game_info game = { argv[2], data, size, NULL };
	if (!p_retro_load_game(&game)) {
		printf("LOAD FAILED\n");
		return 3;
	}
	// CORETEST_PORTS="device,device": libretro device ids for ports 1 and 2
	if (getenv("CORETEST_PORTS")) {
		unsigned devices[2] = { 1, 1 };
		sscanf(getenv("CORETEST_PORTS"), "%i,%i", (int*) &devices[0], (int*) &devices[1]);
		p_retro_set_controller_port_device(0, devices[0]);
		p_retro_set_controller_port_device(1, devices[1]);
	}
	struct retro_system_av_info av;
	p_retro_get_system_av_info(&av);
	// CORETEST_BENCH=frames: just the time retro_run takes for that many frames
	if (getenv("CORETEST_BENCH")) {
		int frames = atoi(getenv("CORETEST_BENCH"));
		struct timespec t0, t1;
		clock_gettime(CLOCK_MONOTONIC, &t0);
		for (frame = 0; frame < (unsigned) frames; frame++) {
			p_retro_run();
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("BENCH %.0f fps\n", frames / seconds);
		return 0;
	}
	unsigned start = getenv("CORETEST_FRAME") ? atoi(getenv("CORETEST_FRAME")) + 1 : 600;
	start = start < 600 ? 600 : start;
	dumpingSound = true;
	// CORETEST_DISK=frame:index: the disk ejected then, and that one put in a second later
	unsigned diskAt = 0, diskIndex = 0;
	if (getenv("CORETEST_DISK")) {
		sscanf(getenv("CORETEST_DISK"), "%u:%u", &diskAt, &diskIndex);
	}
	for (frame = 0; frame < start; frame++) {
		if (diskAt && disk.set_eject_state && frame == diskAt) {
			disk.set_eject_state(true);
			disk.set_image_index(diskIndex);
		} else if (diskAt && disk.set_eject_state && frame == diskAt + 60) {
			disk.set_eject_state(false);
		}
		p_retro_run();
	}
	dumpingSound = false;
	size_t stateSize = p_retro_serialize_size();
	void* state = malloc(stateSize);
	if (!p_retro_serialize(state, stateSize)) {
		printf("SERIALIZE FAILED\n");
		return 1;
	}
	uint32_t hashes[2][2];
	for (int pass = 0; pass < 2; pass++) {
		if (pass && !p_retro_unserialize(state, stateSize)) {
			printf("UNSERIALIZE FAILED\n");
			return 1;
		}
		videoHash = audioHash = 0;
		for (frame = start; frame < start + 120; frame++) {
			p_retro_run();
		}
		hashes[pass][0] = videoHash;
		hashes[pass][1] = audioHash;
	}
	bool same = hashes[0][0] == hashes[1][0] && hashes[0][1] == hashes[1][1];
	printf("%s %ux%u aspect %.4f %.4f fps %.0f Hz, state %zu bytes, %zu audio frames, replay %s\n", same ? "OK" : "FAIL",
		frameWidth, frameHeight, av.geometry.aspect_ratio, av.timing.fps, av.timing.sample_rate, stateSize, audioFrames, same ? "same" : "DIFFERS");
	// CORETEST_RAM=n: the first n bytes of the console's RAM
	if (getenv("CORETEST_RAM")) {
		const uint8_t* ram = p_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
		for (int i = 0; ram && i < atoi(getenv("CORETEST_RAM")); i++) {
			printf("%02X%c", ram[i], i % 16 == 15 ? '\n' : ' ');
		}
	}
	p_retro_unload_game();
	p_retro_deinit();
	return same ? 0 : 1;
}
