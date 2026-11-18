# Endianness verification build.
# - strict warnings catch implicit conversions and type pitfalls
# - ASan+UBSan catch overflow/UB that could hide endianness bugs
CC ?= gcc
STD ?= -std=c99
WARN = -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
SAN  = -fsanitize=address,undefined -fno-sanitize-recover=all
OPT  = -O2

test: qoi_endian_test
	./qoi_endian_test

qoi_endian_test: qoi_endian_test.c qoi_alt.h qoi_be.h ../qoi.h
	$(CC) $(STD) $(OPT) $(WARN) $(SAN) $(CFLAGS) $< -o $@ $(LDFLAGS)

# Same harness built with each -O level and as C++ to catch UB/linkage issues.
matrix:
	for std in c99 gnu99 c11 c17; do \
	  for opt in -O0 -O1 -O2 -O3; do \
	    echo "--- $$std $$opt ---"; \
	    $(CC) -std=$$std $$opt -Wall -Wextra $(SAN) qoi_endian_test.c -o qet || exit 1; \
	    ./qet || exit 1; \
	  done; \
	done
	$(CC) -x c++ -std=c++11 -O2 -Wall -Wextra $(SAN) qoi_endian_test.c -o qetxx || exit 1
	./qetxx

clean:
	rm -f qoi_endian_test qet qetxx

.PHONY: test matrix clean
