/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

#include <stdlib.h>
#include <zlib.h>

// The Famicom Disk System: the RAM adapter's 32 KB of RAM at $6000-$DFFF, the BIOS at $E000, 8 KB
// of CHR RAM, a timer IRQ, the disk drive and the wavetable sound. Disks are kept as the drive
// sees them, a byte every 149 CPU cycles: a long gap, then each block as a start mark, its data and
// CRC, and a short gap.

enum {
	FDS_SIDE_BYTES = 65500,
	LEAD_GAP = 3537,
	BLOCK_GAP = 122,
	BYTE_CYCLES = 149,
	// From the motor starting (or the head returning) to the start of the disk
	SPIN_UP = 50000,
};

struct Fds {
	uint16_t timerReload, timerCounter;
	bool timerRepeat, timerEnabled, timerIrq, diskIrq;
	bool diskEnabled, soundEnabled;
	uint8_t control, writeData, readData, external;
	bool transferred, endOfHead, scanning, gapEnded;
	int side;
	uint32_t position;
	int32_t delay;
	uint16_t crc;
	struct FdsSound sound;
};

#define FD STATE(Fds)

static void updateIrq(struct NES* nes) {
	nesSetIrq(nes, IRQ_MAPPER, FD->timerIrq || FD->diskIrq);
}

static uint8_t* sideData(struct NES* nes) {
	return FD->side >= 0 ? nes->cart.disk + FD->side * nes->cart.diskSideSize : NULL;
}

// CRC-16 as the drive adds it, low bit first
static uint16_t crcByte(uint16_t crc, uint8_t value) {
	for (int i = 0; i < 8; i++) {
		bool bit = (crc ^ (value >> i)) & 1;
		crc = (crc >> 1) ^ (bit ? 0x8408 : 0);
	}
	return crc;
}

// The drive: one byte under the head every BYTE_CYCLES while the motor runs
static void driveCycle(struct NES* nes) {
	struct Fds* f = FD;
	uint8_t* disk = sideData(nes);
	if (!disk || !(f->control & 0x01)) {
		f->endOfHead = true;
		f->scanning = false;
		return;
	}
	if ((f->control & 0x02) && !f->scanning) {
		return;
	}
	if (f->endOfHead) {
		f->endOfHead = false;
		f->delay = SPIN_UP;
		f->position = 0;
		f->gapEnded = false;
		return;
	}
	if (--f->delay > 0) {
		return;
	}
	f->scanning = true;
	bool started = f->control & 0x40;
	bool irq = f->control & 0x80;
	uint8_t value;
	if (f->control & 0x04) {
		value = disk[f->position];
		if (!started) {
			f->gapEnded = false;
			f->crc = 0;
		} else if (!f->gapEnded) {
			// The start mark ends the gap; data follows it
			if (value) {
				f->gapEnded = true;
				f->crc = crcByte(0, value);
			}
		} else {
			if (!(f->control & 0x10)) {
				f->crc = crcByte(f->crc, value);
			}
			f->readData = value;
			f->transferred = true;
			if (irq) {
				f->diskIrq = true;
				updateIrq(nes);
			}
		}
	} else {
		value = 0;
		if (!started) {
			f->crc = 0;
		} else {
			if (f->control & 0x10) {
				value = f->crc & 0xFF;
				f->crc >>= 8;
			} else {
				value = f->writeData;
				f->crc = crcByte(f->crc, value);
				f->transferred = true;
				if (irq) {
					f->diskIrq = true;
					updateIrq(nes);
				}
			}
		}
		disk[f->position] = value;
		f->gapEnded = false;
	}
	if (++f->position >= nes->cart.diskSideSize) {
		f->endOfHead = true;
		f->scanning = false;
		f->control &= ~0x01;
	} else {
		f->delay = BYTE_CYCLES;
	}
}

// Sound

static void envelopeTick(uint8_t* gain, bool increase) {
	if (increase && *gain < 32) {
		(*gain)++;
	} else if (!increase && *gain > 0) {
		(*gain)--;
	}
}

