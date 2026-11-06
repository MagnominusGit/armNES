
	/**************************************************************************\
	*                    Copyright Tony La Porta                               *
	*                                                                          *
	\**************************************************************************/

#include <string.h>
#include "pic16c5x.h"

static uint16_t m_PC;
static uint16_t m_PREVPC;
static uint8_t m_W;
static uint8_t m_OPTION;
static uint16_t m_CONFIG;
static uint8_t m_ALU;
static uint16_t m_WDT;
static uint8_t m_TRISA;
static uint8_t m_TRISB;
static uint8_t m_TRISC;
static uint16_t m_STACK[2];
static uint16_t m_prescaler;
static uint16_t m_opcode;
static uint8_t m_internalram[128];
static uint8_t *m_rom;

static int m_icount;
static int m_picmodel;
static int m_delay_timer;
static uint16_t m_temp_config;
static int m_rtcc;
static uint8_t m_count_pending;
static int8_t m_old_data;
static uint8_t m_picRAMmask;
static uint16_t m_picROMmask;
static int m_inst_cycles;
static int m_clock2cycle;

static pic16c5x_readfunc m_read;
static pic16c5x_writefunc m_write;

static void PIC16C5x(int program_width, int data_width, int picmodel, uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr)
{
	m_rom = rom;
	m_picmodel = picmodel;
	m_temp_config = 0;
	m_picRAMmask = ((1 << data_width) - 1);
	m_picROMmask = ((1 << program_width) - 1);
	m_read = _rd;
	m_write = _wr;
	m_PC = 0;
	m_PREVPC = 0;
	m_W = 0;
	m_OPTION = 0;
	m_CONFIG = 0;
	m_ALU = 0;
	m_WDT = 0;
	m_TRISA = 0;
	m_TRISB = 0;
	m_TRISC = 0;
	m_prescaler = 0;
	m_icount = 0;
	m_delay_timer = 0;
	m_rtcc = 0;
	m_count_pending = 0;
	m_old_data = 0;
	m_inst_cycles = 0;
	m_clock2cycle = 0;
}

void pic16c54_init(uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr) { PIC16C5x( 9, 5, 0x16C54, rom, _rd, _wr); }
void pic16c55_init(uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr) { PIC16C5x( 9, 5, 0x16C55, rom, _rd, _wr); }
void pic16c56_init(uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr) { PIC16C5x(10, 5, 0x16C56, rom, _rd, _wr); }
void pic16c57_init(uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr) { PIC16C5x(11, 7, 0x16C57, rom, _rd, _wr); }
void pic16c58_init(uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr) { PIC16C5x(11, 7, 0x16C58, rom, _rd, _wr); }
void pic1650_init (uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr) { PIC16C5x( 9, 5, 0x1650,  rom, _rd, _wr); }
void pic1655_init (uint8_t *rom, pic16c5x_readfunc _rd, pic16c5x_writefunc _wr) { PIC16C5x( 9, 5, 0x1655,  rom, _rd, _wr); }

#define M_OPCODE_B0 (m_opcode & 0xFF)
#define M_OPCODE_B1 ((m_opcode >> 8) & 0xFF)
#define M_OPCODE_S0 m_opcode

#define M_RDRAM(A)      m_internalram[A &m_picRAMmask]
#define M_WRTRAM(A,V)   m_internalram[A &m_picRAMmask] =(V)
#define M_RDOP(A)       m_rom[(A &m_picROMmask) <<1 |0] | m_rom[(A &m_picROMmask) <<1 |1] <<8
#define ADDR_MASK       0x7ff

#define TMR0    m_internalram[1]
#define PCL     m_internalram[2]
#define STATUS  m_internalram[3]
#define FSR     m_internalram[4]
#define PORTA   m_internalram[5]
#define PORTB   m_internalram[6]
#define PORTC   m_internalram[7]
#define PORTD   m_internalram[8]
#define INDF    M_RDRAM(FSR)

#define ADDR    (M_OPCODE_B0 & 0x1f)

#define PA_REG      0xe0
#define TO_FLAG     0x10
#define PD_FLAG     0x08
#define Z_FLAG      0x04
#define DC_FLAG     0x02
#define C_FLAG      0x01

#define PA      (STATUS & PA_REG)
#define TO      (STATUS & TO_FLAG)
#define PD      (STATUS & PD_FLAG)
#define ZERO    (STATUS & Z_FLAG)
#define DC      (STATUS & DC_FLAG)
#define CARRY   (STATUS & C_FLAG)

