/*
 * Omnipod 5: the pod's side of OmnipodKit's O5LTKExchanger (P-256 pairing
 * with certificates), the pod's certificate, and answers to the AID setup
 * commands (O5AidCommands).
 *
 * The pod's certificate is self-signed with a key the simulator generates:
 * OmnipodKit reads the key from it and checks the pod's signature, but not
 * who issued it. Nothing here comes from, or stands in for, Insulet's keys.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>

#include "dash_internal.h"

/* OmnipodKit's O5LTKExchanger.FIRMWARE_ID. */
const uint8_t DASH_O5_FIRMWARE_ID[6] = {0x9b, 0x0a, 0xb9, 0x6a, 0x76, 0xf4};

#define O5_ALGORITHM_P256_CERT 0x09

/* ---- "KEY=" + 2-byte length + value fields ----------------------------------- */

static int get_field(const dash_msg_t *m, const char *key, const uint8_t **val, size_t *len)
{
    size_t kl = strlen(key);
    if (m->payload_len < kl + 2 || memcmp(m->payload, key, kl) != 0) {
        DLOGW("expected %s", key);
        return -1;
    }
    size_t l = (size_t)m->payload[kl] << 8 | m->payload[kl + 1];
    if (m->payload_len < kl + 2 + l) {
        DLOGW("%s is truncated", key);
        return -1;
    }
    *val = m->payload + kl + 2;
    *len = l;
    return 0;
}

static void reply(const dash_o5_pair_t *c, dash_msg_t *out, const char *key, const uint8_t *val, size_t len)
{
    dash_msg_new(out, DASH_MSG_PAIRING, c->pod_id, c->pdm_id);
    out->sas = true;
    size_t kl = strlen(key);
    memcpy(out->payload, key, kl);
    out->payload[kl] = (uint8_t)(len >> 8);
    out->payload[kl + 1] = (uint8_t)len;
    memcpy(out->payload + kl + 2, val, len);
    out->payload_len = kl + 2 + len;
}

int dash_o5_parse_sp1sp2(dash_o5_pair_t *c, const dash_msg_t *m)
{
    const uint8_t *v;
    size_t l;
    if (get_field(m, "SP1=", &v, &l) != 0) {
        return -1;
    }
    const uint8_t *key = c->identity_key;
    memset(c, 0, sizeof *c);
    c->identity_key = key;
    memcpy(c->pod_id, m->dst, 4);
    memcpy(c->pdm_id, m->src, 4);
    DLOGI("O5 pairing: SP1/SP2 from %02x%02x%02x%02x", c->pdm_id[0], c->pdm_id[1], c->pdm_id[2], c->pdm_id[3]);
    return 0;
}

int dash_o5_sps0(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out)
{
    const uint8_t *v;
    size_t l;
    if (get_field(in, "SPS0=", &v, &l) != 0 || l != 5) {
        return -1;
    }
    uint16_t crc = (uint16_t)(v[3] << 8 | v[4]);
    if (dash_crc16_xmodem(v, 3) != crc) {
        DLOGW("SPS0 CRC mismatch");
        return -1;
    }
    c->algorithm = v[2];
    if (c->algorithm != O5_ALGORITHM_P256_CERT) {
        DLOGW("SPS0 asks for algorithm 0x%02x; the simulator answers 0x09 (P-256 with certificates)", c->algorithm);
    }
    uint8_t r[5] = {0x00, 0x00, O5_ALGORITHM_P256_CERT, 0, 0}; /* direction 0x00: pod to phone */
    crc = dash_crc16_xmodem(r, 3);
    r[3] = (uint8_t)(crc >> 8);
    r[4] = (uint8_t)crc;
    reply(c, out, "SPS0=", r, sizeof r);
    return 0;
}

