/* Mednafen - Multi-system Emulator
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

///* This resampler has only been designed for frequencies 1.5MHz - 2MHz as the input rate, and output rates of
//   22050-192000 in mind, though preferably output rates between 48000 to 96000(inclusive) will be used.
//*/

#include "../mednafen.h"
#include <math.h>
#include <limits.h>
#include <algorithm>

#include <libretro.h>

#include "OwlResampler.h"

extern retro_get_cpu_features_t perf_get_cpu_features_cb;

#ifndef M_PI
#define M_PI 3.1415926535
#endif

#if defined(ARCH_POWERPC_ALTIVEC) && defined(HAVE_ALTIVEC_H)
 #include <altivec.h>
#endif

//#ifdef __FAST_MATH__
// #error "OwlResampler.cpp not compatible with unsafe math optimizations!"
//#endif

OwlBuffer::OwlBuffer()
{
 memset(HRBuf, 0, sizeof(HRBuf));

 accum = 0;
 filter_state[0] = 0;
 filter_state[1] = 0;

 leftover = 0;

 InputIndex = 0;
 InputPhase = 0;

 debias = 0;
}


OwlBuffer::~OwlBuffer()
{



}

template<unsigned DoExMix, bool Integrate, unsigned IntegrateShift, bool Lowpass, bool Highpass, bool FloatOutput>
static int32 ProcessLoop(unsigned count, int32 a, int32* b, int32* exmix0 = NULL, int32* exmix1 = NULL, unsigned lp_shift = 0, unsigned hp_shift = 0, int64* f_in = NULL)
{
 int64 lp_f;
 int64 hp_f;

 if(Lowpass)
 {
  lp_f = f_in[0];
 }

 if(Highpass)
 {
  hp_f = f_in[1];
 }

 while(count--)
 {
  int32 tmp;

  if(Integrate)
  {
   a += *b;
   tmp = a >> IntegrateShift;
  }
  else
   tmp = *b;

  if(Lowpass)
  {
   lp_f += (((int64)tmp << 16) - lp_f) >> lp_shift;
   tmp = lp_f >> 16;
  }

  if(Highpass)
  {
   hp_f += (((int64)tmp << 16) - hp_f) >> hp_shift;
   tmp = tmp - (hp_f >> 16);
  }

  if(DoExMix >= 1)
  {
   tmp += *exmix0;
   exmix0++;
  }

  if(DoExMix >= 2)
  {
   tmp += *exmix1;
   exmix1++;
  }

  if(FloatOutput)
   *(float*)b = tmp;
  else
   *b = tmp;

  b++;
 }

 if(Lowpass)
  f_in[0] = lp_f;

 if(Highpass)
  f_in[1] = hp_f;

 return(a);
}

void OwlBuffer::ResampleSkipped(unsigned count)
{
 memmove(HRBuf, &HRBuf[count], HRBUF_OVERFLOW_PADDING * sizeof(HRBuf[0]));
 memset(&HRBuf[HRBUF_OVERFLOW_PADDING], 0, count * sizeof(HRBuf[0]));
}

void OwlBuffer::Integrate(unsigned count, unsigned lp_shift, unsigned hp_shift, RavenBuffer* mixin0, RavenBuffer* mixin1)
{
 //lp_shift = hp_shift = 0;
 if(lp_shift != 0 || hp_shift != 0)
 {
  if(mixin0 && mixin1)
   accum = ProcessLoop<2, true, 3, true, true, true>(count, accum, Buf(), mixin0->Buf(), mixin1->Buf(),	lp_shift, hp_shift, filter_state);
  else if(mixin0)
   accum = ProcessLoop<1, true, 3, true, true, true>(count, accum, Buf(), mixin0->Buf(), NULL,		lp_shift, hp_shift, filter_state);
  else
   accum = ProcessLoop<0, true, 3, true, true, true>(count, accum, Buf(), NULL, 	 NULL,		lp_shift, hp_shift, filter_state);
 }
 else
 {
  if(mixin0 && mixin1)
   accum = ProcessLoop<2, true, 3, false, false, true>(count, accum, Buf(), mixin0->Buf(), mixin1->Buf());
  else if(mixin0)
   accum = ProcessLoop<1, true, 3, false, false, true>(count, accum, Buf(), mixin0->Buf());
  else
   accum = ProcessLoop<0, true, 3, false, false, true>(count, accum, Buf());
 }
}

