#!/bin/sh
# show that line-delimited CDC works in 'split'.

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
    same_bytes_ 128M | split --line-bytes "$fn/1M" $rs \
      --filter 'wc --bytes' > out || fail=1
    compare "$sp/exp-C-${seed}-${fn}" out || fail=1

    # Ensure that the file is split at EOL
    same_bytes_ 128M | split --line-bytes "$fn/1M" $rs \
      --filter 'tail --bytes 1' > out || fail=1
    count=$(wc -l < "$sp/exp-C-${seed}-${fn}")
    yes '' | head -n $(( count - 1 )) >exp
    same_bytes_ 128M | tail --bytes 1 >>exp
    compare exp out || fail=1
  done
  # Test non-default WINDOW and one that is a multiple of the register width.
  for window in 42 512; do
    for fn in buz32 buz64; do
      same_bytes_ 128M | split --line-bytes "${fn}[${window}]/1M" $rs \
        --filter 'wc --bytes' > out || fail=1
      compare "$sp/exp-C-${seed}-${fn}-${window}" out || fail=1

      same_bytes_ 128M | split --line-bytes "$fn[${window}]/1M" $rs \
        --filter 'tail --bytes 1' > out || fail=1
      count=$(wc -l < "$sp/exp-C-${seed}-${fn}-${window}")
      yes '' | head -n $(( count - 1 )) >exp
      same_bytes_ 128M | tail --bytes 1 >>exp
      compare exp out || fail=1
    done
  done
done
rm -f out

# Ensure that <(same_bytes_ 128M) is still the same after split.
printf 'BLAKE2b-256 (-) = 3be8bea6b02ec4e6e85af9c3bfda278f480e49fd390b89d55668535ec4e53259' \
  > 128M.sum
same_bytes_ 128M | cksum --check 128M.sum || framework_failure_
for fn in buz32 buz64 gear32 gear64; do
  same_bytes_ 128M | split --line-bytes "$fn/1M" - out.x || fail=1
  cat out.x?? | cksum --check 128M.sum || fail=1
  rm -f out.x??
done

Exit $fail