static void put_be64(uint8_t *p, uint64_t v)
{
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

/* conf || ltk = SHA-256 of the length-prefixed firmware id, four zero bytes,
 * both public keys and the shared secret (O5KeyExchange.o5generateKeys). */
void dash_o5_derive(dash_o5_pair_t *c, const uint8_t shared[32])
{
    uint8_t buf[8 + 6 + 8 + 4 + 8 + 64 + 8 + 64 + 8 + 32], *p = buf;
    put_be64(p, 6);
    memcpy(p + 8, DASH_O5_FIRMWARE_ID, 6);
    p += 14;
    put_be64(p, 4);
    memset(p + 8, 0, 4);
    p += 12;
    put_be64(p, 64);
    memcpy(p + 8, c->pdm_pub, 64);
    p += 72;
    put_be64(p, 64);
    memcpy(p + 8, c->pod_pub, 64);
    p += 72;
    put_be64(p, 32);
    memcpy(p + 8, shared, 32);
    uint8_t digest[32];
    dash_sha256(buf, sizeof buf, digest);
    memcpy(c->conf, digest, 16);
    memcpy(c->ltk, digest + 16, 16);
}

int dash_o5_sps1(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out)
{
    const uint8_t *v;
    size_t l;
    uint8_t shared[32];
    if (get_field(in, "SPS1=", &v, &l) != 0 || l != 80) {
        DLOGW("SPS1 of %u bytes, want 80", (unsigned)l);
        return -1;
    }
    memcpy(c->pdm_pub, v, 64);
    memcpy(c->pdm_nonce, v + 64, 16);
    if (dash_p256_keygen(c->pod_priv) != 0) {
        return -1;
    }
    dash_random(c->pod_nonce, 16);
    if (dash_p256_public(c->pod_priv, c->pod_pub) != 0 || dash_p256_ecdh(c->pod_priv, c->pdm_pub, shared) != 0) {
        DLOGW("the phone's P-256 key is not valid");
        return -1;
    }
    dash_o5_derive(c, shared);
    uint8_t r[80];
    memcpy(r, c->pod_pub, 64);
    memcpy(r + 64, c->pod_nonce, 16);
    reply(c, out, "SPS1=", r, sizeof r);
    return 0;
}

/* The 13-byte nonce: 01 + pdm nonce[0..6] + pod nonce[0..6] for the phone's
 * messages, 02 + pod nonce[0..6] + pdm nonce[0..6] for the pod's. */
void dash_o5_sps_nonce(const dash_o5_pair_t *c, bool from_pdm, uint8_t nonce[13])
{
    nonce[0] = from_pdm ? 0x01 : 0x02;
    memcpy(nonce + 1, from_pdm ? c->pdm_nonce : c->pod_nonce, 6);
    memcpy(nonce + 7, from_pdm ? c->pod_nonce : c->pdm_nonce, 6);
}

/* Each side's nonce counts its messages in its first 8 bytes, little-endian. */
void dash_o5_nonce_increment(uint8_t nonce[16])
{
    for (int i = 0; i < 8; i++) {
        if (++nonce[i] != 0) {
            break;
        }
    }
}

/* What the pod signs in SPS2 (O5KeyExchange.buildPodChannelBindingTranscript). */
void dash_o5_pod_transcript(const dash_o5_pair_t *c, uint8_t out[171])
{
    out[0] = 0x02;
    memset(out + 1, 0, 4);
    memcpy(out + 5, DASH_O5_FIRMWARE_ID, 6);
    memcpy(out + 11, c->pod_pub, 64);
    memcpy(out + 75, c->pdm_pub, 64);
    memcpy(out + 139, c->pod_nonce, 16);
    memcpy(out + 155, c->pdm_nonce, 16);
}

/* Decrypts the phone's SPS2.1 / SPS2 in place into buf. Returns its length. */
static int open_phone(dash_o5_pair_t *c, const dash_msg_t *in, const char *key, uint8_t *buf)
{
    const uint8_t *v;
    size_t l;
    uint8_t nonce[13];
    if (get_field(in, key, &v, &l) != 0 || l < 8) {
        return -1;
    }
    dash_o5_sps_nonce(c, true, nonce);
    if (dash_ccm_decrypt(c->conf, nonce, NULL, 0, v, l, buf) != 0) {
        DLOGW("could not decrypt the phone's %s", key);
        return -1;
    }
    dash_o5_nonce_increment(c->pdm_nonce);
    return (int)(l - 8);
}

static void seal_pod(dash_o5_pair_t *c, dash_msg_t *out, const char *key, const uint8_t *pt, size_t n)
{
    static uint8_t ct[DASH_MSG_MAX];
    uint8_t nonce[13];
    dash_o5_sps_nonce(c, false, nonce);
    dash_ccm_encrypt(c->conf, nonce, NULL, 0, pt, n, ct);
    dash_o5_nonce_increment(c->pod_nonce);
    reply(c, out, key, ct, n + 8);
}

int dash_o5_sps2_1(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out)
{
    static uint8_t buf[DASH_MSG_MAX];
    int n = open_phone(c, in, "SPS2.1=", buf);
    if (n < 0) {
        return -1;
    }
    DLOGI("O5 pairing: the phone's certificate, %d bytes", n);
    n = dash_o5_certificate(c->identity_key, buf, sizeof buf);
    if (n < 0) {
        return -1;
    }
    seal_pod(c, out, "SPS2.1=", buf, (size_t)n);
    return 0;
}

int dash_o5_sps2(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out)
{
    static uint8_t buf[DASH_MSG_MAX];
    int n = open_phone(c, in, "SPS2=", buf);
    if (n < 64) {
        return -1;
    }
    DLOGI("O5 pairing: the phone's certificate and signature, %d bytes", n);
    n = dash_o5_certificate(c->identity_key, buf, sizeof buf - 64);
    if (n < 0) {
        return -1;
    }
    uint8_t transcript[171];
    dash_o5_pod_transcript(c, transcript);
    if (dash_p256_sign(c->identity_key, transcript, sizeof transcript, buf + n) != 0) {
        return -1;
    }
    seal_pod(c, out, "SPS2=", buf, (size_t)n + 64);
    return 0;
}

/* ---- the pod's certificate ------------------------------------------------------ */

static size_t der_len(uint8_t *out, size_t len)
{
    if (len < 0x80) {
        out[0] = (uint8_t)len;
        return 1;
    }
    if (len < 0x100) {
        out[0] = 0x81;
        out[1] = (uint8_t)len;
        return 2;
    }
    out[0] = 0x82;
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)len;
    return 3;
}

