#ifndef __FCEU_GIT
#define __FCEU_GIT

#define GIT_CART  0
#define GIT_VSUNI 1
#define GIT_FDS   2
#define GIT_NSF   3

#define GIV_NTSC  0
#define GIV_PAL   1
#define GIV_USER  2

typedef struct {
	uint8_t *name;

	int type;
	int vidsys;
	int input[2];
	int inputfc;
	int cspecial;
	uint8_t MD5[16];
	int soundrate;
	int soundchan;
	int gameid;
} FCEUGI;

#endif
