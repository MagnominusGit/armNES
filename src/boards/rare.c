/* Copyright (c) 2026 Magnominus
 * Register behaviour after NintendulatorNRS's mappers 127, 169, 515 and 547 (Copyright (C) 2002-2011
 * QMT Productions, and NewRisingSun), under the GNU General Public License, version 2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"
#include "mmc3.h"

#include "libretro.h"
#include "sound/emu2413.h"

// Boards with a game or two each.

// 127: a pirate board. Four 8 KB PRG banks (the last from the top 32 KB), eight
// 1 KB CHR banks, each name table from either CIRAM page, and an IRQ 256 CPU cycles after it's
// enabled.
struct M127 {
	uint8_t prg[4], chr[8], nametable[4];
	bool irqEnabled;
	uint8_t irqCounter;
};

#define M127S STATE(M127)

static void m127Sync(struct NES* nes) {
	struct M127* m = M127S;
	for (int i = 0; i < 4; i++) {
		prg8(nes, 0x8000 + i * 0x2000, m->prg[i]);
	}
	for (int i = 0; i < 8; i++) {
		chr1(nes, i * 0x400, m->chr[i]);
	}
	mirrorPages(nes, m->nametable[0], m->nametable[1], m->nametable[2], m->nametable[3]);
}

static void m127Write(struct NES* nes, uint16_t address, uint8_t value) {
	struct M127* m = M127S;
	if (address < 0x8000) {
		return;
	}
	int reg = address & 0x73;
	switch (reg >> 4) {
	case 0:
		m->prg[reg & 3] = (reg & 3) == 3 ? (value & 3) | 0x0C : value & 0x0F;
		break;
	case 1:
	case 2:
		m->chr[(reg >> 4) * 4 - 4 + (reg & 3)] = value & 0x7F;
		break;
	case 3:
		m->irqEnabled = true;
		break;
	case 4:
		m->irqEnabled = false;
		m->irqCounter = 0;
		nesSetIrq(nes, IRQ_MAPPER, false);
		break;
	case 5:
		m->nametable[reg & 3] = value & 1;
		break;
	}
	m127Sync(nes);
}

static void m127Cycle(struct NES* nes) {
	struct M127* m = M127S;
	if (m->irqEnabled && !--m->irqCounter) {
		nesSetIrq(nes, IRQ_MAPPER, true);
	}
}

static void m127Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct M127));
		memset(M127S->prg, 0x0F, sizeof(M127S->prg));
	}
	m127Sync(nes);
}

const struct Board board127 = {
	.name = "Mapper 127", .reset = m127Reset, .write = m127Write, .cpuCycle = m127Cycle, .sync = m127Sync,
	.stateSize = sizeof(struct M127),
};

// 122: J.Y.'s JY043, 32 KB of PRG and two 4 KB CHR banks picked by even and odd addresses. Mapper
// 171 carts with CHR ROM are this board too.
struct Jy043 {
	uint8_t chr[2];
};

static void jy043Sync(struct NES* nes) {
	struct Jy043* j = STATE(Jy043);
	prg32(nes, 0);
	chr4(nes, 0x0000, j->chr[0]);
	chr4(nes, 0x1000, j->chr[1]);
}

static void jy043Write(struct NES* nes, uint16_t address, uint8_t value) {
	if (address >= 0x8000) {
		STATE(Jy043)->chr[address & 1] = value & 7;
		jy043Sync(nes);
	}
}

static void jy043Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Jy043));
	}
	jy043Sync(nes);
}

const struct Board boardJy043 = {
	.name = "JY043", .reset = jy043Reset, .write = jy043Write, .sync = jy043Sync, .stateSize = sizeof(struct Jy043),
};

// 515: a Korean karaoke board. A 16 KB bank at $8000 from the main ROM (the last 1 MB) or
// the expansion cart's (the rest), the last bank fixed at $C000, and a YM2413 clone at $6000-$6001.
// The microphone reads as silence.
struct Karaoke {
	uint8_t bank, audioAddress;
	uint8_t audioRegs[0x40];
	uint8_t audioClock;
	int16_t sample;
};

static OPLL* ym2413;

static void karaokeBanks(struct NES* nes) {
	struct Karaoke* n = STATE(Karaoke);
	int banks = (int) (nes->cart.prgSize / 0x4000);
	int internal = banks > 64 ? banks - 64 : 0;
	prg16(nes, 0x8000, (n->bank & 0x3F) + ((n->bank & 0x80) ? internal : 0));
	prg16(nes, 0xC000, banks - 1);
}

static void karaokeSync(struct NES* nes) {
	struct Karaoke* n = STATE(Karaoke);
	karaokeBanks(nes);
	for (int i = 0; i < 0x40; i++) {
		OPLL_writeReg(ym2413, i, n->audioRegs[i]);
	}
}

static void karaokeWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Karaoke* n = STATE(Karaoke);
	if ((address & 0xE003) == 0x6000) {
		n->audioAddress = value;
	} else if ((address & 0xE003) == 0x6001) {
		n->audioRegs[n->audioAddress & 0x3F] = value;
		OPLL_writeReg(ym2413, n->audioAddress, value);
	} else if ((address & 0xE00F) == 0x8000) {
		n->bank = value;
		karaokeBanks(nes);
	}
}

static uint8_t karaokeRead(struct NES* nes, uint16_t address) {
	return (address & 0xE003) == 0x6003 ? 0 : nes->cpu.openBus;
}

static void karaokeCycle(struct NES* nes) {
	struct Karaoke* n = STATE(Karaoke);
	if (++n->audioClock == 36) {
		n->audioClock = 0;
		n->sample = OPLL_calc(ym2413);
	}
}

static int karaokeAudio(struct NES* nes) {
	return STATE(Karaoke)->sample * 2;
}

static void karaokeReset(struct NES* nes, bool hard) {
	if (!ym2413) {
		ym2413 = OPLL_new(3579545, 49716);
		OPLL_setChipType(ym2413, 0);
	}
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Karaoke));
		OPLL_reset(ym2413);
	}
	nesUnmapPrg(nes, 0x6000, 0x2000);
	nes->readHooks |= 1 << 6;
	nesMapChr(nes, 0, 0x2000, 0);
	karaokeBanks(nes);
}

const struct Board boardKaraoke = {
	.name = "Karaoke (515)", .reset = karaokeReset, .write = karaokeWrite, .read = karaokeRead,
	.cpuCycle = karaokeCycle, .audio = karaokeAudio, .sync = karaokeSync, .stateSize = sizeof(struct Karaoke),
};

// 547: Konami's QTa adapter (VRC5), with its own 128 KB of PRG ROM before the cart's 512 KB, 8 KB of
// RAM beside the cart's battery-backed 8 KB, a 128 KB kanji ROM as CHR, and 8 KB of CHR RAM. A second
// set of name tables (QTRAM, kept in the cart's extra name table space) gives each background tile a
// bank byte: CHR RAM's left or right half, or a 4 KB bank of kanji, whose second bitplane is all 0s
// or all 1s. Registers are at $D000-$DD00; $DC00 and $DD00 read back the tile and bank byte for the
// JIS X 0208 character written to them.
enum { QT_WRAM6, QT_WRAM7, QT_PRG8, QT_PRGA, QT_PRGC, QT_CHR, QT_LATCHLO, QT_LATCHHI, QT_ACK, QT_IRQ, QT_NT,
	QT_JISFLAG, QT_JISCOL, QT_JISROW };

struct Qta {
	uint8_t reg[16];
	uint16_t irqCounter;
	uint8_t qtByte;
};

#define QT STATE(Qta)

static int qtaPrgBank(uint8_t value) {
	return (value & 0x40) ? (value & 0x3F) + 0x10 : value & 0x0F;
}

static void qtaSync(struct NES* nes) {
	struct Qta* q = QT;
	nesMapWram(nes, 0x6000, 0x1000, (q->reg[QT_WRAM6] & 1) | (q->reg[QT_WRAM6] >> 2), true);
	nesMapWram(nes, 0x7000, 0x1000, (q->reg[QT_WRAM7] & 1) | (q->reg[QT_WRAM7] >> 2), true);
	prg8(nes, 0x8000, qtaPrgBank(q->reg[QT_PRG8]));
	prg8(nes, 0xA000, qtaPrgBank(q->reg[QT_PRGA]));
	prg8(nes, 0xC000, qtaPrgBank(q->reg[QT_PRGC]));
	prg8(nes, 0xE000, 0x4F);
	nesMapVram(nes, 0x0000, 0x1000, q->reg[QT_CHR] & 1);
	nesMapVram(nes, 0x1000, 0x1000, 1);
	nesSetMirroring(nes, (q->reg[QT_NT] & 2) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	// With QTRAM selected, writes through $2007 go there instead of CIRAM
	if (q->reg[QT_NT] & 1) {
		bool horizontal = q->reg[QT_NT] & 2;
		for (int i = 0; i < 4; i++) {
			int page = (horizontal ? i >> 1 : i) & 1;
			nes->chrPages[8 + i] = nes->chrPages[12 + i] = nes->cart.extraNt + page * 0x400;
		}
	}
}

static void qtaWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Qta* q = QT;
	if ((address & 0xF000) != 0xD000) {
		return;
	}
	int reg = (address >> 8) & 15;
	q->reg[reg] = value;
	if (reg == QT_ACK) {
		q->reg[QT_IRQ] = (q->reg[QT_IRQ] & 1) ? q->reg[QT_IRQ] | 2 : q->reg[QT_IRQ] & ~2;
		nesSetIrq(nes, IRQ_MAPPER, false);
	} else if (reg == QT_IRQ) {
		if (value & 2) {
			q->irqCounter = q->reg[QT_LATCHLO] | q->reg[QT_LATCHHI] << 8;
		}
		nesSetIrq(nes, IRQ_MAPPER, false);
	}
	qtaSync(nes);
}

static const uint8_t kanjiPages[0x24] = {
	0x0, 0x0, 0x2, 0x2, 0x1, 0x1, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
	0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0xD, 0xD,
};

static uint8_t qtaRead(struct NES* nes, uint16_t address) {
	struct Qta* q = QT;
	if (address != 0xDC00 && address != 0xDD00) {
		return nes->prgPages[0xD][address & 0xFFF];
	}
	uint8_t row = q->reg[QT_JISROW] - 0x20, col = q->reg[QT_JISCOL] - 0x20;
	if (row >= 0x60 || col >= 0x60) {
		return 0;
	}
	// The JIS grid in thirds of 32 columns and 16 rows, then the ROM's page for each 256 codes
	uint16_t code = (col % 32) + (row % 16) * 32 + (col / 32) * 512 + (row / 16) * 1536;
	uint32_t tile = ((code & 0xFF) | kanjiPages[code >> 8] << 8) * 4;
	if (address == 0xDC00) {
		return (tile & 0xFF) | (q->reg[QT_JISFLAG] & 3);
	}
	return (tile >> 8) | ((q->reg[QT_JISFLAG] & 4) ? 0x80 : 0) | 0x40;
}

static uint8_t qtaPpuRead(struct NES* nes, uint16_t address) {
	struct Qta* q = QT;
	const struct Ppu* ppu = &nes->ppu;
	bool background = (ppu->showBg || ppu->showSprites) && (ppu->scanline < 240 || ppu->scanline >= 261) &&
		!ppu->spriteFetch;
	if (address >= 0x2000) {
		// Name tables come from CIRAM, with QTRAM's byte for the same spot alongside
		int page = (q->reg[QT_NT] & 2) ? (address >> 11) & 1 : (address >> 10) & 1;
		int offset = page * 0x400 + (address & 0x3FF);
		if (background && (address & 0x3FF) < 0x3C0) {
			q->qtByte = nes->cart.extraNt[offset];
		}
		return nes->ppu.ntRam[offset];
	}
	if (!background) {
		uint8_t* page = nes->chrPages[address >> 10];
		return page ? page[address & 0x3FF] : 0;
	}
	if (q->qtByte & 0x40) {
		if (address & 8) {
			return (q->qtByte & 0x80) ? 0xFF : 0x00;
		}
		// The kanji ROM is one bitplane: a 16-bit word per row
		uint32_t a = (q->qtByte & 0x3F) << 12 | (address & 0xFFF);
		a = ((a & 7) << 1) | ((a & 0x10) >> 4) | ((a & 0x3FFE0) >> 1);
		return nes->cart.chr[a % nes->cart.chrSize];
	}
	return nes->cart.vram[((q->qtByte & 1) << 12) | (address & 0xFFF)];
}

static void qtaCycle(struct NES* nes) {
	struct Qta* q = QT;
	if ((q->reg[QT_IRQ] & 2) && !++q->irqCounter) {
		nesSetIrq(nes, IRQ_MAPPER, true);
		q->irqCounter = q->reg[QT_LATCHLO] | q->reg[QT_LATCHHI] << 8;
	}
}

static void qtaReset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Qta));
		memset(nes->cart.extraNt, 0, sizeof(nes->cart.extraNt));
	}
	nes->readHooks |= 1 << 0xD;
	nes->ppuReadHook = true;
	qtaSync(nes);
}

const struct Board boardQta = {
	.name = "Konami QTa", .reset = qtaReset, .write = qtaWrite, .read = qtaRead, .ppuRead = qtaPpuRead,
	.cpuCycle = qtaCycle, .sync = qtaSync, .stateSize = sizeof(struct Qta),
};

// 169: Yuxing's educational computers. 1 MB of RAM and 256 KB of CHR RAM; in its own mode it maps
// 32 KB of ROM or RAM (written through a latch when $4800 bit 5 is set), with two special CHR modes:
// the screen in four bands of 4 KB CHR banks, picked as the name table reaches each, and 1 bpp
// graphics, both bitplanes read from the one picked by the name table address. A MMC3 mode runs
// games from RAM. The keyboard is a matrix of 14 rows read at $4207; the floppy controller reads as
// ready with no disk.
struct Yuxing {
	uint8_t reg4800, reg5500, reg5501, ramLatch;
	uint16_t keyboardRow;
	bool pa00, pa09, pa13;
};

#define YX ((struct Yuxing*) STATE(Mmc3)->extra)

static bool yuxingMmc3(struct NES* nes) {
	return YX->reg5501 & 0x80;
}

static void yuxingWram(struct NES* nes) {
	uint8_t mode = YX->reg5500 & 3;
	nesMapWram(nes, 0x6000, 0x2000, mode ? mode : 0x3C, true);
}

static void yuxingPrg(struct NES* nes, uint16_t address, int bank) {
	nesMapWram(nes, address, 0x2000, bank & 0x3F, false);
}

static int yuxingChrBank(struct Yuxing* y) {
	return (y->reg5501 & 0x10) | ((y->reg5501 << 2) & 0x0C) | ((y->reg5501 >> 2) & 3);
}

static const struct Mmc3Variant yuxingVariant = { yuxingPrg, NULL, NULL, yuxingWram };

static void yuxingSync(struct NES* nes) {
	struct Yuxing* y = YX;
	nes->ppuReadHook = false;
	if (yuxingMmc3(nes)) {
		mmc3Sync(nes);
		return;
	}
	if (y->reg5500 & 4) {
		bool writable = !(y->reg4800 & 0x20);
		if (y->reg5500 & 0x40) {
			nesMapWram(nes, 0x8000, 0x8000, y->ramLatch >> 1, writable);
		} else {
			nesMapWram(nes, 0x8000, 0x4000, y->ramLatch, writable);
			nesMapWram(nes, 0xC000, 0x4000, 0xFF, writable);
		}
	} else {
		prg32(nes, y->reg4800 & 0x1F);
	}
	chr8(nes, yuxingChrBank(y));
	nes->ppuReadHook = y->reg4800 & 0x80;
	nesSetMirroring(nes, (y->reg5500 & 8) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
	yuxingWram(nes);
}

static const uint16_t yuxingKeys[14][8] = {
	{ RETROK_ESCAPE, RETROK_F9, RETROK_7, RETROK_r, RETROK_a, 0, 0, RETROK_LSHIFT },
	{ 0, RETROK_KP_ENTER, 0, RETROK_KP_MULTIPLY, RETROK_KP_DIVIDE, RETROK_UP, RETROK_BACKSPACE, RETROK_F12 },
	{ 0, 0, 0, RETROK_KP_PLUS, RETROK_NUMLOCK, RETROK_LEFT, RETROK_DOWN, RETROK_RIGHT },
	{ 0, RETROK_KP7, RETROK_KP8, RETROK_KP_MINUS, RETROK_KP9, RETROK_F11, RETROK_END, RETROK_PAGEDOWN },
	{ RETROK_F8, RETROK_6, RETROK_e, RETROK_RIGHTBRACKET, RETROK_l, RETROK_z, RETROK_n, RETROK_SPACE },
	{ RETROK_F7, RETROK_5, RETROK_w, RETROK_LEFTBRACKET, RETROK_k, RETROK_DELETE, RETROK_b, RETROK_SLASH },
	{ RETROK_F6, RETROK_4, RETROK_q, RETROK_p, RETROK_j, RETROK_BACKSLASH, RETROK_v, RETROK_PERIOD },
	{ RETROK_F5, RETROK_3, RETROK_EQUALS, RETROK_o, RETROK_h, RETROK_QUOTE, RETROK_c, RETROK_COMMA },
	{ RETROK_F4, RETROK_2, RETROK_MINUS, RETROK_i, RETROK_g, RETROK_SEMICOLON, RETROK_x, RETROK_m },
	{ RETROK_F3, RETROK_1, RETROK_0, RETROK_u, RETROK_f, RETROK_CAPSLOCK, RETROK_LSUPER, 0 },
	{ RETROK_F2, RETROK_BACKQUOTE, RETROK_9, RETROK_y, RETROK_d, RETROK_LCTRL, RETROK_LALT, RETROK_SCROLLOCK },
	{ RETROK_F1, RETROK_RETURN, RETROK_8, RETROK_t, RETROK_s, 0, RETROK_RSUPER, RETROK_MENU },
	{ RETROK_KP_PERIOD, RETROK_F10, RETROK_KP4, RETROK_KP6, RETROK_KP5, RETROK_INSERT, RETROK_HOME, RETROK_PAGEUP },
	{ 0, RETROK_KP0, RETROK_KP1, RETROK_KP3, RETROK_KP2, RETROK_SYSREQ, RETROK_TAB, RETROK_PAUSE },
};

static uint8_t yuxingRead(struct NES* nes, uint16_t address) {
	switch (address) {
	case 0x4207: {
		// The selected rows' keys shift through, the last eight read
		uint8_t result = 0;
		for (int row = 0; row < 14; row++) {
			if (YX->keyboardRow & (1 << row)) {
				for (int column = 0; column < 8; column++) {
					uint16_t key = yuxingKeys[row][column];
					bool down = key && (nes->keys[key >> 3] >> (key & 7)) & 1;
					result = (result >> 1) | (down ? 0x80 : 0);
				}
			}
		}
		return result;
	}
	case 0x4204:
	case 0x4304:
		return 0x80;
	case 0x4205:
	case 0x4305:
		return 0xFF;
	case 0x5002:
		// No VCD player
		return 0x02;
	}
	return nes->cpu.openBus;
}

static void yuxingWrite(struct NES* nes, uint16_t address, uint8_t value) {
	struct Yuxing* y = YX;
	if (address >= 0x8000) {
		if (yuxingMmc3(nes)) {
			mmc3Write(nes, address, value);
		} else if ((y->reg5500 & 4) && (y->reg4800 & 0x20)) {
			y->ramLatch = value;
			yuxingSync(nes);
		}
		return;
	}
	switch (address) {
	case 0x4202:
	case 0x4302:
	case 0x5004:
		y->keyboardRow = (y->keyboardRow & 0xFF00) | value;
		break;
	case 0x4203:
	case 0x4303:
	case 0x5005:
		y->keyboardRow = (y->keyboardRow & 0x00FF) | value << 8;
		break;
	case 0x4800:
		y->reg4800 = value;
		yuxingSync(nes);
		break;
	case 0x5500:
		y->reg5500 = value;
		yuxingSync(nes);
		break;
	case 0x5501:
		y->reg5501 = value;
		yuxingSync(nes);
		break;
	}
}

static uint8_t yuxingPpuRead(struct NES* nes, uint16_t address) {
	struct Yuxing* y = YX;
	if (y->reg5500 & 0x80) {
		// Four bands: each 256 bytes of name table brings its own 4 KB bank for $1000
		if (address >= 0x2000 && !(address & 0xFF)) {
			chr4(nes, 0x0000, ((y->reg5501 >> 1) & 6) | ((y->reg5501 << 3) & 0x18));
			chr4(nes, 0x1000, ((address >> 7) & 6) | ((y->reg5501 << 3) & 0x18) | 1);
		}
	} else if (address >= 0x2000) {
		// 1 bpp: the name table fetch picks the 4 KB half and the bitplane for the pattern fetches
		if (!y->pa13) {
			y->pa00 = address & 1;
			y->pa09 = address & 0x200;
		}
		y->pa13 = true;
	} else {
		y->pa13 = false;
		address = (((address >> 10) & 3) | (y->pa09 ? 4 : 0)) << 10 | (address & 0x3F7) | (y->pa00 ? 8 : 0);
	}
	uint8_t* page = nes->chrPages[(address >> 10) & 15];
	return page ? page[address & 0x3FF] : 0;
}

// At the end of the picture the CPU's CHR writes go to the 8 KB bank again
static void yuxingScanline(struct NES* nes, int line) {
	if (line == 240 && !yuxingMmc3(nes) && (YX->reg4800 & 0x80) && (YX->reg5500 & 0x80)) {
		chr8(nes, yuxingChrBank(YX));
	}
}

static void yuxingPpuBus(struct NES* nes, uint16_t address) {
	if (yuxingMmc3(nes)) {
		mmc3PpuBus(nes, address);
	}
}

static int yuxingIrqQuietDots(struct NES* nes) {
	return yuxingMmc3(nes) ? mmc3IrqQuietDots(nes) : 1 << 20;
}

static void yuxingReset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Mmc3));
	}
	mmc3Reset(nes, hard);
	nes->readHooks |= 1 << 4 | 1 << 5;
	yuxingSync(nes);
}

const struct Board boardYuxing = {
	.name = "Yuxing", .reset = yuxingReset, .write = yuxingWrite, .read = yuxingRead, .ppuRead = yuxingPpuRead,
	.ppuBus = yuxingPpuBus, .busA12 = true, .irqQuietDots = yuxingIrqQuietDots, .scanline = yuxingScanline, .sync = yuxingSync,
	.stateSize = sizeof(struct Mmc3), .data = &yuxingVariant,
};

// VS. System (99): $4016's bit 2 picks the 8 KB of CHR, and on boards with 40 KB of PRG also the
// first 8 KB at $8000
struct Vs99 {
	uint8_t out;
};

static void vs99Sync(struct NES* nes) {
	int bank = (STATE(Vs99)->out >> 2) & 1;
	if (nes->cart.prgSize > 0x8000) {
		prg8(nes, 0x8000, bank * 4);
		for (int i = 1; i < 4; i++) {
			prg8(nes, 0x8000 + i * 0x2000, i);
		}
	} else {
		prg32(nes, 0);
	}
	chr8(nes, bank);
	// A DualSystem's boards share RAM there instead (nes.c)
	if (!nes->vsShared) {
		wram8(nes);
	}
}

static void vs99Out(struct NES* nes, uint8_t value) {
	if ((STATE(Vs99)->out ^ value) & 4) {
		STATE(Vs99)->out = value;
		vs99Sync(nes);
	}
	STATE(Vs99)->out = value;
}

static void vs99Reset(struct NES* nes, bool hard) {
	if (hard) {
		memset(nes->boardState, 0, sizeof(struct Vs99));
	}
	vs99Sync(nes);
}

const struct Board boardVs99 = {
	.name = "VS. System", .reset = vs99Reset, .out = vs99Out, .sync = vs99Sync, .stateSize = sizeof(struct Vs99),
};

// Kaiser KS-7030 (347), behaviour per Mesen: an FDS conversion mapping 1 KB pieces. ROM: two
// registers set by the address written to $8000 and $9000 pick banks at $7000 and at $C000 plus
// $6C00; the last 32 KB fill the rest of $8000-$FFFF. RAM shows through at $6000-$6BFF,
// $B800-$BFFF and $CC00-$D7FF. Pages that mix them go through here.
struct Kaiser7030 {
	uint8_t regs[2];
};

static uint8_t* kaiser7030Byte(struct NES* nes, uint16_t address) {
	const uint8_t* r = STATE(Kaiser7030)->regs;
	uint8_t* rom = nes->cart.prg;
	uint8_t* ram = nes->cart.wram;
	if (address < 0x6C00) {
		return ram + (address - 0x6000);
	} else if (address < 0x7000) {
		return rom + (((r[1] & 0x0F) << 2) + 3) * 0x400 + (address - 0x6C00);
	} else if (address < 0x8000) {
		return rom + 0x10000 + (r[0] & 7) * 0x1000 + (address - 0x7000);
	} else if (address >= 0xB800 && address < 0xC000) {
		return ram + 0x0C00 + (address - 0xB800);
	} else if (address >= 0xC000 && address < 0xCC00) {
		return rom + (r[1] & 0x0F) * 0x1000 + (address - 0xC000);
	} else if (address >= 0xCC00 && address < 0xD800) {
		return ram + 0x1400 + (address - 0xCC00);
	}
	return rom + 0x18000 + (address - 0x8000);
}

static uint8_t kaiser7030Read(struct NES* nes, uint16_t address) {
	return *kaiser7030Byte(nes, address);
}

static void kaiser7030Write(struct NES* nes, uint16_t address, uint8_t value) {
	if (address >= 0x8000 && address < 0xA000) {
		STATE(Kaiser7030)->regs[(address >> 12) & 1] = address & 0x0F;
		nesSetMirroring(nes, (STATE(Kaiser7030)->regs[0] & 8) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
		return;
	}
	if (address >= 0x6000 && (address < 0x8000 || (address >= 0xB800 && address < 0xD800))) {
		*kaiser7030Byte(nes, address) = value;
	}
}

static void kaiser7030Sync(struct NES* nes) {
	nesMapPrg(nes, 0x8000, 0x8000, 3);
	nesUnmapPrg(nes, 0x6000, 0x2000);
	nesUnmapPrg(nes, 0xB000, 0x3000);
	nes->readHooks = 1 << 6 | 1 << 7 | 1 << 0xB | 1 << 0xC | 1 << 0xD;
	chr8(nes, 0);
	nesSetMirroring(nes, (STATE(Kaiser7030)->regs[0] & 8) ? MIRROR_HORIZONTAL : MIRROR_VERTICAL);
}

static void kaiser7030Reset(struct NES* nes, bool hard) {
	if (hard) {
		STATE(Kaiser7030)->regs[0] = STATE(Kaiser7030)->regs[1] = 0x0F;
	}
	kaiser7030Sync(nes);
}

const struct Board boardKaiser7030 = {
	.name = "Kaiser KS-7030", .reset = kaiser7030Reset, .write = kaiser7030Write, .read = kaiser7030Read,
	.sync = kaiser7030Sync, .stateSize = sizeof(struct Kaiser7030),
};
