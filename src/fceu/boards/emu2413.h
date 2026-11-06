#ifndef _EMU2413_H_
#define _EMU2413_H_

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../fceu-types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PI 3.14159265358979323846

enum { OPLL_VRC7_TONE=0 };

typedef struct {
	uint32_t TL, FB, EG, ML, AR, DR, SL, RR, KR, KL, AM, PM, WF;
} OPLL_PATCH;

typedef struct {
	OPLL_PATCH patch;

	int32_t type;

	int32_t feedback;
	int32_t output[2];

	const uint16_t *sintbl;
	uint32_t phase;
	uint32_t dphase;
	uint32_t pgout;

	int32_t fnum;
	int32_t block;
	int32_t volume;
	int32_t sustine;
	uint32_t tll;
	uint32_t rks;
	int32_t eg_mode;
	uint32_t eg_phase;
	uint32_t eg_dphase;
	uint32_t egout;
} OPLL_SLOT;

#define OPLL_MASK_CH(x) (1 << (x))

typedef struct {
	uint32_t adr;
	int32_t out;

	uint32_t realstep;
	uint32_t oplltime;
	uint32_t opllstep;
	int32_t prev, next;

	uint8_t LowFreq[6];
	uint8_t HiFreq[6];
	uint8_t InstVol[6];

	uint8_t CustInst[8];

	int32_t slot_on_flag[6 * 2];

	uint32_t pm_phase;
	int32_t lfo_pm;

	int32_t am_phase;
	int32_t lfo_am;

	uint32_t quality;

	int32_t patch_number[6];
	int32_t key_status[6];

	OPLL_SLOT slot[6 * 2];

	uint32_t mask;
} OPLL;

OPLL *OPLL_new(uint32_t clk, uint32_t rate);
void OPLL_delete(OPLL *);

void OPLL_reset(OPLL *);
void OPLL_set_rate(OPLL *opll, uint32_t r);
void OPLL_set_quality(OPLL *opll, uint32_t q);

void OPLL_writeIO(OPLL *, uint32_t reg, uint32_t val);
void OPLL_writeReg(OPLL *, uint32_t reg, uint32_t val);

int16_t OPLL_calc(OPLL *);

void OPLL_forceRefresh(OPLL *);

uint32_t OPLL_setMask(OPLL *, uint32_t mask);
uint32_t OPLL_toggleMask(OPLL *, uint32_t mask);

void OPLL_fillbuf(OPLL* opll, int32_t *buf, int32_t len, int shift);

#ifdef __cplusplus
}
#endif

#endif
