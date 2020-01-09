/*
  Copyright (c) 2013 Julien Pommier.

  Small test & bench for PFFFT, comparing its performance with the scalar FFTPACK, FFTW, and Apple vDSP

  How to build: 

  on linux, with fftw3:
  gcc -o test_pffft -DHAVE_FFTW -msse -mfpmath=sse -O3 -Wall -W pffft.c test_pffft.c fftpack.c -L/usr/local/lib -I/usr/local/include/ -lfftw3f -lm

  on macos, without fftw3:
  clang -o test_pffft -DHAVE_VECLIB -O3 -Wall -W pffft.c test_pffft.c fftpack.c -L/usr/local/lib -I/usr/local/include/ -framework Accelerate

  on macos, with fftw3:
  clang -o test_pffft -DHAVE_FFTW -DHAVE_VECLIB -O3 -Wall -W pffft.c test_pffft.c fftpack.c -L/usr/local/lib -I/usr/local/include/ -lfftw3f -framework Accelerate

  as alternative: replace clang by gcc.

  on windows, with visual c++:
  cl /Ox -D_USE_MATH_DEFINES /arch:SSE test_pffft.c pffft.c fftpack.c
  
  build without SIMD instructions:
  gcc -o test_pffft -DPFFFT_SIMD_DISABLE -O3 -Wall -W pffft.c test_pffft.c fftpack.c -lm

 */

#include "pffft.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <string.h>


/* EXPECTED_DYN_RANGE in dB:
 * single precision float has 24 bits mantissa
 * => 24 Bits * 6 dB = 144 dB
 * allow a few dB tolerance (even 144 dB looks good on my PC)
 */
#define EXPECTED_DYN_RANGE  140.0

/* maximum allowed phase error in degree */
#define DEG_ERR_LIMIT   1E-4

/* maximum allowed magnitude error in amplitude (of 1.0 or 1.1) */
#define MAG_ERR_LIMIT  1E-6


#define PRINT_SPEC  0

#define PWR2LOG(PWR)  ( (PWR) < 1E-30 ? 10.0*log10(1E-30) : 10.0*log10(PWR) )


int isPowerOfTwo(unsigned v) {
  /* https://graphics.stanford.edu/~seander/bithacks.html#DetermineIfPowerOf2 */
  int f = v && !(v & (v - 1));
  return f;
}


