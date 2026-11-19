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

# The 3-way advertising rotation is chosen by src/adv_rotation.h, the same
# pure header src/radio.cpp's tick() uses, so this tests the production
# selection rather than a copy.
adv_tests: test_adv.c ../../src/adv_rotation.h
	$(CC) $(CFLAGS) $(WARN) -I../../src -o $@ test_adv.c

clean:
	rm -f medtrum_tests adv_tests

.PHONY: all clean