#define T0CS_FLAG   0x20
#define T0SE_FLAG   0x10
#define PSA_FLAG    0x08
#define PS_REG      0x07

#define T0CS    (m_OPTION & T0CS_FLAG)
#define T0SE    (m_OPTION & T0SE_FLAG)
#define PSA     (m_OPTION & PSA_FLAG)
#define PS      (m_OPTION & PS_REG)

#define WDTE_FLAG   0x04
#define FOSC_FLAG   0x03

#define WDTE    (m_CONFIG & WDTE_FLAG)
#define FOSC    (m_CONFIG & FOSC_FLAG)

#define CLR(flagreg, flag) ( flagreg &= (uint8_t)(~flag) )
#define SET(flagreg, flag) ( flagreg |=  flag )

#define POS  ((M_OPCODE_B0 >> 5) & 7)
static const unsigned int bit_clr[8] = { 0xfe, 0xfd, 0xfb, 0xf7, 0xef, 0xdf, 0xbf, 0x7f };
static const unsigned int bit_set[8] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80 };

static INLINE void CALCULATE_Z_FLAG(void)
{
	if (m_ALU == 0) SET(STATUS, Z_FLAG);
	else CLR(STATUS, Z_FLAG);
}

static INLINE void CALCULATE_ADD_CARRY(void)
{
	if ((uint8_t)(m_old_data) > (uint8_t)(m_ALU)) {
		SET(STATUS, C_FLAG);
	}
	else {
		CLR(STATUS, C_FLAG);
	}
}

static INLINE void CALCULATE_ADD_DIGITCARRY(void)
{
	if (((uint8_t)(m_old_data) & 0x0f) > ((uint8_t)(m_ALU) & 0x0f)) {
		SET(STATUS, DC_FLAG);
	}
	else {
		CLR(STATUS, DC_FLAG);
	}
}

static INLINE void CALCULATE_SUB_CARRY(void)
{
	if ((uint8_t)(m_old_data) < (uint8_t)(m_ALU)) {
		CLR(STATUS, C_FLAG);
	}
	else {
		SET(STATUS, C_FLAG);
	}
}

static INLINE void CALCULATE_SUB_DIGITCARRY(void)
{
	if (((uint8_t)(m_old_data) & 0x0f) < ((uint8_t)(m_ALU) & 0x0f)) {
		CLR(STATUS, DC_FLAG);
	}
	else {
		SET(STATUS, DC_FLAG);
	}
}

static INLINE uint16_t POP_STACK(void)
{
	uint16_t data = m_STACK[1];
	m_STACK[1] = m_STACK[0];
	return (data & ADDR_MASK);
}
static INLINE void PUSH_STACK(uint16_t data)
{
	m_STACK[0] = m_STACK[1];
	m_STACK[1] = (data & ADDR_MASK);
}

static INLINE uint8_t GET_REGFILE(uint32_t addr)
{
	uint8_t data = 0;

	if (addr == 0) {
		addr = (FSR & m_picRAMmask);
	}

	if ((m_picmodel == 0x16C57) || (m_picmodel == 0x16C58)) {
		addr |= (FSR & 0x60);
	}

	if ((addr & 0x10) == 0) addr &= 0x0f;

	switch(addr)
	{
		case 0:
					data = 0;
					break;
		case 4:     data = (FSR | (uint8_t)(~m_picRAMmask));
					break;
		case 5:
					if (m_picmodel == 0x1650) {
						data = m_read(PIC16C5x_PORTA) & PORTA;
					}
					else if (m_picmodel == 0x1655) {
						data = m_read(PIC16C5x_PORTA) & 0x0f;
					}
					else {
						data = m_read(PIC16C5x_PORTA);
						data &= m_TRISA;
						data |= ((uint8_t)(~m_TRISA) & PORTA);
						data &= 0x0f;
					}
					break;
		case 6:
					if (m_picmodel == 0x1650) {
						data = m_read(PIC16C5x_PORTB) & PORTB;
					}
					else if (m_picmodel != 0x1655) {
						data = m_read(PIC16C5x_PORTB);
						data &= m_TRISB;
						data |= ((uint8_t)(~m_TRISB) & PORTB);
					}
					break;
		case 7:
					if (m_picmodel == 0x1650 || m_picmodel == 0x1655) {
						data = m_read(PIC16C5x_PORTC) & PORTC;
					}
					else if ((m_picmodel == 0x16C55) || (m_picmodel == 0x16C57)) {
						data = m_read(PIC16C5x_PORTC);
						data &= m_TRISC;
						data |= ((uint8_t)(~m_TRISC) & PORTC);
					}
					else {
						data = M_RDRAM(addr);
					}
					break;
		case 8:
					if (m_picmodel == 0x1650) {
						data = m_read(PIC16C5x_PORTD) & PORTD;
					}
					else {
						data = M_RDRAM(addr);
					}
					break;
		default:    data = M_RDRAM(addr);
					break;
	}
	return data;
}

