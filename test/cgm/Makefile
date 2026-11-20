# Host tests for the portable CGM core (Dexcom G6/G7).
#
# Plain cc: no mbedTLS, no ESP-IDF, no PlatformIO. cgm.c's only platform
# dependency is the AES-128 block cipher and an RNG; platform_mbedtls.c is
# excluded (it includes the mbedTLS headers) and platform_aes.c supplies those
# two symbols for the test.
#
#   make -C test/cgm        builds and runs them

POD := ../../lib/dashpod
CGM := ../../lib/cgmsim

SRCS := $(CGM)/src/cgm.c $(POD)/src/crypto.c host_io.c platform_aes.c test_cgm.c main.c
CFLAGS ?= -O1 -g
WARN := -Wall -Wextra -Werror -Wno-unused-parameter
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -I$(POD)/include -I$(CGM)/include -I.

all: cgm_tests
	./cgm_tests

cgm_tests: $(SRCS) $(wildcard $(POD)/include/*.h) $(wildcard $(CGM)/include/*.h) test.h host_io.h
	$(CC) $(CFLAGS) $(WARN) -o $@ $(SRCS) -lpthread

clean:
	rm -f cgm_tests

.PHONY: all clean
