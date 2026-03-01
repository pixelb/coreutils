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
#include "split_cdc.h"
#include "assure.h"

/* GCC default align-loops is 8, but 16 and 32 were producing interesting
   results during benchmarking of various version of this code.  However,
   the current code doesn't seem to get clear benefits from loop alignment.

   Comparing performance of --bytes gear64/1M, buz64/1M and 1M on i7-6600U
   suggests that GearHash runs at 1.33 cpb and BUZHash runs at 2.75 cpb.

   It is tempting to remove bounds checks from buz64_find() and buz32_find()
   caching WINDOW bytes of the first match, but BUZHash is already heavy
   on well-cached memory reads, so buz64_rawfind() relying on terminator and
   re-computation performs 16% worse than buz64_find() relying on bounds check:

   Intel Core i7-6600U: gets -25% instructions and -46% branches,
   but +16% cycles and +16% cycle_activity.cycles_mem_any.

   That's why BUZHash works like memchr and GearHash like rawmemchr.  */

extern unsigned char const *
buz32_find (void *phash_, void const *ple_, unsigned char const *p,
            unsigned char const *const end, idx_t window)
{
  /* It's possible to optimize UNBUZ access on i386 placing it at (BUZ + 256).
     That saves one register spill out of two in this loop with offseted LEA.
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