static INLINE void STORE_REGFILE(uint32_t addr, uint8_t data)
{
	if (addr == 0) {
		addr = (FSR & m_picRAMmask);
	}

	if ((m_picmodel == 0x16C57) || (m_picmodel == 0x16C58)) {
		addr |= (FSR & 0x60);
	}

	if ((addr & 0x10) == 0) addr &= 0x0f;

	switch(addr)
	{
		case 0:
					break;
		case 1:     m_delay_timer = 2;
					if (PSA == 0) m_prescaler = 0;
					TMR0 = data;
					break;
		case 2:     PCL = data;
					m_PC = ((STATUS & PA_REG) << 4) | data;
					break;
		case 3:     STATUS = (STATUS & (TO_FLAG | PD_FLAG)) | (data & (uint8_t)(~(TO_FLAG | PD_FLAG)));
					break;
		case 4:     FSR = (data | (uint8_t)(~m_picRAMmask));
					break;
		case 5:
					if (m_picmodel == 0x1650) {
						m_write(PIC16C5x_PORTA, data);
					}
					else if (m_picmodel != 0x1655) {
						data &= 0x0f;
						m_write(PIC16C5x_PORTA, data & (uint8_t)(~m_TRISA));
					}
					PORTA = data;
					break;
		case 6:
					if (m_picmodel == 0x1650 || m_picmodel == 0x1655) {
						m_write(PIC16C5x_PORTB, data);
					}
					else {
						m_write(PIC16C5x_PORTB, data & (uint8_t)(~m_TRISB));
					}
					PORTB = data;
					break;
		case 7:
					if (m_picmodel == 0x1650 || m_picmodel == 0x1655) {
						m_write(PIC16C5x_PORTC, data);
					}
					else if ((m_picmodel == 0x16C55) || (m_picmodel == 0x16C57)) {
						m_write(PIC16C5x_PORTC, data & (uint8_t)(~m_TRISC));
					}
					PORTC = data;
					break;
		case 8:
					if (m_picmodel == 0x1650) {
						m_write(PIC16C5x_PORTD, data);
					}
					PORTD = data;
					break;
		default:    M_WRTRAM(addr, data);
					break;
	}
}

static INLINE void STORE_RESULT(uint32_t addr, uint8_t data)
{
	if (M_OPCODE_B0 & 0x20)
	{
		STORE_REGFILE(addr, data);
	}
	else
	{
		m_W = data;
	}
}

static INLINE void illegal(void)
{

}

static INLINE void addwf(void)
{
	m_old_data = GET_REGFILE(ADDR);
	m_ALU = m_old_data + m_W;
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
	CALCULATE_ADD_CARRY();
	CALCULATE_ADD_DIGITCARRY();
}

static INLINE void andwf(void)
{
	m_ALU = GET_REGFILE(ADDR) & m_W;
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
}

static INLINE void andlw(void)
{
	m_ALU = M_OPCODE_B0 & m_W;
	m_W = m_ALU;
	CALCULATE_Z_FLAG();
}

static INLINE void bcf(void)
{
	m_ALU = GET_REGFILE(ADDR);
	m_ALU &= bit_clr[POS];
	STORE_REGFILE(ADDR, m_ALU);
}

static INLINE void bsf(void)
{
	m_ALU = GET_REGFILE(ADDR);
	m_ALU |= bit_set[POS];
	STORE_REGFILE(ADDR, m_ALU);
}

static INLINE void btfss(void)
{
	if ((GET_REGFILE(ADDR) & bit_set[POS]) == bit_set[POS])
	{
		m_PC++;
		PCL = m_PC & 0xff;
		m_inst_cycles += 1;
	}
}

