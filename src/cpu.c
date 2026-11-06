/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "nes.h"

// The 2A03's 6502: every access is a CPU cycle, split around the moment the bus is sampled so the PPU
// is caught up to it. Interrupts are polled at the end of each cycle and taken after an instruction
// whose second-to-last cycle saw them.

enum { FLAG_C = 1, FLAG_Z = 2, FLAG_I = 4, FLAG_D = 8, FLAG_B = 16, FLAG_U = 32, FLAG_V = 64, FLAG_N = 128 };

// One master clock besides the CPU's own access, in TriCNES's order: the NMI line at clock 4, the
// IRQ line at 7 (M2 going low), the PPU's dot and half dot, then the APU in the CPU's slot.
static inline __attribute__((always_inline)) void tick(struct NES* nes) {
	struct Cpu* cpu = &nes->cpu;
	if (nes->cpuClock == 4) {
		// The PPU raises the NMI line; it only drops again on an opcode fetch
		bool out = ppuNmiOutput(nes);
		cpu->nmiLine |= out;
		if (cpu->opcodeCycle && !out) {
			cpu->nmiLine = false;
		}
	} else if (nes->cpuClock == 7) {
		apuLatchIrq(nes);
		cpu->irqSampled = cpu->irqLine != 0;
		if (nes->board->m2Rise) {
			nes->board->m2Rise(nes);
		}
	}
	if (nes->ppuClock == nes->ppuPeriod) {
		nes->ppuClock = 0;
		ppuStep(nes);
	}
	if (nes->ppuClock == 2) {
		ppuHalfStep(nes);
	}
	if (nes->cpuClock == 0) {
		apuCycle(nes);
	}
	nes->ppuClock++;
	nes->cpuClock++;
	nes->masterClock++;
}

void nesTick(struct NES* nes) {
	tick(nes);
}

static inline void startCycle(struct NES* nes) {
	while (nes->cpuClock != nes->cpuPeriod) {
		tick(nes);
	}
	nes->cpuClock = 0;
	nes->cpu.cycles++;
}

static inline void endCycle(struct NES* nes) {
	struct Cpu* cpu = &nes->cpu;
	// A 1-cycle DMA that this cycle didn't run never runs
	if (cpu->dmcDma && nes->apu.dmc.implicitAbort) {
		nes->apu.dmc.implicitAbort = false;
	}
	if (nes->board->cpuCycle) {
		nes->board->cpuCycle(nes);
	}
	do {
		tick(nes);
	} while (nes->cpuClock != nes->cpuPeriod);
	cpu->prevNeedNmi = cpu->needNmi;
	if (!cpu->nmiPrevLine && cpu->nmiLine) {
		cpu->needNmi = true;
	}
	cpu->nmiPrevLine = cpu->nmiLine;
	cpu->prevRunIrq = cpu->runIrq;
	cpu->runIrq = cpu->irqSampled && !(cpu->p & FLAG_I);
}

// The DMA unit (after TriCNES): while a sprite or sample transfer is pending the CPU halts on its
// next read. Get cycles read, put cycles write; each transfer starts with a halt cycle, and halted
// or alignment cycles repeat the CPU's read.
static inline bool dmaPending(struct NES* nes) {
	struct Cpu* cpu = &nes->cpu;
	return (cpu->dmcDma && (nes->apu.dmc.enabled || nes->apu.dmc.implicitAbort)) || cpu->oamDma;
}

static void oamGet(struct NES* nes) {
	struct Cpu* cpu = &nes->cpu;
	cpu->oamAligned = true;
	cpu->oamValue = busRead(nes, (cpu->oamPage << 8) | cpu->oamIndex);
}

static void oamPut(struct NES* nes) {
	struct Cpu* cpu = &nes->cpu;
	if (!cpu->oamAligned) {
		busRead(nes, cpu->addressBus);
		return;
	}
	busWrite(nes, 0x2004, cpu->oamValue);
	if (++cpu->oamIndex == 0) {
		cpu->oamDma = false;
		cpu->oamAligned = false;
	}
}

static void dmaCycle(struct NES* nes) {
	struct Cpu* cpu = &nes->cpu;
	bool put = nes->apu.putCycle;
	startCycle(nes);
	if (cpu->oamDma && cpu->oamFirst) {
		cpu->oamFirst = false;
		if (!put) {
			cpu->oamHalt = true;
		}
	}
	bool both = cpu->dmcDma && cpu->oamDma;
	if (put) {
		if (both) {
			if (cpu->dmcHalt && cpu->oamHalt) {
				busRead(nes, cpu->addressBus);
			} else if (cpu->oamHalt) {
				busRead(nes, cpu->addressBus);
			} else {
				oamPut(nes);
			}
		} else if (cpu->dmcDma || cpu->oamHalt) {
			busRead(nes, cpu->addressBus);
		} else {
			oamPut(nes);
		}
	} else {
		if (both) {
			if (cpu->dmcHalt && cpu->oamHalt) {
				busRead(nes, cpu->addressBus);
			} else if (cpu->dmcHalt) {
				oamGet(nes);
			} else {
				apuDmcDmaGet(nes);
			}
		} else if (cpu->dmcDma) {
			if (cpu->dmcHalt) {
				busRead(nes, cpu->addressBus);
			} else {
				apuDmcDmaGet(nes);
			}
		} else if (cpu->oamHalt) {
			busRead(nes, cpu->addressBus);
		} else {
			oamGet(nes);
		}
		cpu->dmcHalt = false;
		cpu->oamHalt = false;
	}
	endCycle(nes);
}