RavenBuffer::RavenBuffer()
{
 memset(BB, 0, sizeof(BB));

 accum = 0;

 filter_state[0] = 0;
 filter_state[1] = 0;
}


RavenBuffer::~RavenBuffer()
{



}



void RavenBuffer::Process(unsigned count, bool integrate, uint32 lp_shift)
{
 if(integrate)
 {
  if(lp_shift != 0)
   accum = ProcessLoop<0, true, 3, true,  false, false>(count, accum, Buf(), NULL, NULL, lp_shift, 0, filter_state);
  else
   accum = ProcessLoop<0, true, 3, false, false, false>(count, accum, Buf(), NULL, NULL, lp_shift, 0);
 }
 else
 {
  if(lp_shift != 0)
   accum = ProcessLoop<0, false, 0, true,  false, false>(count, accum, Buf(), NULL, NULL, lp_shift, 0, filter_state);
  else
   accum = ProcessLoop<0, false, 0, false, false, false>(count, accum, Buf(), NULL, NULL, lp_shift, 0);
 }
}

void RavenBuffer::Finish(unsigned count)
{
 memmove(BB, &BB[count], OwlBuffer::HRBUF_OVERFLOW_PADDING * sizeof(BB[0]));
 memset(&BB[OwlBuffer::HRBUF_OVERFLOW_PADDING], 0, count * sizeof(BB[0]));
}



static void kaiser_window( double* io, int count, double beta )
{
        int const accuracy = 16; //12;

        double* end = io + count;

        double beta2    = beta * beta * (double) -0.25;
        double to_fract = beta2 / ((double) count * count);
        double i        = 0;
        double rescale = 0; // Doesn't need an initializer, to shut up gcc

        for ( ; io < end; ++io, i += 1 )
        {
                double x = i * i * to_fract - beta2;
                double u = x;
                double k = x + 1;

                double n = 2;
                do
                {
                        u *= x / (n * n);
                        n += 1;
                        k += u;
                }
                while ( k <= u * (1 << accuracy) );

                if ( !i )
                        rescale = 1 / k; // otherwise values get large

                *io *= k * rescale;
        }
}

static void gen_sinc( double* out, int size, double cutoff, double kaiser )
{
	int const half_size = size / 2;
	double* const mid = &out [half_size];
 
	// Generate right half of sinc
	for ( int i = 0; i < half_size; i++ )
	{
		double angle = (i * 2 + 1) * (M_PI / 2);
		mid [i] = sin( angle * cutoff ) / angle;
	}
 
	kaiser_window( mid, half_size, kaiser );
 
	// Mirror for left half
	for ( int i = 0; i < half_size; i++ )
		out [i] = mid [half_size - 1 - i];
}
 
static void normalize( double* io, int size, double gain = 1.0 )
{
	double sum = 0;
	for ( int i = 0; i < size; i++ )
		sum += io [i];

	double scale = gain / sum;
	for ( int i = 0; i < size; i++ )
		io [i] *= scale;
}


static INLINE void DoMAC(float *wave, float *coeffs, int32 count, int32 *accum_output)
{
 float acc[4] = { 0, 0, 0, 0 };

 for(int c = 0; c < count; c += 4)
 {
  acc[0] += wave[c + 0] * coeffs[c + 0];
  acc[1] += wave[c + 1] * coeffs[c + 1];
  acc[2] += wave[c + 2] * coeffs[c + 2];
  acc[3] += wave[c + 3] * coeffs[c + 3];
 }

 *accum_output = (acc[0] + acc[2]) + (acc[1] + acc[3]);
}

#if defined(ARCH_X86)

#if defined(__x86_64__) && !defined(__ILP32__)
#define X86_REGC "r"
#define X86_REGAT ""
#else
#define X86_REGC "e"
#define X86_REGAT "l"
#endif