static INLINE void btfsc(void)
{
	if ((GET_REGFILE(ADDR) & bit_set[POS]) == 0)
	{
		m_PC++;
		PCL = m_PC & 0xff;
		m_inst_cycles += 1;
	}
}

static INLINE void call(void)
{
	PUSH_STACK(m_PC);
	m_PC = ((STATUS & PA_REG) << 4) | M_OPCODE_B0;
	m_PC &= 0x6ff;
	PCL = m_PC & 0xff;
}

static INLINE void clrw(void)
{
	m_W = 0;
	SET(STATUS, Z_FLAG);
}

static INLINE void clrf(void)
{
	STORE_REGFILE(ADDR, 0);
	SET(STATUS, Z_FLAG);
}

static INLINE void clrwdt(void)
{
	m_WDT = 0;
	if (PSA) m_prescaler = 0;
	SET(STATUS, TO_FLAG);
	SET(STATUS, PD_FLAG);
}

static INLINE void comf(void)
{
	m_ALU = (uint8_t)(~(GET_REGFILE(ADDR)));
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
}

static INLINE void decf(void)
{
	m_ALU = GET_REGFILE(ADDR) - 1;
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
}

static INLINE void decfsz(void)
{
	m_ALU = GET_REGFILE(ADDR) - 1;
	STORE_RESULT(ADDR, m_ALU);
	if (m_ALU == 0)
	{
		m_PC++;
		PCL = m_PC & 0xff;
		m_inst_cycles += 1;
	}
}

static INLINE void goto_op(void)
{
	m_PC = ((STATUS & PA_REG) << 4) | (M_OPCODE_S0 & 0x1ff);
	m_PC &= ADDR_MASK;
	PCL = m_PC & 0xff;
}

static INLINE void incf(void)
{
	m_ALU = GET_REGFILE(ADDR) + 1;
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
}

static INLINE void incfsz(void)
{
	m_ALU = GET_REGFILE(ADDR) + 1;
	STORE_RESULT(ADDR, m_ALU);
	if (m_ALU == 0)
	{
		m_PC++;
		PCL = m_PC & 0xff;
		m_inst_cycles += 1;
	}
}

static INLINE void iorlw(void)
{
	m_ALU = M_OPCODE_B0 | m_W;
	m_W = m_ALU;
	CALCULATE_Z_FLAG();
}

static INLINE void iorwf(void)
{
	m_ALU = GET_REGFILE(ADDR) | m_W;
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
}

static INLINE void movf(void)
{
	m_ALU = GET_REGFILE(ADDR);
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
}

static INLINE void movlw(void)
{
	m_W = M_OPCODE_B0;
}

static INLINE void movwf(void)
{
	STORE_REGFILE(ADDR, m_W);
}

static INLINE void nop(void)
{

}

static INLINE void option(void)
{
	m_OPTION = m_W & (T0CS_FLAG | T0SE_FLAG | PSA_FLAG | PS_REG);
}

static INLINE void retlw(void)
{
	m_W = M_OPCODE_B0;
	m_PC = POP_STACK();
	PCL = m_PC & 0xff;
}

static INLINE void rlf(void)
{
	uint8_t bit7;
	m_ALU = GET_REGFILE(ADDR);
	bit7 = m_ALU & 0x80;
	m_ALU <<= 1;
	if (STATUS & C_FLAG) m_ALU |= 1;
	STORE_RESULT(ADDR, m_ALU);
	if (bit7) SET(STATUS, C_FLAG);
	else CLR(STATUS, C_FLAG);
}

static INLINE void rrf(void)
{
	uint8_t bit0;
	m_ALU = GET_REGFILE(ADDR);
	bit0 = m_ALU & 1;
	m_ALU >>= 1;
	if (STATUS & C_FLAG) m_ALU |= 0x80;
	STORE_RESULT(ADDR, m_ALU);
	if (bit0) SET(STATUS, C_FLAG);
	else CLR(STATUS, C_FLAG);
}

static INLINE void sleepic(void)
{
	if (WDTE) m_WDT = 0;
	if (PSA) m_prescaler = 0;
	SET(STATUS, TO_FLAG);
	CLR(STATUS, PD_FLAG);
}

static INLINE void subwf(void)
{
	m_old_data = GET_REGFILE(ADDR);
	m_ALU = m_old_data - m_W;
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
	CALCULATE_SUB_CARRY();
	CALCULATE_SUB_DIGITCARRY();
}

