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
very_expensive_ # 2.5 minutes
sp="$srcdir/tests/split"

IO_BLKSIZE=$(( 256 * 1024 ))

# Let's prepend few extra bytes to shift the 32-byte window through
# the IO_BLKSIZE boundary slowly and check for possible off-by-one errors.
exp="$sp/exp-intseed-gear32"
off=1216420
test $(head -n 1 "$exp") -eq "$off" || framework_failure_

base=$(( $IO_BLKSIZE - ($off % $IO_BLKSIZE) - 35 ))
for extra in $(seq $base $(( $base + 70 ))); do
  # That's file and not pipe to ensure that full IO_BLKSIZE is utilized
  { same_bytes_ $extra $seed_d && same_bytes_ 128M; } > input \
    || framework_failure_

  split ---io-blksize=$IO_BLKSIZE --bytes gear32/1M \
    --filter 'wc --bytes' input > out || fail=1

  out1=$(head -n 1 out)
  tail -n +2 out > out2 \
    && tail -n +2 "$exp" > exp2 \
    || framework_failure_
  test $out1 -eq $(( $off + $extra )) || fail=1
  compare exp2 out2 || fail=1
  rm -f input
done

# Do the same for BUZHash as it behaves a bit differently at the boundary:
exp="$sp/exp-intseed-buz32-42"
off=786013
test $(head -n 1 "$exp") -eq "$off" || framework_failure_

base=$(( $IO_BLKSIZE - ($off % $IO_BLKSIZE) - 45 ))
for extra in $(seq $base $(( $base + 90 ))); do
  { same_bytes_ $extra $seed_d && same_bytes_ 128M; } > input \
    || framework_failure_

  split ---io-blksize=$IO_BLKSIZE --bytes buz32[42]/1M \
    --filter 'wc --bytes' input > out || fail=1

  out1=$(head -n 1 out)
  tail -n +2 out > out2 \
    && tail -n +2 "$exp" > exp2 \
    || framework_failure_
  test $out1 -eq $(( $off + $extra )) || fail=1
  compare exp2 out2 || fail=1
  rm -f input
done

Exit $fail
