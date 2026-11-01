/*
 * fmcmd.c — see fmcmd.h.
 */

#include "fmcmd.h"

void en_fm_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

uint16_t en_fm_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Common head of both forms: H4 type, opcode little-endian, parameter length,
   I2C address, read/write mode. Returns the offset just past it. */
static uint8_t head(uint8_t *out, uint8_t addr, en_fm_mode_t mode,
                    uint8_t param_len)
{
    out[0] = EN_H4_COMMAND;
    out[1] = (uint8_t)(EN_FM_OPCODE & 0xFFu);          /* 0x15 */
    out[2] = (uint8_t)((EN_FM_OPCODE >> 8) & 0xFFu);   /* 0xFC */
    out[3] = param_len;
    out[4] = addr;
    out[5] = (uint8_t)mode;
    return 6;
}

uint8_t en_fm_build_write(uint8_t addr, const uint8_t *payload, uint8_t len,
                          uint8_t *out, uint8_t out_cap)
{
    if (!out) return 0;
    if (len && !payload) return 0;

    /* H4 type + opcode + length byte + address + mode + payload. */
    uint32_t total = 4u + 2u + (uint32_t)len;
    if (total > out_cap || total > EN_FM_CMD_MAX) return 0;

    uint8_t n = head(out, addr, EN_FM_WRITE, (uint8_t)(2u + len));
    for (uint8_t i = 0; i < len; i++) out[n + i] = payload[i];
    return (uint8_t)(n + len);
}

uint8_t en_fm_build_read(uint8_t addr, uint8_t read_len,
                         uint8_t *out, uint8_t out_cap)
{
    if (!out || read_len == 0) return 0;

    /* Read_Length is present only when the mode is Read, and it is part of the
       binary message — so a read always carries exactly three parameters. */
    uint32_t total = 4u + 3u;
    if (total > out_cap || total > EN_FM_CMD_MAX) return 0;

    uint8_t n = head(out, addr, EN_FM_READ, 3u);
    out[n] = read_len;
    return (uint8_t)(n + 1u);
}

bool en_fm_parse_event(const uint8_t *evt, uint16_t len, en_fm_reply_t *out)
{
    if (!evt || !out) return false;

    /* Everything up to and including Read_Write_Mode has to be present before
       any of it can be trusted. */
    if (len < EN_FM_VALUE_OFFSET) return false;

    if (evt[0] != EN_H4_EVENT) return false;
    if (evt[1] != EN_HCI_COMMAND_COMPLETE) return false;

    /* The event's own length field has to agree with what was received, or the
       buffer has been truncated and the tail is somebody else's memory. */
    uint16_t plen = evt[2];
    if ((uint16_t)(plen + 3u) > len) return false;

    /* Parameter length also has to cover the packet count, opcode and the
       three leading return parameters. */
    if (plen < 6u) return false;

    uint16_t opcode = (uint16_t)(evt[4] | ((uint16_t)evt[5] << 8));
    if (opcode != EN_FM_OPCODE) return false;

    out->status = evt[6];
    out->addr   = evt[7];
    out->mode   = (evt[8] == 0) ? EN_FM_WRITE : EN_FM_READ;

    /* plen counts from evt[3]; the value is whatever is left after the packet
       count, the two opcode bytes and the three leading parameters. */
    uint16_t value_len = (uint16_t)(plen - 6u);
    if (value_len == 0 || out->mode == EN_FM_WRITE) {
        out->value = 0;
        out->value_len = 0;
    } else {
        out->value = &evt[EN_FM_VALUE_OFFSET];
        out->value_len = (value_len > 255u) ? 255u : (uint8_t)value_len;
    }
    return true;
}
