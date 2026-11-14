/*
 * The link layer over the pod's two characteristics: a message is offered
 * with RTS on the command characteristic, cleared with CTS, sent as 20-byte
 * fragments on the data characteristic with a CRC-32, and acknowledged with
 * Success (or Fail).
 *
 * The packet layout copies the Go simulator's exactly, quirks included:
 * a short message's first packet holds up to 14 bytes (21 bytes in all),
 * and only multi-fragment messages wait for the phone's Success.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "dash_internal.h"

enum { CMD_RTS = 0, CMD_CTS = 1, CMD_NACK = 2, CMD_ABORT = 3, CMD_SUCCESS = 4, CMD_FAIL = 5 };

#define PACKET_WAIT_MS 10000u /* a phone mid-message sends its next packet in well under this */

static int64_t now(dash_pod_t *p)
{
    return p->io->now_ms(p->io->ctx);
}

/* Waits for a packet of one kind. A (re)connect or a stop ends the wait;
 * packets of the other kind are left over from an abandoned exchange. */
static dash_link_t wait_packet(dash_pod_t *p, uint8_t want, uint8_t *pkt, size_t *n, uint32_t timeout_ms)
{
    int64_t deadline = timeout_ms == DASH_WAIT_FOREVER ? 0 : now(p) + timeout_ms;
    for (;;) {
        uint32_t wait = DASH_WAIT_FOREVER;
        if (deadline) {
            int64_t left = deadline - now(p);
            if (left <= 0) {
                return DASH_LINK_TIMEOUT;
            }
            wait = (uint32_t)left;
        }
        dash_ev_t ev;
        if (!p->io->next_event(p->io->ctx, &ev, wait)) {
            continue; /* re-check the deadline */
        }
        switch (ev.type) {
        case DASH_EV_CONNECTED:
            p->connected = true;
            DLOGI("phone connected");
            return DASH_LINK_RECONNECT;
        case DASH_EV_DISCONNECTED:
            p->connected = false;
            DLOGI("phone disconnected");
            continue;
        case DASH_EV_STOP:
            return DASH_LINK_STOP;
        default:
            break;
        }
        if (ev.type != want) {
            DLOGD("dropping a stray %s packet", ev.type == DASH_EV_CMD ? "CMD" : "DATA");
            continue;
        }
        memcpy(pkt, ev.data, ev.len);
        *n = ev.len;
        return DASH_LINK_OK;
    }
}

dash_link_t dash_link_read_cmd(dash_pod_t *p, uint8_t *pkt, size_t *n, uint32_t timeout_ms)
{
    return wait_packet(p, DASH_EV_CMD, pkt, n, timeout_ms);
}

static void send_cmd(dash_pod_t *p, uint8_t c)
{
    p->io->notify_cmd(p->io->ctx, &c, 1);
}

static dash_link_t expect_cmd(dash_pod_t *p, uint8_t want)
{
    uint8_t pkt[DASH_PKT_MAX];
    size_t n;
    dash_link_t r = wait_packet(p, DASH_EV_CMD, pkt, &n, PACKET_WAIT_MS);
    if (r != DASH_LINK_OK) {
        return r == DASH_LINK_TIMEOUT ? DASH_LINK_ERROR : r;
    }
    if (n == 0 || pkt[0] != want) {
        DLOGW("expected command %02x, got %02x", want, n ? pkt[0] : 0xff);
        return DASH_LINK_ERROR;
    }
    return DASH_LINK_OK;
}

static dash_link_t read_data(dash_pod_t *p, uint8_t *pkt, size_t *n, size_t min_len)
{
    dash_link_t r = wait_packet(p, DASH_EV_DATA, pkt, n, PACKET_WAIT_MS);
    if (r == DASH_LINK_TIMEOUT) {
        DLOGW("the phone stopped sending a message");
        return DASH_LINK_ERROR;
    }
    if (r == DASH_LINK_OK && *n < min_len) {
        DLOGW("data packet of %u bytes, want at least %u", (unsigned)*n, (unsigned)min_len);
        return DASH_LINK_ERROR;
    }
    return r;
}

