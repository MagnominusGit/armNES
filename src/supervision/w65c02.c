/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "w65c02.h"

// Decimal mode follows "Decimal Mode" by Bruce Clark (6502.org), the 65C02's sequences: its N and Z
// from the result, V as the NMOS 6502 has it.

enum { C = 1, Z = 2, I = 4, D = 8, B = 16, U = 32, V = 64, N = 128 };

#define RD(a) cpu->read(cpu->context, (uint16_t) (a))
#define WR(a, v) cpu->write(cpu->context, (uint16_t) (a), (uint8_t) (v))

static inline uint8_t fetch(struct W65c02* cpu) {
	return RD(cpu->pc++);
}

static inline uint16_t fetch16(struct W65c02* cpu) {
	uint8_t lo = fetch(cpu);
	return lo | fetch(cpu) << 8;
}

static inline void push(struct W65c02* cpu, uint8_t v) {
	WR(0x100 | cpu->s--, v);
}

static inline uint8_t pull(struct W65c02* cpu) {
	return RD(0x100 | ++cpu->s);
}

static inline void nz(struct W65c02* cpu, uint8_t v) {
	cpu->p = (cpu->p & ~(N | Z)) | (v & N) | (v ? 0 : Z);
}

static inline uint16_t read16zp(struct W65c02* cpu, uint8_t a) {
	return RD(a) | RD((uint8_t) (a + 1)) << 8;
}

// Addresses by mode; *extra gets a cycle where an index crosses a page.
static inline uint16_t zpx(struct W65c02* cpu) {
	return (uint8_t) (fetch(cpu) + cpu->x);
}

static inline uint16_t zpy(struct W65c02* cpu) {
	return (uint8_t) (fetch(cpu) + cpu->y);
}

static inline uint16_t absIndexed(struct W65c02* cpu, uint8_t index, int* extra) {
	uint16_t base = fetch16(cpu), a = base + index;
	*extra += (base ^ a) >> 8 ? 1 : 0;
	return a;
}

static inline uint16_t indX(struct W65c02* cpu) {
	return read16zp(cpu, (uint8_t) (fetch(cpu) + cpu->x));
}

static inline uint16_t indY(struct W65c02* cpu, int* extra) {
	uint16_t base = read16zp(cpu, fetch(cpu)), a = base + cpu->y;
	*extra += (base ^ a) >> 8 ? 1 : 0;
	return a;
}

static inline uint16_t indZp(struct W65c02* cpu) {
	return read16zp(cpu, fetch(cpu));
}

static void adc(struct W65c02* cpu, uint8_t m, int* extra) {
	int c = cpu->p & C, a = cpu->a;
	if (!(cpu->p & D)) {
		int r = a + m + c;
		cpu->p = (cpu->p & ~(C | V)) | (r > 0xff ? C : 0) | ((~(a ^ m) & (a ^ r) & 0x80) ? V : 0);
		cpu->a = (uint8_t) r;
		nz(cpu, cpu->a);
		return;
	}
	if (cpu->nmos) {
		// NMOS (Bruce Clark): Z from the binary sum, N and V from the high digit before its correction
		int lo = (a & 0x0f) + (m & 0x0f) + c;
		int hi = (a >> 4) + (m >> 4) + (lo > 9);
		lo += lo > 9 ? 6 : 0;
		uint8_t const binary = (uint8_t) (a + m + c);
		uint8_t const n = (uint8_t) (hi << 4);
		bool const v = ((n ^ a) & 0x80) && !((a ^ m) & 0x80);
		hi += hi > 9 ? 6 : 0;
		cpu->a = (uint8_t) (hi << 4 | (lo & 0x0f));
		cpu->p = (uint8_t) ((cpu->p & ~(C | V | N | Z)) | (hi > 15 ? C : 0) | (v ? V : 0) | (n & N) | (binary ? 0 : Z));
		return;
	}
	int lo = (a & 0x0f) + (m & 0x0f) + c;
	if (lo >= 0x0a) {
		lo = ((lo + 0x06) & 0x0f) + 0x10;
	}
	int r = (a & 0xf0) + (m & 0xf0) + lo;
	int s = (int8_t) (a & 0xf0) + (int8_t) (m & 0xf0) + lo;
	if (r >= 0xa0) {
		r += 0x60;
	}
	cpu->p = (cpu->p & ~(C | V)) | (r >= 0x100 ? C : 0) | (s < -128 || s > 127 ? V : 0);
	cpu->a = (uint8_t) r;
	nz(cpu, cpu->a);
	++*extra;
}