static int modulatedPitch(const struct FdsSound* s) {
	int counter = s->modCounter;
	int temp = counter * s->modGain;
	int remainder = temp & 0x0F;
	temp >>= 4;
	if (remainder && !(temp & 0x80)) {
		temp += counter < 0 ? -1 : 2;
	}
	if (temp >= 192) {
		temp -= 256;
	} else if (temp < -64) {
		temp += 256;
	}
	temp *= s->pitch;
	remainder = temp & 0x3F;
	temp >>= 6;
	if (remainder >= 32) {
		temp++;
	}
	int pitch = s->pitch + temp;
	return pitch < 0 ? 0 : pitch;
}

void fdsSoundCycle(struct FdsSound* s) {
	if (!s->envelopeHalt && !s->waveHalt && s->masterSpeed) {
		if (!s->volumeDirect && --s->volumeTimer <= 0) {
			s->volumeTimer = 8 * (s->volumeSpeed + 1) * s->masterSpeed;
			envelopeTick(&s->volume, s->volumeIncrease);
		}
		if (!s->modDirect && --s->modTimer <= 0) {
			s->modTimer = 8 * (s->modSpeed + 1) * s->masterSpeed;
			envelopeTick(&s->modGain, s->modIncrease);
		}
	}
	if (!s->modHalt && s->modPitch) {
		s->modPhase += s->modPitch;
		if (s->modPhase >= 0x10000) {
			s->modPhase -= 0x10000;
			static const int8_t steps[8] = { 0, 1, 2, 4, 0, -4, -2, -1 };
			uint8_t step = s->modTable[s->modPosition];
			s->modPosition = (s->modPosition + 1) & 63;
			s->modCounter = step == 4 ? 0 : (int8_t) ((uint8_t) ((s->modCounter + steps[step]) << 1)) >> 1;
		}
	}
	if (!s->waveHalt) {
		int pitch = s->modHalt ? s->pitch : modulatedPitch(s);
		s->wavePhase = (s->wavePhase + pitch) & 0x3FFFFF;
	}
	if (!s->waveWrite) {
		s->waveOutput = s->wave[s->wavePhase >> 16];
	}
	// About 2.4 times a square at full volume, through the adapter's low-pass at about 2 kHz
	static const int32_t master[4] = { 1815, 1210, 908, 726 };
	int32_t level = (s->waveOutput * (s->volume < 32 ? s->volume : 32) * master[s->masterVolume]) >> 8;
	s->filtered += ((level - s->filtered) * 460) >> 16;
}

void fdsSoundReset(struct FdsSound* s) {
	memset(s, 0, sizeof(*s));
	s->masterSpeed = 0xE8;
	s->waveHalt = s->modHalt = true;
}

// $4040-$4092
uint8_t fdsSoundRead(const struct FdsSound* s, uint16_t address, uint8_t open) {
	if (address >= 0x4040 && address < 0x4080) {
		return (open & 0xC0) | (s->waveWrite ? s->wave[address & 0x3F] : s->waveOutput);
	}
	if (address == 0x4090) {
		return (open & 0xC0) | s->volume;
	}
	if (address == 0x4092) {
		return (open & 0xC0) | s->modGain;
	}
	return open;
}

