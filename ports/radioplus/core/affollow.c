/*
 * affollow.c — see affollow.h.
 */

#include "affollow.h"

void en_af_init(en_affollow_t *a)
{
    if (!a) return;
    a->enabled = false;

    /* Defaults chosen to be reluctant. The failure everyone has heard is a
       radio that hops away from a perfectly good station, so every threshold
       here errs towards staying put. */
    a->weak_rssi = 40;
    a->weak_ms = 6000;
    a->try_ms = 900;
    a->margin = 12;
    a->cooldown_ms = 45000;

    a->phase = EN_AF_IDLE;
    a->elapsed = 0;
    a->next = 0;
    a->home_khz = 0;
    a->home_pi = 0;
    a->home_rssi = 0;
    a->moves = 0;
    a->rejects = 0;
}

/* Stop looking for a while, without moving. Used where no candidate was ever
   tried, so the tuner is still where it started and a "return home" would be
   a retune to the frequency we are already on - inaudible, but it drops RDS
   and restarts the decoder for nothing. */
static en_af_act_t back_off(en_affollow_t *a)
{
    a->phase = EN_AF_COOLDOWN;
    a->elapsed = 0;
    a->next = 0;
    return EN_AF_STAY;
}

/* Abandon a round that did move the tuner, and put it back exactly. */
static en_af_act_t go_home(en_affollow_t *a, uint32_t *khz_out)
{
    back_off(a);
    if (!a->home_khz) return EN_AF_STAY;
    *khz_out = a->home_khz;
    return EN_AF_GOTO;
}

en_af_act_t en_af_enable(en_affollow_t *a, bool on, uint32_t *khz)
{
    uint32_t dummy;
    if (!khz) khz = &dummy;
    if (!a) return EN_AF_STAY;

    a->enabled = on;
    if (on) {
        a->phase = EN_AF_WATCH;
        a->elapsed = 0;
        a->next = 0;
        return EN_AF_STAY;
    }

    /* Turning it off during an attempt has to undo the attempt. Leaving the
       listener parked on a candidate they never chose, because they switched
       the feature off at the wrong moment, would be the worst possible
       behaviour for the control that exists to be a way out. */
    bool mid = (a->phase == EN_AF_TRYING);
    a->phase = EN_AF_IDLE;
    a->elapsed = 0;
    a->next = 0;
    if (mid && a->home_khz) {
        *khz = a->home_khz;
        return EN_AF_GOTO;
    }
    return EN_AF_STAY;
}

void en_af_retuned(en_affollow_t *a, uint32_t khz)
{
    if (!a) return;
    a->home_khz = khz;
    a->home_pi = 0;
    a->home_rssi = 0;
    a->next = 0;
    a->elapsed = 0;
    if (a->enabled) a->phase = EN_AF_WATCH;
}

/* Start the next candidate, or give up on the round. */
static en_af_act_t try_next(en_affollow_t *a, const en_rds_t *rds,
                            uint32_t *khz_out)
{
    while (rds && a->next < rds->af_count) {
        uint32_t cand = rds->af[a->next++];
        /* The list often contains the frequency you are already on. */
        if (cand == a->home_khz || !cand) continue;

        a->phase = EN_AF_TRYING;
        a->elapsed = 0;
        *khz_out = cand;
        return EN_AF_GOTO;
    }
    return go_home(a, khz_out);
}

en_af_act_t en_af_tick(en_affollow_t *a, uint32_t dt_ms, uint32_t khz,
                       uint8_t rssi, const en_rds_t *rds, uint32_t *khz_out)
{
    uint32_t dummy;
    if (!khz_out) khz_out = &dummy;
    if (!a || !a->enabled) return EN_AF_STAY;

    a->elapsed += dt_ms;

    switch (a->phase) {
    case EN_AF_IDLE:
        a->phase = EN_AF_WATCH;
        a->elapsed = 0;
        return EN_AF_STAY;

    case EN_AF_COOLDOWN:
        if (a->elapsed < a->cooldown_ms) {
            /* Recovering on its own ends the cooldown early: the reason to
               wait was that nothing better was found, and a signal that came
               back means there is nothing to look for. */
            if (rssi > a->weak_rssi + a->margin) {
                a->phase = EN_AF_WATCH;
                a->elapsed = 0;
            }
            return EN_AF_STAY;
        }
        a->phase = EN_AF_WATCH;
        a->elapsed = 0;
        return EN_AF_STAY;

    case EN_AF_WATCH:
        /* Home is wherever we are while things are going well, and its
           strength is what a candidate will have to beat. */
        a->home_khz = khz;
        a->home_rssi = rssi;
        if (rds && rds->pi_valid) a->home_pi = rds->pi;

        if (rssi > a->weak_rssi) { a->elapsed = 0; return EN_AF_STAY; }
        a->phase = EN_AF_WEAK;
        a->elapsed = 0;
        return EN_AF_STAY;

    case EN_AF_WEAK:
        if (rssi > a->weak_rssi) {          /* it came back */
            a->phase = EN_AF_WATCH;
            a->elapsed = 0;
            return EN_AF_STAY;
        }
        if (a->elapsed < a->weak_ms) return EN_AF_STAY;

        /* Nothing to move to. Wait before asking again rather than checking
           an empty list every tick for the rest of the journey. */
        if (!rds || rds->af_count == 0 || !a->home_pi)
            return back_off(a);

        a->next = 0;
        return try_next(a, rds, khz_out);

    case EN_AF_TRYING:
        if (a->elapsed < a->try_ms) return EN_AF_STAY;

        /*
         * Two conditions, and both matter.
         *
         * The PI has to match, because that is the only thing that makes this
         * the same station rather than a different one that happens to be on
         * a frequency our station listed.
         *
         * And it has to be meaningfully stronger than what we left. "The same
         * station, but worse" is not a reason to move, and accepting equal
         * signals is how a radio ends up swapping between two transmitters
         * for ever at the point where they cross.
         */
        if (rds && rds->pi_valid && rds->pi == a->home_pi &&
            rssi > (uint32_t)a->home_rssi + a->margin) {
            a->moves++;
            a->home_khz = khz;
            a->home_rssi = rssi;
            a->phase = EN_AF_WATCH;
            a->elapsed = 0;
            a->next = 0;
            return EN_AF_STAY;
        }

        a->rejects++;
        return try_next(a, rds, khz_out);
    }
    return EN_AF_STAY;
}

const char *en_af_state_text(const en_affollow_t *a)
{
    if (!a || !a->enabled) return "AF";
    switch (a->phase) {
    case EN_AF_WEAK:     return "AF WEAK";
    case EN_AF_TRYING:   return "AF TRY";
    case EN_AF_COOLDOWN: return "AF WAIT";
    default:             return "AF ON";
    }
}