static inline uint8_t readCycle(struct NES* nes, uint16_t address) {
	nes->cpu.addressBus = address;
	while (__builtin_expect(dmaPending(nes), 0)) {
		dmaCycle(nes);
	}
	startCycle(nes);
	uint8_t value = busRead(nes, address);
	endCycle(nes);
	return value;
}

static inline void writeCycle(struct NES* nes, uint16_t address, uint8_t value) {
	nes->cpu.addressBus = address;
	startCycle(nes);
	busWrite(nes, address, value);
	endCycle(nes);
}

static inline uint8_t fetch(struct NES* nes) {
	return readCycle(nes, nes->cpu.pc++);
}

static inline uint16_t fetch16(struct NES* nes) {
	uint8_t lo = fetch(nes);
	return lo | (fetch(nes) << 8);
}

static inline void dummyFetch(struct NES* nes) {
	readCycle(nes, nes->cpu.pc);
}

static inline void push(struct NES* nes, uint8_t value) {
	writeCycle(nes, 0x100 | nes->cpu.s--, value);
}

static inline uint8_t pop(struct NES* nes) {
	return readCycle(nes, 0x100 | ++nes->cpu.s);
}

static inline void setZN(struct Cpu* cpu, uint8_t value) {
	cpu->p = (cpu->p & ~(FLAG_Z | FLAG_N)) | (value ? 0 : FLAG_Z) | (value & FLAG_N);
}

static inline void setFlag(struct Cpu* cpu, uint8_t flag, bool on) {
	cpu->p = on ? cpu->p | flag : cpu->p & ~flag;
}

// Addressing: each returns the effective address after the cycles the hardware spends on it.
// forWrite: stores and read-modify-writes always take the indexed dummy read.
static inline uint16_t addrZp(struct NES* nes) {
	return fetch(nes);
}

static inline uint16_t addrZpIndexed(struct NES* nes, uint8_t index) {
	uint8_t base = fetch(nes);
	readCycle(nes, base);
	return (uint8_t) (base + index);
}

static inline uint16_t addrAbs(struct NES* nes) {
	return fetch16(nes);
}

static inline uint16_t addrAbsIndexed(struct NES* nes, uint8_t index, bool forWrite) {
	uint16_t base = fetch16(nes);
	uint16_t address = base + index;
	if (forWrite || (address & 0xFF00) != (base & 0xFF00)) {
		readCycle(nes, (base & 0xFF00) | (address & 0xFF));
	}
	return address;
}

static inline uint16_t addrIndirectX(struct NES* nes) {
	uint8_t pointer = fetch(nes);
	readCycle(nes, pointer);
	pointer += nes->cpu.x;
	uint8_t lo = readCycle(nes, pointer);
	uint8_t hi = readCycle(nes, (uint8_t) (pointer + 1));
	return lo | (hi << 8);
}

static inline uint16_t addrIndirectY(struct NES* nes, bool forWrite) {
	uint8_t pointer = fetch(nes);
	uint8_t lo = readCycle(nes, pointer);
	uint8_t hi = readCycle(nes, (uint8_t) (pointer + 1));
	uint16_t base = lo | (hi << 8);
	uint16_t address = base + nes->cpu.y;
	if (forWrite || (address & 0xFF00) != (base & 0xFF00)) {
		readCycle(nes, (base & 0xFF00) | (address & 0xFF));
	}
	return address;
}

// Operations
static inline void adc(struct Cpu* cpu, uint8_t value) {
	unsigned sum = cpu->a + value + (cpu->p & FLAG_C);
	setFlag(cpu, FLAG_C, sum > 0xFF);
	setFlag(cpu, FLAG_V, ~(cpu->a ^ value) & (cpu->a ^ sum) & 0x80);
	cpu->a = sum;
	setZN(cpu, cpu->a);
}

static inline void sbc(struct Cpu* cpu, uint8_t value) {
	adc(cpu, value ^ 0xFF);
}

static inline void cmp(struct Cpu* cpu, uint8_t reg, uint8_t value) {
	setFlag(cpu, FLAG_C, reg >= value);
	setZN(cpu, reg - value);
}

static inline uint8_t asl(struct Cpu* cpu, uint8_t value) {
	setFlag(cpu, FLAG_C, value & 0x80);
	value <<= 1;
	setZN(cpu, value);
	return value;
}

static inline uint8_t lsr(struct Cpu* cpu, uint8_t value) {
	setFlag(cpu, FLAG_C, value & 1);
	value >>= 1;
	setZN(cpu, value);
	return value;
}

static inline uint8_t rol(struct Cpu* cpu, uint8_t value) {
	uint8_t carry = cpu->p & FLAG_C;
	setFlag(cpu, FLAG_C, value & 0x80);
	value = (value << 1) | carry;
	setZN(cpu, value);
	return value;
}

