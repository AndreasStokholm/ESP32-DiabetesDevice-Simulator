/* The one advertising set's 3-way rotation, as src/radio.cpp's tick() picks
 * it: the wanted devices in order (G7, pod, Medtrum), indexed by
 * (now_ms / 1500) % n. The selection itself lives in radio.cpp, which pulls in
 * NimBLE and Arduino and so cannot be built for the host; this header is the
 * same choice written as a pure function, so the host tests can cover it
 * (including that the old G7/pod alternation is preserved).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* The three choices match radio.cpp's local enum; ADV_NONE is this header's
 * sentinel for "nobody wants the set". */
enum {
    ADV_G7 = 0,
    ADV_POD,
    ADV_MED,
    ADV_NONE = -1,
};

/* Which devices want the set right now. */
typedef struct {
    int g7;  /* the G7 has an advertisement to make */
    int pod; /* the pod is unpaired, or no phone is connected */
    int med; /* the Medtrum pump base is on and no phone is connected */
} adv_wants_t;

/* The device that gets the set this tick, or ADV_NONE when nobody wants it. */
static inline int adv_rotation_slot(const adv_wants_t *w, long long now_ms) {
    int want[3], n = 0;
    if (w->g7) want[n++] = ADV_G7;
    if (w->pod) want[n++] = ADV_POD;
    if (w->med) want[n++] = ADV_MED;
    if (n == 0) return ADV_NONE;
    return want[(now_ms / 1500) % n];
}