static INLINE void DoMAC_SSE(float *wave, float *coeffs, int32 count, int32 *accum_output)
{
 // Multiplies 16 coefficients at a time.
 int dummy;

/*
	?di = wave pointer
	?si = coeffs pointer
	ecx = count / 16
	edx = 32-bit int output pointer

	
*/
 // Will read 16 bytes of input waveform past end.
 asm volatile(
"xorps %%xmm3, %%xmm3\n\t"	// For a loop optimization

"xorps %%xmm4, %%xmm4\n\t"
"xorps %%xmm5, %%xmm5\n\t"
"xorps %%xmm6, %%xmm6\n\t"
"xorps %%xmm7, %%xmm7\n\t"

"movups  0(%%" X86_REGC "di), %%xmm0\n\t"
"SSE_Loop:\n\t"

"movups 16(%%" X86_REGC "di), %%xmm1\n\t"
"mulps   0(%%" X86_REGC "si), %%xmm0\n\t"
"addps  %%xmm3, %%xmm7\n\t"

"movups 32(%%" X86_REGC "di), %%xmm2\n\t"
"mulps  16(%%" X86_REGC "si), %%xmm1\n\t"
"addps  %%xmm0, %%xmm4\n\t"

"movups 48(%%" X86_REGC "di), %%xmm3\n\t"
"mulps  32(%%" X86_REGC "si), %%xmm2\n\t"
"addps  %%xmm1, %%xmm5\n\t"

"movups 64(%%" X86_REGC "di), %%xmm0\n\t"
"mulps  48(%%" X86_REGC "si), %%xmm3\n\t"
"addps  %%xmm2, %%xmm6\n\t"

"add" X86_REGAT " $64, %%" X86_REGC "si\n\t"
"add" X86_REGAT " $64, %%" X86_REGC "di\n\t"
"subl $1, %%ecx\n\t"
"jnz SSE_Loop\n\t"

"addps  %%xmm3, %%xmm7\n\t"	// For a loop optimization

//
// Add the four summation xmm regs together into one xmm register, xmm7
//
"addps  %%xmm4, %%xmm5\n\t"
"addps  %%xmm6, %%xmm7\n\t"
"addps  %%xmm5, %%xmm7\n\t"

//
// Now for the "fun" horizontal addition...
//
// 
"movaps %%xmm7, %%xmm4\n\t"
// (3 * 2^0) + (2 * 2^2) + (1 * 2^4) + (0 * 2^6) = 27
"shufps $27, %%xmm7, %%xmm4\n\t"
"addps  %%xmm4, %%xmm7\n\t"

// At this point, xmm7:
// (3 + 0), (2 + 1), (1 + 2), (0 + 3)
//
// (1 * 2^0) + (0 * 2^2) = 1
"movaps %%xmm7, %%xmm4\n\t"
"shufps $1, %%xmm7, %%xmm4\n\t"
"addss %%xmm4, %%xmm7\n\t"	// No sense in doing packed addition here.

"cvtss2si %%xmm7, %%ecx\n\t"
"movl %%ecx, (%%" X86_REGC "dx)\n\t"
 : "=D" (dummy), "=S" (dummy), "=c" (dummy)
 : "D" (wave), "S" (coeffs), "c" (count >> 4), "d" (accum_output)
#ifdef __SSE__
 : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "cc", "memory"
#else
 : "cc", "memory"
#endif
);
}
#elif defined(ARCH_POWERPC_ALTIVEC)
static INLINE void DoMAC_AltiVec(float* wave, float* coeffs, int32 count, int32* accum_output)
{
 register vector float acc0, acc1, acc2, acc3;

 acc0 = (vector float)vec_splat_u8(0);
 acc1 = acc0;
 acc2 = acc0;
 acc3 = acc0;


 count >>= 4;

 if(!((uint64)wave & 0xF))
 {
  register vector float w, c;
  do
  {
   w = vec_ld(0, wave);
   c = vec_ld(0, coeffs);
   acc0 = vec_madd(w, c, acc0);

   w = vec_ld(16, wave);
   c = vec_ld(16, coeffs);
   acc1 = vec_madd(w, c, acc1);

   w = vec_ld(32, wave);
   c = vec_ld(32, coeffs);
   acc2 = vec_madd(w, c, acc2);

   w = vec_ld(48, wave);
   c = vec_ld(48, coeffs);
   acc3 = vec_madd(w, c, acc3);

   coeffs += 16;
   wave += 16;
  } while(--count);
 }
 else
 {
  register vector unsigned char lperm;
  register vector float loado;

  lperm = vec_lvsl(0, wave);
  loado = vec_ld(0, wave);

  do
  {
   register vector float tl;
   register vector float w;
   register vector float c;

   tl = vec_ld(15 + 0, wave);
   w = vec_perm(loado, tl, lperm);
   c = vec_ld(0, coeffs);
   loado = tl;
   acc0 = vec_madd(w, c, acc0);

   tl = vec_ld(15 + 16, wave);
   w = vec_perm(loado, tl, lperm);
   c = vec_ld(16, coeffs);
   loado = tl;
   acc1 = vec_madd(w, c, acc1);

   tl = vec_ld(15 + 32, wave);
   w = vec_perm(loado, tl, lperm);
   c = vec_ld(32, coeffs);
   loado = tl;
   acc2 = vec_madd(w, c, acc2);

   tl = vec_ld(15 + 48, wave);
   w = vec_perm(loado, tl, lperm);
   c = vec_ld(48, coeffs);
   loado = tl;
   acc3 = vec_madd(w, c, acc3);

   coeffs += 16;
   wave += 16;
  } while(--count);
 }

 {
  vector float sum;
  vector float sums0;
  vector signed int sum_i;

  sum = vec_add(vec_add(acc0, acc1), vec_add(acc2, acc3));
  sums0 = vec_sld(sum, sum, 8);
  sum = vec_add(sum, sums0);
  sums0 = vec_sld(sum, sum, 4);
  sum = vec_add(sum, sums0);

  sum_i = vec_cts(sum, 0);
  vec_ste(sum_i, 0, accum_output);
 }
}
#endif

