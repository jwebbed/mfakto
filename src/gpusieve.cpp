/*
This file is part of mfaktc (mfakto).
Copyright (C) 2009 - 2013  Oliver Weihe (o.weihe@t-online.de)
                           Bertram Franz (bertramf@gmx.net)

mfaktc (mfakto) is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

mfaktc (mfakto) is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
                                
You should have received a copy of the GNU General Public License
along with mfaktc (mfakto).  If not, see <http://www.gnu.org/licenses/>.
*/


/*
This source is an OpenCL port of the CUDA code by George Woltman.
This code is a GPU-based sieve for mfakto. GW's gpusieve.cu is split into
gpusieve.cl for the OpenCL part (device code) and gpusieve.cpp (host code).

Thanks go also to Ben Buhrow for his erato.cu program and to Rocke Verser for his gpusieve program.
See (http://www.mersenneforum.org/showthread.php?t=11900) for Ben's initial work.

*/

#include <cstdlib>
#include "CL/cl.h"
#include <iostream>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "my_types.h"
#include "compatibility.h"
#include "mfakto.h"

// valgrind tests complain a lot about the blocks being uninitialized
#define malloc(x) calloc(x,1)

extern  cl_command_queue    QUEUE;

#define gen_pinv(p)	(0xFFFFFFFF / (p) + 1)
#define gen_sloppy_pinv(p)	((cl_uint) floor (4294967296.0 / (p) - 0.5))

const cl_uint block_size_in_bytes = 8192;		// Size of shared memory array in bytes
const cl_uint block_size = block_size_in_bytes * 8;	// Number of bits generated by each block
const cl_uint threadsPerBlock = 256;			// Threads per block
#ifdef MORE_CLASSES
const cl_uint primesNotSieved = 5;			// Primes 2, 3, 5, 7, 11 are not sieved
//const cl_uint primesHandledWithSpecialCode = 13;	// Count of primes handled with inline code (not using primes array)
							// Primes 13 through 61 are handled specially
//const cl_uint primesHandledWithSpecialCode = 26;	// Count of primes handled with inline code (not using primes array)
							// Primes 13 through 127 are handled specially
const cl_uint primesHandledWithSpecialCode = 49;		// Count of primes handled with inline code (not using primes array)
							// Primes 13 through 251 are handled specially
//const cl_uint primesHandledWithSpecialCode = 92;	// Count of primes handled with inline code (not using primes array)
							// Primes 13 through 509 are handled specially
#else
const cl_uint primesNotSieved = 4;			// Primes 2, 3, 5, 7 are not sieved
//const cl_uint primesHandledWithSpecialCode = 14;	// Count of primes handled with inline code (not using primes array)
							// Primes 11 through 61 are handled specially
//const cl_uint primesHandledWithSpecialCode = 27;	// Count of primes handled with inline code (not using primes array)
							// Primes 11 through 127 are handled specially
const cl_uint primesHandledWithSpecialCode = 50;		// Count of primes handled with inline code (not using primes array)
							// Primes 11 through 251 are handled specially
//const cl_uint primesHandledWithSpecialCode = 93;	// Count of primes handled with inline code (not using primes array)
							// Primes 11 through 509 are handled specially
#endif

// Various useful constants

const cl_uint primesBelow64K = 6542;			// There are 6542 16-bit primes
const cl_uint primesBelow128K = 12251;			// There are 12251 17-bit primes
const cl_uint primesBelow1M = 82025;			// There are 82025 20-bit primes
const cl_uint sieving64KCrossover = (primesBelow64K - primesNotSieved - primesHandledWithSpecialCode) / threadsPerBlock;
							// Number of thread loops processing primes below 64K
const cl_uint sieving128KCrossover = (primesBelow128K - primesNotSieved - primesHandledWithSpecialCode) / threadsPerBlock;
							// Number of thread loops processing primes below 128K
const cl_uint sieving1MCrossover = (primesBelow1M - primesNotSieved - primesHandledWithSpecialCode) / threadsPerBlock - 3;  // bug - awkward hard coded -3 here
							// Number of thread loops processing primes below 1M

