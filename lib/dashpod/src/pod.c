/*
 * The simulated DASH pod: pairing, EAP-AKA sessions, and the command loop,
 * as the Go simulator's pkg/pod does them, plus the control panel's state.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dash_internal.h"

#define SETUP_WAIT_MS 60000u       /* the Go simulator re-waits each minute */
#define COMMAND_WAIT_MS (3u * 60000u) /* idle phone: hang up, wait for the next session */

/* ---- state ---------------------------------------------------------------- */

void dash_state_fresh(dash_state_t *st, int64_t now_ms, dash_pod_type_t type)
{
    memset(st, 0, sizeof *st);
    st->magic = DASH_STATE_MAGIC;
    st->version = DASH_STATE_VERSION;
    st->pod_type = (uint8_t)type;
    if (type == DASH_POD_O5 && dash_p256_keygen(st->o5_key) != 0) {
        DLOGE("could not make the pod's identity key");
    }
    st->reservoir = 3000; /* 150 U */
    st->activation_ms = now_ms;
}

bool dash_state_valid(const dash_state_t *st)
{
    return st->magic == DASH_STATE_MAGIC && st->version == DASH_STATE_VERSION;
}

static int64_t now(dash_pod_t *p)
{
    return p->io->now_ms(p->io->ctx);
}

static uint16_t minutes_active(const dash_state_t *st, int64_t now_ms)
{
    int64_t d = now_ms - st->activation_ms;
    if (d <= 0) {
        return 0;
    }
    int64_t m = (d + 30000) / 60000; /* Go's Round(time.Minute) */
    return m > 0xffff ? 0xffff : (uint16_t)m;
}

static uint16_t bolus_remaining(const dash_state_t *st, int64_t now_ms)
{
    if (st->bolus_end_ms <= now_ms) {
        return 0;
    }
    /* Plus one second, so a bolus command's response reports its full size. */
    int64_t secs = (st->bolus_end_ms - now_ms + 1000) / 1000;
    int per = st->progress > DASH_PROGRESS_INSERTING_CANNULA ? 2 : 1;
    return (uint16_t)(secs / per);
}

static void save(dash_pod_t *p)
{
    p->st.saved_ms = now(p);
    p->io->save(p->io->ctx, &p->st);
}

static void lock(dash_pod_t *p)
{
    if (p->io->lock) {
        p->io->lock(p->io->ctx);
    }
}

static void unlock(dash_pod_t *p)
{
    if (p->io->unlock) {
        p->io->unlock(p->io->ctx);
    }
}

/* ---- advertising -------------------------------------------------------------- */

void dash_advertised_services(const uint8_t *id, uint8_t fault, uint8_t alerts, uint16_t out[9])
{
    uint16_t one = 0xffff, two = 0xfffe, second = 0x2470;
    if (id) {
        one = (uint16_t)(id[0] << 8 | id[1]);
        two = (uint16_t)(id[2] << 8 | id[3]);
        /* Real pods go C001 -> C005 when an alert fires -> C00A on a fault. */
        second = fault ? 0xC00A : alerts ? 0xC005 : 0xC001;
    }
    const uint16_t s[9] = {0x4024, second, 0x000a, one, two, 0x0814, 0x6DB1, 0x0006, 0xE451};
    memcpy(out, s, sizeof s);
}

void dash_manufacturer_data(uint8_t fault, uint8_t alerts, uint8_t out[25])
{
    memset(out, 0, 25);
    out[0] = 0x4c;
    out[1] = 0x00;
    out[2] = 0x02;
    out[3] = 0x15;
    out[18] = 0x00;
    out[19] = fault ? fault : 0x02;
    out[20] = fault;
    out[21] = alerts;
    out[22] = 0x00;
    out[23] = 0x00;
    out[24] = 0xc5;
}

static void advertise(dash_pod_t *p, bool force)
{
    if (!force && p->adv_valid && p->adv_fault == p->st.fault && p->adv_alerts == p->st.alerts) {
        return;
    }
    p->adv_valid = true;
    p->adv_fault = p->st.fault;
    p->adv_alerts = p->st.alerts;
    dash_adv_t adv = {
        .pod_type = p->st.pod_type,
        .id = p->st.has_id ? p->st.id : NULL,
        .pdm_id = p->st.o5_has_pdm_id ? p->st.o5_pdm_id : NULL,
        .fault = p->st.fault,
        .alerts = p->st.alerts,
    };
    p->io->advertise(p->io->ctx, &adv);
}

