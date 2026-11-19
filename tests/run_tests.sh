#!/bin/sh
# Build and run every check for the streaming QOI encoder.
#
#   ./tests/run_tests.sh           # library + driver tests
#   ./tests/run_tests.sh --quick   # library tests only (no 8K image)
#
# Builds and test data live in a temporary directory that is removed on exit;
# nothing is written into the repository. Requires a C compiler and python3
# (the latter only to generate the 8K test image).

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
status=0

say() { printf '\n=== %s ===\n' "$1"; }

say "build: strict C99 (pedantic, -Werror)"
if "$CC" -std=c99 -pedantic -Wall -Wextra -Werror -O2 -I"$ROOT" -I"$ROOT/tests" \
	"$ROOT/tests/qoi_stream_test.c" -o "$TMP/qoi_stream_test"; then
	echo "build OK"
else
	echo "build FAILED"
	exit 1
fi

say "run: library tests (split identity, resume state, resume chain, invalid input)"
"$TMP/qoi_stream_test" || status=1

say "build: qoistream and the reference encoder"
"$CC" -std=c99 -pedantic -Wall -Wextra -Werror -O2 -I"$ROOT" "$ROOT/qoistream.c" \
	-o "$TMP/qoistream" || status=1
"$CC" -std=gnu99 -Wall -Wextra -O2 -I"$ROOT" "$ROOT/tests/ref_encoder.c" \
	-o "$TMP/ref_encoder" || status=1

if [ "${1:-}" = "--quick" ]; then
	say "skipped: driver tests (--quick)"
else
	say "run: sanitizers on the library tests (ASan + UBSan)"
	if "$CC" -std=gnu99 -O1 -g -fsanitize=address,undefined \
		-fno-sanitize-recover=all -I"$ROOT" -I"$ROOT/tests" \
		"$ROOT/tests/qoi_stream_test.c" -o "$TMP/qoi_stream_test_san"; then
		"$TMP/qoi_stream_test_san" || status=1
	else
		echo "sanitizer build failed"
		status=1
	fi

	say "run: driver tests (8K, five quotas, identity, watermark, resume)"
	if [ -x "$TMP/qoistream" ] && [ -x "$TMP/ref_encoder" ]; then
		sh "$ROOT/tests/qoi_stream_driver.sh" "$TMP/qoistream" \
			"$TMP/ref_encoder" "$TMP/driver" || status=1
	else
		echo "skipped: build failed"
		status=1
	fi
fi

say "run: existing qoifuzz decoder harness (100k runs)"
if command -v clang >/dev/null 2>&1; then
	(
		cd "$TMP" || exit 1
		if clang -fsanitize=address,fuzzer -g -O1 -I"$ROOT" \
			"$ROOT/qoifuzz.c" -o qoifuzz 2>fuzz_build.log; then
			mkdir -p corpus
			./qoifuzz -runs=100000 corpus >fuzz.log 2>&1
			rc=$?
			tail -2 fuzz.log
			rm -f crash-* slow-unit-*
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

say "result"
if [ "$status" -eq 0 ]; then
	echo "ALL TESTS PASSED"
else
	echo "FAILURES PRESENT"
fi
exit "$status"