// Global vars.  These could be moved to mystuff, but no other code needs to know about these internal values.

cl_uint primes_per_thread = 0;		// Number of "rows" in the GPU sieving info array that each thread processes

// Bit masks for small prime sieving

#define BITSLL11 (1 | (1<<11) | (1<<22))
#define BITSLL13 (1 | (1<<13) | (1<<26))
#define BITSLL17 (1 | (1<<17))
#define BITSLL19 (1 | (1<<19))
#define BITSLL23 (1 | (1<<23))
#define BITSLL29 (1 | (1<<29))
#define BITSLL31 (1 | (1<<31))

// Various padding required to keep warps accessing primes data on 128-byte boundaries

#define PINFO_PAD1		1024			// Allows room for lots of initial bit_to_clr values


//
// Sieve initialization done on the CPU
//

// Simple CPU sieve of erathosthenes for small limits - not efficient for large limits.

void tiny_soe (cl_uint limit, cl_uint *primes)
{
	cl_uchar *flags;
	cl_ushort prime;
	cl_uint i, j, sieve_size;
	cl_uint it;

	// Allocate flags (assume we can generate N primes by sieving up to 40*N.  We only need flags for odd numbers)
	sieve_size = limit * 40 / 2;
	flags = (cl_uchar *) malloc (sieve_size);
	if (flags == NULL) {
		printf ("error allocating tiny_soe flags\n");
		exit (1);
	}
	memset (flags, 1, sieve_size);

	primes[0] = 2;
	it = 1;

	// sieve using primes less than the sqrt of the desired limit
	for (i = 1; i < (cl_uint) sqrt ((double) (limit * 40)); i++) {
		if (flags[i] == 1) {
			prime = (cl_uint) (2*i + 1);
			for (j = i + prime; j < sieve_size; j += prime)
				flags[j] = 0;

			primes[it] = prime;
			it++;
		}
	}

	//now find the rest of the prime flags and compute the sieving primes
	for ( ; it < limit; i++) {
		if (flags[i] == 1) {
			primes[it] = (cl_uint) (2*i + 1);
			it++;
		}
	}

	free (flags);
}