static inline uint8_t ror(struct Cpu* cpu, uint8_t value) {
	uint8_t carry = (cpu->p & FLAG_C) << 7;
	setFlag(cpu, FLAG_C, value & 1);
	value = (value >> 1) | carry;
	setZN(cpu, value);
	return value;
}

static inline void bit(struct Cpu* cpu, uint8_t value) {
	setFlag(cpu, FLAG_Z, !(cpu->a & value));
	cpu->p = (cpu->p & ~(FLAG_N | FLAG_V)) | (value & (FLAG_N | FLAG_V));
}

enum Op {
	OP_ORA, OP_AND, OP_EOR, OP_ADC, OP_SBC, OP_CMP, OP_CPX, OP_CPY, OP_LDA, OP_LDX, OP_LDY, OP_LAX, OP_BIT, OP_NOP,
	OP_ANC, OP_ALR, OP_ARR, OP_ANE, OP_AXS, OP_LXA, OP_LAS,
	OP_ASL, OP_LSR, OP_ROL, OP_ROR, OP_INC, OP_DEC, OP_SLO, OP_RLA, OP_SRE, OP_RRA, OP_DCP, OP_ISC,
	OP_STA, OP_STX, OP_STY, OP_SAX,
};

static inline void readOp(struct NES* nes, enum Op op, uint8_t value) {
	struct Cpu* cpu = &nes->cpu;
	switch (op) {
	case OP_ORA:
		cpu->a |= value;
		setZN(cpu, cpu->a);
		break;
	case OP_AND:
		cpu->a &= value;
		setZN(cpu, cpu->a);
		break;
	case OP_EOR:
		cpu->a ^= value;
		setZN(cpu, cpu->a);
		break;
	case OP_ADC:
		adc(cpu, value);
		break;
	case OP_SBC:
		sbc(cpu, value);
		break;
	case OP_CMP:
		cmp(cpu, cpu->a, value);
		break;
	case OP_CPX:
		cmp(cpu, cpu->x, value);
		break;
	case OP_CPY:
		cmp(cpu, cpu->y, value);
		break;
	case OP_LDA:
		cpu->a = value;
		setZN(cpu, value);
		break;
	case OP_LDX:
		cpu->x = value;
		setZN(cpu, value);
		break;
	case OP_LDY:
		cpu->y = value;
		setZN(cpu, value);
		break;
	case OP_LAX:
		cpu->a = cpu->x = value;
		setZN(cpu, value);
		break;
	case OP_BIT:
		bit(cpu, value);
		break;
	case OP_ANC:
		cpu->a &= value;
		setZN(cpu, cpu->a);
		setFlag(cpu, FLAG_C, cpu->a & 0x80);
		break;
	case OP_ALR:
		cpu->a = lsr(cpu, cpu->a & value);
		break;
	case OP_ARR:
		cpu->a = ((cpu->a & value) >> 1) | ((cpu->p & FLAG_C) << 7);
		setZN(cpu, cpu->a);
		setFlag(cpu, FLAG_C, cpu->a & 0x40);
		setFlag(cpu, FLAG_V, ((cpu->a >> 6) ^ (cpu->a >> 5)) & 1);
		break;
	case OP_ANE:
		cpu->a = (cpu->a | 0xEE) & cpu->x & value;
		setZN(cpu, cpu->a);
		break;
	case OP_LXA:
		cpu->a = cpu->x = (cpu->a | 0xFF) & value;
		setZN(cpu, cpu->a);
		break;
	case OP_AXS: {
		uint8_t ax = cpu->a & cpu->x;
		setFlag(cpu, FLAG_C, ax >= value);
		cpu->x = ax - value;
		setZN(cpu, cpu->x);
		break;
	}
	case OP_LAS:
		cpu->a = cpu->x = cpu->s = cpu->s & value;
		setZN(cpu, cpu->a);
		break;
	default:
		break;
	}
}

static inline uint8_t modifyOp(struct NES* nes, enum Op op, uint8_t value) {
	struct Cpu* cpu = &nes->cpu;
	switch (op) {
	case OP_ASL:
		return asl(cpu, value);
	case OP_LSR:
		return lsr(cpu, value);
	case OP_ROL:
		return rol(cpu, value);
	case OP_ROR:
		return ror(cpu, value);
	case OP_INC:
		value++;
		setZN(cpu, value);
		return value;
	case OP_DEC:
		value--;
		setZN(cpu, value);
		return value;
	case OP_SLO:
		value = asl(cpu, value);
		cpu->a |= value;
		setZN(cpu, cpu->a);
		return value;
	case OP_RLA:
		value = rol(cpu, value);
		cpu->a &= value;
		setZN(cpu, cpu->a);
		return value;
	case OP_SRE:
		value = lsr(cpu, value);
		cpu->a ^= value;
		setZN(cpu, cpu->a);
		return value;
	case OP_RRA:
		value = ror(cpu, value);
		adc(cpu, value);
		return value;
	case OP_DCP:
		value--;
		cmp(cpu, cpu->a, value);
		return value;
	case OP_ISC:
		value++;
		sbc(cpu, value);
		return value;
	default:
		return value;
	}
}