static void sbc(struct W65c02* cpu, uint8_t m, int* extra) {
	int c = cpu->p & C, a = cpu->a;
	int r = a - m + c - 1;
	cpu->p = (cpu->p & ~(C | V)) | (r >= 0 ? C : 0) | (((a ^ m) & (a ^ r) & 0x80) ? V : 0);
	if (!(cpu->p & D)) {
		cpu->a = (uint8_t) r;
		nz(cpu, cpu->a);
		return;
	}
	if (cpu->nmos) {
		// NMOS: every flag from the binary difference (set above, N and Z here), the digits corrected
		nz(cpu, (uint8_t) r);
		int lo = (a & 0x0f) - (m & 0x0f) + c - 1;
		int hi = (a >> 4) - (m >> 4) - (lo < 0);
		lo -= lo < 0 ? 6 : 0;
		hi -= hi < 0 ? 6 : 0;
		cpu->a = (uint8_t) (hi << 4 | (lo & 0x0f));
		return;
	}
	int lo = (a & 0x0f) - (m & 0x0f) + c - 1;
	if (r < 0) {
		r -= 0x60;
	}
	if (lo < 0) {
		r -= 0x06;
	}
	cpu->a = (uint8_t) r;
	nz(cpu, cpu->a);
	++*extra;
}

static inline void compare(struct W65c02* cpu, uint8_t reg, uint8_t m) {
	int r = reg - m;
	cpu->p = (cpu->p & ~C) | (r >= 0 ? C : 0);
	nz(cpu, (uint8_t) r);
}

static inline void bit(struct W65c02* cpu, uint8_t m) {
	cpu->p = (cpu->p & ~(N | V | Z)) | (m & (N | V)) | (cpu->a & m ? 0 : Z);
}

static inline uint8_t asl(struct W65c02* cpu, uint8_t v) {
	cpu->p = (cpu->p & ~C) | (v >> 7);
	v <<= 1;
	nz(cpu, v);
	return v;
}

static inline uint8_t lsr(struct W65c02* cpu, uint8_t v) {
	cpu->p = (cpu->p & ~C) | (v & 1);
	v >>= 1;
	nz(cpu, v);
	return v;
}

static inline uint8_t rol(struct W65c02* cpu, uint8_t v) {
	uint8_t r = (uint8_t) (v << 1 | (cpu->p & C));
	cpu->p = (cpu->p & ~C) | (v >> 7);
	nz(cpu, r);
	return r;
}

static inline uint8_t ror(struct W65c02* cpu, uint8_t v) {
	uint8_t r = (uint8_t) (v >> 1 | (cpu->p & C) << 7);
	cpu->p = (cpu->p & ~C) | (v & 1);
	nz(cpu, r);
	return r;
}

static inline uint8_t inc(struct W65c02* cpu, uint8_t v) {
	nz(cpu, ++v);
	return v;
}

static inline uint8_t dec(struct W65c02* cpu, uint8_t v) {
	nz(cpu, --v);
	return v;
}

static inline int branch(struct W65c02* cpu, bool taken) {
	int8_t offset = (int8_t) fetch(cpu);
	if (!taken) {
		return 2;
	}
	uint16_t to = cpu->pc + offset;
	int cycles = 3 + ((to ^ cpu->pc) >> 8 ? 1 : 0);
	cpu->pc = to;
	return cycles;
}

