#include "host_io.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void dash_host_log(char level, const char *fmt, ...)
{
    static int min = -1;
    if (min < 0) {
        const char *e = getenv("DASH_LOG");
        min = e && *e == 'D' ? 0 : e && *e == 'I' ? 1 : e && *e == 'W' ? 2 : 3;
    }
    int l = level == 'D' ? 0 : level == 'I' ? 1 : level == 'W' ? 2 : 3;
    if (l < min) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%c] ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int64_t clock_offset;
static pthread_mutex_t clock_mu = PTHREAD_MUTEX_INITIALIZER;

void host_clock_advance(int64_t ms)
{
    pthread_mutex_lock(&clock_mu);
    clock_offset += ms;
    pthread_mutex_unlock(&clock_mu);
}

int64_t host_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    pthread_mutex_lock(&clock_mu);
    int64_t off = clock_offset;
    pthread_mutex_unlock(&clock_mu);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + off;
}

void host_push(host_end_t *e, uint8_t type, const uint8_t *data, size_t n)
{
    pthread_mutex_lock(&e->mu);
    if (e->count == HOST_QUEUE) {
        fprintf(stderr, "host queue full\n");
        abort();
    }
    dash_ev_t *ev = &e->q[(e->head + e->count) % HOST_QUEUE];
    ev->type = type;
    ev->len = (uint8_t)n;
    if (n) {
        memcpy(ev->data, data, n);
    }
    e->count++;
    pthread_cond_broadcast(&e->cv);
    pthread_mutex_unlock(&e->mu);
}

static bool next_event(void *ctx, dash_ev_t *ev, uint32_t timeout_ms)
{
    host_end_t *e = ctx;
    pthread_mutex_lock(&e->mu);
    if (timeout_ms == DASH_WAIT_FOREVER) {
        while (e->count == 0) {
            pthread_cond_wait(&e->cv, &e->mu);
        }
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        while (e->count == 0) {
            if (pthread_cond_timedwait(&e->cv, &e->mu, &ts) == ETIMEDOUT) {
                break;
            }
        }
    }
    bool got = e->count > 0;
    if (got) {
        *ev = e->q[e->head];
        e->head = (e->head + 1) % HOST_QUEUE;
        e->count--;
    }
    pthread_mutex_unlock(&e->mu);
    return got;
}

static void notify_cmd(void *ctx, const uint8_t *p, size_t n)
{
    host_end_t *e = ctx;
    if (e->peer) {
        host_push(e->peer, DASH_EV_CMD, p, n);
    }
}

static void notify_data(void *ctx, const uint8_t *p, size_t n)
{
    host_end_t *e = ctx;
    if (e->peer) {
        host_push(e->peer, DASH_EV_DATA, p, n);
    }
}

static void disconnect(void *ctx)
{
    host_end_t *e = ctx;
    pthread_mutex_lock(&e->state_mu);
    e->disconnects++;
    pthread_mutex_unlock(&e->state_mu);
    host_push(e, DASH_EV_DISCONNECTED, NULL, 0);
}

static void advertise(void *ctx, const dash_adv_t *adv)
{
    host_end_t *e = ctx;
    pthread_mutex_lock(&e->state_mu);
    e->adverts++;
    e->adv_type = adv->pod_type;
    e->adv_has_id = adv->id != NULL;
    if (adv->id) {
        memcpy(e->adv_id, adv->id, 4);
    }
    e->adv_has_pdm_id = adv->pdm_id != NULL;
    if (adv->pdm_id) {
        memcpy(e->adv_pdm_id, adv->pdm_id, 4);
    }
    e->adv_fault = adv->fault;
    e->adv_alerts = adv->alerts;
    pthread_mutex_unlock(&e->state_mu);
}

static int64_t now_ms(void *ctx)
{
    (void)ctx;
    return host_now_ms();
}

static void save(void *ctx, const dash_state_t *st)
{
    host_end_t *e = ctx;
    pthread_mutex_lock(&e->state_mu);
    e->saved = *st;
    e->saves++;
    pthread_mutex_unlock(&e->state_mu);
}

static pthread_mutex_t pod_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock(void *ctx)
{
    (void)ctx;
    pthread_mutex_lock(&pod_lock);
}
static void unlock(void *ctx)
{
    (void)ctx;
    pthread_mutex_unlock(&pod_lock);
}

static void crash(void *ctx)
{
    host_end_t *e = ctx;
    pthread_mutex_lock(&e->state_mu);
    e->crashes++;
    pthread_mutex_unlock(&e->state_mu);
}

void host_end_init(host_end_t *e)
{
    memset(e, 0, sizeof *e);
    pthread_mutex_init(&e->mu, NULL);
    pthread_cond_init(&e->cv, NULL);
    pthread_mutex_init(&e->state_mu, NULL);
    e->io = (dash_io_t){
        .ctx = e,
        .next_event = next_event,
        .notify_cmd = notify_cmd,
        .notify_data = notify_data,
        .disconnect = disconnect,
        .advertise = advertise,
        .now_ms = now_ms,
        .save = save,
        .lock = lock,
        .unlock = unlock,
        .crash = crash,
    };
}

void host_link(host_end_t *a, host_end_t *b)
{
    a->peer = b;
    b->peer = a;
}