void dash_pod_init(dash_pod_t *p, const dash_io_t *io, const dash_state_t *restored)
{
    memset(p, 0, sizeof *p);
    p->io = io;
    if (restored && dash_state_valid(restored)) {
        p->st = *restored;
    } else {
        dash_state_fresh(&p->st, io->now_ms(io->ctx), DASH_POD_DASH);
    }
}

/* ---- pairing and sessions ------------------------------------------------------- */

static dash_link_t start_over(dash_pod_t *p, const char *why)
{
    DLOGE("%s; hanging up", why);
    p->io->disconnect(p->io->ctx);
    return DASH_LINK_ERROR;
}

static dash_link_t read_setup(dash_pod_t *p, dash_msg_t *m)
{
    for (;;) {
        dash_link_t r = dash_link_read_msg(p, m, SETUP_WAIT_MS);
        if (r == DASH_LINK_TIMEOUT) {
            continue;
        }
        if (r == DASH_LINK_RECONNECT) {
            DLOGI("new connection: starting over");
        }
        return r;
    }
}

/* Sends a setup message. A lost one is the phone's to ask for again. */
static dash_link_t write_setup(dash_pod_t *p, const dash_msg_t *m)
{
    dash_link_t r = dash_link_write_msg(p, m);
    if (r == DASH_LINK_ERROR) {
        DLOGW("a setup message was not delivered");
        return DASH_LINK_OK;
    }
    return r;
}

#define TRY(expr)                        \
    do {                                 \
        dash_link_t r_ = (expr);         \
        if (r_ != DASH_LINK_OK) {        \
            return r_;                   \
        }                                \
    } while (0)

static dash_link_t command_loop(dash_pod_t *p);

static dash_link_t eap_aka(dash_pod_t *p)
{
    static dash_msg_t m;
    dash_eap_t e;

    dash_eap_new(&e, p->st.ltk, p->st.eap_aka_seq);
    TRY(read_setup(p, &m));
    if (dash_eap_parse_challenge(&e, &m) != 0) {
        return start_over(p, "could not parse the EAP-AKA challenge");
    }
    dash_eap_response(&e, &m);
    TRY(write_setup(p, &m));
    TRY(read_setup(p, &m));
    if (dash_eap_parse_success(&m) != 0) {
        return start_over(p, "could not parse the EAP-AKA success");
    }

    lock(p);
    dash_eap_session(&e, p->st.ck, p->st.nonce_prefix);
    p->st.has_session = true;
    p->st.nonce_seq = 1;
    p->st.msg_seq = 1;
    p->st.eap_aka_seq = e.sqn;
    save(p);
    advertise(p, false);
    unlock(p);
    DLOGI("session established (EAP-AKA SQN %llu)", (unsigned long long)e.sqn);
    return command_loop(p);
}

static dash_link_t activation(dash_pod_t *p)
{
    static dash_msg_t m;
    static dash_pair_t c;

    TRY(read_setup(p, &m));
    if (dash_pair_parse_sp1sp2(&c, &m) != 0) {
        return start_over(p, "could not parse SP1/SP2");
    }
    TRY(read_setup(p, &m));
    if (dash_pair_parse_sps1(&c, &m) != 0) {
        return start_over(p, "could not parse SPS1");
    }
    dash_pair_sps1(&c, &m);
    TRY(write_setup(p, &m));
    TRY(read_setup(p, &m));
    if (dash_pair_parse_sps2(&c, &m) != 0) {
        return start_over(p, "could not parse SPS2");
    }
    dash_pair_sps2(&c, &m);
    TRY(write_setup(p, &m));
    TRY(read_setup(p, &m));
    if (dash_pair_parse_sp0gp0(&c, &m) != 0) {
        return start_over(p, "could not parse SP0,GP0");
    }
    dash_pair_p0(&c, &m);
    TRY(write_setup(p, &m));

    lock(p);
    memcpy(p->st.ltk, c.ltk, 16);
    p->st.paired = true;
    p->st.eap_aka_seq = 1;
    save(p);
    unlock(p);
    DLOGI("paired");
    return eap_aka(p);
}