static int interrupt(struct W65c02* cpu, uint16_t vector, bool brk) {
	push(cpu, cpu->pc >> 8);
	push(cpu, (uint8_t) cpu->pc);
	push(cpu, (cpu->p | U | (brk ? B : 0)) & (brk ? 0xff : ~B));
	// The 65C02 clears decimal mode for the handler; an NMOS 6502 leaves it
	cpu->p = (uint8_t) ((cpu->p | I) & (cpu->nmos ? 0xff : ~D));
	cpu->pc = RD(vector) | RD(vector + 1) << 8;
	return 7;
}

void w65c02Reset(struct W65c02* cpu) {
	cpu->a = cpu->x = cpu->y = 0;
	cpu->s = 0xfd;
	cpu->p = (cpu->p | I | U) & ~D;
	cpu->nmi = cpu->waiting = cpu->stopped = false;
	cpu->pc = RD(0xfffc) | RD(0xfffd) << 8;
}

// The read-modify-write instructions on memory.
#define RMW(address, op, cycles) \
	do { \
		uint16_t at_ = (address); \
		WR(at_, op(cpu, RD(at_))); \
		return (cycles) + extra; \
	} while (0)

int w65c02Step(struct W65c02* cpu) {
	if (cpu->stopped) {
		return 4;
	}
	if (cpu->nmi) {
		cpu->nmi = false;
		cpu->waiting = false;
		return interrupt(cpu, 0xfffa, false);
	}
	if (cpu->irq) {
		cpu->waiting = false;
		if (!(cpu->p & I)) {
			return interrupt(cpu, 0xfffe, false);
		}
	}
	if (cpu->waiting) {
		return 4;
	}
	int extra = 0;
	uint8_t op = fetch(cpu);
	switch (op) {
	// Loads and stores
	case 0xa9: nz(cpu, cpu->a = fetch(cpu)); return 2;
	case 0xa5: nz(cpu, cpu->a = RD(fetch(cpu))); return 3;
	case 0xb5: nz(cpu, cpu->a = RD(zpx(cpu))); return 4;
	case 0xad: nz(cpu, cpu->a = RD(fetch16(cpu))); return 4;
	case 0xbd: nz(cpu, cpu->a = RD(absIndexed(cpu, cpu->x, &extra))); return 4 + extra;
	case 0xb9: nz(cpu, cpu->a = RD(absIndexed(cpu, cpu->y, &extra))); return 4 + extra;
	case 0xa1: nz(cpu, cpu->a = RD(indX(cpu))); return 6;
	case 0xb1: nz(cpu, cpu->a = RD(indY(cpu, &extra))); return 5 + extra;
	case 0xb2: nz(cpu, cpu->a = RD(indZp(cpu))); return 5;
	case 0xa2: nz(cpu, cpu->x = fetch(cpu)); return 2;
	case 0xa6: nz(cpu, cpu->x = RD(fetch(cpu))); return 3;
	case 0xb6: nz(cpu, cpu->x = RD(zpy(cpu))); return 4;
	case 0xae: nz(cpu, cpu->x = RD(fetch16(cpu))); return 4;
	case 0xbe: nz(cpu, cpu->x = RD(absIndexed(cpu, cpu->y, &extra))); return 4 + extra;
	case 0xa0: nz(cpu, cpu->y = fetch(cpu)); return 2;
	case 0xa4: nz(cpu, cpu->y = RD(fetch(cpu))); return 3;
	case 0xb4: nz(cpu, cpu->y = RD(zpx(cpu))); return 4;
	case 0xac: nz(cpu, cpu->y = RD(fetch16(cpu))); return 4;
	case 0xbc: nz(cpu, cpu->y = RD(absIndexed(cpu, cpu->x, &extra))); return 4 + extra;
	case 0x85: WR(fetch(cpu), cpu->a); return 3;
	case 0x95: WR(zpx(cpu), cpu->a); return 4;
	case 0x8d: WR(fetch16(cpu), cpu->a); return 4;
	case 0x9d: WR(absIndexed(cpu, cpu->x, &extra), cpu->a); return 5;
	case 0x99: WR(absIndexed(cpu, cpu->y, &extra), cpu->a); return 5;
	case 0x81: WR(indX(cpu), cpu->a); return 6;
	case 0x91: WR(indY(cpu, &extra), cpu->a); return 6;
	case 0x92: WR(indZp(cpu), cpu->a); return 5;
	case 0x86: WR(fetch(cpu), cpu->x); return 3;
	case 0x96: WR(zpy(cpu), cpu->x); return 4;
	case 0x8e: WR(fetch16(cpu), cpu->x); return 4;
	case 0x84: WR(fetch(cpu), cpu->y); return 3;
	case 0x94: WR(zpx(cpu), cpu->y); return 4;
	case 0x8c: WR(fetch16(cpu), cpu->y); return 4;
	case 0x64: WR(fetch(cpu), 0); return 3;
	case 0x74: WR(zpx(cpu), 0); return 4;
	case 0x9c: WR(fetch16(cpu), 0); return 4;
	case 0x9e: WR(absIndexed(cpu, cpu->x, &extra), 0); return 5;

	// Arithmetic and logic: ORA (0x0x/1x), AND (2x/3x), EOR (4x/5x), ADC (6x/7x), CMP (Cx/Dx), SBC (Ex/Fx)
#define ALU(base, body) \
	case base + 0x09: { uint8_t m = fetch(cpu); body; return 2 + extra; } \
	case base + 0x05: { uint8_t m = RD(fetch(cpu)); body; return 3 + extra; } \
	case base + 0x15: { uint8_t m = RD(zpx(cpu)); body; return 4 + extra; } \
	case base + 0x0d: { uint8_t m = RD(fetch16(cpu)); body; return 4 + extra; } \
	case base + 0x1d: { uint8_t m = RD(absIndexed(cpu, cpu->x, &extra)); body; return 4 + extra; } \
	case base + 0x19: { uint8_t m = RD(absIndexed(cpu, cpu->y, &extra)); body; return 4 + extra; } \
	case base + 0x01: { uint8_t m = RD(indX(cpu)); body; return 6 + extra; } \
	case base + 0x11: { uint8_t m = RD(indY(cpu, &extra)); body; return 5 + extra; } \
	case base + 0x12: { uint8_t m = RD(indZp(cpu)); body; return 5 + extra; }
	ALU(0x00, nz(cpu, cpu->a |= m))
	ALU(0x20, nz(cpu, cpu->a &= m))
	ALU(0x40, nz(cpu, cpu->a ^= m))
	ALU(0x60, adc(cpu, m, &extra))
	ALU(0xc0, compare(cpu, cpu->a, m))
	ALU(0xe0, sbc(cpu, m, &extra))
#undef ALU
	case 0xe0: compare(cpu, cpu->x, fetch(cpu)); return 2;
	case 0xe4: compare(cpu, cpu->x, RD(fetch(cpu))); return 3;
	case 0xec: compare(cpu, cpu->x, RD(fetch16(cpu))); return 4;
	case 0xc0: compare(cpu, cpu->y, fetch(cpu)); return 2;
	case 0xc4: compare(cpu, cpu->y, RD(fetch(cpu))); return 3;
	case 0xcc: compare(cpu, cpu->y, RD(fetch16(cpu))); return 4;
	case 0x89: cpu->p = (cpu->p & ~Z) | (cpu->a & fetch(cpu) ? 0 : Z); return 2;
	case 0x24: bit(cpu, RD(fetch(cpu))); return 3;
	case 0x34: bit(cpu, RD(zpx(cpu))); return 4;
	case 0x2c: bit(cpu, RD(fetch16(cpu))); return 4;
	case 0x3c: bit(cpu, RD(absIndexed(cpu, cpu->x, &extra))); return 4 + extra;

	// Shifts, increments and decrements, and the 65C02's bit set and reset
	case 0x0a: cpu->a = asl(cpu, cpu->a); return 2;
	case 0x4a: cpu->a = lsr(cpu, cpu->a); return 2;
	case 0x2a: cpu->a = rol(cpu, cpu->a); return 2;
	case 0x6a: cpu->a = ror(cpu, cpu->a); return 2;
#define SHIFTS(base, fn) \
	case base + 0x06: RMW(fetch(cpu), fn, 5); \
	case base + 0x16: RMW(zpx(cpu), fn, 6); \
	case base + 0x0e: RMW(fetch16(cpu), fn, 6); \
	case base + 0x1e: RMW(absIndexed(cpu, cpu->x, &extra), fn, 6);
	SHIFTS(0x00, asl)
	SHIFTS(0x40, lsr)
	SHIFTS(0x20, rol)
	SHIFTS(0x60, ror)
#undef SHIFTS
	case 0xe6: RMW(fetch(cpu), inc, 5);
	case 0xf6: RMW(zpx(cpu), inc, 6);
	case 0xee: RMW(fetch16(cpu), inc, 6);
	case 0xfe: { uint16_t a = absIndexed(cpu, cpu->x, &extra); WR(a, inc(cpu, RD(a))); return 7; }
	case 0xc6: RMW(fetch(cpu), dec, 5);
	case 0xd6: RMW(zpx(cpu), dec, 6);
	case 0xce: RMW(fetch16(cpu), dec, 6);
	case 0xde: { uint16_t a = absIndexed(cpu, cpu->x, &extra); WR(a, dec(cpu, RD(a))); return 7; }
	case 0x1a: nz(cpu, ++cpu->a); return 2;
	case 0x3a: nz(cpu, --cpu->a); return 2;
	case 0xe8: nz(cpu, ++cpu->x); return 2;
	case 0xca: nz(cpu, --cpu->x); return 2;
	case 0xc8: nz(cpu, ++cpu->y); return 2;
	case 0x88: nz(cpu, --cpu->y); return 2;
	case 0x04: case 0x0c: case 0x14: case 0x1c: {
		uint16_t a = op & 8 ? fetch16(cpu) : fetch(cpu);
		uint8_t m = RD(a);
		cpu->p = (cpu->p & ~Z) | (cpu->a & m ? 0 : Z);
		WR(a, op & 0x10 ? m & ~cpu->a : m | cpu->a);
		return op & 8 ? 6 : 5;
	}
	case 0x07: case 0x17: case 0x27: case 0x37: case 0x47: case 0x57: case 0x67: case 0x77:
	case 0x87: case 0x97: case 0xa7: case 0xb7: case 0xc7: case 0xd7: case 0xe7: case 0xf7: {
		uint8_t a = fetch(cpu), mask = (uint8_t) (1 << ((op >> 4) & 7));
		uint8_t m = RD(a);
		WR(a, op & 0x80 ? m | mask : m & ~mask);
		return 5;
	}

	// Transfers, the stack and the flags
	case 0xaa: nz(cpu, cpu->x = cpu->a); return 2;
	case 0x8a: nz(cpu, cpu->a = cpu->x); return 2;
	case 0xa8: nz(cpu, cpu->y = cpu->a); return 2;
	case 0x98: nz(cpu, cpu->a = cpu->y); return 2;
	case 0xba: nz(cpu, cpu->x = cpu->s); return 2;
	case 0x9a: cpu->s = cpu->x; return 2;
	case 0x48: push(cpu, cpu->a); return 3;
	case 0xda: push(cpu, cpu->x); return 3;
	case 0x5a: push(cpu, cpu->y); return 3;
	case 0x08: push(cpu, cpu->p | B | U); return 3;
	case 0x68: nz(cpu, cpu->a = pull(cpu)); return 4;
	case 0xfa: nz(cpu, cpu->x = pull(cpu)); return 4;
	case 0x7a: nz(cpu, cpu->y = pull(cpu)); return 4;
	case 0x28: cpu->p = (pull(cpu) | U) & ~B; return 4;
	case 0x18: cpu->p &= ~C; return 2;
	case 0x38: cpu->p |= C; return 2;
	case 0x58: cpu->p &= ~I; return 2;
	case 0x78: cpu->p |= I; return 2;
	case 0xd8: cpu->p &= ~D; return 2;
	case 0xf8: cpu->p |= D; return 2;
	case 0xb8: cpu->p &= ~V; return 2;

	// Jumps, calls, returns and branches
	case 0x4c: cpu->pc = fetch16(cpu); return 3;
	case 0x6c: {
		// An NMOS 6502 takes the high byte from the start of the same page when the pointer ends one
		uint16_t a = fetch16(cpu);
		uint16_t high = cpu->nmos ? (uint16_t) ((a & 0xff00) | ((a + 1) & 0xff)) : (uint16_t) (a + 1);
		cpu->pc = (uint16_t) (RD(a) | RD(high) << 8);
		return cpu->nmos ? 5 : 6;
	}
	case 0x7c: { uint16_t a = fetch16(cpu) + cpu->x; cpu->pc = RD(a) | RD(a + 1) << 8; return 6; }
	case 0x20: {
		uint16_t to = fetch16(cpu);
		uint16_t back = cpu->pc - 1;
		push(cpu, back >> 8);
		push(cpu, (uint8_t) back);
		cpu->pc = to;
		return 6;
	}
	case 0x60: { uint8_t lo = pull(cpu); cpu->pc = (uint16_t) ((lo | pull(cpu) << 8) + 1); return 6; }
	case 0x40: {
		cpu->p = (pull(cpu) | U) & ~B;
		uint8_t lo = pull(cpu);
		cpu->pc = lo | pull(cpu) << 8;
		return 6;
	}
	case 0x00: cpu->pc++; return interrupt(cpu, 0xfffe, true);
	case 0x10: return branch(cpu, !(cpu->p & N));
	case 0x30: return branch(cpu, cpu->p & N);
	case 0x50: return branch(cpu, !(cpu->p & V));
	case 0x70: return branch(cpu, cpu->p & V);
	case 0x90: return branch(cpu, !(cpu->p & C));
	case 0xb0: return branch(cpu, cpu->p & C);
	case 0xd0: return branch(cpu, !(cpu->p & Z));
	case 0xf0: return branch(cpu, cpu->p & Z);
	case 0x80: return branch(cpu, true);
	case 0x0f: case 0x1f: case 0x2f: case 0x3f: case 0x4f: case 0x5f: case 0x6f: case 0x7f:
	case 0x8f: case 0x9f: case 0xaf: case 0xbf: case 0xcf: case 0xdf: case 0xef: case 0xff: {
		uint8_t m = RD(fetch(cpu));
		bool set = (m >> ((op >> 4) & 7)) & 1;
		return 3 + branch(cpu, op & 0x80 ? set : !set);
	}

	// Waiting and stopping
	case 0xcb: cpu->waiting = true; return 3;
	case 0xdb: cpu->stopped = true; return 3;
	case 0xea: return 2;

	// The rest are NOPs of their lengths and times
	case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xc2: case 0xe2: cpu->pc++; return 2;
	case 0x44: cpu->pc++; return 3;
	case 0x54: case 0xd4: case 0xf4: cpu->pc++; return 4;
	case 0x5c: cpu->pc += 2; return 8;
	case 0xdc: case 0xfc: cpu->pc += 2; return 4;
	default: return 1;
	}
}
