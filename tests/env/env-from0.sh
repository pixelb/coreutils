#!/bin/sh
# Verify env --from0.

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
print_ver_ env

CU_ENV_FROM0_PARENT=parent
CU_ENV_FROM0_REPLACE=parent
CU_ENV_FROM0_REMOVE=parent
export CU_ENV_FROM0_PARENT CU_ENV_FROM0_REPLACE CU_ENV_FROM0_REMOVE

printf '%s\0' \
  CU_ENV_FROM0_REPLACE=file \
  CU_ENV_FROM0_NEW=new \
  CU_ENV_FROM0_REMOVE=file >vars || framework_failure_

# Without -i, merge the file with the inherited environment.  Apply -u
# afterwards, and let command line assignments take final precedence.
QUOTING_STYLE=literal env --from0=vars -u CU_ENV_FROM0_REMOVE \
  CU_ENV_FROM0_REPLACE=operand >all || fail=1
grep -E '^CU_ENV_FROM0_(NEW|PARENT|REMOVE|REPLACE)=' all |
  LC_ALL=C sort >out || framework_failure_
cat <<\EOF >exp || framework_failure_
CU_ENV_FROM0_NEW=new
CU_ENV_FROM0_PARENT=parent
CU_ENV_FROM0_REPLACE=operand
EOF
compare exp out || fail=1

# With -i (or a mere -), use only the assignments read from the file.
QUOTING_STYLE=literal env --from0=vars -i >all || fail=1
LC_ALL=C sort all >out || framework_failure_
cat <<\EOF >exp || framework_failure_
CU_ENV_FROM0_NEW=new
CU_ENV_FROM0_REMOVE=file
CU_ENV_FROM0_REPLACE=file
EOF
compare exp out || fail=1
QUOTING_STYLE=literal env --from0=vars - >all || fail=1
LC_ALL=C sort all >out || framework_failure_
compare exp out || fail=1

# Preserve values containing newlines, and accept an empty variable name.
printf 'A=line 1\nline 2\0' >values || framework_failure_
env -i --from0=values -0 >out || fail=1
compare values out || fail=1
printf '=value\0' >empty-name || framework_failure_
env -i --from0=empty-name -0 >out || fail=1
compare empty-name out || fail=1

# A later record for the same name takes precedence.
printf 'A=first\0B=second\0A=last\0' >duplicates || framework_failure_
cat <<\EOF >exp || framework_failure_
A=last
B=second
EOF
QUOTING_STYLE=literal env -i --from0=duplicates >all || fail=1
LC_ALL=C sort all >out || framework_failure_
compare exp out || fail=1

# Read '-' in binary mode from standard input.
printf 'A=stdin\0' >exp || framework_failure_
env -i --from0=- -0 <exp >out || fail=1
compare exp out || fail=1

# PATH loaded from the file must be used to find the command.
mkdir bin || framework_failure_
cat <<EOF >bin/from0-command || framework_failure_
#!$SHELL
echo found
EOF
chmod +x bin/from0-command || framework_failure_
printf 'PATH=%s\0' "$PWD/bin" >path || framework_failure_
echo found >exp || framework_failure_
env -i --from0=path from0-command >out || fail=1
compare exp out || fail=1

# An empty file makes no changes, unless -i also requests an empty base.
: >empty || framework_failure_
env -i --from0=empty -0 >out || fail=1
compare /dev/null out || fail=1

# Every nonempty record must be a complete NUL-terminated assignment.
printf 'A=unterminated' >invalid || framework_failure_
returns_ 125 env --from0=invalid >out 2>err || fail=1
printf 'not-an-assignment\0' >invalid || framework_failure_
returns_ 125 env --from0=invalid >out 2>err || fail=1
printf '\0' >invalid || framework_failure_
returns_ 125 env --from0=invalid >out 2>err || fail=1
returns_ 125 env --from0=does-not-exist >out 2>err || fail=1

# Invalid -u names are still diagnosed after loading variables from a file.
returns_ 125 env --from0=empty -u '' true || fail=1
returns_ 125 env --from0=empty -u A=B true || fail=1

Exit $fail