/* Writes tag, length and value at out. Returns the bytes written. */
static size_t tlv(uint8_t *out, uint8_t tag, const uint8_t *val, size_t len)
{
    out[0] = tag;
    size_t h = 1 + der_len(out + 1, len);
    memmove(out + h, val, len);
    return h + len;
}

/* A DER INTEGER from a 32-byte unsigned big-endian number. */
static size_t der_uint(uint8_t *out, const uint8_t v[32])
{
    size_t i = 0;
    while (i < 31 && v[i] == 0) {
        i++;
    }
    uint8_t tmp[33];
    size_t n = 0;
    if (v[i] & 0x80) {
        tmp[n++] = 0;
    }
    memcpy(tmp + n, v + i, 32 - i);
    return tlv(out, 0x02, tmp, n + 32 - i);
}

int dash_o5_certificate(const uint8_t key[32], uint8_t *out, size_t size)
{
    static const uint8_t version[] = {0xa0, 0x03, 0x02, 0x01, 0x02};
    static const uint8_t ecdsa_sha256[] = {0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};
    static const uint8_t spki_header[] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d,
                                          0x02, 0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01,
                                          0x07, 0x03, 0x42, 0x00, 0x04};
    static const char cn[] = "ESP-Sim Omnipod 5 pod simulator";
    static const char validity[] = "\x17\x0d" "260101000000Z" "\x17\x0d" "491231235959Z";
    uint8_t pub[64], hash[32], tbs[400], tmp[200], name[80], sig[64], sigder[80], body[600];
    size_t n, t;

    if (dash_p256_public(key, pub) != 0) {
        return -1;
    }

    /* Name: SEQUENCE { SET { SEQUENCE { OID commonName, UTF8String } } } */
    static const uint8_t cn_oid[] = {0x06, 0x03, 0x55, 0x04, 0x03};
    n = 0;
    memcpy(tmp, cn_oid, sizeof cn_oid);
    n = sizeof cn_oid + tlv(tmp + sizeof cn_oid, 0x0c, (const uint8_t *)cn, sizeof cn - 1);
    n = tlv(name, 0x30, tmp, n);
    n = tlv(tmp, 0x31, name, n);
    size_t name_len = tlv(name, 0x30, tmp, n);

    /* The serial: from the key, positive, 8 bytes. */
    dash_sha256(pub, 64, hash);
    hash[0] = (uint8_t)((hash[0] & 0x7f) | 0x40);

    t = 0;
    memcpy(body + t, version, sizeof version);
    t += sizeof version;
    t += tlv(body + t, 0x02, hash, 8);
    memcpy(body + t, ecdsa_sha256, sizeof ecdsa_sha256);
    t += sizeof ecdsa_sha256;
    memcpy(body + t, name, name_len);
    t += name_len;
    t += tlv(body + t, 0x30, (const uint8_t *)validity, sizeof validity - 1);
    memcpy(body + t, name, name_len);
    t += name_len;
    memcpy(body + t, spki_header, sizeof spki_header);
    t += sizeof spki_header;
    memcpy(body + t, pub, 64);
    t += 64;
    size_t tbs_len = tlv(tbs, 0x30, body, t);

    if (dash_p256_sign(key, tbs, tbs_len, sig) != 0) {
        return -1;
    }
    n = der_uint(tmp, sig);
    n += der_uint(tmp + n, sig + 32);
    n = tlv(sigder + 1, 0x30, tmp, n);
    sigder[0] = 0x00; /* BIT STRING: no unused bits */

    t = 0;
    memcpy(body, tbs, tbs_len);
    t = tbs_len;
    memcpy(body + t, ecdsa_sha256, sizeof ecdsa_sha256);
    t += sizeof ecdsa_sha256;
    t += tlv(body + t, 0x03, sigder, n + 1);
    if (t + 4 > size) {
        return -1;
    }
    return (int)tlv(out, 0x30, body, t);
}

