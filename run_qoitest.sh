#!/bin/sh
# Build and run the qoi color-transform test suite in one go.
# Uses only a C compiler and the C library; no third-party code or network.
set -e

cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c99 -O3 -Wall -Wextra}"
OUT="${OUT:-./qoitest}"

echo "# Building: $CC $CFLAGS qoitest.c -o $OUT -lm"
$CC $CFLAGS qoitest.c -o "$OUT" -lm

echo "# Running $OUT"
"$OUT"
