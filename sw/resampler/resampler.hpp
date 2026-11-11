#pragma once

/*
 * SPDX-FileCopyrightText: Korneliusz Osmenda <korneliuszo@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 */


#include <taps.hpp>
#include <stdint.h>
#include <cmath>
#include "audio/audio_fifo.h"  // for sample_pair


// Compute FIR coefficients for a 13-tap circular buffer.
// Shared by mono and stereo resamplers.
__attribute__((always_inline))
static inline void resampler_compute_fir(int16_t *fir, std::size_t fir_pos,
		int32_t &c0, int32_t &c1, int32_t &c2, int32_t &c3) {
	const int32_t* lfir = fir_coeff.data();
	int32_t lc0,lc1,lc2,lc3;
#if !defined(__ARM_ARCH_6M__)
	// Cortex-M33 (RP2350) and host builds. Same arithmetic as the Cortex-M0+
	// assembly below: 32-bit wrapping multiply-accumulate over the 13 taps,
	// oldest sample first.
	uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0;
	for (std::size_t n = 0; n < 13; ++n) {
		std::size_t i = fir_pos + n;
		if (i >= 13)
			i -= 13;
		const uint32_t s = (uint32_t)(int32_t)fir[i];
		a0 += s * (uint32_t)lfir[0];
		a1 += s * (uint32_t)lfir[1];
		a2 += s * (uint32_t)lfir[2];
		a3 += s * (uint32_t)lfir[3];
		lfir += 4;
	}
	lc0 = (int32_t)a0;
	lc1 = (int32_t)a1;
	lc2 = (int32_t)a2;
	lc3 = (int32_t)a3;
#else
	int16_t* inp = &fir[fir_pos];
	int16_t* inp2 = &fir[0];
	int16_t* fir_split = &fir[13];
	int16_t* fir_split2 = &fir[fir_pos];

	uint32_t TMP,TMP2;
	lc0=0;
	lc1=0;
	lc2=0;
	lc3=0;
	asm (
		"1:\n"
		"ldrh %[TMP2], [%[INP],#0]\n"
		"SXTH %[TMP2],%[TMP2]\n"
		"ldr %[TMP], [%[FIR],#0]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C0], %[C0], %[TMP]\n"
		"ldr %[TMP], [%[FIR],#4]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C1], %[C1], %[TMP]\n"
		"ldr %[TMP], [%[FIR],#8]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C2], %[C2], %[TMP]\n"
		"ldr %[TMP], [%[FIR],#12]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C3], %[C3], %[TMP]\n"
		"add %[FIR],%[FIR],#16\n"
		"add %[INP],%[INP],#2\n"
		"cmp %[INP],%[FIR_SPLIT]\n"
		"bne 1b\n"
		"mov %[INP],%[INP2]\n"
		"2:\n"
		"cmp %[INP],%[FIR_SPLIT2]\n"
		"beq 3f\n"
		"ldrh %[TMP2], [%[INP],#0]\n"
		"SXTH %[TMP2],%[TMP2]\n"
		"ldr %[TMP], [%[FIR],#0]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C0], %[C0], %[TMP]\n"
		"ldr %[TMP], [%[FIR],#4]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C1], %[C1], %[TMP]\n"
		"ldr %[TMP], [%[FIR],#8]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C2], %[C2], %[TMP]\n"
		"ldr %[TMP], [%[FIR],#12]\n"
		"mul %[TMP], %[TMP2], %[TMP]\n"
		"add %[C3], %[C3], %[TMP]\n"
		"add %[FIR],%[FIR],#16\n"
		"add %[INP],%[INP],#2\n"
		"b 2b\n"
		"3:\n"

	: [INP]"+l"(inp)
	, [FIR]"+l"(lfir)
	, [TMP]"+l"(TMP)
	, [TMP2]"+l"(TMP2)
	, [C0]"+r"(lc0)
	, [C1]"+r"(lc1)
	, [C2]"+r"(lc2)
	, [C3]"+r"(lc3)
	, [FIR_SPLIT]"+r"(fir_split)
	, [FIR_SPLIT2]"+r"(fir_split2)
	, [INP2]"+r"(inp2)
	);
#endif
	c0=lc0; // maxsignal 2.30 tap 2.
	c1=lc1>>(30-14); // tap 2.30 -> 2.14
	c2=lc2>>15; // tap 1.30 -> 1.15
	c3=lc3>>15; // tap 1.30 -> 1.15
}

// Interpolate output from precomputed FIR coefficients and phase
__attribute__((always_inline))
static inline int16_t resampler_interpolate(int32_t c0, int32_t c1, int32_t c2, int32_t c3, int64_t phase) {
	int32_t lphase = phase>>16; //-1.16
	int32_t val = (c0 +
			((lphase*(c1 +
					((lphase*(c2 +
							((lphase*(c3))>>(16+15-15)) //1.15
							))>>(16+15-14)) //2.14
							))>>(16+14-30)) //2.30
							);
	return val>>16;
}


template<int16_t (*IN_FN)()>
class Resampler {
	int64_t phase; //in 31.32
	uint64_t ratio;
	std::size_t fir_pos;
	int16_t fir[13];
	int32_t c0,c1,c2,c3; //in 0.15
public:
	void set_ratio(uint32_t in, uint32_t out)
	{
		uint64_t val = ((uint64_t)in)<<32;
		ratio = val/out;
	}
	int16_t get_sample()
	{
		phase+=ratio;
		while(phase>=1UL<<31) //0.5
		{
			fir[fir_pos]=IN_FN(); //0.15
			fir_pos++;
			phase-=1ULL<<32;
			if(fir_pos>=13)
				fir_pos=0;
			resampler_compute_fir(fir, fir_pos, c0, c1, c2, c3);
			//broken downsampling
			return resampler_interpolate(c0, c1, c2, c3, phase);
		};
	}
};


template<sample_pair (*IN_FN)()>
class StereoResampler {
	int64_t phase; //in 31.32
	uint64_t ratio;
	std::size_t fir_pos;
	int16_t fir_l[13];
	int16_t fir_r[13];
	int32_t c0_l,c1_l,c2_l,c3_l;
	int32_t c0_r,c1_r,c2_r,c3_r;
public:
	void set_ratio(uint32_t in, uint32_t out)
	{
		uint64_t val = ((uint64_t)in)<<32;
		ratio = val/out;
	}
	sample_pair get_sample()
	{
		phase+=ratio;
		bool recalculate_fir = false;
		while(phase>=1UL<<31) //0.5
		{
			sample_pair in = IN_FN();
			fir_l[fir_pos]=in.data16[0];
			fir_r[fir_pos]=in.data16[1];
			fir_pos++;
			phase-=1ULL<<32;
			if(fir_pos>=13)
				fir_pos=0;
			recalculate_fir = true;
		}
		if(recalculate_fir){
			resampler_compute_fir(fir_l, fir_pos, c0_l, c1_l, c2_l, c3_l);
			resampler_compute_fir(fir_r, fir_pos, c0_r, c1_r, c2_r, c3_r);
		};
		sample_pair out;
		out.data16[0] = resampler_interpolate(c0_l, c1_l, c2_l, c3_l, phase);
		out.data16[1] = resampler_interpolate(c0_r, c1_r, c2_r, c3_r, phase);
		return out;
	}
};
