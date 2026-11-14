/*
 * The platform crypto on mbedTLS: AES, SHA-256 and P-256. The same file runs
 * on the ESP32 (ESP-IDF's mbedTLS, with its AES and SHA accelerators) and in
 * the host tests (built from ESP-IDF's copy of mbedTLS).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"

#include "dash.h"

#ifdef ESP_PLATFORM
#include "esp_random.h"
#else
#include <stdlib.h>
#endif

void (*dash_random_hook)(uint8_t *p, size_t n);

void dash_random(uint8_t *p, size_t n)
{
    if (dash_random_hook) {
        dash_random_hook(p, n);
        return;
    }
#ifdef ESP_PLATFORM
    esp_fill_random(p, n);
#else
    arc4random_buf(p, n);
#endif
}

static int rng(void *ctx, unsigned char *p, size_t n)
{
    (void)ctx;
    dash_random(p, n);
    return 0;
}

void dash_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, key, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

void dash_sha256(const uint8_t *p, size_t n, uint8_t out[32])
{
    mbedtls_sha256(p, n, out, 0);
}

/* ---- P-256 ------------------------------------------------------------------- */

typedef struct {
    mbedtls_ecp_group grp;
    mbedtls_mpi d, r, s, z;
    mbedtls_ecp_point q;
} p256_t;

static int p256_open(p256_t *k)
{
    mbedtls_ecp_group_init(&k->grp);
    mbedtls_mpi_init(&k->d);
    mbedtls_mpi_init(&k->r);
    mbedtls_mpi_init(&k->s);
    mbedtls_mpi_init(&k->z);
    mbedtls_ecp_point_init(&k->q);
    return mbedtls_ecp_group_load(&k->grp, MBEDTLS_ECP_DP_SECP256R1);
}

static void p256_close(p256_t *k)
{
    mbedtls_ecp_group_free(&k->grp);
    mbedtls_mpi_free(&k->d);
    mbedtls_mpi_free(&k->r);
    mbedtls_mpi_free(&k->s);
    mbedtls_mpi_free(&k->z);
    mbedtls_ecp_point_free(&k->q);
}

static int read_priv(p256_t *k, const uint8_t priv[32])
{
    int rc = mbedtls_mpi_read_binary(&k->d, priv, 32);
    return rc ? rc : mbedtls_ecp_check_privkey(&k->grp, &k->d);
}

static int read_pub(p256_t *k, const uint8_t pub[64])
{
    uint8_t buf[65];
    buf[0] = 0x04;
    memcpy(buf + 1, pub, 64);
    int rc = mbedtls_ecp_point_read_binary(&k->grp, &k->q, buf, sizeof buf);
    return rc ? rc : mbedtls_ecp_check_pubkey(&k->grp, &k->q);
}

int dash_p256_keygen(uint8_t priv[32])
{
    p256_t k;
    int rc = p256_open(&k);
    for (int tries = 0; rc == 0 && tries < 16; tries++) {
        dash_random(priv, 32);
        if (read_priv(&k, priv) == 0) {
            p256_close(&k);
            return 0;
        }
    }
    p256_close(&k);
    return -1;
}

int dash_p256_public(const uint8_t priv[32], uint8_t pub[64])
{
    p256_t k;
    uint8_t buf[65];
    size_t len = 0;
    int rc = p256_open(&k);
    if (rc == 0) rc = read_priv(&k, priv);
    if (rc == 0) rc = mbedtls_ecp_mul(&k.grp, &k.q, &k.d, &k.grp.G, rng, NULL);
    if (rc == 0) rc = mbedtls_ecp_point_write_binary(&k.grp, &k.q, MBEDTLS_ECP_PF_UNCOMPRESSED, &len, buf, sizeof buf);
    if (rc == 0 && len == 65) {
        memcpy(pub, buf + 1, 64);
    }
    p256_close(&k);
    return rc == 0 && len == 65 ? 0 : -1;
}

int dash_p256_ecdh(const uint8_t priv[32], const uint8_t peer[64], uint8_t shared[32])
{
    p256_t k;
    int rc = p256_open(&k);
    if (rc == 0) rc = read_priv(&k, priv);
    if (rc == 0) rc = read_pub(&k, peer);
    if (rc == 0) rc = mbedtls_ecdh_compute_shared(&k.grp, &k.z, &k.q, &k.d, rng, NULL);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&k.z, shared, 32);
    p256_close(&k);
    return rc == 0 ? 0 : -1;
}

int dash_p256_sign(const uint8_t priv[32], const uint8_t *msg, size_t n, uint8_t sig[64])
{
    p256_t k;
    uint8_t hash[32];
    dash_sha256(msg, n, hash);
    int rc = p256_open(&k);
    if (rc == 0) rc = read_priv(&k, priv);
    if (rc == 0) rc = mbedtls_ecdsa_sign(&k.grp, &k.r, &k.s, &k.d, hash, sizeof hash, rng, NULL);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&k.r, sig, 32);
    if (rc == 0) rc = mbedtls_mpi_write_binary(&k.s, sig + 32, 32);
    p256_close(&k);
    return rc == 0 ? 0 : -1;
}

int dash_p256_verify(const uint8_t pub[64], const uint8_t *msg, size_t n, const uint8_t sig[64])
{
    p256_t k;
    uint8_t hash[32];
    dash_sha256(msg, n, hash);
    int rc = p256_open(&k);
    if (rc == 0) rc = read_pub(&k, pub);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&k.r, sig, 32);
    if (rc == 0) rc = mbedtls_mpi_read_binary(&k.s, sig + 32, 32);
    if (rc == 0) rc = mbedtls_ecdsa_verify(&k.grp, hash, sizeof hash, &k.q, &k.r, &k.s);
    p256_close(&k);
    return rc == 0 ? 0 : -1;
}