template<typename T, unsigned sa>
static T SDP2(T v)
{
 T tmp;

 tmp = (v >> ((sizeof(T) * 8) - 1)) & (((T)1 << sa) - 1);
 
 return ((v + tmp) >> sa);
}

int32 OwlResampler::Resample(OwlBuffer* in, const uint32 in_count, int16* out, const uint32 max_out_count)
{
	uint32 count = 0;
	int32 *boobuf = &IntermediateBuffer[0];
	int32 *I32Out = boobuf;
	const uint32 in_count_WLO = in->leftover + in_count;
	const uint32 max = std::max<int64>(0, (int64)in_count_WLO - NumCoeffs);
        uint32 InputPhase = in->InputPhase;
        uint32 InputIndex = in->InputIndex;
	OwlBuffer::I32_F_Pudding* InSamps = in->BufPudding() - in->leftover;
	int32 leftover;

   while(InputIndex < max)
   {
      bool handled      = false;
      float* wave       = &InSamps[InputIndex].f;
      float* coeffs     = &FIR_Coeffs[InputPhase][0].f;
      int32 coeff_count = NumCoeffs;

#ifdef ARCH_X86
      if(cpuext & RETRO_SIMD_SSE2)
      {
         DoMAC_SSE(wave, coeffs, coeff_count, I32Out);
         handled = true;
      }
#elif defined(ARCH_POWERPC_ALTIVEC)
      {
         DoMAC_AltiVec(wave, coeffs, coeff_count, I32Out);
         handled = true;
      }
#endif

      if (!handled)
         DoMAC(wave, coeffs, coeff_count, I32Out);;

      I32Out++;
      count++;

      InputPhase = PhaseNext[InputPhase];
      InputIndex += PhaseStep[InputPhase];
   }

   if(InputIndex > in_count_WLO)
   {
      leftover = 0;
      InputIndex -= in_count_WLO;
   }
   else
   {
      leftover = (int32)in_count_WLO - (int32)InputIndex;
      InputIndex = 0;
   }

   {
      int64 debias = in->debias;

      for(uint32 x = 0; x < count; x++)
      {
         int32 sample = boobuf[x];
         int32 s;

         debias += ((((int64)sample << 16) - debias) * debias_multiplier) >> 16;
         s = SDP2<int32, 8>(sample - (debias >> 16));

         if(s < -32768 || s > 32767)
         {
            if(s < -32768)
               s = -32768;
            else if(s > 32767)
               s = 32767;
         }
         out[x * 2] = s;
      }

      in->debias = debias;
   }

   memmove(in->Buf() - leftover,
         in->Buf() + in_count - leftover,
         sizeof(int32) * (leftover + OwlBuffer::HRBUF_OVERFLOW_PADDING));

   memset(in->Buf() + OwlBuffer::HRBUF_OVERFLOW_PADDING, 0, sizeof(int32) * in_count);

	in->leftover = leftover;
	in->InputPhase = InputPhase;
	in->InputIndex = InputIndex;

	return(count);
}

