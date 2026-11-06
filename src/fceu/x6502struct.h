#ifndef _X6502STRUCTH
#define _X6502STRUCTH

typedef struct __X6502 {
	int32_t tcount;
	uint16_t PC;

	uint8_t A, X, Y, S, P, mooPI;
	uint8_t jammed;

	int32_t count;
	uint32_t IRQlow;
	uint8_t DB;
} X6502;

#endif
