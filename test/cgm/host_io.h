/* A fake radio for the host tests: two ends whose notifications are each
 * other's writes, a clock tests can move, and in-memory storage. */
#pragma once

#include <pthread.h>

#include "dash.h"

#define HOST_QUEUE 512

typedef struct host_end {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    dash_ev_t q[HOST_QUEUE];
    int head, count;

    struct host_end *peer;
    dash_io_t io;

    pthread_mutex_t state_mu;
    dash_state_t saved;
    int saves, disconnects, crashes, adverts;
    bool adv_has_id, adv_has_pdm_id;
    uint8_t adv_type, adv_id[4], adv_pdm_id[4], adv_fault, adv_alerts;
} host_end_t;

void host_end_init(host_end_t *e);
void host_link(host_end_t *a, host_end_t *b);
void host_push(host_end_t *e, uint8_t type, const uint8_t *data, size_t n);
/* Moves the tests' clock forward. */
void host_clock_advance(int64_t ms);
int64_t host_now_ms(void);
