/*
 * wav.c - see wav.h.
 */

#include "wav.h"

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);  p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);        p[1] = (uint8_t)(v >> 8);
}

static void put4(uint8_t *p, const char *s)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)s[i];
}

uint32_t en_wav_header(uint8_t *out, uint32_t cap, uint32_t rate,
                       uint16_t channels, uint16_t bits, uint32_t data_bytes)
{
    if (!out || cap < EN_WAV_HDR_BYTES) return 0;
    if (!rate || !channels || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return 0;

    uint16_t block = (uint16_t)(channels * (bits / 8u));
    uint32_t byte_rate = rate * block;

    put4(out + 0, "RIFF");
    put32(out + 4, 36u + data_bytes);
    put4(out + 8, "WAVE");

    put4(out + 12, "fmt ");
    put32(out + 16, 16);            /* PCM fmt chunk size */
    put16(out + 20, 1);             /* PCM */
    put16(out + 22, channels);
    put32(out + 24, rate);
    put32(out + 28, byte_rate);
    put16(out + 32, block);
    put16(out + 34, bits);

    put4(out + 36, "data");
    put32(out + 40, data_bytes);

    return EN_WAV_HDR_BYTES;
}

bool en_wav_patch_len(uint8_t *hdr, uint32_t cap, uint32_t data_bytes)
{
    if (!hdr || cap < EN_WAV_HDR_BYTES) return false;
    put32(hdr + 4, 36u + data_bytes);
    put32(hdr + 40, data_bytes);
    return true;
}

uint32_t en_wav_data_len(const uint8_t *hdr, uint32_t len)
{
    if (!hdr || len < EN_WAV_HDR_BYTES) return 0;
    if (hdr[0] != 'R' || hdr[1] != 'I' || hdr[2] != 'F' || hdr[3] != 'F') return 0;
    if (hdr[36] != 'd' || hdr[37] != 'a' || hdr[38] != 't' || hdr[39] != 'a') return 0;
    return (uint32_t)hdr[40] | ((uint32_t)hdr[41] << 8)
         | ((uint32_t)hdr[42] << 16) | ((uint32_t)hdr[43] << 24);
}
