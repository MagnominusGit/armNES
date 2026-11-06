/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "libretro.h"

#include <dlfcn.h>
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

static bool environment(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixelFormat = *(enum retro_pixel_format*) data;
		return true;
	case RETRO_ENVIRONMENT_SET_MESSAGE:
		fprintf(stderr, "MESSAGE: %s\n", ((const struct retro_message*) data)->msg);
		return true;
	// CORETEST_SYSTEM=<dir>: where cores find BIOS files
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
	for (frame = 0; frame < start; frame++) {
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
	p_retro_unload_game();
	p_retro_deinit();
	return same ? 0 : 1;
}