/* Omnipod 5 pairing: the pod's side of OmnipodKit's O5LTKExchanger. */
static dash_link_t activation_o5(dash_pod_t *p)
{
    static dash_msg_t m, r;
    static dash_o5_pair_t c;

    c.identity_key = p->st.o5_key;
    TRY(read_setup(p, &m));
    if (dash_o5_parse_sp1sp2(&c, &m) != 0) {
        return start_over(p, "could not parse SP1/SP2");
    }
    TRY(read_setup(p, &m));
    if (dash_o5_sps0(&c, &m, &r) != 0) {
        return start_over(p, "could not parse SPS0");
    }
    TRY(write_setup(p, &r));
    TRY(read_setup(p, &m));
    if (dash_o5_sps1(&c, &m, &r) != 0) {
        return start_over(p, "could not parse SPS1");
    }
    TRY(write_setup(p, &r));
    TRY(read_setup(p, &m));
    if (dash_o5_sps2_1(&c, &m, &r) != 0) {
        return start_over(p, "could not parse SPS2.1");
    }
    TRY(write_setup(p, &r));
    TRY(read_setup(p, &m));
    if (dash_o5_sps2(&c, &m, &r) != 0) {
        return start_over(p, "could not parse SPS2");
    }
    TRY(write_setup(p, &r));
    TRY(read_setup(p, &m));
    {
        static const char want[] = "SP0,GP0";
        if (m.payload_len != sizeof want - 1 || memcmp(m.payload, want, sizeof want - 1) != 0) {
            return start_over(p, "expected SP0,GP0");
        }
    }
    static const uint8_t p0[] = {'P', '0', '=', 0x00, 0x01, 0xa5};
    dash_msg_new(&r, DASH_MSG_PAIRING, c.pod_id, c.pdm_id);
    r.sas = true;
    memcpy(r.payload, p0, sizeof p0);
    r.payload_len = sizeof p0;
    TRY(write_setup(p, &r));

    lock(p);
    memcpy(p->st.ltk, c.ltk, 16);
    p->st.paired = true;
    p->st.eap_aka_seq = 1;
    memcpy(p->st.o5_pdm_id, c.pdm_id, 4);
    p->st.o5_has_pdm_id = true;
    save(p);
    advertise(p, true); /* now with the controller's id, which it scans for */
    unlock(p);
    DLOGI("paired (Omnipod 5)");
    return eap_aka(p);
}

/* One phone connection: its first command, then pairing or a session. */
static dash_link_t session(dash_pod_t *p)
{
    uint8_t pkt[DASH_PKT_MAX];
    size_t n;
    DLOGI("waiting for a phone");
    dash_link_t r = dash_link_read_cmd(p, pkt, &n, DASH_WAIT_FOREVER);
    if (r != DASH_LINK_OK) {
        return r;
    }
    DLOGI("first command from the phone (%u bytes)", (unsigned)n);
    if (p->st.paired) {
        return eap_aka(p);
    }
    return p->st.pod_type == DASH_POD_O5 ? activation_o5(p) : activation(p);
}

void dash_pod_run(dash_pod_t *p)
{
    lock(p);
    advertise(p, true);
    unlock(p);
    for (;;) {
        if (session(p) == DASH_LINK_STOP) {
            return;
        }
    }
}

/* ---- commands --------------------------------------------------------------------- */

static void clear_alerts(dash_state_t *st, uint8_t mask)
{
    st->alerts &= (uint8_t)~mask;
    for (int i = 0; i < 8; i++) {
        if (mask & (1u << i)) {
            st->trigger_times[i] = 0;
        }
    }
}

static void crash(dash_pod_t *p, const char *when, uint8_t seq)
{
    DLOGE("crashing %s processing command with sequence %u (control panel)", when, seq);
    p->io->crash(p->io->ctx);
}

