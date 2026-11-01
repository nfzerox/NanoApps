/*
 * timer.c — see timer.h.
 */

#include "timer.h"

/* Five minutes to three hours. Long enough at the top for a whole programme,
   short enough at the bottom to catch a news bulletin, and every value is one
   a person would actually say out loud. */
const uint16_t EN_REC_LIMITS[] = { 0, 5, 15, 30, 60, 90, 120, 180 };
const uint8_t  EN_REC_LIMITS_COUNT =
    (uint8_t)(sizeof EN_REC_LIMITS / sizeof EN_REC_LIMITS[0]);

void en_rectimer_init(en_rectimer_t *t)
{
    if (!t) return;
    t->limit_min = 0;
    t->at_min = EN_REC_AT_NONE;
    t->fired = false;
}

en_rectimer_act_t en_rectimer_tick(en_rectimer_t *t, bool have_clock,
                                   int now_min, bool recording,
                                   uint32_t rec_ms)
{
    if (!t) return EN_REC_NOTHING;

    /* Stopping comes first. If a recording is running and has reached its
       length, that is true whatever the schedule says, and checking it first
       means a scheduled start cannot extend a recording past its limit by
       firing again during the same minute. */
    if (recording && t->limit_min) {
        uint32_t limit_ms = (uint32_t)t->limit_min * 60000u;
        if (rec_ms >= limit_ms) return EN_REC_STOP;
    }

    if (t->at_min == EN_REC_AT_NONE) {
        t->fired = false;
        return EN_REC_NOTHING;
    }

    /* No clock, no scheduled start. Firing whenever the time eventually
       arrives would start a recording at a moment nobody chose, and the user
       could not tell that from the one they asked for. */
    if (!have_clock) return EN_REC_NOTHING;

    if (now_min != (int)t->at_min) {
        /* Past the minute, or not yet at it. Either way the next arrival is a
           fresh one - which is what makes the same time work again tomorrow. */
        t->fired = false;
        return EN_REC_NOTHING;
    }

    if (recording || t->fired) return EN_REC_NOTHING;

    t->fired = true;
    return EN_REC_START;
}

int en_rectimer_until(const en_rectimer_t *t, bool have_clock, int now_min)
{
    if (!t || t->at_min == EN_REC_AT_NONE || !have_clock) return -1;

    int d = (int)t->at_min - now_min;
    if (d < 0) d += 24 * 60;      /* tomorrow, not a negative wait */
    return d;
}
