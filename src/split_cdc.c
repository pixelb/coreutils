/* Hot lookup functions for CDC in split(1).

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

#include <config.h>
#include "system.h"

#include "assure.h"
#include "split_cdc.h"

/* "Expected" BUZHash and GearHash performance in cycles/byte on various CPUs:

  Vendor; CPU; Microarchitecture                        GA  bz32 bz64 gr32 gr64
 ------------------------------------------------------------------------------
 Intel Xeon L5630; Nehalem/Westmere                    10Q1 4.06 4.05 2.10 2.10
 AMD A4-3310MX; A-Series, Llano                        11Q2 4.22 4.16 2.36 2.36
 Intel Xeon E3-1245 V2; Sandy Bridge/Ivy Bridge        12Q2 3.39 3.42 2.01 2.01
 Intel Celeron N2830; Silvermont/Bay Trail             14Q1 6.16 6.17 4.18 4.19
 Intel Xeon E5-2620 v3; Haswell                        14Q3 3.04 3.04 1.34 1.35
 Intel 6th-gen Core i7-6600U; Skylake/Skylake          15Q3 2.59 2.58 1.26 1.27
 Intel Xeon Gold 6133; Skylake/Skylake                 17Q3 2.62 2.62 1.26 1.26
 Intel 8th-gen Core i5-8250U; Skylake/Kaby Lake R      17Q3 2.58 2.58 1.27 1.27
 AMD Ryzen 5 PRO 3400GE; Ryzen 3000                    19Q3 2.44 2.47 2.03 2.03
 Intel 10th-gen Core i7-10750H; Skylake/Comet Lake     20Q2 2.59 2.59 1.26 1.26
 Apple M1 (Icestorm)                                   20Q4 3.35 3.39 2.21 2.23
 Apple M1 (Firestorm)                                  20Q4 2.03 2.03 2.01 2.00
 AMD EPYC 7773X 64-Core; EPYC 7003                     21Q1 2.04 2.07 1.04 1.06
 Intel Xeon Platinum 8592+; Raptor Cove/Emerald Rapids 23Q4 2.42 2.42 2.00 2.00
 Intel 14th-gen Core i7-14700HX; Raptor Lake (Atom)    24Q1 2.86 2.86 1.34 1.34
 Intel 14th-gen Core i7-14700HX; Raptor Lake (Core)    24Q1 2.42 2.42 2.00 2.00

   cycles:u were measured with perf(1) launching split(1) over pseudo-random
   256M file derived from fixed seed. split(1) process was pinned to specific
   CPU, ASLR was disabled.  Output files were sym-linked to /dev/null to make
   write() closer to nop than to memcpy().  Clang-21 and GCC-15 were used,
   the best cpb value was taken.

   The values above are here for a reference.  All tested compilers: GCC 11.4,
   12.3, 13.4, 14.3, 15.2, Clang 19.1 and 21.1 produce BUZHash code that slows
   down to 4.0 cpb when run on Skylake Intel CPU (affected by Jcc erratum).
   JCCERR_CFLAGS may carry compiler/assembler flags to prevent the issue.

   GCC-11.4, 12.3 and 13.4 generate GearHash code that benefits greatly from
   -falign-loops=32. `16` and lower values may cause GearHash speed to degrade
   to 2.0 cpb depending on resulting alignment.  CDC_CFLAGS provide the flag.
   GCC-14.3 and 15.2 align GearHash loop code to 32-byte boundary in all
   the tests.  GCC-15.1 also introduces tight&hot loop alignment in b64412623
   that aligns GearHash to 32-byte boundary given the generated code size.

   BUZHash works like memchr() and GearHash like rawmemchr().  It is tempting
   to remove bounds checks from buz64_find() and buz32_find() caching WINDOW
   bytes of the first match, but BUZHash is already heavy on well-cached memory
   reads, so buz64_rawfind() relying on terminator and re-computation performs
   16% worse than buz64_find() relying on bounds check: Intel Core i7-6600U:
   gets -25% instructions and -46% branches, but +16% cycles and +16%
   cycle_activity.cycles_mem_any.  */