static void handle_command(dash_pod_t *p, const dash_cmd_t *c, int64_t t)
{
    dash_state_t *st = &p->st;

    /* Progress a real pod's pump makes on its own. */
    if (st->progress == DASH_PROGRESS_PRIMING && st->bolus_end_ms < t) {
        DLOGI("priming finished");
        st->progress = DASH_PROGRESS_PRIMING_COMPLETED;
    }
    if (st->progress == DASH_PROGRESS_INSERTING_CANNULA && st->bolus_end_ms < t) {
        DLOGI("cannula inserted: running");
        st->progress = DASH_PROGRESS_RUNNING_ABOVE_50U;
    }
    if (st->progress == DASH_PROGRESS_RUNNING_ABOVE_50U && st->reservoir <= 1000) {
        st->progress = DASH_PROGRESS_RUNNING_BELOW_50U;
    }

    if (p->crash_next == DASH_CRASH_BEFORE && dash_cmd_mutates(c)) {
        crash(p, "before", c->seq);
    }

    switch (c->type) {
    case DASH_CMD_GET_VERSION:
        st->progress = DASH_PROGRESS_REMINDER_INITIALIZED;
        break;
    case DASH_CMD_SET_UNIQUE_ID:
        st->progress = DASH_PROGRESS_PAIRING_COMPLETED;
        break;
    case DASH_CMD_SILENCE_ALERTS:
    case DASH_CMD_PROGRAM_ALERTS:
        clear_alerts(st, c->alert_mask);
        break;
    case DASH_CMD_PROGRAM_INSULIN:
        if (st->progress < DASH_PROGRESS_PRIMING) {
            st->progress = DASH_PROGRESS_PRIMING; /* the prime */
        } else if (st->progress < DASH_PROGRESS_BASAL_INITIALIZED) {
            st->progress = DASH_PROGRESS_BASAL_INITIALIZED; /* the basal schedule */
        } else if (st->progress < DASH_PROGRESS_INSERTING_CANNULA) {
            st->progress = DASH_PROGRESS_INSERTING_CANNULA; /* the cannula insertion */
        }
        if (c->table_num == 0) {
            st->basal_active = true;
        }
        if (c->table_num == 1) {
            st->temp_basal_end_ms = t + (int64_t)c->duration * 30 * 60000;
        }
        if (c->table_num == 2) {
            /* The whole bolus leaves the reservoir now; its time runs at
             * 2 s a pulse (1 s during pod setup). */
            st->delivered = (uint16_t)(st->delivered + c->pulses);
            st->reservoir = (uint16_t)(st->reservoir - c->pulses);
            int per = st->progress >= DASH_PROGRESS_RUNNING_ABOVE_50U ? 2000 : 1000;
            st->bolus_end_ms = t + (int64_t)c->pulses * per;
        }
        break;
    case DASH_CMD_STOP_DELIVERY:
        if (c->stop_bolus) {
            st->extended_bolus_active = false;
        }
        if (c->stop_temp_basal) {
            st->temp_basal_end_ms = 0;
        }
        if (c->stop_basal) {
            st->basal_active = false;
        }
        break;
    default:
        break;
    }

    if (dash_cmd_mutates(c)) {
        st->last_prog_seq = c->seq;
        if (p->crash_next == DASH_CRASH_AFTER) {
            save(p);
            crash(p, "after", c->seq);
        }
    }
}

static void status_fields(const dash_state_t *st, int64_t t, dash_status_t *s)
{
    memset(s, 0, sizeof *s);
    s->last_prog_seq = st->last_prog_seq;
    s->reservoir = st->reservoir;
    s->alerts = st->alerts;
    s->bolus_active = st->bolus_end_ms > t;
    s->temp_basal_active = st->temp_basal_end_ms > t;
    s->basal_active = st->basal_active;
    s->extended_bolus_active = st->extended_bolus_active;
    s->progress = st->progress;
    s->delivered = st->delivered;
    s->bolus_remaining = bolus_remaining(st, t);
    s->minutes_active = minutes_active(st, t);
    s->fault = st->fault;
    s->fault_time = st->fault_time;
}

