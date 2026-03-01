/* Generating BUZHash and GearHash S-boxes from a random seed.

   Contributed to the GNU project by Leonid Evdokimov.

   Copyright (C) 2026 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* BUZHash substitution table should be balanced according to Robert Uzgalis.
   Each bit should have exactly 128 zeros and ones.  So, each bit of BUZTable
   needs log2(256! / 128! / 128!) ~251.67 bits of entropy.  Whole 64-bit
   BUZHash S-box needs at least ~16107 entropy bits.  However, this code
   depends on sampling with rejection and uses permutation instead of selection
   to construct the table, so it needs much higher number of bytes as an input.

   GearHash paper places no explicit demand on the S-box being balanced.
   Reusing the same table for BUZHash and GearHash seems to be okay as the size
   of PRNG seed used to generate built-in seed has way less bits than either
   16107 or 16384, so _some_ bias is practically unavoidable.

   The file embeds stripped-down PCG PRNG as the output should be stable
   across releases.  Seeded ISAAC is unsuitable as it works differently
   on 32-bit and 64-bit platforms.  Bundled BLAKE2 code does not include
   BLAKE2X XOF.  Depending on `openssl` binary to provide a canonical random
   stream in a build time looks like overkill.

   The generated table is 64-bit assuming uintmax_t to be uint64_t.  There might
   be machines having 32-bit uintmax_t, but it's unclear if those are still
   operational and are capable of using modern coreutils.  Please, be kind
   to specify a test platform if you find a machine & compiler like that.

   Bits are generated from the LSB to MSB.  So, unsigned __int128 support might
   extend GearHash window to 128 bytes in future releases while maintaining
   compatibility.  */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <limits.h>

enum { N_CHARS = UCHAR_MAX + 1 };

typedef unsigned char randchar;

typedef struct
{
  uint64_t state;
  uint64_t inc;
} pcg32_random_t;

static void pcg32_srandom_r (pcg32_random_t *rng, uint64_t initstate,
                             uint64_t initseq);
static uint32_t pcg32_random_r (pcg32_random_t *rng);
static uint32_t pcg32_boundedrand_r (pcg32_random_t *rng, uint32_t bound);

static void
randpermchar (randchar *v, pcg32_random_t *src, size_t h, size_t n)
{
  for (size_t i = 0; i < n; i++)
    v[i] = i;
  for (size_t i = 0; i < h; i++)
    {
      size_t const j = i + pcg32_boundedrand_r (src, n - i);
      randchar const tmp = v[i];
      v[i] = v[j];
      v[j] = tmp;
    }
}

int
main (int argc, char **argv)
{
  if (argc != 1)
    {
      fprintf (stderr,
               "Usage: %s\n"
               "Produces BUZHash and GearHash substitution tables\n",
               argv[0]);
      return EXIT_FAILURE;
    }

  /* Seed comes from commit 89b2cd58ac895e3fc0d24d8f10e7e4ba132e7fb6 (v9.10) */
  pcg32_random_t rng;
  pcg32_srandom_r (&rng, UINT64_C (0x89b2cd58ac895e3f),
                   UINT64_C (0xc0d24d8f10e7e4ba));

  uint64_t buz_table64[N_CHARS];
  memset (buz_table64, 0, sizeof (buz_table64));
  for (unsigned bit = 0; bit < sizeof (buz_table64[0]) * CHAR_BIT; bit++)
    {
      randchar perm[N_CHARS];
      randpermchar (perm, &rng, N_CHARS / 2, N_CHARS);
      uint64_t const buzbit = UINT64_C (1) << bit;
      for (unsigned c = 0; c < N_CHARS / 2; c++)
        buz_table64[perm[c]] |= buzbit;
    }

  puts ("/* Generated file -- DO NOT EDIT */\n"
        "#include <config.h>\n"
        "#include <stdint.h>\n"
        "#include \"split_cdc.h\"\n");
  printf (
      "alignas (CDC_TABLE_DEFAULT_ALIGNAS) uint64_t const buz_seed[%zu] = {\n",
      (size_t)N_CHARS);
  for (unsigned c = 0; c < N_CHARS; c++)
    printf ("  UINT64_C (0x%016" PRIx64 "),\n", buz_table64[c]);
  puts ("};");

  if (ferror (stdout) || fclose (stdout))
    error (EXIT_FAILURE, errno, "write error");

  return EXIT_SUCCESS;
}

/*
 * PCG Random Number Generation for C.
 *
 * Copyright 2014 Melissa O'Neill <oneill@pcg-random.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For additional information about the PCG random number generation scheme,
 * including its license and other licensing options, visit
 *
 *       http://www.pcg-random.org
 */

/*
 * This code is derived from the full C implementation, which is in turn
 * derived from the canonical C++ PCG implementation. The C++ version
 * has many additional features and is preferable if you can use C++ in
 * your project.
 */

// pcg32_srandom_r(rng, initstate, initseq):
//     Seed the rng.  Specified in two parts, state initializer and a
//     sequence selection constant (a.k.a. stream id)

static
void pcg32_srandom_r (pcg32_random_t* rng, uint64_t initstate, uint64_t initseq)
{
    rng->state = 0U;
    rng->inc = (initseq << 1u) | 1u;
    pcg32_random_r (rng);
    rng->state += initstate;
    pcg32_random_r (rng);
}

// pcg32_random_r(rng)
//     Generate a uniformly distributed 32-bit random number

static
uint32_t pcg32_random_r (pcg32_random_t* rng)
{
    uint64_t oldstate = rng->state;
    rng->state = oldstate * UINT64_C (6364136223846793005) + rng->inc;
    uint32_t xorshifted = ((oldstate >> 18u) ^ oldstate) >> 27u;
    uint32_t rot = oldstate >> 59u;
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

// pcg32_boundedrand_r(rng, bound):
//     Generate a uniformly distributed number, r, where 0 <= r < bound

static
uint32_t pcg32_boundedrand_r (pcg32_random_t* rng, uint32_t bound)
{
    // To avoid bias, we need to make the range of the RNG a multiple of
    // bound, which we do by dropping output less than a threshold.
    // A naive scheme to calculate the threshold would be to do
    //
    //     uint32_t threshold = 0x100000000ull % bound;
    //
    // but 64-bit div/mod is slower than 32-bit div/mod (especially on
    // 32-bit platforms).  In essence, we do
    //
    //     uint32_t threshold = (0x100000000ull-bound) % bound;
    //
    // because this version will calculate the same modulus, but the LHS
    // value is less than 2^32.

    uint32_t threshold = -bound % bound;

    // Uniformity guarantees that this loop will terminate.  In practice, it
    // should usually terminate quickly; on average (assuming all bounds are
    // equally likely), 82.25% of the time, we can expect it to require just
    // one iteration.  In the worst case, someone passes a bound of 2^31 + 1
    // (i.e., 2147483649), which invalidates almost 50% of the range.  In
    // practice, bounds are typically small and only a tiny amount of the range
    // is eliminated.
    for (;;) {
        uint32_t r = pcg32_random_r (rng);
        if (r >= threshold)
            return r % bound;
    }
}
