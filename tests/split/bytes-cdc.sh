#!/bin/sh
# show that content-defined chunking works in 'split'.

# Copyright (C) 2026 Free Software Foundation, Inc.

# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

. "${srcdir=.}/tests/init.sh"; path_prepend_ ./src
print_ver_ split
sp="$srcdir/tests/split"

# Ensure the same behavior across little-/big-endian and 64-/32-bit platforms
# with canonical input.  First, using the PRNG'ed input with the built-in seed.
# Second, with the same input and PRNG'ed seed as a --random-source.
for seed in intseed extseed; do
  if [ $seed = intseed ]; then
    rs=
  else
    same_bytes_ 10K "$seed_c" > "$seed" || framework_failure_
    rs="--random-source $seed"
  fi

  for fn in buz32 buz64 gear32 gear64; do
    same_bytes_ 128M | split --bytes "$fn/1M" $rs \
      --filter 'wc --bytes' > out || fail=1
    compare "$sp/exp-${seed}-${fn}" out || fail=1
  done
  # Test non-default WINDOW and one that is a multiple of the register width.
  for window in 42 512; do
    for fn in buz32 buz64; do
      same_bytes_ 128M | split --bytes "${fn}[${window}]/1M" $rs \
        --filter 'wc --bytes' > out || fail=1
      compare "$sp/exp-${seed}-${fn}-${window}" out || fail=1
    done
  done
done
rm -f out

# Ensure that <(same_bytes_ 128M) is still the same after split.
printf 'BLAKE2b-256 (-) = 3be8bea6b02ec4e6e85af9c3bfda278f480e49fd390b89d55668535ec4e53259' \
  > 128M.sum
same_bytes_ 128M | cksum --check 128M.sum || framework_failure_
for fn in buz32 buz64 gear32 gear64; do
  same_bytes_ 128M | split --bytes "$fn/1M" - out.x || fail=1
  cat out.x?? | cksum --check 128M.sum || fail=1
  rm -f out.x??
done

# Ensure that --filter failing with EPIPE works as expected...
for fn in buz32 buz64 gear32 gear64; do
  same_bytes_ 128M | split --bytes $fn/1M  - out.x

  # both with buffer larger than Linux pipe capacity and with a small one.
  head --silent --bytes 8 out.x?? | cksum -a blake2b > exp-8.sum
  head --silent --bytes 72K out.x?? | cksum -a blake2b > exp-72K.sum

  for bs in 72K 8; do
    rm -f out.x??
    same_bytes_ 128M | \
      split --bytes $fn/1M --filter "head --bytes $bs"' > $FILE' - out.x
    cat out.x?? | cksum --check exp-$bs.sum || fail=1
  done

  # Double-check using number of bytes for a small 8-byte buffer.
  # That's not true for a larger buffer as some chunks are smaller than 72K.
  exp=$(wc -l < "$sp/exp-intseed-$fn")
  test $(cat out.x?? | wc --bytes) -eq $(( exp * 8 )) || fail=1
  rm -f out.x??
done

# Ensure that chunk max-size limit works.
for fn in buz32 buz64 gear32 gear64; do
  maxsz=2850325
  same_bytes_ 128M | split --bytes "$fn/1M/$maxsz" \
    --filter 'wc --bytes' > out || fail=1
  # This AWK code doesn't work for arbitrary MAXSZ, but it works for test data.
  awk -v M=$maxsz '
    ($1 > M) {
      for (i = 0; i < int($1 / M); i++)
        print M;
      print ($1 % M);
    }
    ($1 <= M)' <"$sp/exp-intseed-${fn}" >exp-$fn-EM
  compare exp-${fn}-EM out || fail=1
done

# Max chunk size must be greater than the average chunk size.
returns_ 1 split --bytes gear32/1M/1M   /dev/null || fail=1
returns_ 1 split --bytes gear32/1M/512K /dev/null || fail=1

Exit $fail