#ifdef JCCERR_CDC
# define buz32_find     buz32_find_jccerr
# define buz64_find     buz64_find_jccerr
# define gear32_rawfind gear32_rawfind_jccerr
# define gear64_rawfind gear64_rawfind_jccerr
#endif

extern unsigned char const *
buz32_find (void *phash_, void const *ple_, unsigned char const *p,
            unsigned char const *const end, idx_t window)
{
  /* It's possible to optimize UNBUZ access on i386 placing it at (BUZ + 256).
     That saves one register spill out of two in this loop with offsetted LEA.
     The second spill might be optimized away with _rawfind()-like code.
     But optimizing file processing code for 32-bit x86 feels odd in 2026.  */
  uint32_t *const phash = phash_;
  uint32_t const *const ple = ple_;
  uint32_t const *const buz = cdc_table;
  uint32_t const *const unbuz = unbuz_table;
  assume (p < end);
  uint32_t hash = *phash;
  uint32_t const le = *ple;
  for (; p != end; p++)
    {
      hash = unbuz[p[-window]] ^ rotl32 (hash, 1) ^ buz[*p];
      if (hash <= le)
        break;
    }
  *phash = hash;
  return p;
}

extern unsigned char const *
buz64_find (void *phash_, void const *ple_, unsigned char const *p,
            unsigned char const *const end, idx_t window)
{
  uint64_t *const phash = phash_;
  uint64_t const *const ple = ple_;
  uint64_t const *const buz = cdc_table;
  uint64_t const *const unbuz = unbuz_table;
  assume (p < end);
  uint64_t hash = *phash;
  uint64_t const le = *ple;
  for (; p != end; p++)
    {
      hash = unbuz[p[-window]] ^ rotl64 (hash, 1) ^ buz[*p];
      if (hash <= le)
        break;
    }
  *phash = hash;
  return p;
}

extern unsigned char const *
gear32_rawfind (void *phash_, void const *ple_, unsigned char const *p,
                unsigned char const *const end, idx_t)
{
  uint32_t *const phash = phash_;
  uint32_t const *const ple = ple_;
  uint32_t const *const cdc = cdc_table;
  assume (p < end);
  uint32_t hash = *phash;
  uint32_t const le = *ple;
  for (;; p++)
    {
      hash = (hash << 1) + cdc[*p];
      if (hash <= le)
        break;
    }
  if (p < end)
    {
      *phash = hash;
      return p;
    }
  else
    {
      idx_t const window = 32;
      gear32 (phash, end - window, window);
      return end;
    }
}

/* It's trivial to compute reverse of GearHash for any hash value.
   Let's drop one branch out of two: put the terminator value hashing to zero
   at the end of the buffer, just like lines_split() does calling rawmemchr().

   Performance gain over gear64_find() is low but noticeable on tested CPUs:

   Intel Core i7-6600U: -22% instructions, -9% cycles.
   Apple Icestorm-M1:                      -8% task-clock.
   Apple Firestorm-M1:  -33% instructions, -1.6% cycles (±0.04%).

   Performance gain for gear32_rawfind() is similar on these CPUs.  */
extern unsigned char const *
gear64_rawfind (void *phash_, void const *ple_, unsigned char const *p,
                unsigned char const *const end, idx_t)
{
  uint64_t *const phash = phash_;
  uint64_t const *const ple = ple_;
  uint64_t const *const cdc = cdc_table;
  assume (p < end);
  uint64_t hash = *phash;
  uint64_t const le = *ple;
  for (;; p++)
    {
      hash = (hash << 1) + cdc[*p];
      if (hash <= le)
        break;
    }
  if (p < end)
    {
      *phash = hash;
      return p;
    }
  else
    {
      idx_t const window = 64;
      gear64 (phash, end - window, window);
      return end;
    }
}
