# Host tests for the portable Medtrum patch core.
#
# The core is portable C: no mbedTLS, no ESP-IDF, no PlatformIO. This builds
# the core sources plus the tests with plain cc and runs them.
#
#   make -C test/medtrum        builds and runs them

MED := ../../lib/medtrumpatch
SRCS := $(wildcard $(MED)/src/*.c) test_medtrum.c
CFLAGS ?= -O1 -g
WARN := -Wall -Wextra -Werror
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -I$(MED)/include -I.

all: medtrum_tests adv_tests
	./medtrum_tests
	./adv_tests

medtrum_tests: $(SRCS) $(wildcard $(MED)/include/*.h) test.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS)

# The 3-way advertising rotation and the three payload builders are in
# src/adv_rotation.h and src/adv_payload.h, the same pure headers src/radio.cpp
# uses, so this tests the production selection and bytes rather than a copy.
adv_tests: test_adv.c ../../src/adv_rotation.h ../../src/adv_payload.h
	$(CC) $(CFLAGS) $(WARN) -I../../src -o $@ test_adv.c

clean:
	rm -f medtrum_tests adv_tests

.PHONY: all clean
