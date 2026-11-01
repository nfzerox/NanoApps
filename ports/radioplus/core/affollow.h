/*
 * affollow.h — follow a station onto another transmitter when this one fades.
 *
 * A network broadcasts the same programme from several transmitters on
 * different frequencies, and group 0A carries the list of them. Driving out of
 * range of one, the radio can move to another and the listener hears nothing
 * happen. That is the whole feature.
 *
 * It is OFF by default and it is one tap to turn off, for a reason that is not
 * theoretical: the only thing making a candidate frequency "the same station"
 * is that its programme identification matches, and PI is sixteen bits sent by
 * whoever is transmitting. Stand next to something misconfigured, or something
 * deliberately lying, and a radio that trusts an AF list will walk itself onto
 * it. So:
 *
 *   - off unless asked for, and the badge that says it is on is also the
 *     control that turns it off;
 *   - a candidate is only accepted if its PI matches AND it is meaningfully
 *     stronger than what we left, because "the same station, but worse" is not
 *     a reason to move;
 *   - anything that fails puts the tuner back where it was, exactly;
 *   - and after a failed round it waits before trying again, so a fringe area
 *     produces one attempt a minute rather than a radio that hops for ever.
 *
 * A state machine over an externally supplied clock, with no knowledge of any
 * tuner: it says where it wants to be and is told what it found. Which is what
 * lets the awkward parts - a PI that does not match, a list where nothing is
 * better, a station that comes back on its own - be tested without a radio.
 */

#ifndef RADIOPLUS_AFFOLLOW_H
#define RADIOPLUS_AFFOLLOW_H

#include <stdbool.h>
#include <stdint.h>

#include "rds.h"

typedef enum {
    EN_AF_IDLE = 0,     /* off, or nothing to do */
    EN_AF_WATCH,        /* on, and the signal is fine */
    EN_AF_WEAK,         /* on, and the signal has been poor for a while */
    EN_AF_TRYING,       /* sitting on a candidate, deciding */
    EN_AF_COOLDOWN      /* a round failed; not trying again just yet */
} en_af_phase_t;

typedef enum {
    EN_AF_STAY = 0,     /* nothing to do */
    EN_AF_GOTO          /* tune to *khz, without persisting it */
} en_af_act_t;

typedef struct {
    bool     enabled;

    /* An RSSI at or below which the signal counts as poor, and how long it
       has to stay there. Seconds rather than an instant, because RSSI dips
       under a bridge and a radio that retunes at every dip is worse than one
       that never retunes at all. */
    uint8_t  weak_rssi;
    uint32_t weak_ms;

    /* How long a candidate gets to prove itself. Long enough for PI to
       arrive - it is in every group, so a fraction of a second on a decent
       signal, and never on a dead one. */
    uint32_t try_ms;

    /* How much stronger a candidate has to be before it is worth moving.
       Equal is not better, and swapping between two equal transmitters for
       ever is the classic AF failure. */
    uint8_t  margin;

    /* How long to leave it after a whole list has been tried and rejected. */
    uint32_t cooldown_ms;

    en_af_phase_t phase;
    uint32_t elapsed;        /* in the current phase */
    uint8_t  next;           /* index into the AF list */

    /* Where we were before this round, and how it was doing. Restored exactly
       when a round fails - the alternative is a radio that goes wandering and
       leaves you somewhere you did not choose. */
    uint32_t home_khz;
    uint16_t home_pi;
    uint8_t  home_rssi;

    uint32_t moves;          /* how many times it has actually moved */
    uint32_t rejects;        /* candidates turned down; worth showing */
} en_affollow_t;

void en_af_init(en_affollow_t *a);

/* Turn it on or off. Turning it off mid-attempt asks for the tuner to go back
   where it started, which is reported through the usual action. */
en_af_act_t en_af_enable(en_affollow_t *a, bool on, uint32_t *khz);

/*
 * Advance by `dt_ms`.
 *
 * `khz` is where the tuner is, `rssi` how it is doing there, and `rds` what
 * has been decoded since the last tune - the PI in particular, which is the
 * only thing that says a candidate is the same station.
 */
en_af_act_t en_af_tick(en_affollow_t *a, uint32_t dt_ms, uint32_t khz,
                       uint8_t rssi, const en_rds_t *rds, uint32_t *khz_out);

/* Something else retuned - the user, a preset, a scan. Whatever round was in
   progress is void, and the new frequency is home now. */
void en_af_retuned(en_affollow_t *a, uint32_t khz);

/* A short word for the badge: what it is doing right now. */
const char *en_af_state_text(const en_affollow_t *a);

#endif /* RADIOPLUS_AFFOLLOW_H */
