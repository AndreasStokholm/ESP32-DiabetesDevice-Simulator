/*
 * ESP-Sim DASH pod: a C port of the Go Omnipod DASH pod simulator
 * (github.com/jeremybarnum/pod, itself from LoopKit/pod and 0pen-dash).
 *
 * TEST EQUIPMENT ONLY. This pretends to be an insulin pod so that controller
 * apps can be developed and tested without a real one. It delivers nothing,
 * and nothing it reports reflects a real pod. Never use it for therapy.
 *
 * The core in this component is portable C with no ESP-IDF dependency: the
 * platform supplies a dash_io_t (radio, clock, storage) and one AES-128
 * block function. The same code runs in the host tests and on the ESP32.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest link-layer packet: DASH packets are 20 bytes (one is 21: see
 * link.c), Omnipod 5 packets 244. */
#define DASH_PKT_MAX 248
/* Largest message payload: Omnipod 5 pairing sends certificates of ~1.1 KB. */
#define DASH_MSG_MAX 2048
#define DASH_WAIT_FOREVER UINT32_MAX

/* ---- messages (one "TW" CTwiPacket) ------------------------------------ */

enum {
    DASH_MSG_CLEAR = 0,
    DASH_MSG_ENCRYPTED = 1,
    DASH_MSG_SESSION = 2,
    DASH_MSG_PAIRING = 3,
    DASH_MSG_ENCRYPTED_SIGNED = 4, /* Omnipod 5: encrypted, then a 64-byte ECDSA signature */
};

typedef struct {
    uint8_t version; /* 3 bits, must be 0 */
    uint8_t eqos;    /* 3 bits */
    bool sas, tfs;
    bool ack, priority, last_message, gateway;
    uint8_t type; /* DASH_MSG_* */
    uint8_t seq;
    uint8_t ack_num;
    uint8_t src[4];
    uint8_t dst[4];
    uint8_t header[16]; /* the header as received: the AES-CCM additional data */
    bool encrypted;     /* payload is ciphertext followed by the 8-byte tag */
    bool signed_;       /* a signature followed the tag (not kept: the pod doesn't check it) */
    size_t payload_len;
    uint8_t payload[DASH_MSG_MAX];
} dash_msg_t;

/* ---- persisted pod state ----------------------------------------------- */

#define DASH_STATE_MAGIC 0x44415348u /* "DASH" */
#define DASH_STATE_VERSION 2u

/* Which pod the board pretends to be. */
typedef enum {
    DASH_POD_DASH = 0,
    DASH_POD_O5 = 1, /* Omnipod 5 */
} dash_pod_type_t;

enum {
    DASH_PROGRESS_INITIAL = 0,
    DASH_PROGRESS_MEMORY_INITIALIZED = 1,
    DASH_PROGRESS_REMINDER_INITIALIZED = 2,
    DASH_PROGRESS_PAIRING_COMPLETED = 3,
    DASH_PROGRESS_PRIMING = 4,
    DASH_PROGRESS_PRIMING_COMPLETED = 5,
    DASH_PROGRESS_BASAL_INITIALIZED = 6,
    DASH_PROGRESS_INSERTING_CANNULA = 7,
    DASH_PROGRESS_RUNNING_ABOVE_50U = 8,
    DASH_PROGRESS_RUNNING_BELOW_50U = 9,
    DASH_PROGRESS_FAULT = 13,
    DASH_PROGRESS_ACTIVATION_TIME_EXCEEDED = 14,
    DASH_PROGRESS_INACTIVE = 15,
};

/* Mirrors the Go simulator's PODState. Times are wall-clock milliseconds
 * since the Unix epoch; 0 is Go's zero time.Time ("never"). */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t pod_type; /* dash_pod_type_t */

    /* Omnipod 5: the controller this pod is paired to (it advertises the id),
     * and the pod's own P-256 identity key, for its certificate. */
    bool o5_has_pdm_id;
    uint8_t o5_pdm_id[4];
    uint8_t o5_key[32];

    bool paired; /* ltk is set */
    uint8_t ltk[16];
    uint64_t eap_aka_seq;

    bool has_id; /* id is set (SET_UNIQUE_ID received) */
    uint8_t id[4];

    uint8_t msg_seq;
    uint8_t cmd_seq;
    uint64_t nonce_seq;
    uint8_t last_prog_seq;

    bool has_session; /* ck and nonce_prefix are set */
    uint8_t nonce_prefix[8];
    uint8_t ck[16];

    int8_t progress;
    int64_t activation_ms;

    uint16_t reservoir; /* pulses of 0.05 U */
    uint8_t alerts;     /* active alert slots */
    uint8_t fault;
    uint16_t fault_time;
    uint16_t delivered; /* pulses */
    uint16_t trigger_times[8];

    int64_t bolus_end_ms;
    int64_t temp_basal_end_ms;
    bool extended_bolus_active;
    bool basal_active;

    int64_t saved_ms; /* wall clock when last saved: restores a clock with no time source */
} dash_state_t;

/* ---- what the platform provides ---------------------------------------- */

