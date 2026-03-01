#!/bin/sh
# show that --random-source for BUZHash is quite funky in 'split'.

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

zero_bytes_ 1M | tr '\0' '\377' > ffff-1M \
  && zero_bytes_ 4096 > zero-4K \
  && zero_bytes_ 4095 > zero-4K-1 \
  && zero_bytes_ 8192 > zero-8K \
  && zero_bytes_ 8191 > zero-8K-1 \
  && same_bytes_ 1024 "$seed_c" > rand-1K \
  && same_bytes_ 1023 "$seed_c" > rand-1K-1 \
  && same_bytes_ 2048 "$seed_c" > rand-2K \
  && same_bytes_ 2047 "$seed_c" > rand-2K-1 \
  && same_bytes_ 4K   "$seed_c" > rand-4K \
  && same_bytes_ 5K   "$seed_c" > rand-5K \
  && same_bytes_ 8K   "$seed_c" > rand-8K \
  && same_bytes_ 10K  "$seed_c" > rand-10K \
  && printf '' > input \
  || framework_failure_

# Absolutely minimal size for buz32 is 4K...
returns_ 1 split -b buz32/1M --random-source zero-4K-1 input || fail=1
returns_ 0 split -b buz32/1M --random-source zero-4K input || fail=1

# and it's 8K for buz64..
returns_ 1 split -b buz64/1M --random-source zero-8K-1 input || fail=1
returns_ 0 split -b buz64/1M --random-source zero-8K input || fail=1

# but it breaks with high-entropy source as sampling RNG throws few bytes away:
returns_ 1 split -b buz32/1M --random-source rand-4K input || fail=1
returns_ 1 split -b buz64/1M --random-source rand-8K input || fail=1

# and BUZHash has no upper bound if random-source degrades to stream of \xFF:
returns_ 1 split -b buz32/1M --random-source ffff-1M input || fail=1
returns_ 1 split -b buz64/1M --random-source ffff-1M input || fail=1

# However, high-entropy 5K and 10K (values from randperm_bound) are okayish.
# They're tested as that's the values in the doc as well.
returns_ 0 split -b buz32/1M --random-source rand-5K input || fail=1
returns_ 0 split -b buz64/1M --random-source rand-10K input || fail=1

# GearHash needs 1K and 2K, but it explicitly demands non-zero entopy:
returns_ 1 split -b gear32/1M --random-source rand-1K-1 input || fail=1
returns_ 0 split -b gear32/1M --random-source rand-1K input || fail=1
returns_ 1 split -b gear32/1M --random-source zero-8K input || fail=1
returns_ 1 split -b gear32/1M --random-source ffff-1M input || fail=1

returns_ 1 split -b gear64/1M --random-source rand-2K-1 input || fail=1
returns_ 0 split -b gear64/1M --random-source rand-2K input || fail=1
returns_ 1 split -b gear64/1M --random-source zero-8K input || fail=1
returns_ 1 split -b gear64/1M --random-source ffff-1M input || fail=1

Exit $fail