/* Reassembles one message after its RTS. */
static dash_link_t read_after_rts(dash_pod_t *p, uint8_t *buf, size_t *len, uint8_t checksum[4])
{
    uint8_t pkt[DASH_PKT_MAX];
    size_t n, l = 0;
    bool one_extra = false;
    dash_link_t r;

    send_cmd(p, CMD_CTS);
    if ((r = read_data(p, pkt, &n, 7)) != DASH_LINK_OK) {
        return r;
    }
    int fragments = pkt[1];
    if (fragments == 0) {
        memcpy(checksum, pkt + 2, 4);
        size_t want = pkt[6];
        size_t end = want + 7;
        if (want > 13) {
            one_extra = true;
            end = 20;
        }
        if (n < end) {
            return DASH_LINK_ERROR;
        }
        memcpy(buf, pkt + 7, end - 7);
        l = end - 7;
    } else {
        if (n < 20) {
            return DASH_LINK_ERROR;
        }
        memcpy(buf, pkt + 2, 18);
        l = 18;
    }
    for (int i = 1; i < fragments; i++) {
        if ((r = read_data(p, pkt, &n, 20)) != DASH_LINK_OK) {
            return r;
        }
        if (l + 19 > DASH_MSG_MAX + 16) {
            return DASH_LINK_ERROR;
        }
        memcpy(buf + l, pkt + 1, 19);
        l += 19;
    }
    if (fragments != 0) {
        if ((r = read_data(p, pkt, &n, 6)) != DASH_LINK_OK) {
            return r;
        }
        size_t want = pkt[1];
        if (want > 14) {
            one_extra = true;
            want = 14;
        }
        memcpy(checksum, pkt + 2, 4);
        if (n < want + 6) {
            return DASH_LINK_ERROR;
        }
        memcpy(buf + l, pkt + 6, want);
        l += want;
    }
    if (one_extra) {
        if ((r = read_data(p, pkt, &n, 2)) != DASH_LINK_OK) {
            return r;
        }
        size_t want = pkt[1];
        if (n < want + 2 || l + want > DASH_MSG_MAX + 16) {
            return DASH_LINK_ERROR;
        }
        memcpy(buf + l, pkt + 2, want);
        l += want;
    }
    *len = l;
    return DASH_LINK_OK;
}

/* ---- Omnipod 5: 244-byte packets, no RTS/CTS -------------------------------- */

#define O5_FIRST_HEADER 7     /* a message that fits one packet (and maybe one more) */
#define O5_FIRST_HEADER_MID 2 /* a message with middle packets */
#define O5_LAST_HEADER 6
#define O5_MAX_FRAGMENTS 15

static bool is_o5(const dash_pod_t *p)
{
    return p->st.pod_type == DASH_POD_O5;
}