static INLINE void swapf(void)
{
	m_ALU  = ((GET_REGFILE(ADDR) << 4) & 0xf0);
	m_ALU |= ((GET_REGFILE(ADDR) >> 4) & 0x0f);
	STORE_RESULT(ADDR, m_ALU);
}

static INLINE void tris(void)
{
	switch(M_OPCODE_B0 & 0x7)
	{
		case 5:     if   (m_TRISA == m_W) break;
					else { m_TRISA = m_W | 0xf0; m_write(PIC16C5x_PORTA, 0x1000 | (PORTA & (uint8_t)(~m_TRISA) & 0x0f)); break; }
		case 6:     if   (m_TRISB == m_W) break;
					else { m_TRISB = m_W; m_write(PIC16C5x_PORTB, 0x1000 | (PORTB & (uint8_t)(~m_TRISB))); break; }
		case 7:     if ((m_picmodel == 0x16C55) || (m_picmodel == 0x16C57)) {
						if   (m_TRISC == m_W) break;
						else { m_TRISC = m_W; m_write(PIC16C5x_PORTC, 0x1000 | (PORTC & (uint8_t)(~m_TRISC))); break; }
					}
					else {
						illegal(); break;
					}
		default:    illegal(); break;
	}
}

static INLINE void xorlw(void)
{
	m_ALU = m_W ^ M_OPCODE_B0;
	m_W = m_ALU;
	CALCULATE_Z_FLAG();
}

static INLINE void xorwf(void)
{
	m_ALU = GET_REGFILE(ADDR) ^ m_W;
	STORE_RESULT(ADDR, m_ALU);
	CALCULATE_Z_FLAG();
}

static const uint8_t s_opcode_main_cycles[256] = {
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,

  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2,

  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1
};

static const uint8_t s_opcode_00x_cycles[16] = {
  1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1
};

enum
{
	PIC16C5x_PC=1, PIC16C5x_STK0, PIC16C5x_STK1, PIC16C5x_FSR,
	PIC16C5x_W,    PIC16C5x_ALU,  PIC16C5x_STR,  PIC16C5x_OPT,
	PIC16C5x_TMR0, PIC16C5x_PRTA, PIC16C5x_PRTB, PIC16C5x_PRTC, PIC16C5x_PRTD,
	PIC16C5x_WDT,  PIC16C5x_TRSA, PIC16C5x_TRSB, PIC16C5x_TRSC,
	PIC16C5x_PSCL
};

static INLINE void PIC16C5x_reset_regs(void)
{
	m_PC     = m_picROMmask;
	m_CONFIG = m_temp_config;
	m_TRISA  = 0xff;
	m_TRISB  = 0xff;
	m_TRISC  = 0xff;
	m_OPTION = (T0CS_FLAG | T0SE_FLAG | PSA_FLAG | PS_REG);
	PCL    = 0xff;
	FSR   |= (uint8_t)(~m_picRAMmask);
	m_prescaler = 0;
	m_delay_timer = 0;
	m_inst_cycles = 0;
	m_count_pending = 0;
}

void pic16c5x_reset(uint8_t hard)
{
	if (hard) {
		memset(m_internalram, 0, sizeof(m_internalram));
		PIC16C5x_reset_regs();
		CLR(STATUS, PA_REG);
		SET(STATUS, (TO_FLAG | PD_FLAG));
		m_icount = 0;
		m_clock2cycle = 0;
	} else {
		SET(STATUS, (TO_FLAG | PD_FLAG | Z_FLAG | DC_FLAG | C_FLAG));
		PIC16C5x_reset_regs();
	}
}

void pic16c5x_set_config(uint16_t data)
{
	m_temp_config = data;
}

static INLINE void PIC16C5x_update_watchdog(int counts)
{

	if ((M_OPCODE_S0 != 3) && (M_OPCODE_S0 != 4))
	{
		uint16_t old_WDT = m_WDT;

		m_WDT -= counts;

		if (m_WDT > 0x464f) {
			m_WDT = 0x464f - (0xffff - m_WDT);
		}

		if (((old_WDT != 0) && (old_WDT < m_WDT)) || (m_WDT == 0))
		{
			if (PSA) {
				m_prescaler++;
				if (m_prescaler >= (1 << PS)) {
					m_prescaler = 0;
					CLR(STATUS, TO_FLAG);

				}
			}
			else {
				CLR(STATUS, TO_FLAG);

			}
		}
	}
}

