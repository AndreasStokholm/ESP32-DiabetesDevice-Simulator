/*
 * The DASH pod's protocol layers, exposed for the host tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "dash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- logging ---------------------------------------------------------- */

#ifdef ESP_PLATFORM
#include "esp_log.h"
#define DASH_TAG "dashpod"
#define DLOGE(...) ESP_LOGE(DASH_TAG, __VA_ARGS__)
#define DLOGW(...) ESP_LOGW(DASH_TAG, __VA_ARGS__)
#define DLOGI(...) ESP_LOGI(DASH_TAG, __VA_ARGS__)
#define DLOGD(...) ESP_LOGD(DASH_TAG, __VA_ARGS__)
#else
void dash_host_log(char level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define DLOGE(...) dash_host_log('E', __VA_ARGS__)
#define DLOGW(...) dash_host_log('W', __VA_ARGS__)
#define DLOGI(...) dash_host_log('I', __VA_ARGS__)
#define DLOGD(...) dash_host_log('D', __VA_ARGS__)
#endif

/* ---- crypto (crypto.c, x25519.c) -------------------------------------- */

uint32_t dash_crc32(const uint8_t *p, size_t n);
/* The Omnipod message CRC-16 (OmnipodKit's crc16Table), and CRC-16/XMODEM. */
uint16_t dash_crc16(const uint8_t *p, size_t n);
uint16_t dash_crc16_xmodem(const uint8_t *p, size_t n);
void dash_cmac(const uint8_t key[16], const uint8_t *m, size_t n, uint8_t out[16]);
/* AES-CCM, 13-byte nonce, 8-byte tag. out holds n+8 bytes. */
void dash_ccm_encrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                      const uint8_t *pt, size_t n, uint8_t *out);
/* in is ciphertext then tag (n includes the tag). Returns 0, or -1 if the tag is wrong. */
int dash_ccm_decrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                     const uint8_t *in, size_t n, uint8_t *out);
void dash_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void dash_x25519_base(uint8_t out[32], const uint8_t scalar[32]);
/* Milenage f2..f5 (3GPP TS 35.206). */
void dash_milenage(const uint8_t k[16], const uint8_t op[16], const uint8_t rand[16], uint8_t res[8],
                   uint8_t ck[16], uint8_t ik[16], uint8_t ak[6]);

/* ---- messages (message.c) --------------------------------------------- */

void dash_msg_new(dash_msg_t *m, uint8_t type, const uint8_t src[4], const uint8_t dst[4]);
/* Returns the raw length, or -1 if out is too small. */
int dash_msg_marshal(const dash_msg_t *m, uint8_t *out, size_t size);
/* Returns 0, or -1 with the reason logged. */
int dash_msg_unmarshal(const uint8_t *raw, size_t n, dash_msg_t *m);

/* The 13-byte nonce for message number seq of a session. */
void dash_nonce(const uint8_t prefix[8], uint64_t seq, bool pod_receiving, uint8_t nonce[13]);
/* Encrypts a marshalled plaintext message in place: raw grows by the 8-byte tag. */
int dash_encrypt_raw(const uint8_t ck[16], const uint8_t prefix[8], uint64_t seq, uint8_t *raw, size_t n,
                     size_t size);
/* Decrypts a received encrypted message in place. Returns 0, or -1. */
int dash_decrypt_msg(const uint8_t ck[16], const uint8_t prefix[8], uint64_t seq, dash_msg_t *m);

/* ---- link layer (link.c) ----------------------------------------------- */

typedef enum {
    DASH_LINK_OK,
    DASH_LINK_TIMEOUT,
    DASH_LINK_RECONNECT, /* a phone (re)connected: start over */
    DASH_LINK_STOP,      /* host tests */
    DASH_LINK_ERROR,     /* this message was lost; the phone retries */
} dash_link_t;

/* Waits for the phone's next command-characteristic write. */
dash_link_t dash_link_read_cmd(dash_pod_t *p, uint8_t *pkt, size_t *n, uint32_t timeout_ms);
/* Waits for, reassembles and checks the phone's next message. */
dash_link_t dash_link_read_msg(dash_pod_t *p, dash_msg_t *m, uint32_t timeout_ms);
/* Sends a marshalled message. */
dash_link_t dash_link_write_raw(dash_pod_t *p, const uint8_t *raw, size_t n);
dash_link_t dash_link_write_msg(dash_pod_t *p, const dash_msg_t *m);

/* The packets dash_link_write_raw sends, without sending them: for tests.
 * Calls emit for each data packet. */
void dash_link_fragment(const uint8_t *raw, size_t n, void (*emit)(void *ctx, const uint8_t *p, size_t n),
                        void *ctx);
