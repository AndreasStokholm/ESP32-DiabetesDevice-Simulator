#pragma once
/* Host-test scaffolding, in the style of test/medtrum/test.h. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern int failures, checks;

#define CHECK(cond)                                                         \
    do {                                                                    \
        checks++;                                                           \
        if (!(cond)) {                                                      \
            failures++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)