// GPU sieve initialization that only needs to be done one time.
#ifdef __cplusplus
extern "C" {
#endif

int gpusieve_init (mystuff_t *mystuff, cl_context context)
{
	cl_uint	*primes;
	cl_uchar	*pinfo, *saveptr;
	cl_uint	*rowinfo, *row;
	cl_uint	i, j, pinfo_size, rowinfo_size;
	cl_uint	k, loop_count, loop_end;
  static	int	gpusieve_initialized = 0;
  cl_int  status;

	// If we've already allocated GPU memory, return
	if (gpusieve_initialized) return 0;
	gpusieve_initialized = 1;

	// Prefer 48KB shared memory
//	cudaThreadSetCacheConfig(cudaFuncCachePreferShared);

	// Allocate the big sieve array (default is 128M bits)
	// checkCudaErrors (cudaMalloc ((void**) &mystuff->d_bitarray, mystuff->gpu_sieve_size / 8));
  if( (mystuff->h_bitarray = (cl_uint *) malloc(mystuff->gpu_sieve_size / 8)) == NULL )  // host array normally not needed - just for verification of the sieve
  {
    printf("ERROR: malloc(h_bitarray) failed\n");
    return 1;
  }
  mystuff->d_bitarray = clCreateBuffer(context, 
                         CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                         mystuff->gpu_sieve_size / 8,
                         mystuff->h_bitarray, 
                        &status);
  if(status != CL_SUCCESS) 
  { 
	  std::cout<<"Error " << status << ": clCreateBuffer (d_bitarray)\n";
  	return 1;
	}

#ifdef DETAILED_INFO
  printf("gpusieve_init: d/h_bitarray (%d bytes) allocated\n", mystuff->gpu_sieve_size / 8);
#endif

#ifdef RAW_GPU_BENCH
	// Quick hack to eliminate sieve time from GPU-code benchmarks.  Can also be used
	// to isolate a bug by eliminating the GPU sieving code as a possible cause.
	// checkCudaErrors (cudaMemset (mystuff->d_bitarray, 0xFF, mystuff->gpu_sieve_size / 8));
  memset (mystuff->h_bitarray, 0xFF, mystuff->gpu_sieve_size / 8);
  status = clEnqueueWriteBuffer(QUEUE,
                mystuff->d_bitarray,
                CL_TRUE,
                0,
                SIEVE_PRIMES_MAX * sizeof(cl_uint),
                mystuff->h_bitarray,
                0,
                NULL,
                NULL);  // primes are written to GPU only once at startup
  if(status != CL_SUCCESS) 
  { 
    std::cout<<"Error " << status << ": clEnqueueWriteBuffer (d_bitarray)\n";
 	  return 1;
  }
#endif  

#undef pinfo32
#define pinfo32		((cl_uint *) pinfo)

	// Round up SIEVE_PRIMES so that all threads stay busy in the last sieving loop
	// The first several primes are handled with special code.  After that, they
	// are processed in chunks of threadsPerBlock (256).

	mystuff->gpu_sieve_primes = ((mystuff->gpu_sieve_primes - primesNotSieved - primesHandledWithSpecialCode) / threadsPerBlock) * threadsPerBlock
				    + primesNotSieved + primesHandledWithSpecialCode;

	// Loop finding a suitable SIEVE_PRIMES value.  Initial value sieves primes below around 1.05M.

	for ( ; ; mystuff->gpu_sieve_primes += threadsPerBlock) {

		// compute how many "rows" of the primes info array each thread will be responsible for
		primes_per_thread = (mystuff->gpu_sieve_primes - primesNotSieved - primesHandledWithSpecialCode) / threadsPerBlock;

		// Make sure there are 0 mod 3 rows in the under 64K section!
		if (primes_per_thread > 1) {
			loop_count = min (primes_per_thread, sieving64KCrossover) - 1;
			if ((loop_count % 3) != 0) continue;
		}

		// Make sure we don't try the 64K crossover row
		if (primes_per_thread == sieving64KCrossover + 1) continue;

		// Make sure there are 1 mod 3 rows in 64K to 128K section!
		if (primes_per_thread > sieving64KCrossover + 1) {
			loop_count = min (primes_per_thread, sieving128KCrossover + 1) - (sieving64KCrossover + 1);
			if ((loop_count % 3) != 1) continue;
		}

		// Make sure there are 1 mod 4 rows in 128K to 1M section!
		if (primes_per_thread > sieving128KCrossover + 1) {
			loop_count = min (primes_per_thread, sieving1MCrossover) - (sieving128KCrossover + 1);
			if ((loop_count % 4) != 1) continue;
		}

		// Make sure there are 1 mod 4 rows in 1M to 16M section!
		loop_count = primes_per_thread - sieving1MCrossover;
		if (primes_per_thread > sieving1MCrossover) {
			loop_count = primes_per_thread - sieving1MCrossover;
			if ((loop_count % 4) != 1) continue;
		}

		// We've found the SIEVE_PRIMES value to use
		break;
	}

	// find seed primes
	primes = (cl_uint *) malloc (mystuff->gpu_sieve_primes * sizeof (cl_uint));
	if (primes == NULL) {
		printf ("error in malloc primes\n");
		exit (1);
	}
	tiny_soe (mystuff->gpu_sieve_primes, primes);

	// allocate memory for compressed prime info -- assumes prime data can be stored in 12 bytes
	pinfo = (cl_uchar *) malloc (mystuff->gpu_sieve_primes * 12);
	if (pinfo == NULL) {
		printf ("error in malloc pinfo\n");
		exit (1);
	}

#ifdef DETAILED_INFO
  printf("gpusieve_init: h_sieve_info (%d bytes) allocated\n", mystuff->gpu_sieve_primes * 12);
#endif

	// allocate memory for info that describes each row of 256 primes AND has the primes and modular inverses
	rowinfo_size = MAX_PRIMES_PER_THREAD*4 * sizeof (cl_uint) + mystuff->gpu_sieve_primes * 8;
	rowinfo = (cl_uint *) malloc (rowinfo_size);
	if (rowinfo == NULL) {
		printf ("error in malloc rowinfo\n");
		exit (1);
	}

#ifdef DETAILED_INFO
  printf("gpusieve_init: h_calc_bit_to_clear_info (%d bytes) allocated\n", rowinfo_size);
#endif

	// In first section (very small primes) we only store a 16-bit value of the bit to clear which is computed later
	saveptr = pinfo;
	i = primesNotSieved + primesHandledWithSpecialCode;
	pinfo += PINFO_PAD1;

	// In this section (primes below 64K) we store p in 16 bits, bit-to-clr in 16 bits, and pinv in 32 bits.
	row = rowinfo;
	loop_end = min (primes_per_thread, sieving64KCrossover);
	for ( ; i < primesNotSieved + primesHandledWithSpecialCode + loop_end * threadsPerBlock; i += threadsPerBlock, pinfo += threadsPerBlock * 8) {
		row[0] = (cl_uint)(pinfo - saveptr);			// Offset to first pinfo byte in the row
		row[MAX_PRIMES_PER_THREAD] = i;			// First pinfo entry is for the i-th prime number
		row[MAX_PRIMES_PER_THREAD*2] = 1;		// Pinfo entries represent successive prime numbers
		row[MAX_PRIMES_PER_THREAD*3] = 0xFFFF0000;	// Mask of bits to preserve when setting bit-to-clear
		row++;
		for (j = 0; j < threadsPerBlock; j++) {
			pinfo32[j] = (primes[i+j] << 16) + 0;
			pinfo32[j+threadsPerBlock] = gen_pinv (primes[i+j]);
		}
	}

	// In this section (primes both below and above 64K) we store bit-to-clr in 32 bits, pinv in 32 bits, and p in 32 bits.
	loop_end = min (primes_per_thread, sieving64KCrossover + 1);
	for ( ; i < primesNotSieved + primesHandledWithSpecialCode + loop_end * threadsPerBlock; i += threadsPerBlock, pinfo += threadsPerBlock * 12) {
		row[0] = (cl_uint)(pinfo - saveptr);			// Offset to first pinfo byte in the row
		row[MAX_PRIMES_PER_THREAD] = i;			// First pinfo entry is for the i-th prime number
		row[MAX_PRIMES_PER_THREAD*2] = 1;		// Pinfo entries represent successive prime numbers
		row[MAX_PRIMES_PER_THREAD*3] = 0;		// Mask of bits to preserve when setting bit-to-clear
		row++;
		for (j = 0; j < threadsPerBlock; j++) {
			pinfo32[j] = 0;
			pinfo32[j+threadsPerBlock] = gen_pinv (primes[i+j]);
			pinfo32[j+threadsPerBlock*2] = primes[i+j];
		}
	}

	// In this section (transitioning to dense primes storage) we store bit-to-clr 32 bits, pinv in 32 bits, and p in 32 bits.
	if (primes_per_thread > sieving64KCrossover + 1) {
		loop_count = min (primes_per_thread, sieving128KCrossover + 1) - (sieving64KCrossover + 1);
		row[0] = (cl_uint)(pinfo - saveptr);			// Offset to first pinfo byte in the row
		row[MAX_PRIMES_PER_THREAD] = i;			// First pinfo entry is for the i-th prime number
		row[MAX_PRIMES_PER_THREAD*2] = loop_count;	// Pinfo entries skip loop_count prime numbers
		row[MAX_PRIMES_PER_THREAD*3] = 0;		// Mask of bits to preserve when setting bit-to-clear
		row++;
		for (j = 0; j < threadsPerBlock; j++) {
			pinfo32[j] = 0;
			pinfo32[j+threadsPerBlock] = gen_pinv (primes[i+j*loop_count]);
			pinfo32[j+threadsPerBlock*2] = primes[i+j*loop_count];
		}
		pinfo += threadsPerBlock * 12;
	}

	// In this section (primes from 64K through 128K) we store bit-to-clr 18 bits, (p diff) / 2 in 7 bits, and pinv diff in 7-bits.
	if (primes_per_thread > sieving64KCrossover + 2) {
		for (k = 1; k < loop_count; k++) {
			row[0] = (cl_uint)(pinfo - saveptr) + (k - 1) * threadsPerBlock * 4;	// Offset to first pinfo byte in the row
			row[MAX_PRIMES_PER_THREAD] = i + k;		// First pinfo entry is for the i+k-th prime number
			row[MAX_PRIMES_PER_THREAD*2] = loop_count;	// Pinfo entries skip loop_count prime numbers
			row[MAX_PRIMES_PER_THREAD*3] = 0xFFFC0000;	// Mask of bits to preserve when setting bit-to-clear
			row++;
		}
		for (k = 1; k < loop_count; k += 3) {
			for (j = 0; j < threadsPerBlock; j++) {
				int	index = i + j * loop_count + k;
				cl_uint	pdiff = (primes[index] - primes[index-1]) / 2;
				cl_uint	pinvdiff = gen_pinv (primes[index-1]) - gen_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 127) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k - 1) * threadsPerBlock + j] = (pinvdiff << 25) + (pdiff << 18) + 0;

				index++;
				pdiff = (primes[index] - primes[index-2]) / 2;
				pinvdiff = gen_pinv (primes[index-2]) - gen_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 127) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[k * threadsPerBlock + j] = (pinvdiff << 25) + (pdiff << 18) + 0;

				index++;
				pdiff = (primes[index] - primes[index-3]) / 2;
				pinvdiff = gen_pinv (primes[index-3]) - gen_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 127) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k + 1) * threadsPerBlock + j] = (pinvdiff << 25) + (pdiff << 18) + 0;
			}
		}
		pinfo += (loop_count - 1) * threadsPerBlock * 4;
		i += loop_count * threadsPerBlock;
	}

	// In this section (first complete row of primes above 128K) we store bit-to-clr 32 bits, pinv in 32 bits, and p in 32-bits.
	if (primes_per_thread > sieving128KCrossover + 1) {
		loop_count = min (primes_per_thread, sieving1MCrossover) - (sieving128KCrossover + 1);
		row[0] = (cl_uint)(pinfo - saveptr);			// Offset to first pinfo byte in the row
		row[MAX_PRIMES_PER_THREAD] = i;			// First pinfo entry is for the i-th prime number
		row[MAX_PRIMES_PER_THREAD*2] = loop_count;	// Pinfo entries skip loop_count prime numbers
		row[MAX_PRIMES_PER_THREAD*3] = 0;		// Mask of bits to preserve when setting bit-to-clear
		row++;
		for (j = 0; j < threadsPerBlock; j++) {
			pinfo32[j] = 0;
			pinfo32[j+threadsPerBlock] = gen_sloppy_pinv (primes[i+j*loop_count]);
			pinfo32[j+threadsPerBlock*2] = primes[i+j*loop_count];
		}
		pinfo += threadsPerBlock * 12;
	}

	// In this section (primes from 128K to 1M) we store bit-to-clr 20 bits, (p diff) / 2 in 7 bits, and pinv diff in 5 bits.
	if (primes_per_thread > sieving128KCrossover + 2) {
		for (k = 1; k < loop_count; k++) {
			row[0] = (cl_uint)(pinfo - saveptr) + (k - 1) * threadsPerBlock * 4;	// Offset to first pinfo byte in the row
			row[MAX_PRIMES_PER_THREAD] = i + k;		// First pinfo entry is for the i+k-th prime number
			row[MAX_PRIMES_PER_THREAD*2] = loop_count;	// Pinfo entries skip loop_count prime numbers
			row[MAX_PRIMES_PER_THREAD*3] = 0xFFF00000;	// Mask of bits to preserve when setting bit-to-clear
			row++;
		}
		for (k = 1; k < loop_count; k += 4) {
			for (j = 0; j < threadsPerBlock; j++) {
				int	index = i + j * loop_count + k;
				cl_uint	pdiff = (primes[index] - primes[index-1]) / 2;
				cl_uint	pinvdiff = gen_sloppy_pinv (primes[index-1]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 31) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k - 1) * threadsPerBlock + j] = (pinvdiff << 27) + (pdiff << 20) + 0;

				index++;
				pdiff = (primes[index] - primes[index-2]) / 2;
				pinvdiff = gen_sloppy_pinv (primes[index-2]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 31) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[k * threadsPerBlock + j] = (pinvdiff << 27) + (pdiff << 20) + 0;

				index++;
				pdiff = (primes[index] - primes[index-3]) / 2;
				pinvdiff = gen_sloppy_pinv (primes[index-3]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 31) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k + 1) * threadsPerBlock + j] = (pinvdiff << 27) + (pdiff << 20) + 0;

				index++;
				pdiff = (primes[index] - primes[index-4]) / 2;
				pinvdiff = gen_sloppy_pinv (primes[index-4]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 31) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k + 2) * threadsPerBlock + j] = (pinvdiff << 27) + (pdiff << 20) + 0;
			}
		}
		pinfo += (loop_count - 1) * threadsPerBlock * 4;
		i += loop_count * threadsPerBlock;
	}

	// In this section (primes both below and above 1M) we store bit-to-clr 32 bits, pinv in 32 bits, and p in 32-bits.
	if (primes_per_thread > sieving1MCrossover) {
		loop_count = primes_per_thread - sieving1MCrossover;
		row[0] = (cl_uint)(pinfo - saveptr);			// Offset to first pinfo byte in the row
		row[MAX_PRIMES_PER_THREAD] = i;			// First pinfo entry is for the i-th prime number
		row[MAX_PRIMES_PER_THREAD*2] = loop_count;	// Pinfo entries skip loop_count prime numbers
		row[MAX_PRIMES_PER_THREAD*3] = 0;		// Mask of bits to preserve when setting bit-to-clear
		row++;
		for (j = 0; j < threadsPerBlock; j++) {
			pinfo32[j] = 0;
			pinfo32[j+threadsPerBlock] = gen_sloppy_pinv (primes[i+j*loop_count]);
			pinfo32[j+threadsPerBlock*2] = primes[i+j*loop_count];
		}
		pinfo += threadsPerBlock * 12;
	}

	// In this section (primes above 1M to 16M) we store bit-to-clr 24 bits, (p diff) / 2 in 7 bits, and pinv diff in 1 bit.
	if (primes_per_thread > sieving1MCrossover + 1) {
		for (k = 1; k < loop_count; k++) {
			row[0] = (cl_uint)(pinfo - saveptr) + (k - 1) * threadsPerBlock * 4;	// Offset to first pinfo byte in the row
			row[MAX_PRIMES_PER_THREAD] = i + k;		// First pinfo entry is for the i+k-th prime number
			row[MAX_PRIMES_PER_THREAD*2] = loop_count;	// Pinfo entries skip loop_count prime numbers
			row[MAX_PRIMES_PER_THREAD*3] = 0xFF000000;	// Mask of bits to preserve when setting bit-to-clear
			row++;
		}
		for (k = 1; k < loop_count; k += 4) {
			for (j = 0; j < threadsPerBlock; j++) {
				int	index = i + j * loop_count + k;
				cl_uint	pdiff = (primes[index] - primes[index-1]) / 2;
				cl_uint	pinvdiff = gen_sloppy_pinv (primes[index-1]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 1) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k - 1) * threadsPerBlock + j] = (pinvdiff << 31) + (pdiff << 24) + 0;

				index++;
				pdiff = (primes[index] - primes[index-2]) / 2;
				pinvdiff = gen_sloppy_pinv (primes[index-2]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 1) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[k * threadsPerBlock + j] = (pinvdiff << 31) + (pdiff << 24) + 0;

				index++;
				pdiff = (primes[index] - primes[index-3]) / 2;
				pinvdiff = gen_sloppy_pinv (primes[index-3]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 1) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k + 1) * threadsPerBlock + j] = (pinvdiff << 31) + (pdiff << 24) + 0;

				index++;
				pdiff = (primes[index] - primes[index-4]) / 2;
				pinvdiff = gen_sloppy_pinv (primes[index-4]) - gen_sloppy_pinv (primes[index]);
				if (pdiff > 127 || pinvdiff > 1) printf ("Bad compress: %d, %d, %d\n", primes[index], pdiff, pinvdiff);
				pinfo32[(k + 2) * threadsPerBlock + j] = (pinvdiff << 31) + (pdiff << 24) + 0;
			}
		}
		pinfo += (loop_count - 1) * threadsPerBlock * 4;
		i += loop_count * threadsPerBlock;
	}
	pinfo_size = (cl_uint)(pinfo - saveptr);
	pinfo = saveptr;

	// Finally, also copy the primes to rowinfo to be used in later calculating bit-to-clear values
	for (i = primesNotSieved; i < (cl_uint) mystuff->gpu_sieve_primes; i++) {
		rowinfo[MAX_PRIMES_PER_THREAD*4 + 2 * i] = primes[i];
	}

	// Allocate and copy the device compressed prime sieving info
	// checkCudaErrors (cudaMalloc ((void**) &mystuff->d_sieve_info, pinfo_size));
	// checkCudaErrors (cudaMemcpy (mystuff->d_sieve_info, pinfo, pinfo_size, cudaMemcpyHostToDevice));
  mystuff->h_sieve_info = (cl_uint *) realloc(pinfo, pinfo_size);
  mystuff->d_sieve_info = clCreateBuffer(context,
                        CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                        pinfo_size,
                        pinfo,
                        &status);
  if(status != CL_SUCCESS)
  {
		std::cout<<"Error " << status << ": clCreateBuffer (d_sieve_info)\n";
  	return 1;
	}