static size_t response_body(dash_pod_t *p, const dash_cmd_t *c, int64_t t, uint8_t *out)
{
    dash_state_t *st = &p->st;
    dash_status_t s;

    if (dash_cmd_response_hardcoded(c)) {
        switch (c->type) {
        case DASH_CMD_GET_VERSION: return dash_rsp_version(out, (dash_pod_type_t)st->pod_type);
        case DASH_CMD_SET_UNIQUE_ID: return dash_rsp_set_unique_id(out, (dash_pod_type_t)st->pod_type);
        case DASH_CMD_GET_STATUS:
            if (c->status_type == 0x46) return dash_rsp_type46(out);
            if (c->status_type == 0x50) return dash_rsp_type50(out);
            if (c->status_type == 0x51) return dash_rsp_type51(out);
            return dash_rsp_nack(out);
        case DASH_CMD_DEACTIVATE:
        case DASH_CMD_CNFG_DELIV_FLAG:
            memset(&s, 0, sizeof s); /* the Go simulator's zero-valued status */
            return dash_rsp_general(&s, out);
        default:
            return dash_rsp_nack(out);
        }
    }

    if (minutes_active(st, t) >= 80 * 60 && st->fault == 0) {
        st->fault = 0x1C; /* exceeded the maximum pod life of 80 hours */
    }
    status_fields(st, t, &s);
    if (c->type != DASH_CMD_GET_STATUS || c->status_type == 0 || c->status_type == 7) {
        return st->fault == 0 ? dash_rsp_general(&s, out) : dash_rsp_detailed(&s, out);
    }
    switch (c->status_type) {
    case 1: return dash_rsp_type1(st->trigger_times, out);
    case 2: return dash_rsp_detailed(&s, out);
    case 3: return dash_rsp_type3(st->fault, st->fault_time, s.minutes_active, out);
    case 5: {
        time_t at = (time_t)(st->activation_ms / 1000);
        struct tm tm;
        localtime_r(&at, &tm);
        return dash_rsp_type5(st->fault, st->fault_time, (uint8_t)(tm.tm_year + 1900 - 2000), (uint8_t)(tm.tm_mon + 1),
                              (uint8_t)tm.tm_mday, (uint8_t)tm.tm_hour, (uint8_t)tm.tm_min, out);
    }
    default: return dash_rsp_nack(out);
    }
}

/* Encrypts and sends a response (called locked; returns unlocked), then
 * takes the phone's empty ACK. DASH_LINK_OK when acknowledged. */
static dash_link_t send_and_take_ack(dash_pod_t *p, dash_msg_t *m, uint8_t *raw, size_t size)
{
    dash_state_t *st = &p->st;
    int n = dash_msg_marshal(m, raw, size);
    n = n < 0 ? n : dash_encrypt_raw(st->ck, st->nonce_prefix, st->nonce_seq, raw, (size_t)n, size);
    if (n < 0) {
        unlock(p);
        return start_over(p, "could not build the response");
    }
    st->nonce_seq++;
    save(p);
    unlock(p);

    dash_link_t r = dash_link_write_raw(p, raw, (size_t)n);
    if (r == DASH_LINK_RECONNECT || r == DASH_LINK_STOP) {
        return r;
    }

    r = dash_link_read_msg(p, m, COMMAND_WAIT_MS);
    if (r != DASH_LINK_OK) {
        lock(p);
        save(p);
        advertise(p, false);
        unlock(p);
        DLOGI("no ACK for the response: the phone asks again next session");
        if (r == DASH_LINK_TIMEOUT || r == DASH_LINK_ERROR) {
            p->io->disconnect(p->io->ctx);
            return DASH_LINK_ERROR;
        }
        return r;
    }
    lock(p);
    bool ok = dash_decrypt_msg(st->ck, st->nonce_prefix, st->nonce_seq, m) == 0;
    if (ok) {
        st->nonce_seq++;
        ok = m->payload_len == 0;
    }
    save(p);
    advertise(p, false);
    unlock(p);
    if (!ok) {
        return start_over(p, "could not read the response's ACK");
    }
    return DASH_LINK_OK;
}

