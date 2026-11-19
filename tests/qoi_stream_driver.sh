#!/bin/sh
# Driver tests for qoistream: 8K image, five quotas, byte identity, memory
# watermark and crash/resume.
#
#   sh tests/qoi_stream_driver.sh <qoistream-binary> <reference-binary> <tmpdir>
#
# The reference binary is tests/ref_encoder.c compiled to a plain
# qoi_encode() over the whole image; it is the definition of "identical".
#
# Everything is generated in <tmpdir>; nothing is written into the repository.

set -u

QS=${1:?usage: qoi_stream_driver.sh <qoistream> <reference> <tmpdir>}
REF=${2:?}
TMP=${3:?}
QS=$(cd "$(dirname "$QS")" && pwd)/$(basename "$QS")
REF=$(cd "$(dirname "$REF")" && pwd)/$(basename "$REF")
mkdir -p "$TMP" || exit 1
TMP=$(cd "$TMP" && pwd)

W=7680
H=4320
CH=4
RAW="$TMP/8k.raw"
QUOTAS="8 16 32 64 128"   # MiB
status=0
failures=0

fail() {
	echo "FAIL: $*"
	failures=$((failures + 1))
	status=1
}

note() {
	printf '\n--- %s ---\n' "$1"
}

# Monotonic milliseconds. `date +%s%N` is not used because the arithmetic is
# done in a 64-bit shell and a nanosecond timestamp overflows it on some
# builds, silently producing negative durations.
now_ms() {
	python3 -c 'import time; print(int(time.monotonic() * 1000))'
}

# --------------------------------------------------------------------------
note "generate 8K RGBA test image ($W x $H)"
# Four horizontal bands: gradient, flat, noise, moving pattern. Each band makes
# a different mix of QOI chunks, so a bug in one chunk type cannot hide.
if command -v python3 >/dev/null 2>&1; then
	python3 - "$RAW" "$W" "$H" "$CH" <<'PY' || exit 1
