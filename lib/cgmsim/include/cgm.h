/*
 * A simulated Dexcom G6 transmitter or G7 sensor, as the controller apps'
 * code sees one (LoopKit's CGMBLEKit for G6, G7SensorKit for G7).
 *
 * TEST EQUIPMENT ONLY: the glucose readings are made up. Never let an app
 * that controls a real pump or pod read them.
 *
 * Portable C: the platform supplies a cgm_io_t (radio, clock, storage) and the
 * AES block cipher (dash_aes128_encrypt_block, from the pod core).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CGM_OFF = 0,
    CGM_G6 = 6,
    CGM_G7 = 7,
} cgm_kind_t;

/* The sensor's GATT characteristics (service F8083532-849E-531C-C594-30F1F86A4EA5). */
typedef enum {
    CGM_CHR_COMM = 0, /* …3533 */
    CGM_CHR_CONTROL,  /* …3534 */
    CGM_CHR_AUTH,     /* …3535 */
    CGM_CHR_BACKFILL, /* …3536 */
    CGM_CHR_COUNT,
} cgm_chr_t;

#define CGM_READING_INTERVAL 300 /* seconds */
#define CGM_HISTORY 36           /* readings kept for backfill: 3 hours */
#define CGM_STATE_VERSION 1

/* Algorithm states, as both G6 and G7 number them. */
enum {
    CGM_STATE_STOPPED = 1,
    CGM_STATE_WARMUP = 2,
    CGM_STATE_OK = 6,
    CGM_STATE_G6_SESSION_FAILURE = 16,
    CGM_STATE_G7_TEMPORARY_ISSUE = 18,
    CGM_STATE_G7_EXPIRED = 24,
    CGM_STATE_G7_FAILED = 25,
    CGM_STATE_G7_SESSION_ENDED = 26,
};

typedef struct {
    uint32_t t;      /* Unix seconds */
    uint32_t seq;    /* reading number */
    uint16_t glucose; /* mg/dL; 0 when there is none */
    uint8_t state;
    int8_t trend;    /* tenths of mg/dL per minute */
} cgm_reading_t;

typedef struct {
    uint8_t version;
    uint8_t kind;          /* cgm_kind_t */
    char id[8];            /* G6: the 6-character transmitter id. G7: the 2 characters after "DXCM" */
    uint8_t addr[6];       /* the sensor's random static Bluetooth address */
    int64_t activated;     /* Unix seconds: transmitter (G6) or sensor (G7) start */
    int64_t session_start; /* Unix seconds: G6 sensor session start, 0 if none. G7: = activated */
    int64_t warmup_done;   /* Unix seconds: warm-up ends (the panel can skip it) */
    /* What the panel set. */
    uint16_t glucose;      /* the next reading, mg/dL */
    int8_t trend;          /* tenths of mg/dL per minute, applied every reading */
    uint8_t forced_state;  /* 0: from the session's age */
    bool signal_loss;      /* stop advertising: readings are missed, and backfilled later */
    /* Readings. */
    uint32_t next_n;       /* the next reading is due at activated + next_n * 5 minutes */
    uint32_t delivered;    /* the last reading's seq the phone was sent, 0 if none */
    uint16_t calibration;  /* G6: the last calibration, mg/dL */
    uint32_t calibration_t;
    uint8_t n_hist;
    cgm_reading_t hist[CGM_HISTORY]; /* oldest first */
} cgm_state_t;

typedef struct {
    void *ctx;
    /* Sends p on chr to the connected phone (a notification). */
    void (*send)(void *ctx, cgm_chr_t chr, const uint8_t *p, size_t n);
    void (*disconnect)(void *ctx);
    /* Advertise (or stop) under name, from the sensor's address. */
    void (*advertise)(void *ctx, bool on, const char *name, const uint8_t addr[6]);
    int64_t (*now)(void *ctx); /* Unix seconds */
    void (*save)(void *ctx, const cgm_state_t *st);
    void (*log)(void *ctx, const char *msg);
} cgm_io_t;

typedef struct {
    cgm_state_t st;
    const cgm_io_t *io;
    /* The connection. */
    bool connected;
    bool sent_glucose;
    bool authed;              /* G6 active mode: the app passed the challenge */
    uint8_t challenge[8];
    uint8_t auth_value[17];   /* what a read of the auth characteristic returns */
    uint8_t auth_len;
    int64_t connected_at, last_activity;
    uint32_t prev_delivered;  /* st.delivered when this connection began */
    /* Advertising. */
    bool advertising;
    int64_t window_end;
} cgm_t;

/* A fresh G6 transmitter (id NULL: a random one) or G7 sensor, started now. */
void cgm_state_fresh(cgm_state_t *st, cgm_kind_t kind, const char *id, int64_t now);
/* The name it advertises: "Dexcom" + the id's last 2 (G6), "DXCM" + 2 (G7). */
void cgm_name(const cgm_state_t *st, char out[16]);
bool cgm_valid_g6_id(const char *id);

void cgm_init(cgm_t *c, const cgm_state_t *st, const cgm_io_t *io);
/* Call about once a second: makes readings, opens and closes the advertising
 * window, ends idle connections. */
void cgm_tick(cgm_t *c);

/* From the radio. read returns the value's length. */
void cgm_on_connect(cgm_t *c);
void cgm_on_disconnect(cgm_t *c);
void cgm_on_subscribe(cgm_t *c, cgm_chr_t chr, bool on);
void cgm_on_write(cgm_t *c, cgm_chr_t chr, const uint8_t *p, size_t n);
size_t cgm_on_read(cgm_t *c, cgm_chr_t chr, uint8_t *out, size_t cap);

/* The control panel. */
void cgm_set_glucose(cgm_t *c, uint16_t glucose, int8_t trend);
void cgm_set_state(cgm_t *c, uint8_t forced_state);
void cgm_set_signal_loss(cgm_t *c, bool on);
void cgm_skip_warmup(cgm_t *c);
void cgm_new_sensor(cgm_t *c, cgm_kind_t kind, const char *id);
/* A JSON object describing the CGM, for the panel. */
size_t cgm_status_json(cgm_t *c, char *out, size_t cap);

/* ---- exposed for tests ------------------------------------------------------ */

uint16_t cgm_crc16(const uint8_t *p, size_t n); /* CRC-CCITT (XModem), as CGMBLEKit */
/* G6: AES-128-ECB of (data || data) under "00"+id+"00"+id, first 8 bytes. */
void cgm_g6_hash(const char *id, const uint8_t data[8], uint8_t out[8]);
/* The state a reading made now would have. */
uint8_t cgm_current_state(const cgm_state_t *st, int64_t now);
size_t cgm_g6_time_msg(const cgm_state_t *st, int64_t now, uint8_t out[16]);
size_t cgm_g6_glucose_msg(const cgm_state_t *st, const cgm_reading_t *r, uint8_t opcode, uint8_t out[16]);
size_t cgm_g7_glucose_msg(const cgm_state_t *st, const cgm_reading_t *r, int64_t now, uint8_t out[19]);
size_t cgm_g7_backfill_record(const cgm_state_t *st, const cgm_reading_t *r, uint8_t out[9]);