static dash_link_t command_loop(dash_pod_t *p)
{
    static dash_msg_t m;
    static uint8_t raw[DASH_MSG_MAX + 32];
    uint8_t body[256];
    uint8_t last_seq = 0;

    for (;;) {
        DLOGI("waiting for the next command");
        dash_link_t r = dash_link_read_msg(p, &m, COMMAND_WAIT_MS);
        if (r == DASH_LINK_TIMEOUT) {
            DLOGI("no command for 3 minutes: hanging up");
            p->io->disconnect(p->io->ctx);
            return DASH_LINK_ERROR;
        }
        if (r != DASH_LINK_OK) {
            return r;
        }
        lock(p);
        bool ignore = p->ignore_messages > 0;
        if (ignore) {
            p->ignore_messages--;
        }
        unlock(p);
        if (ignore) {
            DLOGI("ignoring message seq %u (ignoreNextMessages)", (unsigned)m.seq);
            continue;
        }
        if (m.seq == last_seq) {
            continue; /* the phone resending one the pod hasn't answered yet */
        }
        last_seq = m.seq;

        lock(p);
        dash_state_t *st = &p->st;
        if (dash_decrypt_msg(st->ck, st->nonce_prefix, st->nonce_seq, &m) != 0) {
            unlock(p);
            return start_over(p, "could not decrypt a command");
        }
        st->nonce_seq++;
        bool o5 = st->pod_type == DASH_POD_O5;
        if (o5 && (m.payload_len < 5 || memcmp(m.payload, "S0.0=", 5) != 0)) {
            /* An Omnipod 5 AID command: its own text, answered without "0.0=". */
            uint8_t aid[256];
            int an = dash_o5_aid(m.payload, m.payload_len, aid, sizeof aid);
            if (an < 0) {
                unlock(p);
                return start_over(p, "could not parse a command");
            }
            uint8_t src[4], dst[4];
            memcpy(src, m.dst, 4);
            memcpy(dst, m.src, 4);
            uint8_t ack_seq = (uint8_t)(m.seq + 1);
            st->msg_seq++;
            dash_msg_new(&m, DASH_MSG_ENCRYPTED, src, dst);
            memcpy(m.payload, aid, (size_t)an);
            m.payload_len = (size_t)an;
            m.seq = st->msg_seq;
            m.ack = true;
            m.ack_num = ack_seq;
            m.eqos = 1;
            r = send_and_take_ack(p, &m, raw, sizeof raw);
            if (r != DASH_LINK_OK) {
                return r;
            }
            continue;
        }
        dash_cmd_t c;
        if (m.payload_len < 16 || dash_cmd_parse(m.payload, m.payload_len, &c) != 0) {
            unlock(p);
            return start_over(p, "could not parse a command");
        }
        st->cmd_seq = c.seq;
        bool deactivate = c.raw_type == DASH_CMD_DEACTIVATE;
        int64_t t = now(p);

        handle_command(p, &c, t);
        size_t body_len = response_body(p, &c, t, body);

        if (c.type == DASH_CMD_SET_UNIQUE_ID) {
            memcpy(st->id, c.unique_id, 4);
            st->has_id = true;
            DLOGI("pod id is now %02X%02X%02X%02X", st->id[0], st->id[1], st->id[2], st->id[3]);
            advertise(p, true);
        }
        if (c.type == DASH_CMD_STOP_DELIVERY && c.stop_bolus) {
            st->bolus_end_ms = 0; /* after the response, which reports what was left */
        }

        st->msg_seq++;
        st->cmd_seq++;
        save(p);
        dash_msg_t *rm = &m; /* reuse: the command is done with */
        uint8_t src[4], dst[4];
        memcpy(src, m.dst, 4);
        memcpy(dst, m.src, 4);
        uint8_t ack_seq = (uint8_t)(m.seq + 1);
        if (dash_rsp_message(body, body_len, st->cmd_seq, st->msg_seq, ack_seq, c.id, src, dst, o5, rm) != 0) {
            unlock(p);
            return start_over(p, "could not build the response");
        }
        r = send_and_take_ack(p, rm, raw, sizeof raw);
        if (r != DASH_LINK_OK) {
            return r;
        }

        if (deactivate) {
            DLOGI("pod deactivated: it becomes a new, unpaired pod");
            dash_pod_reset(p);
            p->io->disconnect(p->io->ctx);
            return DASH_LINK_ERROR;
        }
    }
}

/* ---- the control panel ------------------------------------------------------------- */

void dash_pod_set_reservoir(dash_pod_t *p, double units)
{
    lock(p);
    p->st.reservoir = (uint16_t)(units * 20);
    save(p);
    advertise(p, false);
    unlock(p);
}

void dash_pod_set_alerts(dash_pod_t *p, uint8_t mask)
{
    lock(p);
    p->st.alerts = mask;
    uint16_t m = minutes_active(&p->st, now(p));
    for (int i = 0; i < 8; i++) {
        if (mask & (1u << i)) {
            p->st.trigger_times[i] = m;
        }
    }
    save(p);
    advertise(p, false);
    unlock(p);
}