/* Whether the message needs the phone's Success after its packets. */
bool dash_link_needs_success(size_t n);
/* Omnipod 5's packets: OmnipodKit's PayloadSplitter with 244-byte packets. */
#define DASH_O5_PACKET 244
void dash_link_fragment_o5(const uint8_t *raw, size_t n, void (*emit)(void *ctx, const uint8_t *p, size_t n),
                           void *ctx);

/* ---- pairing (pair.c) -------------------------------------------------- */

typedef struct {
    uint8_t pod_priv[32], pod_pub[32], pod_nonce[16];
    uint8_t pdm_pub[32], pdm_nonce[16];
    uint8_t shared[32];
    uint8_t conf_key[16], ltk[16];
    uint8_t pod_conf[16], pdm_conf[16];
    uint8_t pod_id[4], pdm_id[4];
    bool have_ltk;
} dash_pair_t;

int dash_pair_parse_sp1sp2(dash_pair_t *c, const dash_msg_t *m);
int dash_pair_parse_sps1(dash_pair_t *c, const dash_msg_t *m);
void dash_pair_sps1(dash_pair_t *c, dash_msg_t *out);
int dash_pair_parse_sps2(dash_pair_t *c, const dash_msg_t *m);
void dash_pair_sps2(dash_pair_t *c, dash_msg_t *out);
int dash_pair_parse_sp0gp0(dash_pair_t *c, const dash_msg_t *m);
void dash_pair_p0(dash_pair_t *c, dash_msg_t *out);

/* ---- EAP-AKA session establishment (eap.c) ---------------------------- */

typedef struct {
    uint8_t pod_id[4], pdm_id[4];
    uint8_t k[16];
    uint8_t rand[16], autn[16];
    uint8_t res[8], ck[16];
    uint8_t pod_iv[4], pdm_iv[4];
    uint64_t sqn;
    uint8_t identifier;
} dash_eap_t;

void dash_eap_new(dash_eap_t *e, const uint8_t ltk[16], uint64_t sqn);
int dash_eap_parse_challenge(dash_eap_t *e, const dash_msg_t *m);
int dash_eap_response(dash_eap_t *e, dash_msg_t *out);
int dash_eap_parse_success(const dash_msg_t *m);
/* The session key and the 8-byte nonce prefix (pdm IV then pod IV). */
void dash_eap_session(const dash_eap_t *e, uint8_t ck[16], uint8_t prefix[8]);

/* ---- pod commands (command.c) ------------------------------------------ */

enum {
    DASH_CMD_SET_UNIQUE_ID = 0x03,
    DASH_CMD_GET_VERSION = 0x07,
    DASH_CMD_CNFG_DELIV_FLAG = 0x08,
    DASH_CMD_GET_STATUS = 0x0e,
    DASH_CMD_SILENCE_ALERTS = 0x11,
    DASH_CMD_PROGRAM_ALERTS = 0x19,
    DASH_CMD_PROGRAM_INSULIN = 0x1a,
    DASH_CMD_DEACTIVATE = 0x1c,
    DASH_CMD_PROGRAM_BEEPS = 0x1e,
    DASH_CMD_STOP_DELIVERY = 0x1f,
    DASH_CMD_NACK = 0x00, /* anything else */
};

typedef struct {
    uint8_t type; /* DASH_CMD_*; DASH_CMD_NACK for one the pod doesn't know */
    uint8_t raw_type;
    uint8_t seq;
    uint8_t id[4];
    uint8_t status_type;  /* GET_STATUS */
    uint8_t alert_mask;   /* SILENCE_ALERTS, PROGRAM_ALERTS */
    uint8_t table_num;    /* PROGRAM_INSULIN: 0 basal, 1 temp basal, 2 bolus */
    uint8_t duration;     /* PROGRAM_INSULIN: half hours */
    uint16_t pulses;      /* PROGRAM_INSULIN */
    bool stop_bolus, stop_temp_basal, stop_basal; /* STOP_DELIVERY */
    uint8_t unique_id[4]; /* SET_UNIQUE_ID */
} dash_cmd_t;

/* Parses a decrypted "S0.0=...,G0.0" payload. Returns 0, or -1. */
int dash_cmd_parse(const uint8_t *p, size_t n, dash_cmd_t *c);
bool dash_cmd_response_hardcoded(const dash_cmd_t *c);
bool dash_cmd_mutates(const dash_cmd_t *c);
const char *dash_cmd_name(uint8_t raw_type);

/* ---- pod responses (response.c) ----------------------------------------- */