#ifdef DETAILED_INFO
  printf("gpusieve_init: d_sieve_info (%d bytes) allocated\n", pinfo_size);
  mystuff->sieve_size = pinfo_size;  // misuse of sieve_size, but for debugging we need to remember how many bytes we used
#endif

	// Allocate and copy the device row info, primes and modular inverses info used to calculate bit-to-clear
	// checkCudaErrors (cudaMalloc ((void**) &mystuff->d_calc_bit_to_clear_info, rowinfo_size));
	// checkCudaErrors (cudaMemcpy (mystuff->d_calc_bit_to_clear_info, rowinfo, rowinfo_size, cudaMemcpyHostToDevice));

  mystuff->h_calc_bit_to_clear_info = (cl_uint *) rowinfo;
  mystuff->d_calc_bit_to_clear_info = clCreateBuffer(context, 
                          CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR,
                          rowinfo_size,
                          rowinfo,
                          &status);
  if(status != CL_SUCCESS)
  {
		std::cout<<"Error " << status << ": clCreateBuffer (d_calc_bit_to_clear_info)\n";
  	return 1;
	}

#ifdef DETAILED_INFO
  printf("gpusieve_init: d_calc_bit_to_clear_info (%d bytes) allocated\n", rowinfo_size);
#endif

	// Free allocated memory
	free (primes);
  // keep the following two as they are saved into mystuff->h_*
	// free (pinfo);
	// free (rowinfo);
  return 0;
}