typedef enum {
    DASH_EV_CMD,          /* the phone wrote the command characteristic */
    DASH_EV_DATA,         /* the phone wrote the data characteristic */
    DASH_EV_CONNECTED,    /* a phone connected */
    DASH_EV_DISCONNECTED, /* the phone disconnected */
    DASH_EV_STOP,         /* host tests: make dash_pod_run return */
} dash_ev_type_t;

typedef struct {
    uint8_t type; /* dash_ev_type_t */
    uint8_t len;
    uint8_t data[DASH_PKT_MAX];
} dash_ev_t;

/* What the pod advertises. */
typedef struct {
    uint8_t pod_type;     /* dash_pod_type_t */
    const uint8_t *id;    /* DASH: the pod's id once set, else NULL */
    const uint8_t *pdm_id; /* Omnipod 5: the paired controller's id, else NULL */
    uint8_t fault, alerts;
} dash_adv_t;

typedef struct {
    void *ctx;
    /* Waits up to timeout_ms (or DASH_WAIT_FOREVER) for the radio's next
     * event. Returns false on timeout. */
    bool (*next_event)(void *ctx, dash_ev_t *ev, uint32_t timeout_ms);
    void (*notify_cmd)(void *ctx, const uint8_t *p, size_t n);
    void (*notify_data)(void *ctx, const uint8_t *p, size_t n);
    void (*disconnect)(void *ctx);
    void (*advertise)(void *ctx, const dash_adv_t *adv);
    int64_t (*now_ms)(void *ctx);
    void (*save)(void *ctx, const dash_state_t *st);
    void (*lock)(void *ctx);   /* guards the state against the control panel */
    void (*unlock)(void *ctx);
    void (*crash)(void *ctx);  /* "crash next command": end the process / reboot */
} dash_io_t;

/* ---- the platform's crypto (platform_mbedtls.c) ---------------------------- */

void dash_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void dash_sha256(const uint8_t *p, size_t n, uint8_t out[32]);
void dash_random(uint8_t *p, size_t n);
/* P-256: public keys are x || y (64 bytes). Each returns 0, or -1. */
int dash_p256_keygen(uint8_t priv[32]);
int dash_p256_public(const uint8_t priv[32], uint8_t pub[64]);
int dash_p256_ecdh(const uint8_t priv[32], const uint8_t peer[64], uint8_t shared[32]);
/* ECDSA with SHA-256 over msg; the signature is r || s (64 bytes). */
int dash_p256_sign(const uint8_t priv[32], const uint8_t *msg, size_t n, uint8_t sig[64]);
int dash_p256_verify(const uint8_t pub[64], const uint8_t *msg, size_t n, const uint8_t sig[64]);
/* Host tests: when set, dash_random takes its bytes from here. */
extern void (*dash_random_hook)(uint8_t *p, size_t n);

/* ---- the pod ------------------------------------------------------------- */

typedef struct {
    dash_state_t st;
    const dash_io_t *io;
    bool connected;
    int crash_next;     /* 0, or DASH_CRASH_BEFORE / DASH_CRASH_AFTER */
    int ignore_messages; /* messages left to leave unanswered (ignoreNextMessages) */
    uint8_t adv_fault, adv_alerts;
    bool adv_valid;
    uint8_t rx_buf[DASH_MSG_MAX + 48]; /* the link layer's reassembly */
    uint8_t tx_buf[DASH_MSG_MAX + 32]; /* the link layer's outgoing message */
} dash_pod_t;

enum { DASH_CRASH_BEFORE = 1, DASH_CRASH_AFTER = 2 };

/* A new, unpaired pod of the given type with a full reservoir (150 U). */
void dash_state_fresh(dash_state_t *st, int64_t now_ms, dash_pod_type_t type);
/* Whether a stored blob is a state this build can use. */
bool dash_state_valid(const dash_state_t *st);

void dash_pod_init(dash_pod_t *p, const dash_io_t *io, const dash_state_t *restored);
/* Serves phones forever (returns only on DASH_EV_STOP). */
void dash_pod_run(dash_pod_t *p);

/* The control panel's commands. Each locks, saves and re-advertises. */
void dash_pod_set_reservoir(dash_pod_t *p, double units);
void dash_pod_set_alerts(dash_pod_t *p, uint8_t mask);
void dash_pod_set_fault(dash_pod_t *p, uint8_t fault);
void dash_pod_set_active_time(dash_pod_t *p, int minutes);
void dash_pod_crash_next(dash_pod_t *p, bool before_processing);
/* Stay connected but leave the next count messages unanswered, as a pod
 * that didn't hear them would. */
void dash_pod_ignore_next_messages(dash_pod_t *p, int count);
/* Forget the pod (pairing, delivery, everything) and start as a new one. */
void dash_pod_reset(dash_pod_t *p);
/* Become a new pod of another type (DASH or Omnipod 5). */
void dash_pod_set_type(dash_pod_t *p, dash_pod_type_t type);

/* The control panel's /status JSON (the Go simulator's Status). Returns
 * the length written, or a negative number if buf is too small. */
int dash_pod_status_json(dash_pod_t *p, char *buf, size_t size);

/* Handles one control-panel JSON command ({"command": ..., "value": ...}).
 * Returns NULL on success, or an error message. */
const char *dash_pod_panel_command(dash_pod_t *p, const char *json, size_t len);

#ifdef __cplusplus
}
#endif