typedef struct {
    uint8_t last_prog_seq;
    uint16_t reservoir;
    uint8_t alerts;
    bool bolus_active, temp_basal_active, basal_active, extended_bolus_active;
    int8_t progress;
    uint16_t delivered;
    uint16_t bolus_remaining;
    uint16_t minutes_active;
    uint8_t fault;
    uint16_t fault_time;
} dash_status_t;

/* Each writes the response body and returns its length. out holds 256 bytes. */
size_t dash_rsp_general(const dash_status_t *s, uint8_t *out);
size_t dash_rsp_detailed(const dash_status_t *s, uint8_t *out);
size_t dash_rsp_type1(const uint16_t triggered[8], uint8_t *out);
size_t dash_rsp_type3(uint8_t fault, uint16_t fault_time, uint16_t minutes_active, uint8_t *out);
size_t dash_rsp_type5(uint8_t fault, uint16_t fault_time, uint8_t year, uint8_t month, uint8_t day, uint8_t hour,
                      uint8_t minute, uint8_t *out);
size_t dash_rsp_type46(uint8_t *out);
size_t dash_rsp_type50(uint8_t *out);
size_t dash_rsp_type51(uint8_t *out);
size_t dash_rsp_nack(uint8_t *out);
size_t dash_rsp_version(uint8_t *out, dash_pod_type_t type);
size_t dash_rsp_set_unique_id(uint8_t *out, dash_pod_type_t type);

/* Wraps a response body for the phone: "0.0=" + length + request id + seq/len
 * header + body + CRC, as an encrypted-type message (not yet encrypted). */
int dash_rsp_message(const uint8_t *body, size_t body_len, uint8_t cmd_seq, uint8_t msg_seq, uint8_t ack_seq,
                     const uint8_t request_id[4], const uint8_t src[4], const uint8_t dst[4], bool with_crc,
                     dash_msg_t *out);

/* ---- Omnipod 5 (o5.c) --------------------------------------------------------- */

/* The 6-byte firmware id both sides put in the key derivation and transcripts. */
extern const uint8_t DASH_O5_FIRMWARE_ID[6];

typedef struct {
    uint8_t pod_id[4], pdm_id[4];
    uint8_t algorithm;
    uint8_t pod_priv[32], pod_pub[64], pod_nonce[16];
    uint8_t pdm_pub[64], pdm_nonce[16];
    uint8_t conf[16], ltk[16];
    const uint8_t *identity_key; /* the pod's certificate key (32 bytes) */
} dash_o5_pair_t;

int dash_o5_parse_sp1sp2(dash_o5_pair_t *c, const dash_msg_t *m);
/* SPS0: the phone's algorithm, answered with the pod's. */
int dash_o5_sps0(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out);
/* SPS1: the phone's key and nonce, answered with the pod's; derives conf and the LTK. */
int dash_o5_sps1(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out);
/* SPS2.1 and SPS2: the phone's certificate (and signature), answered with the pod's. */
int dash_o5_sps2_1(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out);
int dash_o5_sps2(dash_o5_pair_t *c, const dash_msg_t *in, dash_msg_t *out);

/* Pieces of the above, for the tests. */
void dash_o5_derive(dash_o5_pair_t *c, const uint8_t shared[32]);
void dash_o5_sps_nonce(const dash_o5_pair_t *c, bool from_pdm, uint8_t nonce[13]);
void dash_o5_nonce_increment(uint8_t nonce[16]);
void dash_o5_pod_transcript(const dash_o5_pair_t *c, uint8_t out[171]);
/* The pod's self-signed X.509 certificate for its identity key. Returns its length. */
int dash_o5_certificate(const uint8_t key[32], uint8_t *out, size_t size);

/* An Omnipod 5 AID command ("S3.9=8,G3.9", "SE255.2=...", "G3.11"): writes the
 * response payload and returns its length, or -1 if it is not one. */
int dash_o5_aid(const uint8_t *cmd, size_t n, uint8_t *out, size_t size);

/* ---- advertising (pod.c) ------------------------------------------------- */

/* The 9 16-bit service UUIDs a DASH pod advertises. */
void dash_advertised_services(const uint8_t *id, uint8_t fault, uint8_t alerts, uint16_t out[9]);
/* Omnipod 5's one 128-bit service UUID, CE1F923D-C539-48EA-7300-0A<pdm id>00,
 * as text and as bytes in NimBLE's order (least significant first). */
void dash_o5_advertised_uuid(const uint8_t *pdm_id, uint8_t le[16]);
/* The pod's iBeacon-shaped manufacturer data (25 bytes). */
void dash_manufacturer_data(uint8_t fault, uint8_t alerts, uint8_t out[25]);
#define DASH_ADVERTISED_NAME " :: Fake POD ::"

#ifdef __cplusplus
}
#endif