import sys, random
path, w, h, ch = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
random.seed(20261119)
row = bytearray(w * ch)
with open(path, 'wb') as f:
    for y in range(h):
        band = (y * 4) // h
        if band == 0:
            for x in range(w):
                i = x * ch
                row[i] = x * 255 // (w - 1)
                row[i + 1] = y * 255 // (h - 1)
                row[i + 2] = 128
                row[i + 3] = 255
        elif band == 1:
            v = 32 + ((y // 29) & 1) * 3
            row[:] = bytes([v, v, v, 255]) * w
        elif band == 2:
            row[:] = bytes(random.getrandbits(8) for _ in range(w * ch))
        else:
            for x in range(w):
                i = x * ch
                row[i] = (x ^ y) & 255
                row[i + 1] = (x * 3) & 255
                row[i + 2] = (y * 5) & 255
                row[i + 3] = 255 if ((x // 97 + y // 83) & 1) else 200
        f.write(row)
PY
else
	fail "python3 is needed to generate the 8K test image"
	exit 1
fi
[ "$(wc -c < "$RAW")" -eq $((W * H * CH)) ] || { fail "raw size"; exit 1; }
echo "raw: $(wc -c < "$RAW") bytes"

# --------------------------------------------------------------------------
note "reference: qoi_encode() over the whole image"
"$REF" "$RAW" "$W" "$H" "$CH" "$TMP/ref.qoi" || { fail "reference encoder"; exit 1; }
REF_SHA=$(sha256sum "$TMP/ref.qoi" | cut -d' ' -f1)
REF_SIZE=$(wc -c < "$TMP/ref.qoi")
echo "reference: $REF_SIZE bytes, sha256 $REF_SHA"

# --------------------------------------------------------------------------
note "byte identity across quotas and a timing/watermark table"
printf '%s\n' "quota  result  bytes     sha256  parts  budget     VmHWM       VmPeak      seconds"
first_sha=
first_size=
for MB in $QUOTAS; do
	OUT="$TMP/out-$MB.qoi"
	rm -rf "$OUT" "$OUT.parts"
	LOG="$TMP/run-$MB.log"
	START=$(now_ms)
	"$QS" --input "$RAW" --width $W --height $H --output "$OUT" \
		--quota-mb "$MB" --verify >"$LOG" 2>&1
	RC=$?
	END=$(now_ms)
	SECS=$(( END - START ))
	if [ "$RC" -ne 0 ]; then
		fail "quota ${MB}MiB: qoistream exit $RC"
		sed 's/^/    /' "$LOG"
		printf '%s\n' "$MB  FAIL"
		continue
	fi
	BYTES=$(wc -c < "$OUT")
	SHA=$(sha256sum "$OUT" | cut -d' ' -f1)
	PARTS=$(sed -n 's/.*, \([0-9][0-9]*\) parts,.*/\1/p' "$LOG" | tail -1)
	BUDGET=$(sed -n 's/.*budget \([0-9][0-9]*\) B.*/\1/p' "$LOG" | tail -1)
	HWM=$(sed -n 's/.*VmHWM \([0-9][0-9]*\) B.*/\1/p' "$LOG" | tail -1)
	PEAK=$(sed -n 's/.*VmPeak \([0-9][0-9]*\) B.*/\1/p' "$LOG" | tail -1)
	[ "$SHA" = "$REF_SHA" ] || fail "quota ${MB}MiB: output differs from qoi_encode()"
	[ "$BYTES" = "$REF_SIZE" ] || fail "quota ${MB}MiB: size $BYTES != $REF_SIZE"
	# The printed budget is a claimed upper bound on the process watermark.
	if [ -n "$BUDGET" ] && [ -n "$HWM" ] && [ "$HWM" -gt "$BUDGET" ]; then
		fail "quota ${MB}MiB: VmHWM $HWM exceeds the printed budget $BUDGET"
	fi
	if [ -n "$HWM" ] && [ "$HWM" -gt $((MB * 1024 * 1024)) ]; then
		fail "quota ${MB}MiB: VmHWM $HWM exceeds the quota"
	fi
	grep -q 'every decoded pixel matches' "$LOG" || fail "quota ${MB}MiB: --verify did not pass"
	[ -d "$OUT.parts" ] && fail "quota ${MB}MiB: work dir left behind"
	if [ -n "$first_sha" ] && [ "$SHA" != "$first_sha" ]; then
		fail "quota ${MB}MiB: output differs from the first quota"
	fi
	[ -n "$first_sha" ] || first_sha=$SHA
	[ -n "$first_size" ] || first_size=$BYTES
	printf '%s\n' "$MB  ok  $BYTES  $(echo "$SHA" | cut -c1-16)...  $PARTS  $BUDGET  $HWM  $PEAK  $SECS ms"
done

# --------------------------------------------------------------------------
note "hard address-space cap: the process runs under ulimit -v = quota"
# This is the strongest form of "stays inside the allowance": the kernel
# refuses the allocation instead of the program merely being careful.
for MB in $QUOTAS; do
	OUT="$TMP/ul-$MB.qoi"
	rm -rf "$OUT" "$OUT.parts"
	bash -c "ulimit -v $((MB * 1024)); ulimit -s 2048; \
		'$QS' --input '$RAW' --width $W --height $H --output '$OUT' \
		--quota-mb $MB --quiet" >"$TMP/ul-$MB.log" 2>&1
	RC=$?
	if [ "$RC" -ne 0 ]; then
		fail "ulimit -v ${MB}MiB: exit $RC"
		sed 's/^/    /' "$TMP/ul-$MB.log"
		continue
	fi
	cmp -s "$OUT" "$TMP/ref.qoi" || fail "ulimit -v ${MB}MiB: output differs"
	echo "ulimit -v ${MB}MiB: ok"
done

# --------------------------------------------------------------------------
note "timing is not dominated by the quota"
# After one warm-up pass the slowest quota may not be more than 4x slower than
# the fastest; a run that re-reads or re-encodes because of the quota would
# blow past that.
MIN=99999999
MAX=0
for MB in $QUOTAS; do
	OUT="$TMP/time-$MB.qoi"
	rm -rf "$OUT" "$OUT.parts"
	START=$(now_ms)
	"$QS" --input "$RAW" --width $W --height $H --output "$OUT" \
		--quota-mb "$MB" --quiet >/dev/null 2>&1 || { fail "quota ${MB} timing"; continue; }
	END=$(now_ms)
	T=$(( END - START ))
	[ "$T" -lt "$MIN" ] && MIN=$T
	[ "$T" -gt "$MAX" ] && MAX=$T
	rm -rf "$OUT" "$OUT.parts"
done
if [ "$MIN" -gt 0 ]; then
	RATIO=$(( (MAX * 100) / MIN ))
	echo "encode time: ${MIN}..${MAX} ms (max/min = $((RATIO))%)"
	[ "$RATIO" -le 400 ] || fail "encode time varies too much with the quota"
fi

# --------------------------------------------------------------------------
note "a run killed during assembly re-assembles without re-encoding"
# --keep leaves the finished stream on disk, which is exactly the state a
# process killed between "byte stream complete" and "output assembled" is in.
OUT="$TMP/assembled.qoi"
rm -rf "$OUT" "$OUT.parts"
"$QS" --input "$RAW" --width $W --height $H --output "$OUT" \
	--quota-mb 32 --keep --quiet >"$TMP/assembled.log" 2>&1 ||
	fail "assembled: first run"
rm -f "$OUT"
"$QS" --input "$RAW" --width $W --height $H --output "$OUT" \
	--quota-mb 32 --quiet >"$TMP/assembled2.log" 2>&1 ||
	fail "assembled: second run"
grep -q '(assembled)' "$TMP/assembled2.log" ||
	fail "assembled: second run did not reuse the finished stream"
cmp -s "$TMP/ref.qoi" "$OUT" || fail "assembled: output differs"

# --------------------------------------------------------------------------
# Start a run, kill it with SIGKILL after it has written some fragments, and
# report how many fragments survived. Usage: kill_midway <out> <quota> [chunk] [segment]
kill_midway() {
	_kout=$1
	_kq=$2
	_kc=${3:-262144}
	_ks=${4:-524288}
	rm -rf "$_kout" "$_kout.parts"
	"$QS" --input "$RAW" --width $W --height $H --output "$_kout" \
		--quota-mb "$_kq" --chunk "$_kc" --segment "$_ks" --quiet \
		>/dev/null 2>&1 &
	_kpid=$!
	sleep 0.2
	kill -9 "$_kpid" 2>/dev/null
	wait "$_kpid" 2>/dev/null
	ls "$_kout.parts"/part-*.qoi 2>/dev/null | wc -l
}

note "resume after SIGKILL, same command"
OUT="$TMP/resume.qoi"
PARTS_BEFORE=$(kill_midway "$OUT" 32)
[ -f "$OUT" ] && fail "output exists although the run was killed"
if [ "$PARTS_BEFORE" -eq 0 ]; then
	fail "no fragment survived the kill; nothing to resume from"
else
	echo "killed with $PARTS_BEFORE parts on disk"
	LOG="$TMP/resume.log"
	"$QS" --input "$RAW" --width $W --height $H --output "$OUT" \
		--quota-mb 32 --chunk 262144 --segment 524288 --verify >"$LOG" 2>&1
	RC=$?
	[ "$RC" -eq 0 ] || { fail "resume run exit $RC"; sed 's/^/    /' "$LOG"; }
	grep -q '^resume:' "$LOG" || fail "resume run did not report a resume"
	grep -q '^resume:' "$LOG" && echo "resumed: $(grep '^resume:' "$LOG")"
	[ -f "$OUT" ] || fail "resume produced no output"
	if [ -f "$OUT" ]; then
		[ "$(sha256sum "$OUT" | cut -d' ' -f1)" = "$REF_SHA" ] ||
			fail "resumed output differs from qoi_encode()"
	fi
fi

# --------------------------------------------------------------------------
note "resume with a different quota (the retry is not the same size)"
OUT="$TMP/resume-quota.qoi"
PARTS_BEFORE=$(kill_midway "$OUT" 8 65536 262144)
if [ "$PARTS_BEFORE" -eq 0 ]; then
	fail "no fragment survived the kill; nothing to resume from"
else
	echo "killed with $PARTS_BEFORE parts on disk"
	LOG="$TMP/resume-quota.log"
	"$QS" --input "$RAW" --width $W --height $H --output "$OUT" \
		--quota-mb 64 --verify >"$LOG" 2>&1
	RC=$?
	[ "$RC" -eq 0 ] || { fail "cross-quota resume exit $RC"; sed 's/^/    /' "$LOG"; }
	grep -q '^resume:' "$LOG" || fail "cross-quota resume did not reuse the checkpoint"
	[ -f "$OUT" ] && [ "$(sha256sum "$OUT" | cut -d' ' -f1)" = "$REF_SHA" ] ||
		fail "cross-quota resumed output differs from qoi_encode()"
fi

# --------------------------------------------------------------------------
note "a changed input prefix invalidates the checkpoint"
# Same image geometry and manifest, but a byte inside the prefix that the
# checkpoint already covered is different. The hash must reject the fragments,
# so the run starts over and the output describes the changed file.
OUT="$TMP/tamper.qoi"
PARTS_BEFORE=$(kill_midway "$OUT" 32 65536 1048576)
if [ "$PARTS_BEFORE" -gt 0 ]; then
	# state.bin layout: magic, version, parts, finished, out_bytes(u64),
	# consumed(u64). The test reads that field, not the whole checkpoint.
	CONSUMED=$(python3 -c 'import struct,sys; b=open(sys.argv[1],"rb").read(); print(struct.unpack_from("<Q", b, 24)[0])' "$OUT.parts/state.bin")
	echo "checkpointed prefix: $CONSUMED input bytes"
	cp "$RAW" "$TMP/tampered.raw"
	printf '\377' | dd of="$TMP/tampered.raw" bs=1 seek=$((CONSUMED - 1)) \
		conv=notrunc 2>/dev/null
	"$QS" --input "$TMP/tampered.raw" --width $W --height $H --output "$OUT" \
		--quota-mb 32 >"$TMP/tamper.log" 2>&1 || fail "tamper: rerun failed"
	grep -q '^resume:' "$TMP/tamper.log" &&
		fail "tamper: reused a checkpoint whose input prefix changed"
	"$REF" "$TMP/tampered.raw" "$W" "$H" "$CH" "$TMP/tampered-ref.qoi" ||
		fail "tamper: reference"
	cmp -s "$OUT" "$TMP/tampered-ref.qoi" ||
		fail "tamper: output does not describe the changed input"
else
	fail "tamper: no fragment survived the kill"
fi

# --------------------------------------------------------------------------
note "a changed input beyond the checkpoint still resumes correctly"
# The hash covers the consumed prefix only, so changing a byte the resumed run
# has not read yet must still resume - and the final bytes must reflect the
# change (the encoder re-reads that part from the file).
OUT="$TMP/tamper-tail.qoi"
PARTS_BEFORE=$(kill_midway "$OUT" 32 65536 1048576)
if [ "$PARTS_BEFORE" -gt 0 ]; then
	cp "$RAW" "$TMP/tamper-tail.raw"
	printf '\377' | dd of="$TMP/tamper-tail.raw" bs=1 seek=$((W * H * CH - 1)) \
		conv=notrunc 2>/dev/null
	"$QS" --input "$TMP/tamper-tail.raw" --width $W --height $H --output "$OUT" \
		--quota-mb 32 >"$TMP/tamper-tail.log" 2>&1 ||
		fail "tamper-tail: rerun failed"
	grep -q '^resume:' "$TMP/tamper-tail.log" ||
		fail "tamper-tail: did not resume although the prefix was intact"
	"$REF" "$TMP/tamper-tail.raw" "$W" "$H" "$CH" "$TMP/tamper-tail-ref.qoi" ||
		fail "tamper-tail: reference"
	cmp -s "$OUT" "$TMP/tamper-tail-ref.qoi" ||
		fail "tamper-tail: output does not describe the changed input"
else
	fail "tamper-tail: no fragment survived the kill"
fi

# --------------------------------------------------------------------------
note "resume refuses a checkpoint from a different image"
OTHER="$TMP/other.raw"
head -c $((64 * 64 * 4)) "$RAW" > "$OTHER"
OUT="$TMP/switch.qoi"
# The first run is killed so that it leaves a checkpoint behind; a completed
# run removes its work dir, and then there would be nothing to refuse.
PARTS_BEFORE=$(kill_midway "$OUT" 32 65536 262144)
if [ "$PARTS_BEFORE" -eq 0 ]; then
	fail "switch: first run left no checkpoint"
else
	# Same work dir, different image: must start over, not mix fragments.
	"$QS" --input "$OTHER" --width 64 --height 64 --output "$OUT" \
		--quota-mb 32 --verify >"$TMP/switch.log" 2>&1 || fail "switch: second run"
	grep -q 'different image' "$TMP/switch.log" ||
		fail "switch: no warning about the stale checkpoint"
	"$REF" "$OTHER" 64 64 4 "$TMP/other-ref.qoi" || fail "switch: reference"
	cmp -s "$TMP/other-ref.qoi" "$OUT" || fail "switch: output is not the new image"
fi

# --------------------------------------------------------------------------
note "small image, 3 channels, explicit fragment size"
SMALL="$TMP/small.raw"
head -c $((321 * 97 * 3)) "$RAW" > "$SMALL"
"$REF" "$SMALL" 321 97 3 "$TMP/small-ref.qoi" || fail "small: reference"
for SEG in 4096 1000000; do
	OUT="$TMP/small-$SEG.qoi"
	rm -rf "$OUT" "$OUT.parts"
	"$QS" --input "$SMALL" --width 321 --height 97 --channels 3 \
		--output "$OUT" --quota-mb 8 --segment $SEG --verify \
		>"$TMP/small-$SEG.log" 2>&1 || { fail "small: segment $SEG"; continue; }
	cmp -s "$OUT" "$TMP/small-ref.qoi" || fail "small: segment $SEG output differs"
done

# --------------------------------------------------------------------------
note "quota too small is refused instead of crashing"
OUT="$TMP/tiny.qoi"
rm -rf "$OUT" "$OUT.parts"
"$QS" --input "$OTHER" --width 64 --height 64 --output "$OUT" \
	--quota 100 --quiet >"$TMP/tiny.log" 2>&1
RC=$?
[ "$RC" -eq 2 ] || fail "tiny quota: expected exit 2, got $RC"
head -1 "$TMP/tiny.log" | sed 's/^/    /'

# --------------------------------------------------------------------------
note "result"
if [ "$failures" -eq 0 ]; then
	echo "DRIVER TESTS PASSED"
else
	echo "DRIVER TESTS FAILED: $failures failure(s)"
fi
exit "$status"
