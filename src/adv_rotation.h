/* The one advertising set's rotation, shared by the firmware (src/radio.cpp)
 * and the host tests. Pure C/C++: no Arduino, no NimBLE.
 *
 * The board advertises the CGM (a Dexcom G6 transmitter or a G7 sensor), the
 * pod and the Medtrum patch on one set, so when more than one wants to be
 * found they take turns. The wanted devices are listed in a fixed order (CGM,
 * pod, Medtrum) and the clock picks the slot. The CGM slot serves either kind:
 * whatever kind the core is set to (G6, G7) wants the same slot.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* The three devices, in rotation order; ADV_NONE is "nobody wants the set".
 * ADV_G7 is the CGM slot and is kept under that name for the pods' tests: the
 * core's G7 and its G6 transmitter both advertise in it. */
enum {
    ADV_G7 = 0,
    ADV_POD,
    ADV_MED,
    ADV_NONE = -1,
};

/* The same slot, named for what it now carries. */
#define ADV_CGM ADV_G7

/* Which devices want the set right now. */
typedef struct {
    int g7;  /* the CGM (G6 or G7) has an advertisement to make */
    int pod; /* the pod is unpaired, or no phone is connected */
    int med; /* the Medtrum pump base is on and no phone is connected */
} adv_wants_t;

/* Whether the CGM wants the slot: either kind does, but "off" (0, as cgm.h
 * numbers it) does not. */
static inline int adv_cgm_wants(int kind, int advertising) {
    return kind != 0 && advertising;
}

/* The device that gets the set this tick: the wanted ones in order (CGM, pod,
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
