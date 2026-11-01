/*
 * rds.h — decode RDS/RBDS groups into station state.
 *
 * Everything here is IEC 62106 (and the NRSC RBDS variant), which is a
 * published standard and completely independent of the tuner chip. That
 * separation is deliberate: how the BCM part frames its FIFO is undocumented in
 * the command specification and its Linux driver is not in this tree, so the
 * one function that guesses at framing is isolated at the bottom of this file
 * and named so nobody mistakes it for established fact. Group decoding above it
 * is verifiable against the standard and testable with no hardware.
 *
 * A group is four 16-bit blocks. Block A carries the programme identification,
 * block B the group type, traffic flag and programme type, and blocks C and D
 * carry whatever that group type says they do. Blocks arrive with error flags,
 * so every block is fed with a validity bit and a bad one is dropped rather
 * than allowed to corrupt accumulated text.
 *
 * RBDS matters here rather than being a footnote: North American broadcasters
 * use a completely different programme-type table, and I2C_RDS_CTRL bit 0
 * selects which the chip is decoding. Displaying "Serious Classical" for a
 * station transmitting "Classical" would be a small lie; displaying "Phone In"
 * for "Public" is a large one.
 *
 * Pure C99, no allocation, no I/O.
 */

#ifndef RADIOPLUS_RDS_H
#define RADIOPLUS_RDS_H

#include <stdbool.h>
#include <stdint.h>

/* Block validity, one bit per block, as passed to en_rds_group(). */
#define EN_RDS_A 0x1u
#define EN_RDS_B 0x2u
#define EN_RDS_C 0x4u
#define EN_RDS_D 0x8u
#define EN_RDS_ALL (EN_RDS_A | EN_RDS_B | EN_RDS_C | EN_RDS_D)

/* What a group changed, so the UI repaints only what moved. */
#define EN_RDS_CH_PI    0x0001u
#define EN_RDS_CH_PS    0x0002u
#define EN_RDS_CH_RT    0x0004u
#define EN_RDS_CH_PTY   0x0008u
#define EN_RDS_CH_TA    0x0010u
#define EN_RDS_CH_TP    0x0020u
#define EN_RDS_CH_MS    0x0040u
#define EN_RDS_CH_CT    0x0080u
#define EN_RDS_CH_AF    0x0100u
#define EN_RDS_CH_PTYN  0x0200u
#define EN_RDS_CH_ECC   0x0400u
#define EN_RDS_CH_PIN   0x0800u
#define EN_RDS_CH_DI    0x1000u
#define EN_RDS_CH_RTPLUS 0x2000u

#define EN_RDS_PS_LEN   8
#define EN_RDS_RT_LEN   64
#define EN_RDS_PTYN_LEN 8
#define EN_RDS_AF_MAX   25

/* Open Data Applications announced in group 3A. Eight is more than any
   broadcaster has ever been observed to run at once, and the ninth is dropped
   rather than allowed to push out the first. */
#define EN_RDS_ODA_MAX  8

typedef struct {
    uint16_t aid;      /* the registered application identifier */
    uint8_t  group;    /* type << 1 | version - the slot it was given */
} en_rds_oda_t;

