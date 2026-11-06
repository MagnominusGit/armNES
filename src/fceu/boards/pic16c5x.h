

	/**************************************************************************\
	*                    Copyright Tony La Porta                               *
	*                                                                          *
	\**************************************************************************/

#ifndef _PIC16C5x_H
#define _PIC16C5x_H

#include "../fceu-types.h"

typedef uint8_t (*pic16c5x_readfunc)(int);
typedef void (*pic16c5x_writefunc)(int, int);

enum {
	PIC16C5x_RTCC = 0
};

#define PIC16C5x_T0CKI PIC16C5x_RTCC

enum {
	PIC16C5x_PORTA = 0,
	PIC16C5x_PORTB,
	PIC16C5x_PORTC,
	PIC16C5x_PORTD
};

void pic16c5x_set_input(int line, int state);
void pic16c5x_set_config(uint16_t data);
void pic16c5x_reset(uint8_t hard);
void pic16c5x_run(void);
void pic16c5x_add_statesinfo(void);

void pic16c54_init(uint8_t* rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr);
void pic16c55_init(uint8_t* rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr);
void pic16c56_init(uint8_t* rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr);
void pic16c57_init(uint8_t* rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr);
void pic16c58_init(uint8_t* rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr);
void pic1650_init(uint8_t* rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr);
void pic1655_init(uint8_t* rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr);

#endif
