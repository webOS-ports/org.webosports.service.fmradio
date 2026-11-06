#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# static-analysis.sh
#
# Static-analysis gate for fmradiod: every file in src/. Fails on any finding from:
#
#   gcc -fanalyzer    use after free, double free, leaks (memory and file
#                     descriptors), NULL dereference, uninitialised reads
#   clang analyzer    the same from a second engine, with the alpha checkers
#                     for unix/security/core
#   clang-tidy        clang-analyzer-*, bugprone-*, cert-*, concurrency-*,
#                     misc-* and the like
#   cppcheck          all checks, exhaustive
#   flawfinder        race-prone calls (TOCTOU: access/stat before use), unsafe
#                     string and buffer functions; level 1 and up
#   sparse            type and endianness confusion
#   Coccinelle        the kernel's generic semantic checks (null, misc): set
#                     COCCI_DIR to a kernel tree's scripts/coccinelle
#
# Needs the headers of glib, luna-service2, pbnjson and libpulse: point
# FM_CFLAGS at them, e.g. FM_CFLAGS="-isystem $SYSROOT/usr/include/glib-2.0 \
#                      -isystem $SYSROOT/usr/lib/glib-2.0/include ...".
# A tool that is not installed is reported and skipped, not passed.
set -u

SRCDIR="$(cd "$(dirname "$0")/.." && pwd)/src"
CFLAGS="${FM_CFLAGS:-$(pkg-config --cflags glib-2.0 luna-service2 pbnjson_c libpulse-mainloop-glib 2>/dev/null)}"
COCCI_DIR="${COCCI_DIR:-$HOME/Documents/GitHub/linux-shr/scripts/coccinelle}"
fail=0
skipped=""

run() {
	name=$1; shift
	if ! command -v "$1" >/dev/null 2>&1; then
		skipped="$skipped $name"
		echo "== $name: not installed, skipped"
		return
	fi
	echo "== $name"
	out=$("$@" 2>&1)
	status=$?
	findings=$(printf '%s\n' "$out" | grep -E "${FILTER}" || true)
	# Findings always fail; a non-zero exit only does for tools whose exit
	# status means "could not analyse" rather than "found something"
	if [ -n "$findings" ] || { [ $status -ne 0 ] && [ "${IGNORE_STATUS:-0}" = 0 ]; }; then
		printf '%s\n' "${findings:-$out}"
		fail=1
	else
		echo "clean"
	fi
}


for SRC in "$SRCDIR"/*.c; do
# Findings are taken from the file under analysis only
FILE_RE=$(basename "$SRC" | sed 's/[.]/[.]/g')
echo "######## $(basename "$SRC")"

# Twice: at -O2 inlining hides some paths from the analyzer, at -O0 others.
for opt in -O0 -O2; do
FILTER='warning:|error:' IGNORE_STATUS=0 \
run "gcc -fanalyzer $opt" gcc -c -o /dev/null $opt -fanalyzer -Wanalyzer-too-complex \
	--param=analyzer-bb-explosion-factor=50 --param=analyzer-max-enodes-per-program-point=64 \
	-Wall -Wextra $CFLAGS "$SRC"
done

FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run "clang analyzer" clang --analyze -Xanalyzer -analyzer-output=text \
	-Xanalyzer -analyzer-checker=core,unix,deadcode,security,nullability,optin.portability,optin.core \
	-Xanalyzer -analyzer-checker=alpha.unix,alpha.security,alpha.core.PointerArithm,alpha.core.CastSize,alpha.core.Conversion,alpha.unix.cstring \
	-o /dev/null $CFLAGS "$SRC"

FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run "clang-tidy" clang-tidy --quiet "$SRC" \
	--checks='clang-analyzer-*,bugprone-*,cert-*,concurrency-*,misc-*,performance-*,portability-*,readability-misleading-indentation,readability-suspicious-call-argument,-bugprone-easily-swappable-parameters,-misc-include-cleaner,-misc-unused-parameters,-cert-err33-c,-bugprone-assignment-in-if-condition,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-bugprone-reserved-identifier,-cert-dcl37-c,-cert-dcl51-cpp' \
	-- $CFLAGS

FILTER=': (error|warning|style|performance|portability):' IGNORE_STATUS=1 \
run cppcheck cppcheck --enable=all --check-level=exhaustive --inconclusive --std=c11 \
	--library=gnu --library=posix --force --inline-suppr --quiet \
	--suppress=missingIncludeSystem --suppress=missingInclude --suppress=unmatchedSuppression \
	--suppress=checkersReport --suppress=unusedFunction --suppress=staticFunction \
	--template='{file}:{line}: {severity}: {message} [{id}]' \
	-D__linux__ -D__GNUC__ "$SRC"

FILTER='^.*:[0-9]+: +\[[1-5]\]' IGNORE_STATUS=1 \
run flawfinder flawfinder --minlevel=1 --falsepositive --columns --dataonly --quiet "$SRC"

FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run sparse sparse -Wsparse-all -Wno-declaration-after-statement -Wno-non-pointer-null \
	-Wno-decl -Wno-shadow $CFLAGS "$SRC"

if [ -d "$COCCI_DIR" ]; then
	for cocci in "$COCCI_DIR"/null/*.cocci "$COCCI_DIR"/misc/*.cocci; do
		FILTER="${FILE_RE}:[0-9]+" IGNORE_STATUS=1 \
		run "spatch $(basename "$cocci")" spatch --very-quiet -D report --no-includes \
			--sp-file "$cocci" "$SRC"
	done
else
	skipped="$skipped coccinelle"
fi
done

[ -n "$skipped" ] && echo "skipped:$skipped"
exit $fail
