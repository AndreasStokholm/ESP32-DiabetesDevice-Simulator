/*
 * The pod's responses: captured real-pod templates with the simulated
 * pod's values written in, exactly as the Go simulator builds them.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>

#include "dash_internal.h"

static size_t unhex(const char *h, uint8_t *out)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        uint8_t v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[k];
            v = (uint8_t)(v << 4);
            if (c >= '0' && c <= '9') v |= (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint8_t)(c - 'A' + 10);
        }
        out[n++] = v;
    }
    return n;
}

#define RESERVOIR_REPORTED_BELOW 1000 /* pulses: 50 U; above it the pod says "50+ U" */

size_t dash_rsp_general(const dash_status_t *r, uint8_t *out)
{
    size_t n = unhex("1D1800A02800000463FF", out);

    out[1] = (uint8_t)(r->progress & 0x0f);
    if (r->extended_bolus_active) {
        out[1] |= 0x08 << 4;
    } else if (r->bolus_active) {
        out[1] |= 0x04 << 4;
    }
    if (r->temp_basal_active) {
        out[1] |= 0x02 << 4;
    } else if (r->basal_active) {
        out[1] |= 0x01 << 4;
    }

    out[2] = (uint8_t)(r->delivered >> 9);
    out[3] = (uint8_t)((r->delivered >> 1) & 0xff);
    out[4] = (uint8_t)((out[4] & 0xfe) | ((r->delivered & 1) << 7));
    out[4] = (uint8_t)((out[4] & 0x87) | ((r->last_prog_seq & 0x0f) << 3));
    out[4] = (uint8_t)((out[4] & 0xf8) | ((r->bolus_remaining >> 8) & 0x07));
    out[5] = (uint8_t)(r->bolus_remaining & 0xff);

    out[6] = (uint8_t)((out[6] & 0x80) | (r->alerts >> 1));
    out[7] = (uint8_t)((out[7] & 0x7f) | (uint8_t)(r->alerts << 7));
    out[7] = (uint8_t)((out[7] & 0x80) | ((r->minutes_active >> 6) & 0x7f));
    out[8] = (uint8_t)((out[8] & 0x03) | ((r->minutes_active << 2) & 0xfc));

    if (r->reservoir <= RESERVOIR_REPORTED_BELOW) {
        out[8] = (uint8_t)((out[8] & 0xfc) | (r->reservoir >> 8));
        out[9] = (uint8_t)(r->reservoir & 0xff);
    } else {
        out[8] |= 0x03;
        out[9] = 0xff;
    }
    return n;
}

size_t dash_rsp_detailed(const dash_status_t *r, uint8_t *out)
{
    size_t n = unhex("0216020d0000000001b200000003ff01cc00000000000000", out);

    if (r->fault == 0) {
        out[3] = (uint8_t)r->progress;
        if (r->extended_bolus_active) {
            out[4] |= 0x08;
        } else if (r->bolus_active) {
            out[4] |= 0x04;
        }
        if (r->temp_basal_active) {
            out[4] |= 0x02;
        } else if (r->basal_active) {
            out[4] |= 0x01;
        }
    }
    out[5] = (uint8_t)(r->bolus_remaining >> 8);
    out[6] = (uint8_t)r->bolus_remaining;
    out[7] = r->last_prog_seq;
    out[8] = (uint8_t)(r->delivered >> 8);
    out[9] = (uint8_t)r->delivered;
    out[10] = r->fault;
    out[11] = (uint8_t)(r->fault_time >> 8);
    out[12] = (uint8_t)r->fault_time;
    if (r->reservoir <= RESERVOIR_REPORTED_BELOW) {
        out[13] = (uint8_t)(r->reservoir >> 8);
        out[14] = (uint8_t)r->reservoir;
    } else {
        out[13] = 0x03;
        out[14] = 0xff;
    }
    out[15] = (uint8_t)(r->minutes_active >> 8);
    out[16] = (uint8_t)r->minutes_active;
    out[17] = r->alerts;
    if (r->fault != 0) {
        out[19] = (uint8_t)(r->progress & 0x0f);
        if (r->bolus_active) {
            out[19] |= 0x10;
        }
    }
    return n;
}

size_t dash_rsp_type1(const uint16_t triggered[8], uint8_t *out)
{
    size_t n = unhex("021301000000000000000000000000000000000000", out);
    for (int i = 0; i < 8; i++) {
        out[2 * i + 5] = (uint8_t)(triggered[i] >> 8);
        out[2 * i + 6] = (uint8_t)triggered[i];
    }
    return n;
}

