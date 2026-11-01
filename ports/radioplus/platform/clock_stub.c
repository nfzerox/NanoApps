/*
 * clock_stub.c — clock.h where there is no clock to set.
 *
 * The host preview and the RetailOS build both read the time and neither has
 * any business writing it: the preview runs on a workstation whose clock is
 * already right, and setting the system time out from under RetailOS is not
 * this app's decision to make.
 *
 * en_clock_now still answers, because the screens show the time either way.
 */

#include "clock.h"

#include <time.h>

int64_t en_clock_now(void)
{
    return (int64_t)time(0);
}

en_clock_err_t en_clock_set_unix(int64_t utc)
{
    (void)utc;
    return EN_CLOCK_UNSUPPORTED;
}

const char *en_clock_backend(void)
{
    return "read only on this platform";
}

const char *en_clock_strerror(en_clock_err_t e)
{
    switch (e) {
    case EN_CLOCK_OK:          return "set";
    case EN_CLOCK_UNSUPPORTED: return "not supported here";
    case EN_CLOCK_DENIED:      return "not permitted";
    case EN_CLOCK_NO_RTC:      return "set until the next reboot; no RTC took it";
    default:                   return "failed";
    }
}
