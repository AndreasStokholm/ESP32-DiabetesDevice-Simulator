/* Shared by the host tests. */
#pragma once
#include <stdio.h>
#include <string.h>
#include <stdint.h>

extern int failures, checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                    \
    } while (0)

static inline void dump(const char *what, const uint8_t *p, size_t n)
{
    fprintf(stderr, "  %s (%zu):", what, n);
    for (size_t i = 0; i < n; i++) {
        fprintf(stderr, "%s%02x", i % 32 ? "" : "\n    ", p[i]);
    }
    fputc('\n', stderr);
}

#define CHECK_BYTES(got, got_len, want, want_len)                                         \
    do {                                                                                  \
        checks++;                                                                         \
        if ((long)(got_len) < 0) {                                                        \
            failures++;                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s failed (%ld)\n", __FILE__, __LINE__, #got, (long)(got_len)); \
        } else if ((size_t)(got_len) != (size_t)(want_len) || memcmp(got, want, want_len) != 0) { \
            failures++;                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #got, #want);   \
            dump("got", got, got_len);                                                    \
            dump("want", want, want_len);                                                 \
        }                                                                                 \
    } while (0)

static inline size_t hex(const char *h, uint8_t *out)
{
    size_t n = 0;
    while (h[0] && h[1]) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (uint8_t)v;
        h += 2;
    }
    return n;
}