size_t dash_rsp_type3(uint8_t fault, uint16_t fault_time, uint16_t minutes_active, uint8_t *out)
{
    size_t n = unhex(
        "02f8030000000d4c043c287331002d733b003072320035733b00387232003d733b004072320045723d00487232004d723c00"
        "5072330055723e00587235805d733d806073348001713e800450338009503c800c50338011513b801475328019723a801c72"
        "328021723b002472330029733f002c7334003172400034723500397240003c733400417241004473340049733e004c733300"
        "51733d005473330059733f805c73348061713e800074338005723d80087233800d723d801073338015733c80187334801d72"
        "3d802073340025724000287336002d7241003072370035734100387337003d7243004072380045724400487238004d714300",
        out);
    out[3] = fault;
    out[4] = (uint8_t)(fault_time >> 8);
    out[5] = (uint8_t)fault_time;
    out[6] = (uint8_t)(minutes_active >> 8);
    out[7] = (uint8_t)minutes_active;
    return n;
}

size_t dash_rsp_type5(uint8_t fault, uint16_t fault_time, uint8_t year, uint8_t month, uint8_t day, uint8_t hour,
                      uint8_t minute, uint8_t *out)
{
    size_t n = unhex("0211051c12c000000000000000000b1917100c", out);
    out[3] = fault;
    out[4] = (uint8_t)(fault_time >> 8);
    out[5] = (uint8_t)fault_time;
    out[14] = month;
    out[15] = day;
    out[16] = year;
    out[17] = hour;
    out[18] = minute;
    return n;
}

size_t dash_rsp_type46(uint8_t *out)
{
    return unhex("0204460000", out);
}

size_t dash_rsp_type50(uint8_t *out)
{
    return unhex(
        "02CB500F184C902A005190320054912A80599033805C902B80019133BF04912ABF099033BF0C312ABF119033BF149029BF1991"
        "34BF1C91283F2190323F24902A3F2990333F2C912A3F3190353F34912A3F3991353F3C312A3F4191363F44902A3F4931343F4C"
        "31293F5131343F5431293F5931353F5C312ABF613035BF643029BF0131338104312981093033810C302A811130308114312A81"
        "193031011C302A012131320124312A01293133012C312A013130320134312A01393131013C312B014130320144312B0149303001",
        out);
}

size_t dash_rsp_type51(uint8_t *out)
{
    return unhex(
        "02CB5100324C90270151902F015490270159902F815C91268161902F810091268005902F80089028800D302F80109127801590"
        "2F80189026801D902F002091270025903100289028002D9031003090280035903100389129003D3131004091280045913100"
        "489027004D9030005090280055913100589128805D913180609128800190318004912980093031800C902980119130801491"
        "288019912E801C9128002191320024902900299132002C9129003190330034902A00393132003C912A004190320044902B00"
        "49913000",
        out);
}

size_t dash_rsp_nack(uint8_t *out)
{
    return unhex("0603070009", out);
}

/* Byte 8 of the version response and byte 15 of the setup response give the
 * product: 04 DASH, 05 Omnipod 5 (OmnipodKit's PodType). */
size_t dash_rsp_version(uint8_t *out, dash_pod_type_t type)
{
    size_t n = unhex("0115040A00010300040208146DB10006E45100FFFFFFFF", out);
    if (type == DASH_POD_O5) {
        out[8] = 0x05;
    }
    return n;
}

size_t dash_rsp_set_unique_id(uint8_t *out, dash_pod_type_t type)
{
    size_t n = unhex("011B13881008340A50040A00010300040308146DB10006E45100001091", out);
    if (type == DASH_POD_O5) {
        out[15] = 0x05;
    }
    return n;
}

int dash_rsp_message(const uint8_t *body, size_t body_len, uint8_t cmd_seq, uint8_t msg_seq, uint8_t ack_seq,
                     const uint8_t request_id[4], const uint8_t src[4], const uint8_t dst[4], bool with_crc,
                     dash_msg_t *out)
{
    size_t inner = 4 + 2 + body_len + 2;
    if (4 + 2 + inner > DASH_MSG_MAX) {
        return -1;
    }
    dash_msg_new(out, DASH_MSG_ENCRYPTED, src, dst);
    uint8_t *d = out->payload;
    memcpy(d, "0.0=", 4);
    d[4] = (uint8_t)(inner >> 8);
    d[5] = (uint8_t)inner;
    memcpy(d + 6, request_id, 4);
    uint16_t header = (uint16_t)((cmd_seq & 0x0f) << 10 | (body_len & 0x03ff));
    d[10] = (uint8_t)(header >> 8);
    d[11] = (uint8_t)header;
    memcpy(d + 12, body, body_len);
    /* The Omnipod CRC-16 over id, header and body. The Go DASH simulator sends
     * 0000, which OmnipodKit accepts from DASH pods but not from Omnipod 5. */
    uint16_t crc = with_crc ? dash_crc16(d + 6, 6 + body_len) : 0;
    d[12 + body_len] = (uint8_t)(crc >> 8);
    d[13 + body_len] = (uint8_t)crc;
    out->payload_len = 6 + inner;
    out->seq = msg_seq;
    out->ack = true;
    out->ack_num = ack_seq;
    out->eqos = 1;
    return 0;
}
