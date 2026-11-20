# Host tests for the pure captive-portal decision.
#
# src/portal.cpp has no Arduino, WebServer or Wi-Fi dependency, so this builds
# the production source straight with cc and runs the test -- no PlatformIO.
#
#   make -C test/portal        builds and runs them
#   make -C test/portal clean  removes the binaries

CC ?= cc
SRC := ../../src/portal.cpp
HDR := ../../src/portal.h
CFLAGS ?= -O1 -g
WARN := -Wall -Wextra -Werror

all: portal_tests
	./portal_tests

portal_tests: portal.o test_portal.o
	$(CC) -o $@ $^

portal.o: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(WARN) -std=c++17 -I../../src -c -o $@ $(SRC)

test_portal.o: test_portal.c test.h $(HDR)
	$(CC) $(CFLAGS) $(WARN) -std=c11 -I../../src -c -o $@ test_portal.c

clean:
	rm -f portal_tests portal.o test_portal.o

.PHONY: all clean
