/*
 * wavout.h — minimal canonical WAV header builder.
 *
 * Deliberately does no I/O. core/ has to compile on a device with no stdio, so
 * this hands back 44 bytes and lets the caller write them with whatever it has:
 * fwrite on the host, hb_fs_stream_write on the iPod.
 *
 * Canonical 44-byte layout only — RIFF / fmt (16-byte PCM) / data, nothing
 * else. The OS loader's parser is not ours to second-guess, so the files we
 * hand it are the least surprising thing a WAV can be.
 */

#ifndef ENTRAIN_WAVOUT_H
#define ENTRAIN_WAVOUT_H

#include <stdint.h>

#define EN_WAV_HEADER_BYTES 44

/* Fill `out` with the header for `frames` frames of 16-bit PCM.
   Returns the total file size in bytes (header + PCM). */
uint32_t en_wav_header(uint8_t out[EN_WAV_HEADER_BYTES],
                       uint32_t sample_rate, uint16_t channels,
                       uint32_t frames);

/* Bytes a 16-bit file of this shape will occupy. Used by the loop planner to
   stay under the loader's 1 MiB ceiling. */
uint32_t en_wav_size(uint32_t frames, uint16_t channels);

#endif /* ENTRAIN_WAVOUT_H */