/* OmnipodKit's PayloadJoiner, from the first packet on. */
static dash_link_t o5_join(dash_pod_t *p, const uint8_t *first, size_t fn, uint8_t *buf, size_t *len,
                           uint8_t crc[4])
{
    const size_t S = DASH_O5_PACKET;
    uint8_t pkt[DASH_PKT_MAX];
    size_t n, l = 0;
    bool one_extra = false;
    dash_link_t r;

    if (fn < O5_FIRST_HEADER || first[0] != 0 || first[1] > O5_MAX_FRAGMENTS) {
        DLOGW("bad first packet");
        return DASH_LINK_ERROR;
    }
    size_t full = first[1];
    if (full == 0) {
        size_t rest = first[6];
        size_t end = rest + O5_FIRST_HEADER < fn ? rest + O5_FIRST_HEADER : fn;
        memcpy(crc, first + 2, 4);
        memcpy(buf, first + O5_FIRST_HEADER, end - O5_FIRST_HEADER);
        l = end - O5_FIRST_HEADER;
        one_extra = rest + O5_FIRST_HEADER > end;
    } else {
        if (fn < S) {
            return DASH_LINK_ERROR;
        }
        memcpy(buf, first + O5_FIRST_HEADER_MID, S - O5_FIRST_HEADER_MID);
        l = S - O5_FIRST_HEADER_MID;
    }
    for (size_t i = 1; i <= full; i++) {
        if ((r = read_data(p, pkt, &n, 3)) != DASH_LINK_OK) {
            return r;
        }
        if (pkt[0] != i) {
            DLOGW("packet %u, want %u", pkt[0], (unsigned)i);
            return DASH_LINK_ERROR;
        }
        if (i < full) {
            if (n < S || l + S - 1 > DASH_MSG_MAX + 16) {
                return DASH_LINK_ERROR;
            }
            memcpy(buf + l, pkt + 1, S - 1);
            l += S - 1;
        } else {
            if (n < O5_LAST_HEADER) {
                return DASH_LINK_ERROR;
            }
            size_t rest = pkt[1];
            size_t end = rest + O5_LAST_HEADER < n ? rest + O5_LAST_HEADER : n;
            memcpy(crc, pkt + 2, 4);
            if (l + end - O5_LAST_HEADER > DASH_MSG_MAX + 16) {
                return DASH_LINK_ERROR;
            }
            memcpy(buf + l, pkt + O5_LAST_HEADER, end - O5_LAST_HEADER);
            l += end - O5_LAST_HEADER;
            one_extra = rest + O5_LAST_HEADER > end;
        }
    }
    if (one_extra) {
        if ((r = read_data(p, pkt, &n, 2)) != DASH_LINK_OK) {
            return r;
        }
        size_t size = pkt[1];
        if (pkt[0] != full + 1 || n < 2 + size || l + size > DASH_MSG_MAX + 16) {
            return DASH_LINK_ERROR;
        }
        memcpy(buf + l, pkt + 2, size);
        l += size;
    }
    *len = l;
    return DASH_LINK_OK;
}

static dash_link_t o5_read_msg(dash_pod_t *p, dash_msg_t *m, uint32_t timeout_ms)
{
    uint8_t *buf = p->rx_buf;
    int64_t deadline = timeout_ms == DASH_WAIT_FOREVER ? 0 : now(p) + timeout_ms;

    for (;;) {
        uint8_t first[DASH_PKT_MAX], crc[4];
        size_t fn, len = 0;
        uint32_t wait = DASH_WAIT_FOREVER;
        if (deadline) {
            int64_t left = deadline - now(p);
            if (left <= 0) {
                return DASH_LINK_TIMEOUT;
            }
            wait = (uint32_t)left;
        }
        dash_link_t r = wait_packet(p, DASH_EV_DATA, first, &fn, wait);
        if (r != DASH_LINK_OK) {
            return r;
        }
        if (fn == 0 || first[0] != 0) {
            DLOGD("dropping a packet that doesn't start a message");
            continue;
        }
        r = o5_join(p, first, fn, buf, &len, crc);
        if (r == DASH_LINK_ERROR) {
            send_cmd(p, CMD_FAIL);
            continue;
        }
        if (r != DASH_LINK_OK) {
            return r;
        }
        uint32_t want = (uint32_t)crc[0] << 24 | (uint32_t)crc[1] << 16 | (uint32_t)crc[2] << 8 | crc[3];
        if (dash_crc32(buf, len) != want) {
            DLOGW("checksum mismatch");
            send_cmd(p, CMD_FAIL);
            continue;
        }
        send_cmd(p, CMD_SUCCESS);
        if (dash_msg_unmarshal(buf, len, m) != 0) {
            continue;
        }
        return DASH_LINK_OK;
    }
}

