/*
 * EAP-AKA session establishment: from the LTK and the phone's challenge,
 * Milenage gives the response (RES) and the session key (CK).
 *
 * Like the Go simulator, the pod doesn't check the phone's AUTN, and its
 * own IV is a constant.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "dash_internal.h"

enum { CODE_REQUEST = 1, CODE_RESPONSE = 2, CODE_SUCCESS = 3, CODE_FAILURE = 4 };
enum { AT_RAND = 1, AT_AUTN = 2, AT_RES = 3, AT_CUSTOM_IV = 126 };
#define EAP_AKA 0x17
#define SUBTYPE_AKA_CHALLENGE 1

static const uint8_t OP[16] = {0xcd, 0xc2, 0x02, 0xd5, 0x12, 0x3e, 0x20, 0xf6,
                               0x2b, 0x6d, 0x67, 0x6a, 0xc7, 0x2c, 0xb3, 0x18};

void dash_eap_new(dash_eap_t *e, const uint8_t ltk[16], uint64_t sqn)
{
    memset(e, 0, sizeof *e);
    memcpy(e->k, ltk, 16);
    e->sqn = sqn + 1;
    memset(e->pod_iv, 0x0a, 4);
}

typedef struct {
    uint8_t code, identifier;
    const uint8_t *rand, *autn, *iv;
} eap_pkt_t;

static int eap_parse(const uint8_t *d, size_t n, eap_pkt_t *out)
{
    memset(out, 0, sizeof *out);
    if (n < 4) {
        DLOGW("EAP packet too short");
        return -1;
    }
    out->code = d[0];
    if (out->code > CODE_FAILURE) {
        DLOGW("invalid EAP code %u", d[0]);
        return -1;
    }
    out->identifier = d[1];
    size_t len = (size_t)d[2] << 8 | d[3];
    if (len <= 4) {
        return 0; /* success / failure */
    }
    if (len < n || n < 8) {
        DLOGW("EAP length %u, packet %u", (unsigned)len, (unsigned)n);
        return -1;
    }
    if (d[4] != EAP_AKA) {
        DLOGW("not an EAP-AKA packet: type %u", d[4]);
        return -1;
    }
    const uint8_t *t = d + 8;
    size_t left = n - 8;
    while (left > 0) {
        if (left < 2) {
            return -1;
        }
        size_t al = (size_t)t[1] * 4;
        if (al < 4 || al > left) {
            DLOGW("bad EAP attribute length");
            return -1;
        }
        switch (t[0]) {
        case AT_RAND:
        case AT_AUTN:
            if (al != 20) {
                return -1;
            }
            if (t[0] == AT_RAND) {
                out->rand = t + 4;
            } else {
                out->autn = t + 4;
            }
            break;
        case AT_RES:
            if (al != 12 || ((t[2] << 8) | t[3]) != 64) {
                return -1;
            }
            break;
        case AT_CUSTOM_IV:
            if (al != 8) {
                return -1;
            }
            out->iv = t + 4;
            break;
        default:
            DLOGW("unknown EAP attribute %u", t[0]);
            return -1;
        }
        t += al;
        left -= al;
    }
    return 0;
}

int dash_eap_parse_challenge(dash_eap_t *e, const dash_msg_t *m)
{
    eap_pkt_t pkt;
    memcpy(e->pdm_id, m->src, 4);
    memcpy(e->pod_id, m->dst, 4);
    if (eap_parse(m->payload, m->payload_len, &pkt) != 0) {
        return -1;
    }
    if (!pkt.rand || !pkt.autn || !pkt.iv) {
        DLOGW("EAP challenge lacks RAND, AUTN or IV");
        return -1;
    }
    memcpy(e->rand, pkt.rand, 16);
    memcpy(e->autn, pkt.autn, 16);
    memcpy(e->pdm_iv, pkt.iv, 4);
    e->identifier = pkt.identifier;
    return 0;
}

int dash_eap_response(dash_eap_t *e, dash_msg_t *out)
{
    dash_milenage(e->k, OP, e->rand, e->res, e->ck, NULL, NULL);
    dash_msg_new(out, DASH_MSG_SESSION, e->pod_id, e->pdm_id);
    uint8_t *d = out->payload;
    size_t n = 0;
    d[n++] = CODE_RESPONSE;
    d[n++] = e->identifier;
    d[n++] = 0;
    d[n++] = 0; /* length, below */
    d[n++] = EAP_AKA;
    d[n++] = SUBTYPE_AKA_CHALLENGE;
    d[n++] = 0;
    d[n++] = 0;
    d[n++] = AT_RES;
    d[n++] = 3;
    d[n++] = 0;
    d[n++] = 64; /* RES length in bits */
    memcpy(d + n, e->res, 8);
    n += 8;
    d[n++] = AT_CUSTOM_IV;
    d[n++] = 2;
    d[n++] = 0;
    d[n++] = 0;
    memcpy(d + n, e->pod_iv, 4);
    n += 4;
    d[2] = (uint8_t)(n >> 8);
    d[3] = (uint8_t)n;
    out->payload_len = n;
    return 0;
}

int dash_eap_parse_success(const dash_msg_t *m)
{
    eap_pkt_t pkt;
    if (eap_parse(m->payload, m->payload_len, &pkt) != 0) {
        return -1;
    }
    if (pkt.code != CODE_SUCCESS) {
        DLOGW("EAP code %u, want success", pkt.code);
        return -1;
    }
    return 0;
}

void dash_eap_session(const dash_eap_t *e, uint8_t ck[16], uint8_t prefix[8])
{
    memcpy(ck, e->ck, 16);
    memcpy(prefix, e->pdm_iv, 4);
    memcpy(prefix + 4, e->pod_iv, 4);
}