static inline uint8_t storeValue(struct Cpu* cpu, enum Op op) {
	switch (op) {
	case OP_STA:
		return cpu->a;
	case OP_STX:
		return cpu->x;
	case OP_STY:
		return cpu->y;
	default:
		return cpu->a & cpu->x;
	}
}

static inline void doRead(struct NES* nes, enum Op op, uint16_t address) {
	readOp(nes, op, readCycle(nes, address));
}

static inline void doModify(struct NES* nes, enum Op op, uint16_t address) {
	uint8_t value = readCycle(nes, address);
	writeCycle(nes, address, value);
	writeCycle(nes, address, modifyOp(nes, op, value));
}

static inline void doStore(struct NES* nes, enum Op op, uint16_t address) {
	writeCycle(nes, address, storeValue(&nes->cpu, op));
}

// SHA, SHX, SHY, TAS: the value stored is ANDed with the base's high byte plus one (unless a DMA
// takes the cycle before the write), and a page cross ANDs the register into the address's high byte.
static void storeHigh(struct NES* nes, uint16_t base, uint8_t index, uint8_t value) {
	uint16_t address = base + index;
	bool ignoreH = dmaPending(nes);
	readCycle(nes, (base & 0xFF00) | (address & 0xFF));
	if (!ignoreH) {
		value &= (base >> 8) + 1;
	}
	if ((address & 0xFF00) != (base & 0xFF00)) {
		address = (address & 0xFF) | ((address >> 8) & value) << 8;
	}
	writeCycle(nes, address, value);
}

// Branches poll interrupts before cycle 2 and, when they cross a page, again before cycle 4; a
// successful first poll can't be undone by the second.
static void branch(struct NES* nes, bool taken) {
	struct Cpu* cpu = &nes->cpu;
	bool firstPoll = cpu->runIrq;
	int8_t offset = fetch(nes);
	if (!taken) {
		return;
	}
	dummyFetch(nes);
	uint16_t target = cpu->pc + offset;
	if ((target & 0xFF00) != (cpu->pc & 0xFF00)) {
		readCycle(nes, (cpu->pc & 0xFF00) | (target & 0xFF));
		cpu->prevRunIrq |= firstPoll;
	} else {
		cpu->prevRunIrq = firstPoll;
	}
	cpu->pc = target;
}

static void interrupt(struct NES* nes, bool brk) {
	struct Cpu* cpu = &nes->cpu;
	push(nes, cpu->pc >> 8);
	push(nes, cpu->pc);
	uint16_t vector;
	// An NMI that arrives by now takes the vector over
	if (cpu->needNmi) {
		cpu->needNmi = false;
		vector = 0xFFFA;
	} else {
		vector = 0xFFFE;
	}
	push(nes, cpu->p | FLAG_U | (brk ? FLAG_B : 0));
	cpu->p |= FLAG_I;
	uint8_t lo = readCycle(nes, vector);
	cpu->pc = lo | (readCycle(nes, vector + 1) << 8);
	// The vector fetch hides an interrupt that comes in the meantime for one instruction
	cpu->prevNeedNmi = false;
}

void cpuReset(struct NES* nes, bool hard) {
	struct Cpu* cpu = &nes->cpu;
	if (hard) {
		cpu->a = cpu->x = cpu->y = 0;
		cpu->s = 0;
		cpu->p = FLAG_I | FLAG_U;
	} else {
		cpu->p |= FLAG_I;
	}
	cpu->nmiLine = cpu->nmiPrevLine = false;
	cpu->needNmi = cpu->prevNeedNmi = false;
	cpu->runIrq = cpu->prevRunIrq = false;
	cpu->irqLine = 0;
	cpu->jammed = false;
	cpu->dmcDma = cpu->dmcHalt = cpu->oamDma = cpu->oamHalt = cpu->oamFirst = cpu->oamAligned = false;
	// The reset sequence: three reads that would be pushes, then the vector
	for (int i = 0; i < 5; i++) {
		readCycle(nes, i < 2 ? cpu->pc : 0x100 | cpu->s);
		if (i >= 2) {
			cpu->s--;
		}
	}
	uint8_t lo = readCycle(nes, 0xFFFC);
	cpu->pc = lo | (readCycle(nes, 0xFFFD) << 8);
}

uint8_t cpuPeek(struct NES* nes, uint16_t address) {
	if (address < 0x2000) {
		return nes->ram[address & 0x7FF];
	}
	if (address >= 0x4020) {
		uint8_t* page = nes->prgPages[address >> 12];
		return page ? page[address & 0xFFF] : 0;
	}
	return 0;
}

#define READ(op, mode) doRead(nes, op, mode); break
#define MODIFY(op, mode) doModify(nes, op, mode); break
#define STORE(op, mode) doStore(nes, op, mode); break