void fdsSoundWrite(struct FdsSound* s, uint16_t address, uint8_t value) {
	if (address < 0x4080) {
		if (s->waveWrite) {
			s->wave[address & 0x3F] = value & 0x3F;
		}
		return;
	}
	switch (address) {
	case 0x4080:
		s->volumeDirect = value & 0x80;
		s->volumeIncrease = value & 0x40;
		s->volumeSpeed = value & 0x3F;
		if (s->volumeDirect) {
			s->volume = value & 0x3F;
		}
		s->volumeTimer = 8 * (s->volumeSpeed + 1) * s->masterSpeed;
		break;
	case 0x4082:
		s->pitch = (s->pitch & 0xF00) | value;
		break;
	case 0x4083:
		s->pitch = (s->pitch & 0xFF) | (value & 0x0F) << 8;
		s->waveHalt = value & 0x80;
		s->envelopeHalt = value & 0x40;
		if (s->waveHalt) {
			s->wavePhase = 0;
		}
		break;
	case 0x4084:
		s->modDirect = value & 0x80;
		s->modIncrease = value & 0x40;
		s->modSpeed = value & 0x3F;
		if (s->modDirect) {
			s->modGain = value & 0x3F;
		}
		s->modTimer = 8 * (s->modSpeed + 1) * s->masterSpeed;
		break;
	case 0x4085:
		s->modCounter = (int8_t) (value << 1) >> 1;
		break;
	case 0x4086:
		s->modPitch = (s->modPitch & 0xF00) | value;
		break;
	case 0x4087:
		s->modPitch = (s->modPitch & 0xFF) | (value & 0x0F) << 8;
		s->modHalt = value & 0x80;
		if (s->modHalt) {
			s->modPhase = 0;
		}
		break;
	case 0x4088:
		if (s->modHalt) {
			s->modTable[s->modPosition] = s->modTable[(s->modPosition + 1) & 63] = value & 7;
			s->modPosition = (s->modPosition + 2) & 63;
		}
		break;
	case 0x4089:
		s->waveWrite = value & 0x80;
		s->masterVolume = value & 3;
		break;
	case 0x408A:
		s->masterSpeed = value;
		break;
	}
}

static bool headerMatches(struct NES* nes, int side, const uint8_t* want) {
	const uint8_t* header = nes->cart.disk + side * nes->cart.diskSideSize + LEAD_GAP + 1 + 15;
	for (int i = 0; i < 10; i++) {
		if (want[i] != 0xFF && want[i] != header[i]) {
			return false;
		}
	}
	return true;
}

// Picks the side the BIOS is about to check for: CheckDiskHeader ($E445) compares the disk's header
// with 10 bytes at ($00), $FF matching anything
static void autoInsert(struct NES* nes) {
	struct Fds* f = FD;
	const uint8_t* want = NULL;
	uint16_t pointer = nes->ram[0] | nes->ram[1] << 8;
	uint8_t id[10];
	for (int i = 0; i < 10; i++) {
		uint16_t a = pointer + i;
		uint8_t* page = a >= 0x6000 ? nes->prgPages[a >> 12] : NULL;
		id[i] = a < 0x2000 ? nes->ram[a & 0x7FF] : page ? page[a & 0xFFF] : 0xFF;
	}
	want = id;
	if (f->side >= 0 && headerMatches(nes, f->side, want)) {
		return;
	}
	for (int side = 0; side < nes->cart.diskSides; side++) {
		if (headerMatches(nes, side, want)) {
			f->side = side;
			f->endOfHead = true;
			f->scanning = false;
			return;
		}
	}
}

static uint8_t fdsRead(struct NES* nes, uint16_t address) {
	struct Fds* f = FD;
	uint8_t open = nes->cpu.openBus;
	if (address >= 0xE000) {
		if (address == 0xE445 && nes->cart.diskSides > 1) {
			autoInsert(nes);
		}
		return nes->cart.prg[address - 0xE000];
	}
	if (f->diskEnabled) {
		switch (address) {
		case 0x4030: {
			uint8_t value = (open & 0x24) | f->timerIrq | f->transferred << 1 | (f->control & 0x08) |
				f->endOfHead << 6 | 0x80;
			f->timerIrq = f->diskIrq = f->transferred = false;
			updateIrq(nes);
			return value;
		}
		case 0x4031:
			f->transferred = false;
			f->diskIrq = false;
			updateIrq(nes);
			return f->readData;
		case 0x4032: {
			bool inserted = f->side >= 0;
			bool ready = inserted && f->scanning;
			return (open & 0xF8) | !inserted | !ready << 1 | !inserted << 2;
		}
		case 0x4033:
			return (f->external & 0x7F) | 0x80;
		}
	}
	if (f->soundEnabled) {
		return fdsSoundRead(&f->sound, address, open);
	}
	return open;
}

