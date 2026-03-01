/* Header for hot lookup functions for CDC in split(1).

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

#ifndef UUID_49BA2172_7262_4B8D_939C_3701442E7FC2
#define UUID_49BA2172_7262_4B8D_939C_3701442E7FC2

#include <idx.h>
#include <stdint.h>

/* 64 is the cache-line size for x86-64, Apple M-series chips use 128 bytes.
   128 works as a good default. It wastes 64 bytes in the worst-case.  */
enum { CDC_TABLE_DEFAULT_ALIGNAS = 128 };

/* HASH and LE values are passed via pointers due to u32|u64 difference.  */
typedef void (*cdchash_fn) (void *phash, unsigned char const *p, idx_t count);
typedef unsigned char const *(*cdcfind_fn) (void *phash, void const *ple,
                                            unsigned char const *p,
                                            unsigned char const *const end,
                                            idx_t window);

extern uint64_t const buz_seed[256];

extern void const *cdc_table;

extern void const *unbuz_table;

void buz32 (void *phash, unsigned char const *p, idx_t count);
void buz64 (void *phash, unsigned char const *p, idx_t count);
void gear32 (void *phash, unsigned char const *p, idx_t count);
void gear64 (void *phash, unsigned char const *p, idx_t count);

unsigned char const *gear32_rawfind (void *phash, void const *ple,
                                     unsigned char const *p,
                                     unsigned char const *const end,
                                     idx_t window);
unsigned char const *gear64_rawfind (void *phash, void const *ple,
                                     unsigned char const *p,
                                     unsigned char const *const end,
                                     idx_t window);
unsigned char const *buz32_find (void *phash, void const *ple,
                                 unsigned char const *p,
                                 unsigned char const *const end, idx_t window);
unsigned char const *buz64_find (void *phash, void const *ple,
                                 unsigned char const *p,
                                 unsigned char const *const end, idx_t window);

static inline uint32_t
rotl32 (uint32_t x, unsigned int n)
{
  return (x << n) | (x >> ((-n) % 32));
}

static inline uint64_t
rotl64 (uint64_t x, unsigned int n)
{
  return (x << n) | (x >> ((-n) % 64));
}

#endif