static INLINE void PIC16C5x_update_timer(int counts)
{
	if (PSA == 0) {
		m_prescaler += counts;
		if (m_prescaler >= (2 << PS)) {
			TMR0 += (m_prescaler / (2 << PS));
			m_prescaler %= (2 << PS);
		}
	}
	else {
		TMR0 += counts;
	}
}

void pic16c5x_set_input(int line, int state)
{
	switch (line)
	{

		case PIC16C5x_RTCC:
			if (T0CS && state != m_rtcc)
				if ((T0SE && !state) || (!T0SE && state))
					m_count_pending = 1;

			m_rtcc = state;
			break;

		default:
			break;
	}
}

void pic16c5x_run(void)
{
	if ((++m_clock2cycle &3) ==0) m_icount++;

	while (m_icount > 0) {
		if (PD == 0)
		{
			m_count_pending = 0;
			m_inst_cycles = 1;
			if (WDTE) {
				PIC16C5x_update_watchdog(1);
			}
		}
		else
		{
			if (m_count_pending) {
				m_count_pending = 0;
				PIC16C5x_update_timer(1);
			}

			m_PREVPC = m_PC;

			M_OPCODE_S0 = M_RDOP(m_PC);

			m_PC++;
			PCL++;

			if (m_picmodel == 0x1650 || m_picmodel == 0x1655 || (M_OPCODE_S0 & 0xff0) != 0x000) {
				uint8_t b1 = (M_OPCODE_S0 >> 4) & 0xff;
				m_inst_cycles = s_opcode_main_cycles[b1];
				switch (b1) {
					#include "pic16c5x_ops_main.inc"
				}
			}
			else {
				uint8_t b1 = M_OPCODE_B0 & 0x1f;
				m_inst_cycles = s_opcode_00x_cycles[b1];
				switch (b1) {
					#include "pic16c5x_ops_00x.inc"
				}
			}

			if (!T0CS) {
				if (m_delay_timer) {
					m_delay_timer--;
				}
				else {
					PIC16C5x_update_timer(m_inst_cycles);
				}
			}
			if (WDTE) {
				PIC16C5x_update_watchdog(m_inst_cycles);
			}
		}

		m_icount -= m_inst_cycles;
	}
}

void AddExState(void *v, uint32_t s, int type, char *desc);

void pic16c5x_add_statesinfo(void) {
	AddExState(&m_PC, sizeof(m_PC), 1, "PC00");
	AddExState(&m_PREVPC, sizeof(m_PREVPC), 1, "PRPC");
	AddExState(&m_W, sizeof(m_W), 0, "W000");
	AddExState(&m_OPTION, sizeof(m_OPTION), 0, "OPT0");
	AddExState(&m_CONFIG, sizeof(m_CONFIG), 1, "CONF");
	AddExState(&m_ALU, sizeof(m_ALU), 0, "ALU");
	AddExState(&m_WDT, sizeof(m_WDT), 1, "WDT");
	AddExState(&m_TRISA, sizeof(m_TRISA), 0, "TRSA");
	AddExState(&m_TRISB, sizeof(m_TRISB), 0, "TRSB");
	AddExState(&m_TRISC, sizeof(m_TRISC), 0, "TRSC");
	AddExState(&m_STACK[0], sizeof(m_STACK[0]), 1, "STC0");
	AddExState(&m_STACK[1], sizeof(m_STACK[1]), 1, "STC1");
	AddExState(&m_prescaler, sizeof(m_prescaler), 1, "PSCL");
	AddExState(&m_opcode, sizeof(m_opcode), 1, "OPS0");
	AddExState(m_internalram, sizeof(m_internalram), 0, "IRAM");
	AddExState(&m_icount, sizeof(m_icount), 1, "ICNT");
	AddExState(&m_delay_timer, sizeof(m_delay_timer), 1, "DTIM");
	AddExState(&m_rtcc, sizeof(m_rtcc), 1, "RTCC");
	AddExState(&m_count_pending, sizeof(m_count_pending), 0, "CNTP");
	AddExState(&m_inst_cycles, sizeof(m_inst_cycles), 1, "ICYC");
	AddExState(&m_clock2cycle, sizeof(m_clock2cycle), 1, "CL2C");
}