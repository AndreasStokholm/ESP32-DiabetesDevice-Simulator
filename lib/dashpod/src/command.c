/*
 * The pod commands inside a decrypted "S0.0=<len><cmd>,G0.0" payload.
 * Command bytes as the Go simulator names them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "dash_internal.h"

const char *dash_cmd_name(uint8_t t)
{
    switch (t) {
    case DASH_CMD_SET_UNIQUE_ID: return "SET_UNIQUE_ID";
    case DASH_CMD_GET_VERSION: return "GET_VERSION";
    case DASH_CMD_CNFG_DELIV_FLAG: return "CNFG_DELIV_FLAG";
    case DASH_CMD_GET_STATUS: return "GET_STATUS";
    case DASH_CMD_SILENCE_ALERTS: return "SILENCE_ALERTS";
    case 0x13: return "PROGRAM_BASAL";
    case 0x16: return "PROGRAM_TEMP_BASAL";
    case 0x17: return "PROGRAM_BOLUS";
    case DASH_CMD_PROGRAM_ALERTS: return "PROGRAM_ALERTS";
    case DASH_CMD_PROGRAM_INSULIN: return "PROGRAM_INSULIN";
    case DASH_CMD_DEACTIVATE: return "DEACTIVATE";
    case DASH_CMD_PROGRAM_BEEPS: return "PROGRAM_BEEPS";
    case DASH_CMD_STOP_DELIVERY: return "STOP_DELIVERY";
    default: return "unknown";
    }
}

#define NEED(n)                                                                  \
    do {                                                                         \
        if (len < (n)) {                                                         \
            DLOGW("%s body of %u bytes is too short", dash_cmd_name(t), (unsigned)len); \
            return -1;                                                           \
        }                                                                        \
    } while (0)

int dash_cmd_parse(const uint8_t *p, size_t n, dash_cmd_t *c)
{
    memset(c, 0, sizeof *c);
    if (n < 10 || memcmp(p, "S0.0=", 5) != 0 || memcmp(p + n - 5, ",G0.0", 5) != 0) {
        DLOGW("command is not S0.0=...,G0.0");
        return -1;
    }
    size_t l = (size_t)p[5] << 8 | p[6];
    if (l != n - 12) {
        DLOGW("command length %u, want %u", (unsigned)l, (unsigned)(n - 12));
        return -1;
    }
    const uint8_t *d = p + 7;
    size_t dn = n - 12;
    if (dn < 7) {
        DLOGW("command too short");
        return -1;
    }
    memcpy(c->id, d, 4);
    uint16_t lsf = (uint16_t)(d[4] << 8 | d[5]);
    size_t length = lsf & 1023;
    c->seq = (lsf >> 10) & 0x0f;
    if (length + 8 != dn) {
        DLOGW("command length field %u, want %u", (unsigned)length, (unsigned)(dn - 8));
        return -1;
    }
    uint8_t t = d[6];
    c->raw_type = t;
    const uint8_t *b = d + 7;
    size_t len = dn - 7 - 2; /* less the type and the CRC */
    DLOGI("command 0x%02x %s seq %u", t, dash_cmd_name(t), c->seq);

    switch (t) {
    case DASH_CMD_GET_VERSION:
        NEED(1);
        if (b[0] != 4) {
            DLOGW("GET_VERSION length %u, want 4", b[0]);
            return -1;
        }
        c->type = t;
        break;
    case DASH_CMD_SET_UNIQUE_ID:
        NEED(5);
        memcpy(c->unique_id, b + 1, 4);
        c->type = t;
        break;
    case DASH_CMD_PROGRAM_ALERTS: {
        NEED(1);
        int count = (uint8_t)(b[0] + 1 - 5) / 6;
        for (int i = 0; i < count; i++) {
            size_t at = 5 + (size_t)i * 6;
            NEED(at + 1);
            c->alert_mask |= (uint8_t)(1u << ((b[at] & 0x70) >> 4));
        }
        c->type = t;
        break;
    }
    case DASH_CMD_PROGRAM_INSULIN:
        NEED(13);
        c->table_num = b[5];
        c->duration = b[8];
        c->pulses = (uint16_t)(b[11] << 8 | b[12]);
        c->type = t;
        break;
    case DASH_CMD_GET_STATUS:
        NEED(2);
        c->status_type = b[1];
        c->type = t;
        break;
    case DASH_CMD_SILENCE_ALERTS:
        NEED(6);
        c->alert_mask = b[5];
        c->type = t;
        break;
    case DASH_CMD_STOP_DELIVERY:
        NEED(6);
        c->stop_bolus = b[5] & 4;
        c->stop_temp_basal = b[5] & 2;
        c->stop_basal = b[5] & 1;
        c->type = t;
        break;
    case DASH_CMD_DEACTIVATE:
    case DASH_CMD_PROGRAM_BEEPS:
    case DASH_CMD_CNFG_DELIV_FLAG:
        c->type = t;
        break;
    default:
        DLOGI("command 0x%02x isn't simulated: answering with a NACK", t);
        c->type = DASH_CMD_NACK;
        break;
    }
    return 0;
}

bool dash_cmd_response_hardcoded(const dash_cmd_t *c)
{
    switch (c->type) {
    case DASH_CMD_GET_STATUS: {
        uint8_t s = c->status_type;
        return !(s == 0 || s == 1 || s == 2 || s == 3 || s == 5 || s == 7);
    }
    case DASH_CMD_SILENCE_ALERTS:
    case DASH_CMD_PROGRAM_ALERTS:
    case DASH_CMD_PROGRAM_INSULIN:
    case DASH_CMD_PROGRAM_BEEPS:
    case DASH_CMD_STOP_DELIVERY:
        return false;
    default:
        return true;
    }
}

bool dash_cmd_mutates(const dash_cmd_t *c)
{
    switch (c->type) {
    case DASH_CMD_SET_UNIQUE_ID:
    case DASH_CMD_SILENCE_ALERTS:
    case DASH_CMD_PROGRAM_ALERTS:
    case DASH_CMD_PROGRAM_INSULIN:
    case DASH_CMD_DEACTIVATE:
    case DASH_CMD_PROGRAM_BEEPS:
    case DASH_CMD_STOP_DELIVERY:
        return true;
    default:
        return false;
    }
}