void dash_pod_set_fault(dash_pod_t *p, uint8_t fault)
{
    lock(p);
    p->st.fault = fault;
    p->st.fault_time = minutes_active(&p->st, now(p));
    save(p);
    advertise(p, false);
    unlock(p);
}

void dash_pod_set_active_time(dash_pod_t *p, int minutes)
{
    lock(p);
    p->st.activation_ms = now(p) - (int64_t)minutes * 60000;
    save(p);
    advertise(p, false);
    unlock(p);
}

void dash_pod_crash_next(dash_pod_t *p, bool before_processing)
{
    lock(p);
    p->crash_next = before_processing ? DASH_CRASH_BEFORE : DASH_CRASH_AFTER;
    unlock(p);
}

void dash_pod_ignore_next_messages(dash_pod_t *p, int count)
{
    lock(p);
    p->ignore_messages = count;
    unlock(p);
}

void dash_pod_reset(dash_pod_t *p)
{
    lock(p);
    dash_state_fresh(&p->st, now(p), (dash_pod_type_t)p->st.pod_type);
    save(p);
    advertise(p, true);
    unlock(p);
}

void dash_pod_set_type(dash_pod_t *p, dash_pod_type_t type)
{
    lock(p);
    DLOGI("becoming a new %s pod", type == DASH_POD_O5 ? "Omnipod 5" : "DASH");
    dash_state_fresh(&p->st, now(p), type);
    save(p);
    advertise(p, true);
    unlock(p);
    /* A connected phone was talking to the old pod. */
    p->io->disconnect(p->io->ctx);
}

static const char *progress_name(int progress)
{
    switch (progress) {
    case 0: return "Initial";
    case 1: return "Memory initialized";
    case 2: return "Reminder initialized";
    case 3: return "Pairing completed";
    case 4: return "Priming";
    case 5: return "Priming completed";
    case 6: return "Basal initialized";
    case 7: return "Inserting cannula";
    case 8: return "Running, above 50 U";
    case 9: return "Running, below 50 U";
    case 13: return "Fault";
    case 14: return "Activation time exceeded";
    case 15: return "Inactive";
    default: return NULL;
    }
}

static void iso_time(int64_t ms, char out[64])
{
    if (ms == 0) {
        strcpy(out, "0001-01-01T00:00:00Z");
        return;
    }
    time_t s = (time_t)(ms / 1000);
    struct tm tm;
    gmtime_r(&s, &tm);
    snprintf(out, 64, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(ms % 1000));
}

int dash_pod_status_json(dash_pod_t *p, char *buf, size_t size)
{
    lock(p);
    dash_state_t s = p->st;
    int crash = p->crash_next;
    bool connected = p->connected;
    unlock(p);

    int64_t t = now(p);
    char id[9] = "", act[64], tb[64], prog[8];
    if (s.has_id) {
        snprintf(id, sizeof id, "%02X%02X%02X%02X", s.id[0], s.id[1], s.id[2], s.id[3]);
    }
    iso_time(s.activation_ms, act);
    iso_time(s.temp_basal_end_ms, tb);
    const char *name = progress_name(s.progress);
    if (!name) {
        snprintf(prog, sizeof prog, "%d", s.progress);
        name = prog;
    }
    int n = snprintf(
        buf, size,
        "{\"podType\":\"%s\",\"id\":\"%s\",\"paired\":%s,\"connected\":%s,\"progress\":%d,\"progressName\":\"%s\","
        "\"reservoir\":%.2f,\"delivered\":%.2f,\"activationTime\":\"%s\",\"minutesActive\":%u,"
        "\"alerts\":%u,\"fault\":%u,\"faultMinutes\":%u,\"basalActive\":%s,\"tempBasalActive\":%s,"
        "\"tempBasalEnd\":\"%s\",\"bolusActive\":%s,\"bolusRemaining\":%.2f,\"extendedBolusActive\":%s,"
        "\"crashNext\":\"%s\",\"msgSeq\":%u,\"cmdSeq\":%u,\"nonceSeq\":%llu,\"eapAkaSeq\":%llu,"
        "\"lastProgSeqNum\":%u}",
        s.pod_type == DASH_POD_O5 ? "Omnipod 5" : "DASH", id, s.paired ? "true" : "false", connected ? "true" : "false", s.progress, name, s.reservoir * 0.05,
        s.delivered * 0.05, act, minutes_active(&s, t), s.alerts, s.fault, s.fault_time,
        s.basal_active ? "true" : "false", s.temp_basal_end_ms > t ? "true" : "false", tb,
        s.bolus_end_ms > t ? "true" : "false", bolus_remaining(&s, t) * 0.05,
        s.extended_bolus_active ? "true" : "false",
        crash == DASH_CRASH_BEFORE ? "before" : crash == DASH_CRASH_AFTER ? "after" : "", s.msg_seq, s.cmd_seq,
        (unsigned long long)s.nonce_seq, (unsigned long long)s.eap_aka_seq, s.last_prog_seq);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

/* ---- a small reader for the panel's flat JSON objects ------------------------------- */

static const char *json_key(const char *j, size_t len, const char *key)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i + kl + 2 <= len; i++) {
        if (j[i] == '"' && strncmp(j + i + 1, key, kl) == 0 && j[i + 1 + kl] == '"') {
            const char *v = j + i + kl + 2;
            const char *end = j + len;
            while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
            if (v < end && *v == ':') {
                v++;
                while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
                return v < end ? v : NULL;
            }
        }
    }
    return NULL;
}