void cpuStep(struct NES* nes) {
	struct Cpu* cpu = &nes->cpu;
	if (cpu->jammed) {
		readCycle(nes, 0xFFFF);
		return;
	}
	cpu->opcodeCycle = true;
	uint8_t opcode = fetch(nes);
	cpu->opcodeCycle = false;
	switch (opcode) {
	// Loads, arithmetic and compares
	case 0x09: READ(OP_ORA, cpu->pc++);
	case 0x05: READ(OP_ORA, addrZp(nes));
	case 0x15: READ(OP_ORA, addrZpIndexed(nes, cpu->x));
	case 0x0D: READ(OP_ORA, addrAbs(nes));
	case 0x1D: READ(OP_ORA, addrAbsIndexed(nes, cpu->x, false));
	case 0x19: READ(OP_ORA, addrAbsIndexed(nes, cpu->y, false));
	case 0x01: READ(OP_ORA, addrIndirectX(nes));
	case 0x11: READ(OP_ORA, addrIndirectY(nes, false));
	case 0x29: READ(OP_AND, cpu->pc++);
	case 0x25: READ(OP_AND, addrZp(nes));
	case 0x35: READ(OP_AND, addrZpIndexed(nes, cpu->x));
	case 0x2D: READ(OP_AND, addrAbs(nes));
	case 0x3D: READ(OP_AND, addrAbsIndexed(nes, cpu->x, false));
	case 0x39: READ(OP_AND, addrAbsIndexed(nes, cpu->y, false));
	case 0x21: READ(OP_AND, addrIndirectX(nes));
	case 0x31: READ(OP_AND, addrIndirectY(nes, false));
	case 0x49: READ(OP_EOR, cpu->pc++);
	case 0x45: READ(OP_EOR, addrZp(nes));
	case 0x55: READ(OP_EOR, addrZpIndexed(nes, cpu->x));
	case 0x4D: READ(OP_EOR, addrAbs(nes));
	case 0x5D: READ(OP_EOR, addrAbsIndexed(nes, cpu->x, false));
	case 0x59: READ(OP_EOR, addrAbsIndexed(nes, cpu->y, false));
	case 0x41: READ(OP_EOR, addrIndirectX(nes));
	case 0x51: READ(OP_EOR, addrIndirectY(nes, false));
	case 0x69: READ(OP_ADC, cpu->pc++);
	case 0x65: READ(OP_ADC, addrZp(nes));
	case 0x75: READ(OP_ADC, addrZpIndexed(nes, cpu->x));
	case 0x6D: READ(OP_ADC, addrAbs(nes));
	case 0x7D: READ(OP_ADC, addrAbsIndexed(nes, cpu->x, false));
	case 0x79: READ(OP_ADC, addrAbsIndexed(nes, cpu->y, false));
	case 0x61: READ(OP_ADC, addrIndirectX(nes));
	case 0x71: READ(OP_ADC, addrIndirectY(nes, false));
	case 0xE9: case 0xEB: READ(OP_SBC, cpu->pc++);
	case 0xE5: READ(OP_SBC, addrZp(nes));
	case 0xF5: READ(OP_SBC, addrZpIndexed(nes, cpu->x));
	case 0xED: READ(OP_SBC, addrAbs(nes));
	case 0xFD: READ(OP_SBC, addrAbsIndexed(nes, cpu->x, false));
	case 0xF9: READ(OP_SBC, addrAbsIndexed(nes, cpu->y, false));
	case 0xE1: READ(OP_SBC, addrIndirectX(nes));
	case 0xF1: READ(OP_SBC, addrIndirectY(nes, false));
	case 0xC9: READ(OP_CMP, cpu->pc++);
	case 0xC5: READ(OP_CMP, addrZp(nes));
	case 0xD5: READ(OP_CMP, addrZpIndexed(nes, cpu->x));
	case 0xCD: READ(OP_CMP, addrAbs(nes));
	case 0xDD: READ(OP_CMP, addrAbsIndexed(nes, cpu->x, false));
	case 0xD9: READ(OP_CMP, addrAbsIndexed(nes, cpu->y, false));
	case 0xC1: READ(OP_CMP, addrIndirectX(nes));
	case 0xD1: READ(OP_CMP, addrIndirectY(nes, false));
	case 0xE0: READ(OP_CPX, cpu->pc++);
	case 0xE4: READ(OP_CPX, addrZp(nes));
	case 0xEC: READ(OP_CPX, addrAbs(nes));
	case 0xC0: READ(OP_CPY, cpu->pc++);
	case 0xC4: READ(OP_CPY, addrZp(nes));
	case 0xCC: READ(OP_CPY, addrAbs(nes));
	case 0x24: READ(OP_BIT, addrZp(nes));
	case 0x2C: READ(OP_BIT, addrAbs(nes));
	case 0xA9: READ(OP_LDA, cpu->pc++);
	case 0xA5: READ(OP_LDA, addrZp(nes));
	case 0xB5: READ(OP_LDA, addrZpIndexed(nes, cpu->x));
	case 0xAD: READ(OP_LDA, addrAbs(nes));
	case 0xBD: READ(OP_LDA, addrAbsIndexed(nes, cpu->x, false));
	case 0xB9: READ(OP_LDA, addrAbsIndexed(nes, cpu->y, false));
	case 0xA1: READ(OP_LDA, addrIndirectX(nes));
	case 0xB1: READ(OP_LDA, addrIndirectY(nes, false));
	case 0xA2: READ(OP_LDX, cpu->pc++);
	case 0xA6: READ(OP_LDX, addrZp(nes));
	case 0xB6: READ(OP_LDX, addrZpIndexed(nes, cpu->y));
	case 0xAE: READ(OP_LDX, addrAbs(nes));
	case 0xBE: READ(OP_LDX, addrAbsIndexed(nes, cpu->y, false));
	case 0xA0: READ(OP_LDY, cpu->pc++);
	case 0xA4: READ(OP_LDY, addrZp(nes));
	case 0xB4: READ(OP_LDY, addrZpIndexed(nes, cpu->x));
	case 0xAC: READ(OP_LDY, addrAbs(nes));
	case 0xBC: READ(OP_LDY, addrAbsIndexed(nes, cpu->x, false));
	// Unofficial loads and immediates
	case 0xA7: READ(OP_LAX, addrZp(nes));
	case 0xB7: READ(OP_LAX, addrZpIndexed(nes, cpu->y));
	case 0xAF: READ(OP_LAX, addrAbs(nes));
	case 0xBF: READ(OP_LAX, addrAbsIndexed(nes, cpu->y, false));
	case 0xA3: READ(OP_LAX, addrIndirectX(nes));
	case 0xB3: READ(OP_LAX, addrIndirectY(nes, false));
	case 0xAB: READ(OP_LXA, cpu->pc++);
	case 0x0B: case 0x2B: READ(OP_ANC, cpu->pc++);
	case 0x4B: READ(OP_ALR, cpu->pc++);
	case 0x6B: READ(OP_ARR, cpu->pc++);
	case 0x8B: READ(OP_ANE, cpu->pc++);
	case 0xCB: READ(OP_AXS, cpu->pc++);
	case 0xBB: READ(OP_LAS, addrAbsIndexed(nes, cpu->y, false));
	// NOPs that read
	case 0x80: case 0x82: case 0x89: case 0xC2: case 0xE2: READ(OP_NOP, cpu->pc++);
	case 0x04: case 0x44: case 0x64: READ(OP_NOP, addrZp(nes));
	case 0x14: case 0x34: case 0x54: case 0x74: case 0xD4: case 0xF4: READ(OP_NOP, addrZpIndexed(nes, cpu->x));
	case 0x0C: READ(OP_NOP, addrAbs(nes));
	case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0xDC: case 0xFC:
		READ(OP_NOP, addrAbsIndexed(nes, cpu->x, false));
	case 0x1A: case 0x3A: case 0x5A: case 0x7A: case 0xDA: case 0xEA: case 0xFA:
		dummyFetch(nes);
		break;

	// Read-modify-writes
	case 0x0A: dummyFetch(nes); cpu->a = asl(cpu, cpu->a); break;
	case 0x4A: dummyFetch(nes); cpu->a = lsr(cpu, cpu->a); break;
	case 0x2A: dummyFetch(nes); cpu->a = rol(cpu, cpu->a); break;
	case 0x6A: dummyFetch(nes); cpu->a = ror(cpu, cpu->a); break;
	case 0x06: MODIFY(OP_ASL, addrZp(nes));
	case 0x16: MODIFY(OP_ASL, addrZpIndexed(nes, cpu->x));
	case 0x0E: MODIFY(OP_ASL, addrAbs(nes));
	case 0x1E: MODIFY(OP_ASL, addrAbsIndexed(nes, cpu->x, true));
	case 0x46: MODIFY(OP_LSR, addrZp(nes));
	case 0x56: MODIFY(OP_LSR, addrZpIndexed(nes, cpu->x));
	case 0x4E: MODIFY(OP_LSR, addrAbs(nes));
	case 0x5E: MODIFY(OP_LSR, addrAbsIndexed(nes, cpu->x, true));
	case 0x26: MODIFY(OP_ROL, addrZp(nes));
	case 0x36: MODIFY(OP_ROL, addrZpIndexed(nes, cpu->x));
	case 0x2E: MODIFY(OP_ROL, addrAbs(nes));
	case 0x3E: MODIFY(OP_ROL, addrAbsIndexed(nes, cpu->x, true));
	case 0x66: MODIFY(OP_ROR, addrZp(nes));
	case 0x76: MODIFY(OP_ROR, addrZpIndexed(nes, cpu->x));
	case 0x6E: MODIFY(OP_ROR, addrAbs(nes));
	case 0x7E: MODIFY(OP_ROR, addrAbsIndexed(nes, cpu->x, true));
	case 0xE6: MODIFY(OP_INC, addrZp(nes));
	case 0xF6: MODIFY(OP_INC, addrZpIndexed(nes, cpu->x));
	case 0xEE: MODIFY(OP_INC, addrAbs(nes));
	case 0xFE: MODIFY(OP_INC, addrAbsIndexed(nes, cpu->x, true));
	case 0xC6: MODIFY(OP_DEC, addrZp(nes));
	case 0xD6: MODIFY(OP_DEC, addrZpIndexed(nes, cpu->x));
	case 0xCE: MODIFY(OP_DEC, addrAbs(nes));
	case 0xDE: MODIFY(OP_DEC, addrAbsIndexed(nes, cpu->x, true));
	case 0x07: MODIFY(OP_SLO, addrZp(nes));
	case 0x17: MODIFY(OP_SLO, addrZpIndexed(nes, cpu->x));
	case 0x0F: MODIFY(OP_SLO, addrAbs(nes));
	case 0x1F: MODIFY(OP_SLO, addrAbsIndexed(nes, cpu->x, true));
	case 0x1B: MODIFY(OP_SLO, addrAbsIndexed(nes, cpu->y, true));
	case 0x03: MODIFY(OP_SLO, addrIndirectX(nes));
	case 0x13: MODIFY(OP_SLO, addrIndirectY(nes, true));
	case 0x27: MODIFY(OP_RLA, addrZp(nes));
	case 0x37: MODIFY(OP_RLA, addrZpIndexed(nes, cpu->x));
	case 0x2F: MODIFY(OP_RLA, addrAbs(nes));
	case 0x3F: MODIFY(OP_RLA, addrAbsIndexed(nes, cpu->x, true));
	case 0x3B: MODIFY(OP_RLA, addrAbsIndexed(nes, cpu->y, true));
	case 0x23: MODIFY(OP_RLA, addrIndirectX(nes));
	case 0x33: MODIFY(OP_RLA, addrIndirectY(nes, true));
	case 0x47: MODIFY(OP_SRE, addrZp(nes));
	case 0x57: MODIFY(OP_SRE, addrZpIndexed(nes, cpu->x));
	case 0x4F: MODIFY(OP_SRE, addrAbs(nes));
	case 0x5F: MODIFY(OP_SRE, addrAbsIndexed(nes, cpu->x, true));
	case 0x5B: MODIFY(OP_SRE, addrAbsIndexed(nes, cpu->y, true));
	case 0x43: MODIFY(OP_SRE, addrIndirectX(nes));
	case 0x53: MODIFY(OP_SRE, addrIndirectY(nes, true));
	case 0x67: MODIFY(OP_RRA, addrZp(nes));
	case 0x77: MODIFY(OP_RRA, addrZpIndexed(nes, cpu->x));
	case 0x6F: MODIFY(OP_RRA, addrAbs(nes));
	case 0x7F: MODIFY(OP_RRA, addrAbsIndexed(nes, cpu->x, true));
	case 0x7B: MODIFY(OP_RRA, addrAbsIndexed(nes, cpu->y, true));
	case 0x63: MODIFY(OP_RRA, addrIndirectX(nes));
	case 0x73: MODIFY(OP_RRA, addrIndirectY(nes, true));
	case 0xC7: MODIFY(OP_DCP, addrZp(nes));
	case 0xD7: MODIFY(OP_DCP, addrZpIndexed(nes, cpu->x));
	case 0xCF: MODIFY(OP_DCP, addrAbs(nes));
	case 0xDF: MODIFY(OP_DCP, addrAbsIndexed(nes, cpu->x, true));
	case 0xDB: MODIFY(OP_DCP, addrAbsIndexed(nes, cpu->y, true));
	case 0xC3: MODIFY(OP_DCP, addrIndirectX(nes));
	case 0xD3: MODIFY(OP_DCP, addrIndirectY(nes, true));
	case 0xE7: MODIFY(OP_ISC, addrZp(nes));
	case 0xF7: MODIFY(OP_ISC, addrZpIndexed(nes, cpu->x));
	case 0xEF: MODIFY(OP_ISC, addrAbs(nes));
	case 0xFF: MODIFY(OP_ISC, addrAbsIndexed(nes, cpu->x, true));
	case 0xFB: MODIFY(OP_ISC, addrAbsIndexed(nes, cpu->y, true));
	case 0xE3: MODIFY(OP_ISC, addrIndirectX(nes));
	case 0xF3: MODIFY(OP_ISC, addrIndirectY(nes, true));

	// Stores
	case 0x85: STORE(OP_STA, addrZp(nes));
	case 0x95: STORE(OP_STA, addrZpIndexed(nes, cpu->x));
	case 0x8D: STORE(OP_STA, addrAbs(nes));
	case 0x9D: STORE(OP_STA, addrAbsIndexed(nes, cpu->x, true));
	case 0x99: STORE(OP_STA, addrAbsIndexed(nes, cpu->y, true));
	case 0x81: STORE(OP_STA, addrIndirectX(nes));
	case 0x91: STORE(OP_STA, addrIndirectY(nes, true));
	case 0x86: STORE(OP_STX, addrZp(nes));
	case 0x96: STORE(OP_STX, addrZpIndexed(nes, cpu->y));
	case 0x8E: STORE(OP_STX, addrAbs(nes));
	case 0x84: STORE(OP_STY, addrZp(nes));
	case 0x94: STORE(OP_STY, addrZpIndexed(nes, cpu->x));
	case 0x8C: STORE(OP_STY, addrAbs(nes));
	case 0x87: STORE(OP_SAX, addrZp(nes));
	case 0x97: STORE(OP_SAX, addrZpIndexed(nes, cpu->y));
	case 0x8F: STORE(OP_SAX, addrAbs(nes));
	case 0x83: STORE(OP_SAX, addrIndirectX(nes));
	case 0x9F: storeHigh(nes, fetch16(nes), cpu->y, cpu->a & cpu->x); break;
	case 0x93: {
		uint8_t pointer = fetch(nes);
		uint8_t lo = readCycle(nes, pointer);
		uint8_t hi = readCycle(nes, (uint8_t) (pointer + 1));
		storeHigh(nes, lo | (hi << 8), cpu->y, cpu->a & cpu->x);
		break;
	}
	case 0x9E: storeHigh(nes, fetch16(nes), cpu->y, cpu->x); break;
	case 0x9C: storeHigh(nes, fetch16(nes), cpu->x, cpu->y); break;
	case 0x9B:
		cpu->s = cpu->a & cpu->x;
		storeHigh(nes, fetch16(nes), cpu->y, cpu->a & cpu->x);
		break;

	// Registers and flags
	case 0xAA: dummyFetch(nes); cpu->x = cpu->a; setZN(cpu, cpu->x); break;
	case 0x8A: dummyFetch(nes); cpu->a = cpu->x; setZN(cpu, cpu->a); break;
	case 0xA8: dummyFetch(nes); cpu->y = cpu->a; setZN(cpu, cpu->y); break;
	case 0x98: dummyFetch(nes); cpu->a = cpu->y; setZN(cpu, cpu->a); break;
	case 0xBA: dummyFetch(nes); cpu->x = cpu->s; setZN(cpu, cpu->x); break;
	case 0x9A: dummyFetch(nes); cpu->s = cpu->x; break;
	case 0xE8: dummyFetch(nes); cpu->x++; setZN(cpu, cpu->x); break;
	case 0xCA: dummyFetch(nes); cpu->x--; setZN(cpu, cpu->x); break;
	case 0xC8: dummyFetch(nes); cpu->y++; setZN(cpu, cpu->y); break;
	case 0x88: dummyFetch(nes); cpu->y--; setZN(cpu, cpu->y); break;
	case 0x18: dummyFetch(nes); cpu->p &= ~FLAG_C; break;
	case 0x38: dummyFetch(nes); cpu->p |= FLAG_C; break;
	case 0x58: dummyFetch(nes); cpu->p &= ~FLAG_I; break;
	case 0x78: dummyFetch(nes); cpu->p |= FLAG_I; break;
	case 0xB8: dummyFetch(nes); cpu->p &= ~FLAG_V; break;
	case 0xD8: dummyFetch(nes); cpu->p &= ~FLAG_D; break;
	case 0xF8: dummyFetch(nes); cpu->p |= FLAG_D; break;

	// Stack
	case 0x48: dummyFetch(nes); push(nes, cpu->a); break;
	case 0x08: dummyFetch(nes); push(nes, cpu->p | FLAG_B | FLAG_U); break;
	case 0x68:
		dummyFetch(nes);
		readCycle(nes, 0x100 | cpu->s);
		cpu->a = pop(nes);
		setZN(cpu, cpu->a);
		break;
	case 0x28:
		dummyFetch(nes);
		readCycle(nes, 0x100 | cpu->s);
		cpu->p = (pop(nes) & ~FLAG_B) | FLAG_U;
		break;

	// Jumps and returns
	case 0x4C: cpu->pc = fetch16(nes); break;
	case 0x6C: {
		uint16_t pointer = fetch16(nes);
		uint8_t lo = readCycle(nes, pointer);
		uint8_t hi = readCycle(nes, (pointer & 0xFF00) | ((pointer + 1) & 0xFF));
		cpu->pc = lo | (hi << 8);
		break;
	}
	case 0x20: {
		uint8_t lo = fetch(nes);
		readCycle(nes, 0x100 | cpu->s);
		push(nes, cpu->pc >> 8);
		push(nes, cpu->pc);
		cpu->pc = lo | (fetch(nes) << 8);
		break;
	}
	case 0x60: {
		dummyFetch(nes);
		readCycle(nes, 0x100 | cpu->s);
		uint8_t lo = pop(nes);
		cpu->pc = lo | (pop(nes) << 8);
		fetch(nes);
		break;
	}
	case 0x40: {
		dummyFetch(nes);
		readCycle(nes, 0x100 | cpu->s);
		cpu->p = (pop(nes) & ~FLAG_B) | FLAG_U;
		uint8_t lo = pop(nes);
		cpu->pc = lo | (pop(nes) << 8);
		break;
	}
	case 0x00:
		fetch(nes);
		interrupt(nes, true);
		break;

	// Branches
	case 0x10: branch(nes, !(cpu->p & FLAG_N)); break;
	case 0x30: branch(nes, cpu->p & FLAG_N); break;
	case 0x50: branch(nes, !(cpu->p & FLAG_V)); break;
	case 0x70: branch(nes, cpu->p & FLAG_V); break;
	case 0x90: branch(nes, !(cpu->p & FLAG_C)); break;
	case 0xB0: branch(nes, cpu->p & FLAG_C); break;
	case 0xD0: branch(nes, !(cpu->p & FLAG_Z)); break;
	case 0xF0: branch(nes, cpu->p & FLAG_Z); break;

	// The opcodes that lock the CPU up
	default:
		cpu->jammed = true;
		break;
	}

	if (cpu->prevNeedNmi || cpu->prevRunIrq) {
		cpu->opcodeCycle = true;
		dummyFetch(nes);
		cpu->opcodeCycle = false;
		dummyFetch(nes);
		interrupt(nes, false);
	}
}
