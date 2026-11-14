/*
 * CRC-32, AES-CMAC (RFC 4493), AES-CCM (RFC 3610) and Milenage
 * (3GPP TS 35.206), built on the platform's AES-128 block function.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "dash_internal.h"

uint32_t dash_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* The Omnipod message CRC-16, with OmnipodKit's table (OmnipodCommon/CRC16.swift). */
static const uint16_t crc16_table[256] = {
    0x0000, 0x8005, 0x800f, 0x000a, 0x801b, 0x001e, 0x0014, 0x8011,
    0x8033, 0x0036, 0x003c, 0x8039, 0x0028, 0x802d, 0x8027, 0x0022,
    0x8063, 0x0066, 0x006c, 0x8069, 0x0078, 0x807d, 0x8077, 0x0072,
    0x0050, 0x8055, 0x805f, 0x005a, 0x804b, 0x004e, 0x0044, 0x8041,
    0x80c3, 0x00c6, 0x00cc, 0x80c9, 0x00d8, 0x80dd, 0x80d7, 0x00d2,
    0x00f0, 0x80f5, 0x80ff, 0x00fa, 0x80eb, 0x00ee, 0x00e4, 0x80e1,
    0x00a0, 0x80a5, 0x80af, 0x00aa, 0x80bb, 0x00be, 0x00b4, 0x80b1,
    0x8093, 0x0096, 0x009c, 0x8099, 0x0088, 0x808d, 0x8087, 0x0082,
    0x8183, 0x0186, 0x018c, 0x8189, 0x0198, 0x819d, 0x8197, 0x0192,
    0x01b0, 0x81b5, 0x81bf, 0x01ba, 0x81ab, 0x01ae, 0x01a4, 0x81a1,
    0x01e0, 0x81e5, 0x81ef, 0x01ea, 0x81fb, 0x01fe, 0x01f4, 0x81f1,
    0x81d3, 0x01d6, 0x01dc, 0x81d9, 0x01c8, 0x81cd, 0x81c7, 0x01c2,
    0x0140, 0x8145, 0x814f, 0x014a, 0x815b, 0x015e, 0x0154, 0x8151,
    0x8173, 0x0176, 0x017c, 0x8179, 0x0168, 0x816d, 0x8167, 0x0162,
    0x8123, 0x0126, 0x012c, 0x8129, 0x0138, 0x813d, 0x8137, 0x0132,
    0x0110, 0x8115, 0x811f, 0x011a, 0x810b, 0x010e, 0x0104, 0x8101,
    0x8303, 0x0306, 0x030c, 0x8309, 0x0318, 0x831d, 0x8317, 0x0312,
    0x0330, 0x8335, 0x833f, 0x033a, 0x832b, 0x032e, 0x0324, 0x8321,
    0x0360, 0x8365, 0x836f, 0x036a, 0x837b, 0x037e, 0x0374, 0x8371,
    0x8353, 0x0356, 0x035c, 0x8359, 0x0348, 0x834d, 0x8347, 0x0342,
    0x03c0, 0x83c5, 0x83cf, 0x03ca, 0x83db, 0x03de, 0x03d4, 0x83d1,
    0x83f3, 0x03f6, 0x03fc, 0x83f9, 0x03e8, 0x83ed, 0x83e7, 0x03e2,
    0x83a3, 0x03a6, 0x03ac, 0x83a9, 0x03b8, 0x83bd, 0x83b7, 0x03b2,
    0x0390, 0x8395, 0x839f, 0x039a, 0x838b, 0x038e, 0x0384, 0x8381,
    0x0280, 0x8285, 0x828f, 0x028a, 0x829b, 0x029e, 0x0294, 0x8291,
    0x82b3, 0x02b6, 0x02bc, 0x82b9, 0x02a8, 0x82ad, 0x82a7, 0x02a2,
    0x82e3, 0x02e6, 0x02ec, 0x82e9, 0x02f8, 0x82fd, 0x82f7, 0x02f2,
    0x02d0, 0x82d5, 0x82df, 0x02da, 0x82cb, 0x02ce, 0x02c4, 0x82c1,
    0x8243, 0x0246, 0x024c, 0x8249, 0x0258, 0x825d, 0x8257, 0x0252,
    0x0270, 0x8275, 0x827f, 0x027a, 0x826b, 0x026e, 0x0264, 0x8261,
    0x0220, 0x8225, 0x822f, 0x022a, 0x823b, 0x023e, 0x0234, 0x8231,
    0x8213, 0x0216, 0x021c, 0x8219, 0x0208, 0x820d, 0x8207, 0x0202,
};

