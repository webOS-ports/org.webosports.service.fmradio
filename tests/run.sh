#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# run.sh [workdir]
#
# Builds each test (tests/test_tuner.c: MediaTek, tests/test_iris.c: Qualcomm)
# with the tuner sources on the host and runs it plain, under ASan + UBSan (gcc
# and clang), and under valgrind memcheck. Needs only glib's development files
# and the kernel's uapi headers: the chips are stand-ins.
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
LIBFILES="$SRC/fm_band.c $SRC/fm_tuner.c $SRC/fm_mtk.c $SRC/fm_iris.c $SRC/fm_rds.c"
TESTS="tuner iris"
fail=0

build() {
	name=$1; shift
	for t in $TESTS; do
		echo "== build $t-$name"
		# shellcheck disable=SC2086
		"$@" -g -O1 -fno-omit-frame-pointer $WARN "$HERE/test_$t.c" $LIBFILES \
			-o "$WORK/test-$t-$name" $GLIB || fail=1
	done
}

run() {
	name=$1; shift
	for t in $TESTS; do
		if "$@" "$WORK/test-$t-$name" >"$WORK/$t-$name.log" 2>&1; then
			echo "PASS $t-$name ($(grep -c '^ok' "$WORK/$t-$name.log") tests)"
		else
			echo "FAIL $t-$name"; tail -30 "$WORK/$t-$name.log"; fail=1
		fi
	done
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
	for t in $TESTS; do cp "$WORK/test-$t-gcc" "$WORK/test-$t-valgrind"; done
	run valgrind valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite
else
	echo "SKIP valgrind: not installed"
fi

exit $fail
