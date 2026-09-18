/*
 * melpe24_driver - a host side MELPe 2400 encoder/decoder, for checking the
 * pika 2400 wrapper against the reference.
 *
 * Upstream's own encoder.c is 1200 only and hardcoded (melpe_i(), short
 * spbuf[540], unsigned char txbuf[11]), so PROVENANCE.md's bit exactness
 * recipe cannot be pointed at 2400 as it stands. This replaces it. One source,
 * four switches, so the same file can be built against this tree or against
 * pristine upstream and the outputs compared:
 *
 *   -DVIA_PIKA_WRAPPER  call melpe_i24()/melpe_a24()/melpe_s24()  <- under test
 *   (default)           set the globals by hand and call analysis()/
 *                       synthesis() directly, exactly as the original SC1200
 *                       command line driver does at 2400      <- the reference
 *   -DWITH_NPP          run npp() over each frame first, as the reference
 *                       driver does. The firmware does not; this exists to
 *                       measure what skipping it costs.
 *   -DDECODE            7 bytes -> 180 samples instead of the reverse
 *
 * Build (against either tree):
 *   gcc -O2 -w -I<tree> -o enc <tree>/*.c tools/melpe24_driver.c -lm
 *
 * Note that the codec is a single global instance and its one shot firstTime
 * statics are never reset, so every comparison must start from a fresh
 * process. That is why this is a whole program and not a function.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc1200.h"
#include "global.h"
#include "melpe.h"

#if defined(WITH_NPP)
#include "npp.h"
#endif

#define BITS24 7 /* 54 bits, packed 8 to the byte */

#if !defined(VIA_PIKA_WRAPPER)
/* Defined in melpe.c and declared in no header, upstream included. */
extern int16_t mode;
extern int16_t bitBufSize, bitBufSize12, bitBufSize24;

/*
 * The reference wiring: what sc1200.c sets for rate 2400 before its main loop.
 * If the pika wrapper is right, building with and without VIA_PIKA_WRAPPER
 * against the same tree produces identical output.
 */
static void ref_init(void)
{
	mode = ANA_SYN;
	rate = RATE2400;
	frameSize = (int16_t) FRAME;

	chwordsize = 8;
	bitNum12 = 81;
	bitNum24 = 54;
	bitBufSize12 = 11;
	bitBufSize24 = 7;
	bitBufSize = bitBufSize24;

	melp_ana_init();
	melp_syn_init();
}
#endif

int main(int argc, char **argv)
{
	FILE *fin, *fout;
	int16_t pcm[FRAME];
	unsigned char bits[BITS24];
	unsigned long frames = 0;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <in> <out>\n", argv[0]);
		return 2;
	}
	if (!(fin = fopen(argv[1], "rb"))) {
		perror(argv[1]);
		return 1;
	}
	if (!(fout = fopen(argv[2], "wb"))) {
		perror(argv[2]);
		return 1;
	}

#if defined(VIA_PIKA_WRAPPER)
	melpe_i24();
#else
	ref_init();
#endif

#if defined(DECODE)
	while (fread(bits, 1, BITS24, fin) == BITS24) {
#if defined(VIA_PIKA_WRAPPER)
		melpe_s24(pcm, bits);
#else
		memcpy(chbuf, bits, BITS24);
		synthesis(melp_par, pcm);
#endif
		fwrite(pcm, sizeof(int16_t), FRAME, fout);
		frames++;
	}
#else
	/* A trailing partial frame is zero padded rather than dropped, so the
	   frame count is a function of the input length alone and the reference
	   and the wrapper cannot disagree about where the stream ends. */
	for (;;) {
		size_t got = fread(pcm, sizeof(int16_t), FRAME, fin);

		if (got == 0)
			break;
		if (got < FRAME)
			memset(&pcm[got], 0, (FRAME - got) * sizeof(int16_t));

#if defined(WITH_NPP)
		npp(pcm, pcm);
#endif

#if defined(VIA_PIKA_WRAPPER)
		melpe_a24(bits, pcm);
#else
		analysis(pcm, melp_par);
		memcpy(bits, chbuf, BITS24);
#endif
		fwrite(bits, 1, BITS24, fout);
		frames++;
	}
#endif

	fclose(fin);
	fclose(fout);
	fprintf(stderr, "%lu frames\n", frames);
	return 0;
}
