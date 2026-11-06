#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# run.sh [workdir]
#
# Builds tests/test_tuner.c with src/fm_band.c, src/fm_tuner.c and src/fm_rds.c on the host
# and runs it plain, under ASan + UBSan (gcc and clang), and under valgrind
# memcheck. Needs only glib's development files: the chip is a stand-in.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/../src
WORK=${1:-$HOME/claude-scratch/fmradio-tests}
mkdir -p "$WORK"
# A test that aborts under valgrind leaves a vgcore.<pid> where it runs: there,
# not in the source tree.
cd "$WORK" || exit 1
GLIB=$(pkg-config --cflags --libs glib-2.0)
WARN="-Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror"
FILES="$HERE/test_tuner.c $SRC/fm_band.c $SRC/fm_tuner.c $SRC/fm_rds.c"
fail=0

build() {
	name=$1; shift
	echo "== build $name"
	# shellcheck disable=SC2086
	"$@" -g -O1 -fno-omit-frame-pointer $WARN $FILES -o "$WORK/test-$name" $GLIB || fail=1
}

run() {
	name=$1; shift
	if "$@" "$WORK/test-$name" >"$WORK/$name.log" 2>&1; then
		echo "PASS $name ($(grep -c '^ok' "$WORK/$name.log") tests)"
	else
		echo "FAIL $name"; tail -30 "$WORK/$name.log"; fail=1
	fi
}

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

build gcc gcc
build gcc-asan gcc -fsanitize=address,undefined -fno-sanitize-recover=all
build clang-asan clang -fsanitize=address,undefined -fno-sanitize-recover=all

run gcc env
run gcc-asan env
run clang-asan env
if command -v valgrind >/dev/null 2>&1; then
	cp "$WORK/test-gcc" "$WORK/test-valgrind"
	run valgrind valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite
else
	echo "SKIP valgrind: not installed"
fi

exit $fail
