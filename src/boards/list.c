/* Copyright (c) 2026 Magnominus
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "board.h"

// Boards by iNES/NES 2.0 mapper number

const struct Board* boardFind(int mapper, int submapper) {
	switch (mapper) {
	case 0:
		return &boardNrom;
	case 1:
		return &boardMmc1;
	case 2:
		return &boardUxrom;
	case 3:
		return &boardCnrom;
	case 4:
		return &boardMmc3;
	case 7:
		return &boardAxrom;
	case 9:
	case 10:
		return &boardMmc2;
	case 18:
		return &boardSs88006;
	case 19:
		return &boardNamco163;
	case 21:
	case 25:
		return submapper == 3 ? &boardVrc2 : &boardVrc24;
	case 22:
		return &boardVrc2;
	case 23:
		return submapper == 3 ? &boardVrc2 : &boardVrc24;
	case 33:
		return &boardTc0190;
	case 35:
	case 90:
	case 209:
	case 211:
		return &boardJy;
	case 69:
		return &boardFme7;
	case 76:
		return &boardNamco3446;
	case 88:
		return &boardNamco3433;
	case 95:
		return &boardNamco3425;
	case 118:
		return &boardTxsrom;
	case 119:
		return &boardTqrom;
	case 176:
		return &boardFk23c;
	case 268:
		return &boardCoolboy;
	case 137:
	case 138:
	case 139:
	case 141:
		return &boardS8259;
	case 133:
	case 145:
	case 148:
	case 149:
		return &boardSachenLatch;
	case 143:
		return &boardTca01;
	case 150:
	case 243:
		return &boardS74ls374n;
	case 154:
		return &boardNamco3453;
	case 206:
		return &boardNamco108;
	case 210:
		return &boardNamco175;
	default:
		return discreteFind(mapper, submapper);
	}
}
