/* vim: set tabstop=4:softtabstop=4:shiftwidth=4:noexpandtab */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __MELPELIB__
#define  __MELPELIB__
//------------NPP----------------------
	void melpe_n(short *sp);	//denoise 180 samples sp->sp
//------------1200---------------------
	void melpe_i(void);	//init melpe codec at 1200 bps
	void melpe_a(unsigned char *buf, short *sp);	//compress 540 samples sp -> 81 bits buf  
	void melpe_s(short *sp, unsigned char *buf);	//decompress 81 bits buf -> 540 samples sp
//------------2400---------------------
/* pika: patch 6, not upstream. No npp() on this path, and melpe_a24() leaves
   sp untouched. See melpe.c and PROVENANCE.md. */
	void melpe_i24(void);	//init melpe codec at 2400 bps
	void melpe_a24(unsigned char *buf, short *sp);	//compress 180 samples sp -> 54 bits buf
	void melpe_s24(short *sp, unsigned char *buf);	//decompress 54 bits buf -> 180 samples sp

#endif
#ifdef __cplusplus
}
#endif
