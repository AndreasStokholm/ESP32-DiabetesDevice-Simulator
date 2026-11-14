/*
 * The "TW" message (CTwiPacket) and its AES-CCM encryption.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "dash_internal.h"

#define BIT(i) (uint8_t)(1u << (7 - (i)))

void dash_msg_new(dash_msg_t *m, uint8_t type, const uint8_t src[4], const uint8_t dst[4])
{
    memset(m, 0, sizeof *m);
    m->type = type;
    memcpy(m->src, src, 4);
    memcpy(m->dst, dst, 4);
}

int dash_msg_marshal(const dash_msg_t *m, uint8_t *out, size_t size)
{
    size_t n = 16 + m->payload_len;
    if (n > size) {
        return -1;
    }
    uint8_t f = 0;
    out[0] = 'T';
    out[1] = 'W';
    if (m->version & 4) f |= BIT(0);
    if (m->version & 2) f |= BIT(1);
    if (m->version & 1) f |= BIT(2);
    if (m->sas) f |= BIT(3);
    if (m->tfs) f |= BIT(4);
    if (m->eqos & 4) f |= BIT(5);
    if (m->eqos & 2) f |= BIT(6);
    if (m->eqos & 1) f |= BIT(7);
    out[2] = f;
    /* The Go simulator never clears its flag byte between the two, so the
     * second carries the first's bits too. Phones accept it; keep it. */
    if (m->ack) f |= BIT(0);
    if (m->priority) f |= BIT(1);
    if (m->last_message) f |= BIT(2);
    if (m->gateway) f |= BIT(3);
    if (m->type & 8) f |= BIT(4);
    if (m->type & 4) f |= BIT(5);
    if (m->type & 2) f |= BIT(6);
    if (m->type & 1) f |= BIT(7);
    out[3] = f;
    out[4] = m->seq;
    out[5] = m->ack_num;
    /* An 11-bit length. An encrypted message gives its plaintext's length. */
    size_t l = m->encrypted && m->payload_len >= 8 ? m->payload_len - 8 : m->payload_len;
    out[6] = (uint8_t)(l >> 3);
    out[7] = (uint8_t)(l << 5);
    memcpy(out + 8, m->src, 4);
    memcpy(out + 12, m->dst, 4);
    memcpy(out + 16, m->payload, m->payload_len);
    return (int)n;
}

int dash_msg_unmarshal(const uint8_t *raw, size_t n, dash_msg_t *m)
{
    memset(m, 0, sizeof *m);
    if (n < 16) {
        DLOGW("message of %u bytes is too short", (unsigned)n);
        return -1;
    }
    if (raw[0] != 'T' || raw[1] != 'W') {
        DLOGW("message has no TW magic");
        return -1;
    }
    uint8_t f = raw[2];
    m->sas = f & BIT(3);
    m->tfs = f & BIT(4);
    m->version = (uint8_t)(((f & BIT(0)) ? 4 : 0) | ((f & BIT(1)) ? 2 : 0) | ((f & BIT(2)) ? 1 : 0));
    m->eqos = (uint8_t)(((f & BIT(5)) ? 4 : 0) | ((f & BIT(6)) ? 2 : 0) | ((f & BIT(7)) ? 1 : 0));
    f = raw[3];
    m->ack = f & BIT(0);
    m->priority = f & BIT(1);
    m->last_message = f & BIT(2);
    m->gateway = f & BIT(3);
    m->type = (uint8_t)(((f & BIT(4)) ? 8 : 0) | ((f & BIT(5)) ? 4 : 0) | ((f & BIT(6)) ? 2 : 0) |
                        ((f & BIT(7)) ? 1 : 0));
    if (m->type > DASH_MSG_ENCRYPTED_SIGNED) {
        DLOGW("invalid message type %u", m->type);
        return -1;
    }
    if (m->version != 0) {
        DLOGW("invalid message version %u", m->version);
        return -1;
    }
    m->seq = raw[4];
    m->ack_num = raw[5];
    size_t len = ((size_t)raw[6] << 3) | (raw[7] >> 5);
    memcpy(m->header, raw, 16);
    memcpy(m->src, raw + 8, 4);
    memcpy(m->dst, raw + 12, 4);
    if (m->type == DASH_MSG_ENCRYPTED || m->type == DASH_MSG_ENCRYPTED_SIGNED) {
        len += 8; /* the tag */
        m->encrypted = true;
    }
    if (len > n - 16 || len > DASH_MSG_MAX) {
        DLOGW("message length %u, but %u bytes remain", (unsigned)len, (unsigned)(n - 16));
        return -1;
    }
    if (m->type == DASH_MSG_ENCRYPTED_SIGNED) {
        /* The controller's ECDSA signature follows, outside the length. The
         * simulator doesn't check it. */
        if (n - 16 - len < 64) {
            DLOGW("signed message without its signature");
            return -1;
        }
        m->signed_ = true;
    }
    m->payload_len = len;
    memcpy(m->payload, raw + 16, len);
    return 0;
}

void dash_nonce(const uint8_t prefix[8], uint64_t seq, bool pod_receiving, uint8_t nonce[13])
{
    seq &= 549755813887ull; /* 39 bits */
    memcpy(nonce, prefix, 8);
    nonce[8] = (uint8_t)(seq >> 32);
    nonce[9] = (uint8_t)(seq >> 24);
    nonce[10] = (uint8_t)(seq >> 16);
    nonce[11] = (uint8_t)(seq >> 8);
    nonce[12] = (uint8_t)seq;
    if (pod_receiving) {
        nonce[8] &= 127;
    } else {
        nonce[8] |= 128;
    }
}

int dash_encrypt_raw(const uint8_t ck[16], const uint8_t prefix[8], uint64_t seq, uint8_t *raw, size_t n,
                     size_t size)
{
    uint8_t nonce[13], header[16];
    if (n < 16 || n + 8 > size) {
        return -1;
    }
    dash_nonce(prefix, seq, false, nonce);
    memcpy(header, raw, 16);
    dash_ccm_encrypt(ck, nonce, header, 16, raw + 16, n - 16, raw + 16);
    return (int)(n + 8);
}

int dash_decrypt_msg(const uint8_t ck[16], const uint8_t prefix[8], uint64_t seq, dash_msg_t *m)
{
    uint8_t nonce[13];
    if (!m->encrypted || m->payload_len < 8) {
        return -1;
    }
    dash_nonce(prefix, seq, true, nonce);
    if (dash_ccm_decrypt(ck, nonce, m->header, 16, m->payload, m->payload_len, m->payload) != 0) {
        return -1;
    }
    m->payload_len -= 8;
    m->encrypted = false;
    return 0;
}
