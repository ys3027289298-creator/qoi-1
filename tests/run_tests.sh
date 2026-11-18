#!/bin/sh
# Build and run every check for the configurable QOI cache.
#
#   ./tests/run_tests.sh            # correctness + existing fuzz harness
#   ./tests/run_tests.sh --bench    # also print the measurement tables
#
# Nothing is written into the repository: all binaries and fuzz corpora live in
# a temporary directory that is removed on exit. Requires a C compiler and,
# for the fuzz step, clang; both are already present in this environment.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM

status=0

say() { printf '\n=== %s ===\n' "$1"; }

say "build: strict C99 (pedantic, -Werror)"
if "$CC" -std=c99 -pedantic -Wall -Wextra -Werror -O2 -I"$ROOT" \
	"$ROOT/tests/qoi_cache_test.c" -o "$TMP/qoi_cache_test"; then
	echo "build OK"
else
	echo "build FAILED"
	status=1
fi

say "run: configurable cache correctness"
if [ -x "$TMP/qoi_cache_test" ]; then
	"$TMP/qoi_cache_test" || status=1
else
	echo "skipped (no binary)"
	status=1
fi

say "run: sanitizers (ASan + UBSan)"
if "$CC" -std=gnu99 -O1 -g -fsanitize=address,undefined \
	-fno-sanitize-recover=all -I"$ROOT" \
	"$ROOT/tests/qoi_cache_test.c" -o "$TMP/qoi_cache_test_san" 2>"$TMP/san_build.log"; then
	"$TMP/qoi_cache_test_san" || status=1
else
	echo "sanitizer build skipped/failed:"
	cat "$TMP/san_build.log"
fi

say "run: existing qoifuzz decoder harness (200k runs)"
if command -v clang >/dev/null 2>&1; then
	(
		cd "$TMP" || exit 1
		if clang -fsanitize=address,fuzzer -g -O1 -I"$ROOT" \
			"$ROOT/qoifuzz.c" -o qoifuzz 2>fuzz_build.log; then
			mkdir -p corpus
			./qoifuzz -runs=200000 corpus >fuzz.log 2>&1
			rc=$?
			tail -3 fuzz.log
			exit $rc
		else
			echo "fuzz build failed:"
			cat fuzz_build.log
			exit 1
		fi
	) || status=1
else
	echo "skipped: clang not found"
fi

if [ "${1:-}" = "--bench" ]; then
	say "bench: measurements"
	if "$CC" -std=gnu99 -O2 -I"$ROOT" \
		"$ROOT/tests/qoi_cache_bench.c" -o "$TMP/qoi_cache_bench"; then
		"$TMP/qoi_cache_bench" || status=1
	else
		status=1
	fi
fi

say "result"
if [ "$status" -eq 0 ]; then
	echo "ALL TESTS PASSED"
else
	echo "FAILURES PRESENT"
fi
exit "$status"
