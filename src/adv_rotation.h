/* The one advertising set's rotation, shared by the firmware (src/radio.cpp)
 * and the host tests. Pure C/C++: no Arduino, no NimBLE.
 *
 * The board advertises the pod, the G7 and the Medtrum patch on one set, so
 * when more than one wants to be found they take turns. The wanted devices are
 * listed in a fixed order (G7, pod, Medtrum) and the clock picks the slot.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The three devices, in rotation order; ADV_NONE is "nobody wants the set". */
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

/* The device that gets the set this tick: the wanted ones in order (G7, pod,
 * Medtrum), indexed by (now_ms / 1500) % n. ADV_NONE when nobody wants it. */
static inline int adv_rotation_slot(const adv_wants_t *w, long long now_ms) {
    int want[3], n = 0;
    if (w->g7) want[n++] = ADV_G7;
    if (w->pod) want[n++] = ADV_POD;
    if (w->med) want[n++] = ADV_MED;
    if (n == 0) return ADV_NONE;
    return want[(now_ms / 1500) % n];
}

#ifdef __cplusplus
}
#endif
