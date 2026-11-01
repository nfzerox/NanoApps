/*
 * fmreg.h — the FM_RDS_Command register map, as data.
 *
 * Thirty-nine registers reached through one HCI command. Describing them rather
 * than coding them is what makes "every single feature of the tuner" a finite
 * job: a screen full of toggles, dropdowns and sliders can be generated from
 * this table, and adding a register is adding a row.
 *
 * Two things in here are deliberately conservative.
 *
 * Read lengths are fixed per register and the command specification is explicit
 * that getting one wrong is a malformed bus transaction, not a wrong answer.
 * Several are marked TBD upstream; those carry EN_FM_TBD rather than a guess,
 * and en_fm_read_len() reports them as unknown so a caller has to decide
 * consciously instead of inheriting a number nobody verified.
 *
 * Three registers say in their own documentation that they need read, modify,
 * write — writing them blind clobbers bits the caller never looked at. They
 * carry EN_FM_RMW so the UI can enforce it.
 *
 * Pure C99, no allocation, no I/O. Testable with no tuner present.
 */

#ifndef RADIOPLUS_FMREG_H
#define RADIOPLUS_FMREG_H

#include <stdbool.h>
#include <stdint.h>

/* ---- register flags ------------------------------------------------------ */

#define EN_FM_R    0x01u   /* readable */
#define EN_FM_W    0x02u   /* writable */
#define EN_FM_RMW  0x04u   /* the docs require read, modify, write */
#define EN_FM_TBD  0x08u   /* read length unverified upstream */

/* Read-length sentinels. Both mean "the caller must choose", for different
   reasons: VAR because the register genuinely returns a variable amount, and
   UNKNOWN because nobody has established what it returns. */
#define EN_FM_LEN_VAR     0xFFu
#define EN_FM_LEN_UNKNOWN 0xFEu

/* ---- field flags --------------------------------------------------------- */

#define EN_FMF_SIGNED 0x01u
#define EN_FMF_BITMAP 0x02u
#define EN_FMF_ENUM   0x04u

typedef struct { uint32_t mask;  const char *name; } en_fm_bit_t;
typedef struct { uint32_t value; const char *name; } en_fm_enum_t;

/*
 * One value inside a register payload.
 *
 * `off` and `width` locate a big-endian integer within the payload; `bit_hi`
 * and `bit_lo` then select a run of bits inside it, inclusive, counting from 0
 * at the least significant bit. A field occupying the whole width has
 * bit_hi = width*8 - 1 and bit_lo = 0.
 *
 * This shape is needed because several registers pack unrelated controls into
 * one value — the audio control register carries a 9-bit bandwidth selector and
 * a 7-bit flag set in a single 16-bit word, and the search control register
 * puts a direction bit above a 7-bit threshold.
 */
typedef struct {
    const char *name;
    const char *doc;
    uint8_t     off;
    uint8_t     width;      /* 1, 2 or 4 */
    uint8_t     bit_hi;
    uint8_t     bit_lo;
    uint8_t     flags;
    int32_t     min;
    int32_t     max;
    const en_fm_bit_t  *bits;
    uint8_t             nbits;
    const en_fm_enum_t *vals;
    uint8_t             nvals;
} en_fm_field_t;

typedef struct {
    uint8_t              addr;
    const char          *name;
    const char          *doc;
    const char          *unit;      /* NULL, or e.g. "kHz offset from 64 MHz" */
    uint8_t              read_len;  /* bytes, or a sentinel above */
    uint8_t              write_len; /* payload bytes for a write; 0 if none */
    uint8_t              flags;
    const en_fm_field_t *fields;
    uint8_t              nfields;
} en_fm_reg_t;

/* The table, and its length. Ordered by address so a binary search is possible,
   though with thirty-nine entries a scan is not worth avoiding. */
extern const en_fm_reg_t en_fm_regs[];
extern const uint8_t     en_fm_reg_count;

/* NULL if the address is not a known register. */
const en_fm_reg_t *en_fm_reg_find(uint8_t addr);

/* Bytes to ask for when reading, or 0 when the caller has to decide — either
   because the register is variable length or because the length was never
   established. Check the register's flags to tell those apart. */
uint8_t en_fm_read_len(uint8_t addr);

/* Extract a field from a payload. Sign-extends when the field is signed.
   Returns 0 if the field would read past `len`. */
int32_t en_fm_field_get(const en_fm_field_t *f, const uint8_t *payload,
                        uint8_t len);

/* Insert a field into a payload, leaving every other bit untouched — which is
   what makes read-modify-write registers safe to edit one control at a time.
   Returns false if the value is out of range or the field would write past
   `len`, in which case the payload is not modified. */
bool en_fm_field_set(const en_fm_field_t *f, uint8_t *payload, uint8_t len,
                     int32_t value);

/* Frequency helpers. The tuner works in kHz offset from 64 MHz, which is a
   convenient encoding and a terrible thing to have scattered through UI code. */
#define EN_FM_BASE_KHZ 64000u

static inline uint16_t en_fm_khz_to_reg(uint32_t khz)
{
    return (uint16_t)(khz - EN_FM_BASE_KHZ);
}

static inline uint32_t en_fm_reg_to_khz(uint16_t reg)
{
    return (uint32_t)reg + EN_FM_BASE_KHZ;
}

/* ---- stereo handling ------------------------------------------------------
 *
 * The three ways the chip can decide between stereo and mono, and the bits in
 * I2C_FM_CTRL that select them. Here rather than in the UI because the bit
 * layout belongs with the rest of the register knowledge, and because a
 * function that edits one register without disturbing the others is worth
 * testing.
 */
typedef enum {
    EN_FM_STEREO_AUTO = 0,   /* the chip decides from signal quality */
    EN_FM_STEREO_MONO,       /* forced mono - steadier on a weak signal */
    EN_FM_STEREO_STEREO      /* forced stereo, whatever the signal is doing */
} en_fm_stereo_t;

#define EN_FM_CTRL_ADDR        0x01u
#define EN_FM_CTRL_AUTO        0x02u
#define EN_FM_CTRL_MANUAL      0x04u
#define EN_FM_CTRL_BLEND       0x08u

/* Apply `mode` to a value read from I2C_FM_CTRL, leaving every other bit -
   the band, the injection side, the blend choice - exactly as it was. */
uint8_t en_fm_ctrl_set_stereo(uint8_t ctrl, en_fm_stereo_t mode);

/* And read it back out, so the UI can show what the register actually says
   rather than only what it last asked for. */
en_fm_stereo_t en_fm_ctrl_stereo(uint8_t ctrl);

#endif /* RADIOPLUS_FMREG_H */
