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

all: medtrum_tests adv_payload_tests serial_tests
	./medtrum_tests
	./adv_payload_tests
	./serial_tests

medtrum_tests: $(SRCS) $(wildcard $(MED)/include/*.h) test.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS)

# The three payload builders are in src/adv_payload.h, the same pure header
# src/radio.cpp uses, so this pins the production bytes rather than a copy.
adv_payload_tests: test_adv_payload.c ../../src/adv_payload.h
	$(CC) $(CFLAGS) $(WARN) -I../../src -o $@ test_adv_payload.c

# The app-order <-> wire-order serial conversion is in src/medtrum_serial.h, the
# same pure header src/medtrum_app.cpp includes, so the panel's serial is pinned.
serial_tests: test_serial.c ../../src/medtrum_serial.h
	$(CC) $(CFLAGS) $(WARN) -I../../src -o $@ test_serial.c

clean:
	rm -f medtrum_tests adv_payload_tests serial_tests

.PHONY: all clean