static void fdsWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Fds* f = FD;
	if (address == 0x4023) {
		f->diskEnabled = value & 1;
		f->soundEnabled = value & 2;
		if (!f->diskEnabled) {
			f->timerEnabled = false;
			f->timerIrq = f->diskIrq = false;
			updateIrq(nes);
		}
		return;
	}
	if (address >= 0x4040 && address <= 0x408A) {
		if (f->soundEnabled) {
			fdsSoundWrite(&f->sound, address, value);
		}
		return;
	}
	if (!f->diskEnabled) {
		return;
	}
	switch (address) {
	case 0x4020:
		f->timerReload = (f->timerReload & 0xFF00) | value;
		break;
	case 0x4021:
		f->timerReload = (f->timerReload & 0x00FF) | value << 8;
		break;
	case 0x4022:
		f->timerRepeat = value & 1;
		f->timerEnabled = value & 2;
		if (f->timerEnabled) {
			f->timerCounter = f->timerReload;
		}
		f->timerIrq = false;
		updateIrq(nes);
		break;
	case 0x4024:
		f->writeData = value;
		f->transferred = false;
		f->diskIrq = false;
		updateIrq(nes);
		break;
	case 0x4025:
		f->control = value;
		f->diskIrq = false;
		updateIrq(nes);
		nesSetMirroring(nes, (value & 0x08) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
		break;
	case 0x4026:
		f->external = value;
		break;
	}
}

static void fdsCycle(struct NES* nes) {
	struct Fds* f = FD;
	if (f->timerEnabled && f->diskEnabled) {
		if (f->timerCounter == 0) {
			f->timerIrq = true;
			updateIrq(nes);
			f->timerCounter = f->timerReload;
			if (!f->timerRepeat) {
				f->timerEnabled = false;
			}
		} else {
			f->timerCounter--;
		}
	}
	driveCycle(nes);
	fdsSoundCycle(&f->sound);
}

static int fdsAudio(struct NES* nes) {
	return FD->sound.filtered;
}

