CC ?= gcc
CFLAGS_BENCH ?= -std=gnu99 -O3
LFLAGS_BENCH ?= -lpng $(LDFLAGS)
CFLAGS_CONV ?= -std=c99 -O3
LFLAGS_CONV ?= $(LDFLAGS)
CFLAGS_DET ?= -std=c99 -Wall -Wextra -O3

TARGET_BENCH ?= qoibench
TARGET_CONV ?= qoiconv
TARGET_DET ?= qoidet

all: $(TARGET_BENCH) $(TARGET_CONV)

bench: $(TARGET_BENCH)
$(TARGET_BENCH):$(TARGET_BENCH).c qoi.h
	$(CC) $(CFLAGS_BENCH) $(CFLAGS) $(TARGET_BENCH).c -o $(TARGET_BENCH) $(LFLAGS_BENCH)

conv: $(TARGET_CONV)
$(TARGET_CONV):$(TARGET_CONV).c qoi.h
	$(CC) $(CFLAGS_CONV) $(CFLAGS) $(TARGET_CONV).c -o $(TARGET_CONV) $(LFLAGS_CONV)

det: $(TARGET_DET)
$(TARGET_DET):$(TARGET_DET).c qoi.h
	$(CC) $(CFLAGS_DET) $(CFLAGS) $(TARGET_DET).c -o $(TARGET_DET)

# Full determinism suite: repeated runs across -O0/-O2/-O3 and a
# UBSan+ASan build, with byte-offset mismatch reporting.
determinism:
	./tests/determinism.sh

.PHONY: clean determinism
clean:
	$(RM) $(TARGET_BENCH) $(TARGET_CONV) $(TARGET_DET)
	$(RM) -r .build-det
