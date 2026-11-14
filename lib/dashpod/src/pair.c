/*
 * Pairing: an X25519 key exchange and CMAC confirmations that leave the pod
 * and phone sharing a long-term key (LTK).
 *
 * Like the Go simulator, the pod's private key and nonce are all zero, so
 * every pairing derives keys a test can predict. A simulator needs no secrecy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "dash_internal.h"

/* Reads "NAME" + 2-byte length + value fields in order. */
static int parse_fields(const dash_msg_t *m, const char *const *names, int count, const uint8_t **vals,
                        size_t *lens)
{
    const uint8_t *d = m->payload;
    size_t left = m->payload_len;
    for (int i = 0; i < count; i++) {
        size_t nl = strlen(names[i]);
        if (left < nl + 2 || memcmp(d, names[i], nl) != 0) {
            DLOGW("field %s not found", names[i]);
            return -1;
        }
        d += nl;
        left -= nl;
        size_t l = (size_t)d[0] << 8 | d[1];
        if (left < 2 + l) {
            DLOGW("field %s is truncated", names[i]);
            return -1;
        }
        vals[i] = d + 2;
        lens[i] = l;
        d += 2 + l;
        left -= 2 + l;
    }
    return 0;
}

static void put_field(dash_msg_t *m, const char *name, const uint8_t *v, size_t l)
{
    size_t nl = strlen(name);
    uint8_t *d = m->payload + m->payload_len;
    memcpy(d, name, nl);
    d[nl] = (uint8_t)(l >> 8);
    d[nl + 1] = (uint8_t)l;
    memcpy(d + nl + 2, v, l);
    m->payload_len += nl + 2 + l;
}

int dash_pair_parse_sp1sp2(dash_pair_t *c, const dash_msg_t *m)
{
    static const char *const names[] = {"SP1=", ",SP2="};
    const uint8_t *v[2];
    size_t l[2];
    if (parse_fields(m, names, 2, v, l) != 0) {
        return -1;
    }
    memset(c, 0, sizeof *c);
    memcpy(c->pod_id, m->dst, 4);
    memcpy(c->pdm_id, m->src, 4);
    DLOGI("pairing: SP1/SP2 from %02x%02x%02x%02x", c->pdm_id[0], c->pdm_id[1], c->pdm_id[2], c->pdm_id[3]);
    return 0;
}

static void derive(dash_pair_t *c)
{
    uint8_t first[16], inter[16], data[1 + 4 + 16 + 16 + 2];

    dash_x25519(c->shared, c->pod_priv, c->pdm_pub);

    memcpy(first, c->pod_pub + 28, 4);
    memcpy(first + 4, c->pdm_pub + 28, 4);
    memcpy(first + 8, c->pod_nonce + 12, 4);
    memcpy(first + 12, c->pdm_nonce + 12, 4);
    dash_cmac(first, c->shared, 32, inter);

    data[0] = 0x01;
    memcpy(data + 1, "TWIt", 4);
    memcpy(data + 5, c->pod_nonce, 16);
    memcpy(data + 21, c->pdm_nonce, 16);
    data[37] = 0x00;
    data[38] = 0x01;
    dash_cmac(inter, data, sizeof data, c->conf_key);
    data[0] = 0x02;
    dash_cmac(inter, data, sizeof data, c->ltk);
    c->have_ltk = true;

    uint8_t conf[6 + 32];
    memcpy(conf, "KC_2_U", 6);
    memcpy(conf + 6, c->pdm_nonce, 16);
    memcpy(conf + 22, c->pod_nonce, 16);
    dash_cmac(c->conf_key, conf, sizeof conf, c->pdm_conf);
    memcpy(conf, "KC_2_V", 6);
    memcpy(conf + 6, c->pod_nonce, 16);
    memcpy(conf + 22, c->pdm_nonce, 16);
    dash_cmac(c->conf_key, conf, sizeof conf, c->pod_conf);
}

int dash_pair_parse_sps1(dash_pair_t *c, const dash_msg_t *m)
{
    static const char *const names[] = {"SPS1="};
    const uint8_t *v[1];
    size_t l[1];
    if (parse_fields(m, names, 1, v, l) != 0) {
        return -1;
    }
    if (l[0] < 48) {
        DLOGW("SPS1 of %u bytes, want 48", (unsigned)l[0]);
        return -1;
    }
    memcpy(c->pdm_pub, v[0], 32);
    memcpy(c->pdm_nonce, v[0] + 32, 16);
    memset(c->pod_priv, 0, 32);
    c->pod_priv[0] &= 248;
    c->pod_priv[31] &= 127;
    c->pod_priv[31] |= 64;
    memset(c->pod_nonce, 0, 16);
    dash_x25519_base(c->pod_pub, c->pod_priv);
    derive(c);
    return 0;
}

void dash_pair_sps1(dash_pair_t *c, dash_msg_t *out)
{
    uint8_t v[48];
    memcpy(v, c->pod_pub, 32);
    memcpy(v + 32, c->pod_nonce, 16);
    dash_msg_new(out, DASH_MSG_PAIRING, c->pod_id, c->pdm_id);
    put_field(out, "SPS1=", v, sizeof v);
}

int dash_pair_parse_sps2(dash_pair_t *c, const dash_msg_t *m)
{
    static const char *const names[] = {"SPS2="};
    const uint8_t *v[1];
    size_t l[1];
    if (parse_fields(m, names, 1, v, l) != 0) {
        return -1;
    }
    if (l[0] != 16 || memcmp(v[0], c->pdm_conf, 16) != 0) {
        DLOGW("the phone's confirmation value is wrong");
        return -1;
    }
    return 0;
}

void dash_pair_sps2(dash_pair_t *c, dash_msg_t *out)
{
    dash_msg_new(out, DASH_MSG_PAIRING, c->pod_id, c->pdm_id);
    put_field(out, "SPS2=", c->pod_conf, 16);
}

int dash_pair_parse_sp0gp0(dash_pair_t *c, const dash_msg_t *m)
{
    (void)c;
    static const char want[] = "SP0,GP0";
    if (m->payload_len != sizeof want - 1 || memcmp(m->payload, want, sizeof want - 1) != 0) {
        DLOGW("expected SP0,GP0");
        return -1;
    }
    return 0;
}

void dash_pair_p0(dash_pair_t *c, dash_msg_t *out)
{
    static const uint8_t a5 = 0xa5;
    dash_msg_new(out, DASH_MSG_PAIRING, c->pod_id, c->pdm_id);
    put_field(out, "P0=", &a5, 1);
}