int test(int N, int cplx, int useOrdered) {
  int Nfloat = (cplx ? N*2 : N);
  float *X = pffft_aligned_malloc((unsigned)Nfloat * sizeof(float));
  float *Y = pffft_aligned_malloc((unsigned)Nfloat * sizeof(float));
  float *Z = pffft_aligned_malloc((unsigned)Nfloat * sizeof(float));
  float *W = pffft_aligned_malloc((unsigned)Nfloat * sizeof(float));
  int k, j, m, iter, kmaxOther, retError = 0;
  double freq, dPhi, phi, phi0;
  double pwr, pwrCar, pwrOther, err, errSum, mag, expextedMag;
  float amp = 1.0F;

  assert( isPowerOfTwo(N) );

  PFFFT_Setup *s = pffft_new_setup(N, cplx ? PFFFT_COMPLEX : PFFFT_REAL);
  assert(s);
  if (!s) {
    printf("Error setting up PFFFT!\n");
    return 1;
  }

  for ( k = m = 0; k < (cplx? N : (1 + N/2) ); k += N/16, ++m )
  {
    amp = ( (m % 3) == 0 ) ? 1.0F : 1.1F;
    freq = (k < N/2) ? ((double)k / N) : ((double)(k-N) / N);
    dPhi = 2.0 * M_PI * freq;
    if ( dPhi < 0.0 )
      dPhi += 2.0 * M_PI;

    iter = -1;
    while (1)
    {
      ++iter;

      if (iter)
        printf("bin %d: dphi = %f for freq %f\n", k, dPhi, freq);

      /* generate cosine carrier as time signal - start at defined phase phi0 */
      phi = phi0 = (m % 4) * 0.125 * M_PI;  /* have phi0 < 90 deg to be normalized */
      for ( j = 0; j < N; ++j )
      {
        if (cplx) {
          X[2*j] = amp * (float)cos(phi);  /* real part */
          X[2*j+1] = amp * (float)sin(phi);  /* imag part */
        }
        else
          X[j] = amp * (float)cos(phi);  /* only real part */

        /* phase increment .. stay normalized - cos()/sin() might degrade! */
        phi += dPhi;
        if ( phi >= M_PI )
          phi -= 2.0 * M_PI;
      }

      /* forward transform from X --> Y  .. using work buffer W */
      if ( useOrdered )
        pffft_transform_ordered(s, X, Y, W, PFFFT_FORWARD );
      else
      {
        pffft_transform(s, X, Z, W, PFFFT_FORWARD );  /* temporarily use Z for reordering */
        pffft_zreorder(s, Z, Y, PFFFT_FORWARD );
      }

      pwrOther = -1.0;
      pwrCar = 0;


      /* for positive frequencies: 0 to 0.5 * samplerate */
      /* and also for negative frequencies: -0.5 * samplerate to 0 */
      for ( j = 0; j < ( cplx ? N : (1 + N/2) ); ++j )
      {
        if (!cplx && !j)  /* special treatment for DC for real input */
          pwr = Y[j]*Y[j];
        else if (!cplx && j == N/2)  /* treat 0.5 * samplerate */
          pwr = Y[1] * Y[1];  /* despite j (for freq calculation) we have index 1 */
        else
          pwr = Y[2*j] * Y[2*j] + Y[2*j+1] * Y[2*j+1];
        if (iter || PRINT_SPEC)
          printf("%s fft %d:  pwr[j = %d] = %g == %f dB\n", (cplx ? "cplx":"real"), N, j, pwr, PWR2LOG(pwr) );
        if (k == j)
          pwrCar = pwr;
        else if ( pwr > pwrOther ) {
          pwrOther = pwr;
          kmaxOther = j;
        }
      }

      if ( PWR2LOG(pwrCar) - PWR2LOG(pwrOther) < EXPECTED_DYN_RANGE ) {
        printf("%s fft %d amp %f iter %d:\n", (cplx ? "cplx":"real"), N, amp, iter);
        printf("  carrier power  at bin %d: %g == %f dB\n", k, pwrCar, PWR2LOG(pwrCar) );
        printf("  carrier mag || at bin %d: %g\n", k, sqrt(pwrCar) );
        printf("  max other pwr  at bin %d: %g == %f dB\n", kmaxOther, pwrOther, PWR2LOG(pwrOther) );
        printf("  dynamic range: %f dB\n\n", PWR2LOG(pwrCar) - PWR2LOG(pwrOther) );
        retError = 1;
        if ( iter == 0 )
          continue;
      }

      if ( k > 0 && k != N/2 )
      {
        phi = atan2( Y[2*k+1], Y[2*k] );
        if ( fabs( phi - phi0) > DEG_ERR_LIMIT * M_PI / 180.0 )
        {
        retError = 1;
        printf("%s fft %d  bin %d amp %f : phase mismatch! phase = %f deg   expected = %f deg\n",
            (cplx ? "cplx":"real"), N, k, amp, phi * 180.0 / M_PI, phi0 * 180.0 / M_PI );
        }
      }

      expextedMag = cplx ? amp : ( (k == 0 || k == N/2) ? amp : (amp/2) );
      mag = sqrt(pwrCar) / N;
      if ( fabs(mag - expextedMag) > MAG_ERR_LIMIT )
      {
        retError = 1;
        printf("%s fft %d  bin %d amp %f : mag = %g   expected = %g\n", (cplx ? "cplx":"real"), N, k, amp, mag, expextedMag );
      }


      /* now convert spectrum back */
      pffft_transform_ordered(s, Y, Z, W, PFFFT_BACKWARD);

      errSum = 0.0;
      for ( j = 0; j < (cplx ? (2*N) : N); ++j )
      {
        /* scale back */
        Z[j] /= N;
        /* square sum errors over real (and imag parts) */
        err = (X[j]-Z[j]) * (X[j]-Z[j]);
        errSum += err;
      }

      if ( errSum > N * 1E-7 )
      {
        retError = 1;
        printf("%s fft %d  bin %d : inverse FFT doesn't match original signal! errSum = %g ; mean err = %g\n", (cplx ? "cplx":"real"), N, k, errSum, errSum / N);
      }

      break;
    }

  }
  pffft_destroy_setup(s);
  pffft_aligned_free(X);
  pffft_aligned_free(Y);
  pffft_aligned_free(Z);
  pffft_aligned_free(W);

  return retError;
}



int main(int argc, char **argv)
{
  int N, result, resN, resAll;

  resAll = 0;
  for ( N = 32; N <= 65536; N *= 2 )
  {
    result = test(N, 1 /* cplx fft */, 1 /* useOrdered */);
    resN = result;
    resAll |= result;

    result = test(N, 0 /* cplx fft */, 1 /* useOrdered */);
    resN |= result;
    resAll |= result;

    result = test(N, 1 /* cplx fft */, 0 /* useOrdered */);
    resN |= result;
    resAll |= result;

    result = test(N, 0 /* cplx fft */, 0 /* useOrdered */);
    resN |= result;
    resAll |= result;

    if (!resN)
      printf("tests for size %d succeeded successfully.\n", N);
  }

  if (!resAll)
    printf("all tests succeeded successfully.\n");

  return resAll;
}