uint16_t dash_crc16(const uint8_t *p, size_t n)
{
    uint16_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (uint16_t)((acc >> 8) ^ crc16_table[(acc ^ p[i]) & 0xff]);
    }
    return acc;
}

uint16_t dash_crc16_xmodem(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) {
            crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
        }
    }
    return crc;
}

static void xor16(uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++) {
        a[i] ^= b[i];
    }
}

/* ---- CMAC ----------------------------------------------------------------- */

static void cmac_subkey(const uint8_t in[16], uint8_t out[16])
{
    uint8_t carry = in[0] & 0x80;
    for (int i = 0; i < 15; i++) {
        out[i] = (uint8_t)((in[i] << 1) | (in[i + 1] >> 7));
    }
    out[15] = (uint8_t)(in[15] << 1);
    if (carry) {
        out[15] ^= 0x87;
    }
}

void dash_cmac(const uint8_t key[16], const uint8_t *m, size_t n, uint8_t out[16])
{
    uint8_t l[16] = {0}, k1[16], k2[16], x[16] = {0}, last[16];

    dash_aes128_encrypt_block(key, l, l);
    cmac_subkey(l, k1);
    cmac_subkey(k1, k2);

    size_t blocks = (n + 15) / 16;
    bool complete = n > 0 && n % 16 == 0;
    if (blocks == 0) {
        blocks = 1;
    }
    for (size_t i = 0; i + 1 < blocks; i++) {
        xor16(x, m + 16 * i);
        dash_aes128_encrypt_block(key, x, x);
    }
    size_t rest = n - 16 * (blocks - 1);
    memset(last, 0, sizeof last);
    memcpy(last, m + 16 * (blocks - 1), rest);
    if (complete) {
        xor16(last, k1);
    } else {
        last[rest] = 0x80;
        xor16(last, k2);
    }
    xor16(x, last);
    dash_aes128_encrypt_block(key, x, out);
}

/* ---- CCM: 13-byte nonce (so a 2-byte length field), 8-byte tag ------------ */

#define CCM_TAG 8

static void ccm_mac(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                    const uint8_t *pt, size_t n, uint8_t tag[16])
{
    uint8_t x[16], blk[16];

    x[0] = (uint8_t)((aad_len ? 0x40 : 0) | (((CCM_TAG - 2) / 2) << 3) | (2 - 1));
    memcpy(x + 1, nonce, 13);
    x[14] = (uint8_t)(n >> 8);
    x[15] = (uint8_t)n;
    dash_aes128_encrypt_block(key, x, x);

    if (aad_len) {
        /* aad_len < 0xff00 here: its length takes 2 bytes. */
        size_t used = 2;
        memset(blk, 0, sizeof blk);
        blk[0] = (uint8_t)(aad_len >> 8);
        blk[1] = (uint8_t)aad_len;
        size_t i = 0;
        while (i < aad_len) {
            size_t take = 16 - used;
            if (take > aad_len - i) {
                take = aad_len - i;
            }
            memcpy(blk + used, aad + i, take);
            i += take;
            used += take;
            if (used == 16 || i == aad_len) {
                xor16(x, blk);
                dash_aes128_encrypt_block(key, x, x);
                memset(blk, 0, sizeof blk);
                used = 0;
            }
        }
    }
    for (size_t i = 0; i < n; i += 16) {
        size_t take = n - i < 16 ? n - i : 16;
        memset(blk, 0, sizeof blk);
        memcpy(blk, pt + i, take);
        xor16(x, blk);
        dash_aes128_encrypt_block(key, x, x);
    }
    memcpy(tag, x, 16);
}