/* ---- AID commands ----------------------------------------------------------- */

static bool is_key(const uint8_t *p, size_t n)
{
    bool dot = false;
    if (n == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '.') {
            dot = true;
        } else if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return dot;
}

static const uint8_t *find(const uint8_t *p, size_t n, uint8_t c)
{
    return memchr(p, c, n);
}

int dash_o5_aid(const uint8_t *cmd, size_t n, uint8_t *out, size_t size)
{
    const uint8_t *eq;
    size_t kl, l = 0;

    if (n > 2 && cmd[0] == 'S' && cmd[1] == 'E') {
        /* SE<f>.<a>=<data>: an extended set, answered ES<f>.<a>=0 */
        if (!(eq = find(cmd, n, '=')) || !is_key(cmd + 2, (size_t)(eq - cmd - 2))) {
            return -1;
        }
        kl = (size_t)(eq - cmd - 2);
        if (kl + 5 > size) {
            return -1;
        }
        out[l++] = 'E';
        out[l++] = 'S';
        memcpy(out + l, cmd + 2, kl);
        l += kl;
        out[l++] = '=';
        out[l++] = '0';
        DLOGI("AID %.*s set (%u bytes)", (int)kl, cmd + 2, (unsigned)(n - kl - 3));
        return (int)l;
    }
    if (n > 1 && cmd[0] == 'S') {
        /* S<f>.<a>=<data>,G<f>.<a>: set, then get: answered <f>.<a>=<data> */
        if (!(eq = find(cmd, n, '=')) || !is_key(cmd + 1, (size_t)(eq - cmd - 1))) {
            return -1;
        }
        kl = (size_t)(eq - cmd - 1);
        if (n < kl + 2 + kl + 2 || cmd[n - kl - 2] != ',' || cmd[n - kl - 1] != 'G' ||
            memcmp(cmd + n - kl, cmd + 1, kl) != 0) {
            return -1;
        }
        size_t vl = n - (kl + 2) - (kl + 2);
        if (kl + 1 + vl > size) {
            return -1;
        }
        memcpy(out, cmd + 1, kl);
        out[kl] = '=';
        memcpy(out + kl + 1, eq + 1, vl);
        DLOGI("AID %.*s set (%u bytes)", (int)kl, cmd + 1, (unsigned)vl);
        return (int)(kl + 1 + vl);
    }
    if (n > 1 && cmd[0] == 'G' && is_key(cmd + 1, n - 1)) {
        /* G<f>.<a>: a get, answered <f>.<a>=0 */
        if (n + 1 > size) {
            return -1;
        }
        memcpy(out, cmd + 1, n - 1);
        out[n - 1] = '=';
        out[n] = '0';
        DLOGI("AID %.*s read", (int)(n - 1), cmd + 1);
        return (int)(n + 1);
    }
    return -1;
}

/* ---- advertising ---------------------------------------------------------------- */

void dash_o5_advertised_uuid(const uint8_t *pdm_id, uint8_t le[16])
{
    static const uint8_t unpaired[4] = {0xff, 0xff, 0xff, 0xfe};
    const uint8_t *id = pdm_id ? pdm_id : unpaired;
    const uint8_t be[16] = {0xce, 0x1f, 0x92, 0x3d, 0xc5, 0x39, 0x48, 0xea,
                            0x73, 0x00, 0x0a, id[0], id[1], id[2], id[3], 0x00};
    for (int i = 0; i < 16; i++) {
        le[i] = be[15 - i];
    }
}