// GPU sieve initialization that needs to be done once for each Mersenne exponent to be factored.

void gpusieve_init_exponent (mystuff_t *mystuff)
{
static cl_uint	last_exponent_initialized = 0;

#ifdef RAW_GPU_BENCH
	// Quick hack (leave bit array set to all ones) to eliminate sieve time from GPU-code benchmarks.
	// Can also be used to isolate a bug by eliminating the GPU sieving code as a possible cause.
	return;
#endif  

	// If we've already initialized this exponent, return
	if (mystuff->exponent == last_exponent_initialized) return;
	last_exponent_initialized = mystuff->exponent;

	// Calculate the modular inverses that will be used by each class to calculate initial bit-to-clear for each prime
	// CalcModularInverses<<<primes_per_thread+1, threadsPerBlock>>>(mystuff->exponent, (int *)mystuff->d_calc_bit_to_clear_info);
	// cudaThreadSynchronize ();
  run_calc_mod_inv(primes_per_thread+1, threadsPerBlock, NULL);
}


// GPU sieve initialization that needs to be done once for each class to be factored.

void gpusieve_init_class (mystuff_t *mystuff, unsigned long long k_min)
{
	int96	k_base;

#ifdef RAW_GPU_BENCH
	// Quick hack (leave bit array set to all ones) to eliminate sieve time from GPU-code benchmarks.
	// Can also be used to isolate a bug by eliminating the GPU sieving code as a possible cause.
	return;
#endif  

	k_base.d0 =  (int) (k_min & 0xFFFFFFFF);
	k_base.d1 =  (int) (k_min >> 32);
	k_base.d2 = 0;

	// Calculate the initial bit-to-clear for each prime
	// CalcBitToClear<<<primes_per_thread+1, threadsPerBlock>>>(mystuff->exponent, k_base, (int *)mystuff->d_calc_bit_to_clear_info, (cl_uchar *)mystuff->d_sieve_info);
	// cudaThreadSynchronize ();
  run_calc_bit_to_clear(primes_per_thread+1, threadsPerBlock, NULL, k_min);
}