static void fdsSync(struct NES* nes) {
	nesMapWram(nes, 0x6000, 0x8000, 0, true);
	nesMapPrg(nes, 0xE000, 0x2000, 0);
	nesMapChr(nes, 0, 0x2000, 0);
	nesSetMirroring(nes, (FD->control & 0x08) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	nes->readHooks = 1 << 4 | 1 << 0xE;
}

static void fdsReset(struct NES* nes, bool hard) {
	struct Fds* f = FD;
	if (hard) {
		memset(f, 0, sizeof(*f));
		f->side = nes->cart.diskSides ? 0 : -1;
		f->endOfHead = true;
		fdsSoundReset(&f->sound);
	}
	fdsSync(nes);
}

static size_t fdsSaveExtra(struct NES* nes, uint8_t* out) {
	size_t size = (size_t) nes->cart.diskSides * nes->cart.diskSideSize;
	if (out) {
		memcpy(out, nes->cart.disk, size);
	}
	return size;
}

static void fdsLoadExtra(struct NES* nes, const uint8_t* in) {
	memcpy(nes->cart.disk, in, (size_t) nes->cart.diskSides * nes->cart.diskSideSize);
	fdsSync(nes);
}

const struct Board boardFds = {
	.name = "FDS", .reset = fdsReset, .write = fdsWrite, .read = fdsRead, .cpuCycle = fdsCycle, .audio = fdsAudio,
	.sync = fdsSync, .stateSize = sizeof(struct Fds), .saveExtra = fdsSaveExtra, .loadExtra = fdsLoadExtra,
};

int fdsSide(struct NES* nes) {
	return nes->board == &boardFds ? FD->side : -1;
}

// The drive's motor runs with a disk in it: the game is loading or saving
bool fdsBusy(struct NES* nes) {
	return nes->board == &boardFds && FD->side >= 0 && FD->scanning;
}

void fdsInsert(struct NES* nes, int side) {
	if (nes->board == &boardFds && side < nes->cart.diskSides) {
		FD->side = side;
		FD->endOfHead = true;
		FD->scanning = false;
	}
}

// Disk images: fwNES's (a 16-byte header, then sides of 65500 bytes) or the bare sides, each block
// as its type byte and data with no gaps or CRCs

static size_t blockLength(const uint8_t* side, size_t at, uint16_t* fileSize) {
	switch (side[at]) {
	case 1:
		return 56;
	case 2:
		return 2;
	case 3:
		*fileSize = side[at + 13] | side[at + 14] << 8;
		return 16;
	case 4:
		return 1 + *fileSize;
	default:
		return 0;
	}
}

// One side as the drive reads it; returns the bytes used
static size_t buildSide(const uint8_t* side, uint8_t* out, size_t outSize) {
	size_t at = 0, o = LEAD_GAP;
	uint16_t fileSize = 0;
	int expect = 1;
	while (at < FDS_SIDE_BYTES) {
		uint8_t type = side[at];
		// Blocks run 1, 2, then pairs of 3 and 4
		if (type != expect && !(expect == 3 && type == 3)) {
			break;
		}
		size_t length = blockLength(side, at, &fileSize);
		if (!length || at + length > FDS_SIDE_BYTES || o + 1 + length + 2 + BLOCK_GAP > outSize) {
			break;
		}
		out[o] = 0x80;
		uint16_t crc = crcByte(0, 0x80);
		for (size_t i = 0; i < length; i++) {
			out[o + 1 + i] = side[at + i];
			crc = crcByte(crc, side[at + i]);
		}
		o += 1 + length;
		out[o++] = crc & 0xFF;
		out[o++] = crc >> 8;
		o += BLOCK_GAP;
		at += length;
		expect = type == 1 ? 2 : type == 2 ? 3 : type == 3 ? 4 : 3;
	}
	return o;
}

bool fdsCartLoad(struct NES* nes, const uint8_t* data, size_t size) {
	struct Cart* cart = &nes->cart;
	if (size >= 16 && !memcmp(data, "FDS\x1A", 4)) {
		data += 16;
		size -= 16;
	}
	int sides = (int) (size / FDS_SIDE_BYTES);
	if (!sides || data[0] != 1 || memcmp(data + 1, "*NINTENDO-HVC*", 14) || !nes->fdsBios) {
		return false;
	}
	memset(cart->header, 0, sizeof(cart->header));
	cart->hasTrainer = false;
	cart->battery = false;
	cart->batterySize = 0;
	cart->vram = cart->misc = NULL;
	cart->vramSize = cart->miscSize = 0;
	cart->mapper = 20;
	cart->submapper = 0;
	cart->jumper = -1;
	cart->nes2 = false;
	cart->region = REGION_NTSC;
	cart->mirroring = MIRROR_HORIZONTAL;
	cart->prgSize = 0x2000;
	cart->prg = malloc(cart->prgSize);
	memcpy(cart->prg, nes->fdsBios, cart->prgSize);
	cart->chrSize = 0x2000;
	cart->chrRam = true;
	cart->chr = calloc(1, cart->chrSize);
	cart->wramSize = 0x8000;
	cart->wram = calloc(1, cart->wramSize);
	cart->diskSides = sides;
	cart->diskSideSize = 0x14000;
	uint8_t* scratch = calloc(1, 0x40000);
	for (int s = 0; s < sides; s++) {
		size_t used = buildSide(data + s * FDS_SIDE_BYTES, scratch, 0x40000);
		uint32_t need = (uint32_t) ((used + 0x2000 + 0xFFF) & ~0xFFF);
		if (need > cart->diskSideSize) {
			cart->diskSideSize = need;
		}
	}
	cart->disk = calloc(sides, cart->diskSideSize);
	for (int s = 0; s < sides; s++) {
		buildSide(data + s * FDS_SIDE_BYTES, cart->disk + s * cart->diskSideSize, cart->diskSideSize);
	}
	free(scratch);
	cart->prgCrc = crc32(0, data, (uInt) (sides * FDS_SIDE_BYTES));
	return true;
}
