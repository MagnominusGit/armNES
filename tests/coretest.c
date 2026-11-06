/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

// coretest <core.so> <rom>: loads the game through the libretro API, runs it, and checks that a
// save state taken and loaded again replays the same picture and sound.

static uint32_t videoHash, audioHash;
static bool rgb565;
static unsigned frameWidth, frameHeight;
static size_t audioFrames;

static bool environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		rgb565 = *(enum retro_pixel_format*) data == RETRO_PIXEL_FORMAT_RGB565;
		return rgb565;
	// CORETEST_SYSTEM=<dir>: where cores find BIOS files
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
		*(const char**) data = getenv("CORETEST_SYSTEM");
		return *(const char**) data != NULL;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
		((struct retro_variable*) data)->value = NULL;
		return false;
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
		const uint16_t* row = (const uint16_t*) ((const uint8_t*) data + y * pitch);
		for (unsigned x = 0; x < width; x++) {
			uint16_t p = row[x];
			uint8_t rgb[3] = { (p >> 11) << 3, ((p >> 5) & 0x3F) << 2, (p & 0x1F) << 3 };
			if (!rgb565) {
				rgb[0] = ((p >> 10) & 0x1F) << 3;
				rgb[1] = ((p >> 5) & 0x1F) << 3;
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
		videoHash = crc32(videoHash, (const uint8_t*) data + y * pitch, width * 2);
	}
}

static size_t audioBatch(const int16_t* data, size_t frames) {
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

static int16_t inputState(unsigned port, unsigned device, unsigned index, unsigned id) {
	(void) device;
	(void) index;
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
	p_retro_set_environment(environment);
	p_retro_set_video_refresh(video);
	p_retro_set_audio_sample(audioSample);
	p_retro_set_audio_sample_batch(audioBatch);
	p_retro_set_input_poll(inputPoll);
	p_retro_set_input_state(inputState);
	p_retro_init();

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
	struct retro_system_av_info av;
	p_retro_get_system_av_info(&av);
	unsigned start = getenv("CORETEST_FRAME") ? atoi(getenv("CORETEST_FRAME")) + 1 : 600;
	start = start < 600 ? 600 : start;
	for (frame = 0; frame < start; frame++) {
		p_retro_run();
	}
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
	printf("%s %ux%u %.4f fps %.0f Hz, state %zu bytes, %zu audio frames, replay %s\n", same ? "OK" : "FAIL",
		frameWidth, frameHeight, av.timing.fps, av.timing.sample_rate, stateSize, audioFrames, same ? "same" : "DIFFERS");
	p_retro_unload_game();
	p_retro_deinit();
	return same ? 0 : 1;
}