// GPU sieve the next chunk

void gpusieve (mystuff_t *mystuff, unsigned long long num_k_remaining)
{
	int	sieve_size;

#ifdef RAW_GPU_BENCH
	// Quick hack (leave bit array set to all ones) to eliminate sieve time from GPU-code benchmarks.
	// Can also be used to isolate a bug by eliminating the GPU sieving code as a possible cause.
	return;
#endif  

	// Sieve at most 128 million k values.
	if ((unsigned long long) mystuff->gpu_sieve_size < num_k_remaining)
		sieve_size = mystuff->gpu_sieve_size;
	else
		sieve_size = (int) num_k_remaining;

	// Do some sieving on the GPU!
	// SegSieve<<<(sieve_size + block_size - 1) / block_size, threadsPerBlock>>>((cl_uchar *)mystuff->d_bitarray, (cl_uchar *)mystuff->d_sieve_info, primes_per_thread);
	// cudaThreadSynchronize ();
  run_cl_sieve((sieve_size + block_size - 1) / block_size, threadsPerBlock, NULL, primes_per_thread);
}

int gpusieve_free (mystuff_t *mystuff)
{
  int status;
	status = clReleaseMemObject(mystuff->d_bitarray);
  if(status != CL_SUCCESS)
	{
		std::cerr<<"Error" << status << ": clReleaseMemObject (mystuff->d_bitarray)\n";
		return 1; 
	}
  free(mystuff->h_bitarray);

	status = clReleaseMemObject(mystuff->d_calc_bit_to_clear_info);
  if(status != CL_SUCCESS)
	{
		std::cerr<<"Error" << status << ": clReleaseMemObject (mystuff->d_calc_bit_to_clear_info)\n";
		return 1; 
	}
  free(mystuff->h_calc_bit_to_clear_info);
  
	status = clReleaseMemObject(mystuff->d_sieve_info);
  if(status != CL_SUCCESS)
	{
		std::cerr<<"Error" << status << ": clReleaseMemObject (mystuff->d_sieve_info)\n";
		return 1; 
	}
  free(mystuff->h_sieve_info);
  return 0;
}

#ifdef __cplusplus
}
#endif
