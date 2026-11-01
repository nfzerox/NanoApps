/*
 * timer.h — a recording that stops itself, and one that starts itself.
 *
 * Two separate things, deliberately.
 *
 * A DURATION is reliable. Press record, say "an hour", walk away: the app is
 * running the whole time by definition, because it is recording. Nothing can
 * go wrong with it that would not also go wrong with a recording you stopped
 * by hand.
 *
 * A START TIME is not, and the interface should not pretend otherwise. This
 * device has no real-time wake: something has to be running to notice that
 * the moment arrived, and that something is this app, on screen or blanked
 * but never closed. The clock it reads is the one broadcast in RDS group 4A,
 * which means it is only as good as the station's, arrives a minute or so
 * after tuning, and is absent entirely on a station that does not send it.
 * All of which is fine for "record the eight o'clock news" and is not fine
 * for anything that must not be missed.
 *
 * So the two are separate fields with separate controls, and the start time
 * is inert until there is a clock to compare it against - it does not guess,
 * and it does not fire late on a clock that turned up an hour after the
 * moment passed.
 */

#ifndef RADIOPLUS_TIMER_H
#define RADIOPLUS_TIMER_H

#include <stdbool.h>
#include <stdint.h>

/* No start time set. Stored rather than a separate flag so the whole thing is
   one number in the settings file. */
#define EN_REC_AT_NONE (-1)

typedef struct {
    /* Stop after this many minutes. 0 means run until stopped. */
    uint16_t limit_min;

    /* Minutes since local midnight to start at, or EN_REC_AT_NONE. */
    int16_t  at_min;

    /* Set once the scheduled recording has fired, so it does not fire again
       every minute for the rest of that minute - and cleared when the clock
       moves past it, so tomorrow works. */
    bool     fired;
} en_rectimer_t;

void en_rectimer_init(en_rectimer_t *t);

/*
 * What the timer wants to happen, given the clock and what is going on.
 *
 * `have_clock` is false when RDS has not delivered a time yet, and in that
 * case a start time never fires - a scheduled recording that begins at some
 * arbitrary later moment because the clock finally arrived is worse than one
 * that does not begin at all, because you would not know which you got.
 *
 * `now_min` is minutes since local midnight. `rec_ms` is how long the current
 * recording has been running, ignored when not recording.
 */
typedef enum {
    EN_REC_NOTHING = 0,
    EN_REC_START,
    EN_REC_STOP
} en_rectimer_act_t;

en_rectimer_act_t en_rectimer_tick(en_rectimer_t *t, bool have_clock,
                                   int now_min, bool recording,
                                   uint32_t rec_ms);

/* Minutes until the scheduled start, or -1 if there is nothing scheduled or
   no clock to measure against. Wraps to tomorrow, so 23:50 scheduled at 00:10
   reads as twenty minutes and not as minus one thousand four hundred. */
int en_rectimer_until(const en_rectimer_t *t, bool have_clock, int now_min);

/* The durations the UI offers, in minutes, 0 first for "until I stop it". */
extern const uint16_t EN_REC_LIMITS[];
extern const uint8_t  EN_REC_LIMITS_COUNT;

#endif /* RADIOPLUS_TIMER_H */