const char *dash_pod_panel_command(dash_pod_t *p, const char *json, size_t len)
{
    char buf[256], cmd[32];
    if (len >= sizeof buf) {
        return "request too large";
    }
    memcpy(buf, json, len);
    buf[len] = 0;

    const char *c = json_key(buf, len, "command");
    if (!c || *c != '"') {
        return "command is not a string or not in msg";
    }
    size_t cl = 0;
    for (c++; *c && *c != '"' && cl + 1 < sizeof cmd; c++) {
        cmd[cl++] = *c;
    }
    cmd[cl] = 0;

    const char *v = json_key(buf, len, "value");
    char *endp = NULL;
    double value = v ? strtod(v, &endp) : 0;
    bool has_value = v && endp != v;

#define NEED_VALUE(what, lo, hi)                                    \
    do {                                                            \
        if (!has_value) return what " is not a number or not in msg"; \
        if (value < (lo) || value > (hi)) return what " out of range"; \
    } while (0)

    if (strcmp(cmd, "changeReservoir") == 0) {
        NEED_VALUE("reservoir", 0, 200);
        dash_pod_set_reservoir(p, value);
    } else if (strcmp(cmd, "setAlerts") == 0) {
        NEED_VALUE("alert mask", 0, 255);
        dash_pod_set_alerts(p, (uint8_t)value);
    } else if (strcmp(cmd, "setFault") == 0) {
        NEED_VALUE("fault", 0, 255);
        dash_pod_set_fault(p, (uint8_t)value);
    } else if (strcmp(cmd, "setActiveTime") == 0) {
        NEED_VALUE("active time in minutes", 0, 100 * 60);
        dash_pod_set_active_time(p, (int)value);
    } else if (strcmp(cmd, "crashNextCommand") == 0) {
        const char *b = json_key(buf, len, "beforeProcessing");
        if (!b || (strncmp(b, "true", 4) != 0 && strncmp(b, "false", 5) != 0)) {
            return "beforeProcessing is not a bool or not in msg";
        }
        dash_pod_crash_next(p, b[0] == 't');
    } else if (strcmp(cmd, "ignoreNextMessages") == 0) {
        NEED_VALUE("message count", 0, 100);
        dash_pod_ignore_next_messages(p, (int)value);
    } else if (strcmp(cmd, "failNextConnects") == 0) {
        NEED_VALUE("connect count", 0, 100);
        /* The Go simulator refuses this on a real radio too: only its
         * ImpossiBLE transport can leave service discovery unanswered. */
        return "failNextConnects needs -transport impossible (a real radio cannot leave service discovery unanswered)";
    } else if (strcmp(cmd, "resetPod") == 0) {
        dash_pod_reset(p);
    } else if (strcmp(cmd, "setPodType") == 0) {
        NEED_VALUE("pod type (0 DASH, 1 Omnipod 5)", 0, 1);
        dash_pod_set_type(p, value >= 1 ? DASH_POD_O5 : DASH_POD_DASH);
    } else {
        return "unknown command";
    }
    return NULL;
}