/* OmnipodKit's PayloadSplitter. */
void dash_link_fragment_o5(const uint8_t *raw, size_t n, void (*emit)(void *ctx, const uint8_t *p, size_t n),
                           void *ctx)
{
    const size_t S = DASH_O5_PACKET;
    uint8_t b[DASH_PKT_MAX];
    uint32_t sum = dash_crc32(raw, n);
    const uint8_t c[4] = {(uint8_t)(sum >> 24), (uint8_t)(sum >> 16), (uint8_t)(sum >> 8), (uint8_t)sum};

    if (n <= S - O5_FIRST_HEADER_MID) {
        size_t end = n < S - O5_FIRST_HEADER ? n : S - O5_FIRST_HEADER;
        b[0] = 0;
        b[1] = 0;
        memcpy(b + 2, c, 4);
        b[6] = (uint8_t)n;
        memcpy(b + 7, raw, end);
        emit(ctx, b, 7 + end);
        if (n > end) {
            memset(b, 0, S);
            b[0] = 1;
            b[1] = (uint8_t)(n - end);
            memcpy(b + 2, raw + end, n - end);
            emit(ctx, b, S);
        }
        return;
    }
    size_t first_cap = S - O5_FIRST_HEADER_MID, mid_cap = S - 1, last_cap = S - O5_LAST_HEADER;
    size_t middles = (n - first_cap) / mid_cap;
    size_t rest = n - middles * mid_cap - first_cap;
    b[0] = 0;
    b[1] = (uint8_t)(middles + 1);
    memcpy(b + 2, raw, first_cap);
    emit(ctx, b, S);
    for (size_t i = 1; i <= middles; i++) {
        b[0] = (uint8_t)i;
        memcpy(b + 1, raw + first_cap + (i - 1) * mid_cap, mid_cap);
        emit(ctx, b, S);
    }
    size_t at = first_cap + middles * mid_cap;
    size_t end = rest < last_cap ? rest : last_cap;
    memset(b, 0, S);
    b[0] = (uint8_t)(middles + 1);
    b[1] = (uint8_t)rest;
    memcpy(b + 2, c, 4);
    memcpy(b + O5_LAST_HEADER, raw + at, end);
    emit(ctx, b, S);
    if (rest > last_cap) {
        memset(b, 0, S);
        b[0] = (uint8_t)(middles + 2);
        b[1] = (uint8_t)(rest - last_cap);
        memcpy(b + 2, raw + at + last_cap, rest - last_cap);
        emit(ctx, b, S);
    }
}

/* ---- DASH: RTS, CTS, 20-byte packets ----------------------------------------- */

dash_link_t dash_link_read_msg(dash_pod_t *p, dash_msg_t *m, uint32_t timeout_ms)
{
    if (is_o5(p)) {
        return o5_read_msg(p, m, timeout_ms);
    }
    uint8_t *buf = p->rx_buf;
    int64_t deadline = timeout_ms == DASH_WAIT_FOREVER ? 0 : now(p) + timeout_ms;

    for (;;) {
        uint8_t pkt[DASH_PKT_MAX];
        size_t n, len = 0;
        uint8_t checksum[4];
        uint32_t wait = DASH_WAIT_FOREVER;
        if (deadline) {
            int64_t left = deadline - now(p);
            if (left <= 0) {
                return DASH_LINK_TIMEOUT;
            }
            wait = (uint32_t)left;
        }
        dash_link_t r = dash_link_read_cmd(p, pkt, &n, wait);
        if (r != DASH_LINK_OK) {
            return r;
        }
        if (n == 0 || pkt[0] != CMD_RTS) {
            DLOGD("expected RTS, got %02x", n ? pkt[0] : 0xff);
            continue;
        }
        r = read_after_rts(p, buf, &len, checksum);
        if (r == DASH_LINK_ERROR) {
            send_cmd(p, CMD_FAIL);
            continue;
        }
        if (r != DASH_LINK_OK) {
            return r;
        }
        uint32_t want = (uint32_t)checksum[0] << 24 | (uint32_t)checksum[1] << 16 | (uint32_t)checksum[2] << 8 |
                        checksum[3];
        uint32_t sum = dash_crc32(buf, len);
        if (sum != want) {
            DLOGW("checksum mismatch: %08x, want %08x", (unsigned)sum, (unsigned)want);
            send_cmd(p, CMD_FAIL);
            continue;
        }
        send_cmd(p, CMD_SUCCESS);
        if (dash_msg_unmarshal(buf, len, m) != 0) {
            continue;
        }
        return DASH_LINK_OK;
    }
}