static void ccm_ctr(const uint8_t key[16], const uint8_t nonce[13], uint16_t i, uint8_t s[16])
{
    uint8_t a[16];
    a[0] = 2 - 1;
    memcpy(a + 1, nonce, 13);
    a[14] = (uint8_t)(i >> 8);
    a[15] = (uint8_t)i;
    dash_aes128_encrypt_block(key, a, s);
}

static void ccm_crypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t s[16];
    for (size_t i = 0; i < n; i += 16) {
        ccm_ctr(key, nonce, (uint16_t)(i / 16 + 1), s);
        size_t take = n - i < 16 ? n - i : 16;
        for (size_t k = 0; k < take; k++) {
            out[i + k] = in[i + k] ^ s[k];
        }
    }
}

void dash_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                      const uint8_t *pt, size_t n, uint8_t *out)
{
    uint8_t t[16], s0[16];
    ccm_mac(key, nonce, aad, aad_len, pt, n, t);
    ccm_ctr(key, nonce, 0, s0);
    ccm_crypt(key, nonce, pt, n, out); /* out may be pt */
    for (int k = 0; k < CCM_TAG; k++) {
        out[n + k] = t[k] ^ s0[k];
    }
}

int dash_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t t[16], s0[16], want[CCM_TAG];
    if (n < CCM_TAG) {
        return -1;
    }
    size_t len = n - CCM_TAG;
    memcpy(want, in + len, CCM_TAG);
    ccm_crypt(key, nonce, in, len, out); /* out may be in */
    ccm_mac(key, nonce, aad, aad_len, out, len, t);
    ccm_ctr(key, nonce, 0, s0);
    uint8_t diff = 0;
    for (int k = 0; k < CCM_TAG; k++) {
        diff |= (uint8_t)((t[k] ^ s0[k]) ^ want[k]);
    }
    return diff ? -1 : 0;
}

/* ---- Milenage ------------------------------------------------------------- */

static void milenage_out(const uint8_t k[16], const uint8_t opc[16], const uint8_t temp[16], int rot_bytes,
                         uint8_t c_last, uint8_t out[16])
{
    uint8_t x[16];
    for (int i = 0; i < 16; i++) {
        x[i] = temp[(i + rot_bytes) % 16] ^ opc[(i + rot_bytes) % 16];
    }
    x[15] ^= c_last;
    dash_aes128_encrypt_block(k, x, out);
    xor16(out, opc);
}

void dash_milenage(const uint8_t k[16], const uint8_t op[16], const uint8_t rand[16], uint8_t res[8],
                   uint8_t ck[16], uint8_t ik[16], uint8_t ak[6])
{
    uint8_t opc[16], temp[16], out[16];

    dash_aes128_encrypt_block(k, op, opc);
    xor16(opc, op);

    memcpy(temp, rand, 16);
    xor16(temp, opc);
    dash_aes128_encrypt_block(k, temp, temp);

    /* f2 and f5: r2 = 0, c2 = 1 */
    milenage_out(k, opc, temp, 0, 1, out);
    if (res) {
        memcpy(res, out + 8, 8);
    }
    if (ak) {
        memcpy(ak, out, 6);
    }
    /* f3: r3 = 32 bits, c3 = 2 */
    if (ck) {
        milenage_out(k, opc, temp, 4, 2, ck);
    }
    /* f4: r4 = 64 bits, c4 = 4 */
    if (ik) {
        milenage_out(k, opc, temp, 8, 4, ik);
    }
}
