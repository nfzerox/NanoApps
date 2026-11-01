/*
 * clock.h — setting the time, from a source that is not a network.
 *
 * This device has no network and a hardware clock that nobody has ever set, so
 * it boots believing it is some time in 1970 until a person tells it
 * otherwise. There is no NTP and there is no cell radio. There is, however, a
 * band full of broadcasters transmitting the time to the millisecond as part
 * of their normal output, which is what ctscan.h collects.
 *
 * Two clocks get set, and they are not the same clock.
 *
 *   The SYSTEM clock is what everything running right now reads. Setting it
 *   fixes timestamps on recordings immediately and is lost at the next
 *   power-off.
 *
 *   The HARDWARE clock is the battery-backed one in the PMIC. Setting it is
 *   what makes the answer survive a reboot, and it is the whole point.
 *
 * Both are UTC. The local offset a station transmits is a display matter and
 * is deliberately not applied here: a hardware clock holding local time is a
 * clock that is wrong twice a year, and this app already knows how to show
 * local time from UTC plus an offset.
 */

#ifndef RADIOPLUS_CLOCK_H
#define RADIOPLUS_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EN_CLOCK_OK = 0,
    EN_CLOCK_UNSUPPORTED,   /* this build cannot set a clock at all */
    EN_CLOCK_DENIED,        /* not privileged enough */
    EN_CLOCK_NO_RTC,        /* system clock set; nothing to make it stick */
    EN_CLOCK_FAILED
} en_clock_err_t;

/* Seconds since 1970-01-01 UTC, from the system clock. */
int64_t en_clock_now(void);

/*
 * Set both clocks to `utc`.
 *
 * EN_CLOCK_NO_RTC is a partial success and is reported as its own outcome
 * rather than folded into failure: the time is right until the next reboot,
 * which is worth knowing and worth saying, and is a completely different thing
 * to do about than being refused.
 */
en_clock_err_t en_clock_set_unix(int64_t utc);

/* Which hardware clock this found, for the settings screen - or why it found
   none. Never NULL. */
const char *en_clock_backend(void);

const char *en_clock_strerror(en_clock_err_t e);

#endif /* RADIOPLUS_CLOCK_H */