typedef struct {
    /* Programme identification. Also the station's identity across an
       alternate-frequency jump, which is why it is tracked separately from
       everything else it implies. */
    uint16_t pi;
    bool     pi_valid;

    uint8_t  pty;          /* programme type; name depends on the RBDS flag */
    bool     tp;           /* this station carries traffic announcements */
    bool     ta;           /* one is on the air right now */
    bool     ms;           /* true for music, false for speech */
    uint8_t  di;           /* decoder identification, four bits */

    /* Programme service name: eight characters delivered two at a time. Only
       shown once every segment has arrived, because a half-filled name looks
       like a glitch rather than a station. */
    char     ps[EN_RDS_PS_LEN + 1];
    uint8_t  ps_seen;      /* bitmask of the four segments */
    bool     ps_valid;

    /* Radio text: 64 characters in group 2A (four at a time) or 32 in 2B (two
       at a time). The A/B flag toggling means the station started a new
       message, at which point the buffer has to be cleared or the old text
       shows through the gaps in the new one. */
    char     rt[EN_RDS_RT_LEN + 1];
    uint16_t rt_seen;
    bool     rt_valid;
    bool     rt_ab;
    bool     rt_ab_known;

    /* Programme type name, group 10A: an eight-character free-text refinement
       of the numeric programme type. */
    char     ptyn[EN_RDS_PTYN_LEN + 1];
    uint8_t  ptyn_seen;
    bool     ptyn_valid;

    /*
     * RadioText+ - the station saying which part of the radio text is the
     * title and which is the artist.
     *
     * `rtplus_group` is where to look for the markers, learnt from the
     * open-data announcement in group 3A. Most stations use 11A and some use
     * 12A, so it is not a constant: a decoder that hard-codes one shows
     * nothing at all on a station that picked the other.
     */
    uint8_t  rtplus_group;    /* group type << 1 | version; 0 = not announced */
    bool     rtplus_known;
    char     rt_title[EN_RDS_RT_LEN + 1];
    char     rt_artist[EN_RDS_RT_LEN + 1];
    bool     rt_title_valid;
    bool     rt_artist_valid;
    bool     rtplus_running;  /* the item is currently on air */
    bool     rtplus_toggle;   /* flips when the item changes */
    bool     rtplus_toggle_known;

    /* Alternate frequencies, in kHz. */
    uint32_t af[EN_RDS_AF_MAX];
    uint8_t  af_count;
    uint8_t  af_expected;  /* from the AF list header, 0 if not yet seen */

    /* Clock time and date, group 4A, already converted from modified Julian
       day and already offset to local time. */
    bool     ct_valid;
    uint16_t ct_year;
    uint8_t  ct_month, ct_day, ct_hour, ct_minute;
    int8_t   ct_offset;    /* local offset in half hours, as transmitted */

    /*
     * And the same instant exactly as it came off the air, before the offset
     * was applied.
     *
     * The fields above are local time, which is what a listener should see.
     * Anything that has to compute with the time wants the other one: a
     * hardware clock is conventionally UTC, and turning local back into UTC
     * means redoing the midnight arithmetic above in reverse and getting it
     * right twice. The modified Julian day and the transmitted hour and minute
     * are what the station actually sent, and en_rds_ct_unix() turns them into
     * a count of seconds with no calendar code at all.
     */
    uint32_t ct_mjd;
    uint8_t  ct_utc_hour, ct_utc_minute;

    /*
     * How many clock-time groups have arrived.
     *
     * Not a statistic. Group 4A is transmitted about once a minute, and the
     * standard puts the minute edge at the start of the group carrying it - so
     * the time is exact at the instant it lands and drifts from then on. A
     * caller that wants to set a clock has to know WHEN it arrived, and this
     * counter changing is the only signal that it just did.
     */
    uint32_t ct_groups;

    uint8_t  ecc;          /* extended country code, group 1A */
    bool     ecc_valid;
    uint16_t pin;          /* programme item number, group 1A */
    bool     pin_valid;

    /* Reception statistics. Useful on their own - a station with groups
       arriving but nothing decoding is a different problem from no signal. */
    en_rds_oda_t oda[EN_RDS_ODA_MAX];
    uint8_t  oda_count;

    uint32_t groups;
    uint32_t blocks_bad;
    uint32_t group_count[32];   /* indexed by type<<1 | version */

    bool     rbds;         /* interpret programme types as RBDS */
} en_rds_t;

/* RadioText+ content types worth naming. The full list runs to sixty-odd
   codes; these are the ones a now-playing display uses. */
#define EN_RTP_TITLE     1u
#define EN_RTP_ALBUM     2u
#define EN_RTP_ARTIST    4u
#define EN_RTP_BAND      9u
#define EN_RTP_PROG_NOW 33u

/* Reset to nothing received. Call on tune, and on any PI change. */
void en_rds_init(en_rds_t *r, bool rbds);

/*
 * Feed one group. `blk` is blocks A to D; `valid` says which of them arrived
 * intact. Returns a bitmask of what changed.
 *
 * Blocks flagged invalid are not merely ignored - a group missing block B has
 * no type and cannot be interpreted at all, and one missing block C or D drops
 * only the payload those blocks carried. Getting this wrong shows up as text
 * that fills with garbage on a weak signal.
 */
uint16_t en_rds_group(en_rds_t *r, const uint16_t blk[4], uint8_t valid);

/* Programme type name, honouring the RBDS flag. Never NULL. */
const char *en_rds_pty_name(uint8_t pty, bool rbds);

/* Decode an alternate-frequency code. Returns 0 for filler and codes that are
   not a frequency. */
uint32_t en_rds_af_khz(uint8_t code);

/*
 * The one guess in this file.
 *
 * I2C_RDS_DATA returns "RDS tuples" and I2C_RDS_WLINE counts them, but neither
 * the command specification nor anything else in this tree states a tuple
 * layout. Three bytes - two of data and one of block type and error status - is
 * what the BCM2048 family is understood to use, and it is the only layout that
 * fits a waterline counted in tuples against a 250-byte read.
 *
 * It is isolated here so that confirming or correcting it on the device changes
 * exactly one function and nothing above it. Everything else in this file is
 * the published standard and stands regardless.
 *
 * Returns the number of complete groups written to `out`, which must have room
 * for len/12 groups and the matching validity bytes.
 */
uint8_t en_rds_unpack_tuples_unverified(const uint8_t *fifo, uint16_t len,
                                        uint16_t (*out)[4], uint8_t *valid,
                                        uint8_t max_groups);

/*
 * The transmitted clock time as seconds since 1970-01-01 UTC, or 0 when no
 * usable clock group has arrived.
 *
 * Modified Julian day 40587 is the Unix epoch, so this is a subtraction and
 * two multiplications - no calendar, no leap years, nothing to get wrong. The
 * value is UTC: the transmitted local offset is reported separately in
 * ct_offset and is a display matter, not a timekeeping one.
 *
 * Exact at the moment the group arrived and not afterwards. See ct_groups.
 */
int64_t en_rds_ct_unix(const en_rds_t *r);

#endif /* RADIOPLUS_RDS_H */
