# Host tests for the pure failure-injection state machine.
#
# src/fail.cpp has no Arduino, NimBLE or logging dependency, so this builds the
# production source straight with cc and runs the test -- no PlatformIO.
#
#   make -C test/fail        builds and runs them
#   make -C test/fail clean  removes the binaries

CC ?= cc
SRC := ../../src/fail.cpp
HDR := ../../src/fail.h
CFLAGS ?= -O1 -g
WARN := -Wall -Wextra -Werror

all: fail_tests
	./fail_tests

fail_tests: fail.o test_fail.o
	$(CC) -o $@ $^

fail.o: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(WARN) -std=c++17 -I../../src -c -o $@ $(SRC)

test_fail.o: test_fail.c test.h $(HDR)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -I../../src -c -o $@ test_fail.c

clean:
	rm -f fail_tests fail.o test_fail.o

.PHONY: all clean
