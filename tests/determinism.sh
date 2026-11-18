#!/usr/bin/env bash
#
# Determinism driver for the QOI codec.
#
# What this proves:
#   1. Intra-process: each build encodes/decodes a fixed set of synthetic
#      images ITERS times in one process; the SHA-256 of every stream must be
#      identical across iterations.
#   2. Cross-process: the harness is invoked several times per build, so drift
#      caused by stale heap/stack contents (uninitialized-memory UB) is caught
#      as well.
#   3. Cross-optimization: the fixture streams dumped by -O0 / -O2 / -O3 (and
#      the UBSan+ASan build) are compared byte for byte. On mismatch the exact
#      first differing byte offset is reported.
#
# Environment:
#   CC     C compiler for the optimization builds (default: gcc)
#   SC     sanitizer compiler (default: clang, falling back to gcc)
#   ITERS  iterations per process (default: 10000)
#
# Exit status is non-zero if any check fails.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

CC="${CC:-gcc}"
SC="${SC:-clang}"
ITERS="${ITERS:-10000}"
BUILD_DIR="$ROOT/.build-det"

# label : extra CFLAGS
OPT_BUILDS=(
	"O0:-std=c99 -Wall -Wextra -O0"
	"O2:-std=c99 -Wall -Wextra -O2"
	"O3:-std=c99 -Wall -Wextra -O3"
)
SAN_FLAGS="-std=c99 -Wall -Wextra -g -O1 -fsanitize=undefined,address -fno-sanitize-recover=all"

# How many processes per build (cross-process determinism).
PROCS_PER_BUILD=3

failures=0
labels=()

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
bold()  { printf '\033[1m%s\033[0m\n' "$*"; }

mkdir -p "$BUILD_DIR"

# ---------------------------------------------------------------------------
# Build every configuration.
bold "== Building harness =="

for entry in "${OPT_BUILDS[@]}"; do
	label="${entry%%:*}"
	flags="${entry#*:}"
	labels+=("$label")
	echo "-- $CC $flags"
	# shellcheck disable=SC2086
	$CC $flags qoidet.c -o "$BUILD_DIR/qoidet-$label"
done

if ! command -v "$SC" >/dev/null 2>&1; then
	SC="$CC"
fi
labels+=("ubsan")
echo "-- $SC $SAN_FLAGS"
# shellcheck disable=SC2086
$SC $SAN_FLAGS qoidet.c -o "$BUILD_DIR/qoidet-ubsan"

# ---------------------------------------------------------------------------
# Intra- and cross-process determinism, per build.
bold "== Repeated-run determinism ($ITERS iterations x $PROCS_PER_BUILD processes per build) =="

for label in "${labels[@]}"; do
	bin="$BUILD_DIR/qoidet-$label"
	for proc in $(seq 1 "$PROCS_PER_BUILD"); do
		echo "-- $label run $proc/$PROCS_PER_BUILD"
		if ! "$bin" "$ITERS" > "$BUILD_DIR/log-$label-$proc.txt"; then
			red "FAIL: $label run $proc reported non-determinism or a sanitizer error"
			cat "$BUILD_DIR/log-$label-$proc.txt"
			failures=$((failures + 1))
		fi
	done
done

# ---------------------------------------------------------------------------
# Cross-build byte-for-byte comparison of dumped fixtures.
bold "== Cross-build fixture comparison =="

for label in "${labels[@]}"; do
	mkdir -p "$BUILD_DIR/dump-$label"
	"$BUILD_DIR/qoidet-$label" dump "$BUILD_DIR/dump-$label" \
		> "$BUILD_DIR/dump-log-$label.txt"
done

ref_label="${labels[0]}"
for label in "${labels[@]:1}"; do
	for ref_fixture in "$BUILD_DIR/dump-$ref_label"/*.qoi; do
		name="$(basename "$ref_fixture")"
		other="$BUILD_DIR/dump-$label/$name"
		echo "-- $ref_label vs $label: $name"
		if ! "$BUILD_DIR/qoidet-$ref_label" cmp "$ref_fixture" "$other"; then
			red "FAIL: stream differs between $ref_label and $label (offset above)"
			failures=$((failures + 1))
		fi
	done
done

# ---------------------------------------------------------------------------
if [[ "$failures" -eq 0 ]]; then
	green
	green "ALL DETERMINISM CHECKS PASSED"
	echo "  builds: ${labels[*]}"
	echo "  iterations/process: $ITERS, processes/build: $PROCS_PER_BUILD"
	echo "  fixtures in: $BUILD_DIR/dump-<build>/"
	exit 0
else
	red
	red "$failures CHECK(S) FAILED"
	exit 1
fi
