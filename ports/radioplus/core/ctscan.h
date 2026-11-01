/*
 * ctscan.h — collect the time of day from the stations on the band.
 *
 * Every RDS station may transmit group 4A, which carries the date and the time
 * in UTC plus the local offset. Broadcasters take this seriously - it is
 * usually fed from the same reference that times the transmitter - so on a
 * device with no network and a hardware clock nobody has set, the band is a
 * perfectly good source of the time. This visits stations, collects what each
 * one says, and lets the caller pick.
 *
 * Two things make it a different job from the band scan next door.
 *
 *   It is SLOW, and unavoidably so. Group 4A is transmitted about once a
 *   minute. The band scan sits on each station only until the name arrives,
 *   which is usually under a second, so it would never see a clock group at
 *   all. This has to be prepared to wait a minute per station, which is why it
 *   is given candidates rather than sweeping for them: the sweep has already
 *   been done, and the strongest few stations are worth a minute each where
 *   two hundred empty channels are not.
 *
 *   The ANSWER HAS AN AGE. The standard puts the minute edge at the start of
 *   the group carrying the time, so what a station sends is exact at the
 *   instant it arrives and stale immediately afterwards. A clock set from a
 *   reading taken forty seconds ago is forty seconds slow, and forty seconds
 *   is a lot to be wrong by when the source was accurate to the millisecond.
 *   So every result records when it landed and the caller is expected to add
 *   the age back on.
 *
 * The whole thing is a state machine over an externally supplied clock with no
 * knowledge of any tuner - it says what it wants tuned and is told what it
 * got, the same shape as scan.h. That is what lets it be tested with no
 * hardware and no waiting.
 *
 * Pure C99, no allocation, no I/O.
 */

#ifndef RADIOPLUS_CTSCAN_H
#define RADIOPLUS_CTSCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "rds.h"

/* Candidates in, results out. Twelve of each is more stations than any band
   has strong ones, and far more than anybody will wait through. */
#define EN_CTSCAN_MAX 12

/* How long to sit on one station before giving up on it. Group 4A comes about
   once a minute, so anything under that is a coin toss; this allows for the
   worst case plus the time it takes RDS to lock after a retune. */
#define EN_CTSCAN_DWELL_MS 70000u

typedef enum {
    EN_CTSCAN_IDLE = 0,
    EN_CTSCAN_RUNNING,
    EN_CTSCAN_DONE
} en_ctscan_phase_t;

typedef enum {
    EN_CTSCAN_WAIT = 0,   /* nothing to do; keep ticking */
    EN_CTSCAN_TUNE        /* tune to *tune_khz */
} en_ctscan_req_t;

typedef struct {
    uint32_t khz;
    char     name[9];       /* the programme service name, if RDS gave one */
    uint16_t pi;

    int64_t  unix_utc;      /* what the station said, seconds since the epoch */
    int8_t   offset;        /* the local offset it transmitted, in half hours */
    uint32_t at_ms;         /* scan time when it landed - see the note above */
    bool     got;
} en_ctscan_hit_t;

typedef struct {
    en_ctscan_phase_t phase;

    uint32_t cand[EN_CTSCAN_MAX];
    uint8_t  n_cand;
    uint8_t  at;            /* which candidate is being listened to */

    en_ctscan_hit_t hits[EN_CTSCAN_MAX];
    uint8_t  n_hits;

    uint32_t dwell_ms;
    uint32_t waited_ms;     /* on the current station */
    uint32_t elapsed_ms;    /* since the scan started, and after it ends */

    uint32_t ct_mark;       /* rds->ct_groups when this station was tuned */
    bool     marked;        /* whether ct_mark has been taken for this one */
    uint32_t resume_khz;
} en_ctscan_t;

/*
 * Begin, with the frequencies to try in the order to try them.
 *
 * Strongest first is the order that matters: a strong station usually yields a
 * clock group well inside the dwell and a weak one usually yields nothing, so
 * the ordering decides whether this takes thirty seconds or six minutes.
 *
 * `resume_khz` is where to go back to at the end. Returns false if there is
 * nothing to do.
 */
bool en_ctscan_start(en_ctscan_t *s, const uint32_t *khz, uint8_t n,
                     uint32_t resume_khz);

/* Give up now and go back. Safe on an idle scan. */
void en_ctscan_stop(en_ctscan_t *s);

/*
 * One tick. `rds` is the live decoder state for whatever is tuned right now.
 *
 * Returns EN_CTSCAN_TUNE with *tune_khz set when it wants to move, which
 * includes the final move back to resume_khz as it finishes.
 *
 * The caller is expected to reset its decoder on every retune, the same as it
 * does for the band scan - what a previous station said is not evidence about
 * this one. This does not depend on it, but the station names in the results
 * do.
 */
en_ctscan_req_t en_ctscan_tick(en_ctscan_t *s, uint32_t dt_ms,
                               const en_rds_t *rds, uint32_t *tune_khz);

/* How stale a result is, in milliseconds, and therefore how much to add to it
   before setting a clock. Grows for as long as the caller keeps ticking. */
uint32_t en_ctscan_age_ms(const en_ctscan_t *s, uint8_t i);

/* The corrected time: what the station said, plus how long ago it said it.
   Zero when that result does not hold a time. */
int64_t en_ctscan_now(const en_ctscan_t *s, uint8_t i);

/* 0..100, for a progress bar. */
uint8_t en_ctscan_percent(const en_ctscan_t *s);

#endif /* RADIOPLUS_CTSCAN_H */