/* ---- writing --------------------------------------------------------------- */

bool dash_link_needs_success(size_t n)
{
    return n > 18;
}

void dash_link_fragment(const uint8_t *raw, size_t n, void (*emit)(void *ctx, const uint8_t *p, size_t n),
                        void *ctx)
{
    uint8_t b[DASH_PKT_MAX];
    size_t l;
    uint32_t sum = dash_crc32(raw, n);

    if (n <= 18) {
        size_t end = n > 14 ? 14 : n;
        b[0] = 0;
        b[1] = 0;
        b[2] = (uint8_t)(sum >> 24);
        b[3] = (uint8_t)(sum >> 16);
        b[4] = (uint8_t)(sum >> 8);
        b[5] = (uint8_t)sum;
        b[6] = (uint8_t)n;
        memcpy(b + 7, raw, end);
        emit(ctx, b, 7 + end);
        if (n > 14) {
            b[0] = 0;
            b[1] = (uint8_t)(n - 14);
            memcpy(b + 2, raw + 14, n - 14);
            emit(ctx, b, 2 + n - 14);
        }
        return;
    }

    size_t full = (n - 18) / 19;
    size_t rest = n - full * 19 - 18;
    size_t index = 0;
    b[0] = 0;
    b[1] = (uint8_t)(full + 1);
    memcpy(b + 2, raw, 18);
    emit(ctx, b, 20);
    for (index = 1; index <= full; index++) {
        b[0] = (uint8_t)index;
        memcpy(b + 1, raw + (index - 1) * 19 + 18, 19);
        emit(ctx, b, 20);
    }
    size_t end = rest > 14 ? 14 : rest;
    b[0] = (uint8_t)index;
    b[1] = (uint8_t)rest;
    b[2] = (uint8_t)(sum >> 24);
    b[3] = (uint8_t)(sum >> 16);
    b[4] = (uint8_t)(sum >> 8);
    b[5] = (uint8_t)sum;
    memcpy(b + 6, raw + full * 19 + 18, end);
    emit(ctx, b, 6 + end);
    if (rest > 14) {
        index++;
        b[0] = (uint8_t)index;
        b[1] = (uint8_t)(rest - 14);
        l = rest - 14;
        memcpy(b + 2, raw + full * 19 + 18 + 14, l);
        l += 2;
        while (l < 20) {
            b[l++] = 0;
        }
        emit(ctx, b, l);
    }
}

static void emit_data(void *ctx, const uint8_t *pkt, size_t n)
{
    dash_pod_t *p = ctx;
    p->io->notify_data(p->io->ctx, pkt, n);
}

dash_link_t dash_link_write_raw(dash_pod_t *p, const uint8_t *raw, size_t n)
{
    dash_link_t r;
    if (is_o5(p)) {
        dash_link_fragment_o5(raw, n, emit_data, p);
        return expect_cmd(p, CMD_SUCCESS);
    }
    send_cmd(p, CMD_RTS);
    if ((r = expect_cmd(p, CMD_CTS)) != DASH_LINK_OK) {
        return r;
    }
    dash_link_fragment(raw, n, emit_data, p);
    if (dash_link_needs_success(n)) {
        return expect_cmd(p, CMD_SUCCESS);
    }
    return DASH_LINK_OK;
}

dash_link_t dash_link_write_msg(dash_pod_t *p, const dash_msg_t *m)
{
    int n = dash_msg_marshal(m, p->tx_buf, sizeof p->tx_buf);
    if (n < 0) {
        DLOGE("message too large to send");
        return DASH_LINK_ERROR;
    }
    return dash_link_write_raw(p, p->tx_buf, (size_t)n);
}
