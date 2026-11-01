/*
 * wavout.c — see wavout.h.
 */

#include "wavout.h"

static uint8_t *put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return p + 4;
}

static uint8_t *put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    return p + 2;
}

static uint8_t *put_tag(uint8_t *p, const char *s)
{
    p[0] = (uint8_t)s[0];
    p[1] = (uint8_t)s[1];
    p[2] = (uint8_t)s[2];
    p[3] = (uint8_t)s[3];
    return p + 4;
}

uint32_t en_wav_size(uint32_t frames, uint16_t channels)
{
    return EN_WAV_HEADER_BYTES + frames * (uint32_t)channels * 2u;
}

uint32_t en_wav_header(uint8_t out[EN_WAV_HEADER_BYTES],
                       uint32_t sample_rate, uint16_t channels,
                       uint32_t frames)
{
    const uint16_t bits = 16;
    const uint16_t block_align = (uint16_t)(channels * (bits / 8));
    const uint32_t data_bytes = frames * (uint32_t)block_align;

    uint8_t *p = out;
    p = put_tag(p, "RIFF");
    p = put_u32(p, 36u + data_bytes);          /* everything after this field */
    p = put_tag(p, "WAVE");
    p = put_tag(p, "fmt ");
    p = put_u32(p, 16);                        /* PCM fmt chunk size */
    p = put_u16(p, 1);                         /* WAVE_FORMAT_PCM */
    p = put_u16(p, channels);
    p = put_u32(p, sample_rate);
    p = put_u32(p, sample_rate * block_align); /* byte rate */
    p = put_u16(p, block_align);
    p = put_u16(p, bits);
    p = put_tag(p, "data");
    (void)put_u32(p, data_bytes);

    return EN_WAV_HEADER_BYTES + data_bytes;
}