void OwlResampler::ResetBufResampState(OwlBuffer* buf)
{
 memset(buf->HRBuf, 0, sizeof(buf->HRBuf[0]) * OwlBuffer::HRBUF_LEFTOVER_PADDING);
 buf->InputPhase = 0;
}


OwlResampler::~OwlResampler()
{
 if(PhaseNext)
  free(PhaseNext);

 if(PhaseStep)
  free(PhaseStep);

 if(PhaseStepSave)
  free(PhaseStepSave);

 if(FIR_Coeffs_Real)
 {
  for(unsigned int i = 0; i < NumPhases; i++)
   if(FIR_Coeffs_Real[i])
    free(FIR_Coeffs_Real[i]);

  free(FIR_Coeffs_Real);
 }

 if(FIR_Coeffs)
  free(FIR_Coeffs);
}

//
// Flush denormals, and coefficients that could lead to denormals, to zero.
//
static float FilterDenormal(float v)
{
 union
 {
  float f;
  uint32 i;
 } cat_pun;

 cat_pun.f = v;

 if(((cat_pun.i >> 23) & 0xFF) <= 24)	// Maybe < 24 is more correct?
  return(0);
 return(v);
}

OwlResampler::OwlResampler(double input_rate, double output_rate, double rate_error, double debias_corner, int quality)
{
 double *FilterBuf = NULL;
 double cutoff;
 double required_bandwidth;
 double k_beta;
 double k_d;

 InputRate = input_rate;
 OutputRate = output_rate;
 RateError = rate_error;
 DebiasCorner = debias_corner;
 Quality = quality;

 IntermediateBuffer.resize(OutputRate * 4 / 50);	// *4 for safety padding, / min(50,60), an approximate calculation

 cpuext = 0;
 if (perf_get_cpu_features_cb)
    cpuext = perf_get_cpu_features_cb();

 // Get the number of phases required, and adjust ratio.
 {
  double s_ratio = (double)input_rate / output_rate;
  double findo = 0;
  uint32 count = 0;
  uint32 findo_i;

  do
  {
   count++;
   findo += s_ratio;
  } while( fabs(1.0 - ((floor(0.5 + findo) / count) / s_ratio)) > rate_error);

  s_ratio = floor(0.5 + findo) / count;
  findo_i = (uint32) floor(0.5 + findo);
  NumPhases = count;

  PhaseNext = (uint32 *)malloc(sizeof(uint32) * NumPhases);
  PhaseStep = (uint32 *)malloc(sizeof(uint32) * NumPhases);
  PhaseStepSave = (uint32 *)malloc(sizeof(uint32) * NumPhases);

  uint32 last_indoo = 0;
  for(unsigned int i = 0; i < NumPhases; i++)
  {
   uint32 index_pos = i * findo_i / NumPhases;

   PhaseNext[i] = (i + 1) % (NumPhases);
   PhaseStepSave[i] = PhaseStep[i] = index_pos - last_indoo;
   last_indoo = index_pos;
  }
  PhaseStepSave[0] = PhaseStep[0] = findo_i - last_indoo;

  Ratio_Dividend = findo_i;
  Ratio_Divisor = NumPhases;
 }

 static const struct
 {
  double beta;
  double d;
  double obw;
 } QualityTable[7] =
 {
  {  5.658, 3.62,  0.65 },
  {  6.764, 4.32,  0.70 },
  {  7.865, 5.00,  0.75 },
  {  8.960, 5.70,  0.80 },
  { 10.056, 6.40,  0.85 },
  { 10.056, 6.40,  0.90 },

  { 10.056, 6.40,  0.9333 }, // 1.0 - (6.40 / 96)
 };

 k_beta = QualityTable[quality].beta;
 k_d = QualityTable[quality].d;


 //
 // As far as filter frequency response design goes, we clamp the output rate parameter
 // to keep PCE CD and PC-FX CD-DA sample amplitudes from going wild since we're not resampling CD-DA totally properly.
 //
#define OWLRESAMP_FCALC_RATE_CLAMP 128000.0 //192000.0 //96000.0 //48000.0

 // A little SOMETHING to widen the transition band a bit to reduce computational complexity with higher output rates.
 const double something = std::min<double>(OWLRESAMP_FCALC_RATE_CLAMP, (48000.0 + std::min<double>(OWLRESAMP_FCALC_RATE_CLAMP, output_rate)) / 2 / QualityTable[quality].obw);

 //
 // Note: Cutoff calculation is performed again(though slightly differently) down below after the SIMD check.
 //
 cutoff = QualityTable[quality].obw * (std::min<double>(something, std::min<double>(input_rate, output_rate)) / input_rate);

 required_bandwidth = (std::min<double>(OWLRESAMP_FCALC_RATE_CLAMP, std::min<double>(input_rate, output_rate)) / input_rate) - cutoff;

 NumCoeffs = ceil(k_d / required_bandwidth);

 //
 // Put this lower limit BEFORE the SIMD stuff, otherwise the NumCoeffs_Padded calculation will be off.
 //
 if(NumCoeffs < 16)
  NumCoeffs = 16;

 if(0)
 {
  abort();	// The sky is falling AAAAAAAAAAAAA
 }
 #ifdef ARCH_X86
 else if(cpuext & RETRO_SIMD_SSE2)
 {

  // SSE loop does 16 MACs per iteration.
  NumCoeffs = (NumCoeffs + 15) &~ 15;
  NumCoeffs_Padded = NumCoeffs;
 }
 #endif
 #ifdef ARCH_POWERPC_ALTIVEC
 else if(1)
 {
  // AltiVec loop does 16 MACs per iteration.
  NumCoeffs = (NumCoeffs + 15) &~ 15;
  NumCoeffs_Padded = NumCoeffs;
 }
 #endif
 else
 {
  // Default loop does 4 MACs per iteration.
  NumCoeffs = (NumCoeffs + 3) &~ 3;
  NumCoeffs_Padded = NumCoeffs;
 }

 // Adjust cutoff now that NumCoeffs may have been increased.
 cutoff = std::min<double>(QualityTable[quality].obw * something / input_rate, (std::min<double>(input_rate, output_rate) / input_rate - ((double)k_d / NumCoeffs)));

 FIR_Coeffs = (OwlBuffer::I32_F_Pudding **)malloc(sizeof(int32 **) * NumPhases);
 FIR_Coeffs_Real = (OwlBuffer::I32_F_Pudding **)malloc(sizeof(int32 **) * NumPhases);

 for(unsigned int i = 0; i < NumPhases; i++)
 {
  uint8 *tmp_ptr = (uint8 *)calloc(sizeof(int32) * NumCoeffs_Padded + 16, 1);

  FIR_Coeffs_Real[i] = (OwlBuffer::I32_F_Pudding *)tmp_ptr;
  tmp_ptr += 0xF;
  tmp_ptr -= ((unsigned long long)tmp_ptr & 0xF);
  FIR_Coeffs[i] = (OwlBuffer::I32_F_Pudding *)tmp_ptr;
 }

 FilterBuf = (double *)malloc(sizeof(double) * NumCoeffs * NumPhases);
 gen_sinc(FilterBuf, NumCoeffs * NumPhases, cutoff / NumPhases, k_beta);
 normalize(FilterBuf, NumCoeffs * NumPhases); 

 for(unsigned int phase = 0; phase < NumPhases; phase++)
 {
  double sum_d = 0;
  float sum_f4[4] = { 0, 0, 0, 0 };

  const unsigned sp = (NumPhases - 1 - (((uint64)phase * Ratio_Dividend) % NumPhases));
  const unsigned tp = phase;

  for(unsigned int i = 0; i < NumCoeffs; i++)
  {
   double tmpcod = FilterBuf[i * NumPhases + sp] * NumPhases;	// Tasty cod.

   FIR_Coeffs[tp][i].f = FilterDenormal(tmpcod);
   sum_d += FIR_Coeffs[tp][i].f;
   sum_f4[i % 4] += FIR_Coeffs[tp][i].f;
  }
 }

 free(FilterBuf);
 FilterBuf = NULL;

 debias_multiplier = (uint32)(((uint64)1 << 16) * debias_corner / output_rate);
}
