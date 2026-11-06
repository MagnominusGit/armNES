/* Copyright (c) 2026 Magnominus
 * Register behaviour after NintendulatorNRS's mappers 127, 515 and 547 (Copyright (C) 2002-2011
 * QMT Productions, and NewRisingSun), under the GNU General Public License, version 2 or later.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

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
