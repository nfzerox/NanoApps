/*
 * wav.h - build a WAV header.
 *
 * No I/O, so the same builder serves the Linux recorder writing through stdio
 * and the RetailOS one writing through hb_fs_stream_write. A recording is
 * written header-first with a length nobody knows yet, so the header is emitted
 * with zero lengths and patched when the file closes - and if the app dies
 * before that, en_wav_repair() turns the placeholder into the real thing from
 * the file size alone, which is the difference between a lost recording and a
 * playable one.
 */

#ifndef RADIOPLUS_WAV_H
#define RADIOPLUS_WAV_H

#include <stdbool.h>
#include <stdint.h>

#define EN_WAV_HDR_BYTES 44u

/* Write a 44-byte canonical PCM header. `data_bytes` may be 0 for a stream
   whose length is not yet known; call en_wav_patch_len() on close. */
uint32_t en_wav_header(uint8_t *out, uint32_t cap, uint32_t rate,
                       uint16_t channels, uint16_t bits, uint32_t data_bytes);

/* Rewrite the two length fields of an existing header in place. */
bool en_wav_patch_len(uint8_t *hdr, uint32_t cap, uint32_t data_bytes);

/* Read the data length a header claims. 0 if it is not a header we wrote. */
uint32_t en_wav_data_len(const uint8_t *hdr, uint32_t len);

#endif /* RADIOPLUS_WAV_H */
