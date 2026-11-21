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

all: medtrum_tests
	./medtrum_tests

medtrum_tests: $(SRCS) $(wildcard $(MED)/include/*.h) test.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS)

clean:
	rm -f medtrum_tests

.PHONY: all clean
