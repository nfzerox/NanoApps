/*
 * clock_linux.c — clock.h on the N31.
 *
 * settimeofday for the running system, and RTC_SET_TIME on the first RTC that
 * accepts it for the part that survives a power cycle.
 */

#include "clock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * The RTC ioctl, spelled out.
 *
 * _IOW('p', 0x0a, struct rtc_time) as ARM EABI packs it: nine ints is
 * thirty-six bytes, so 0x40000000 | (36 << 16) | ('p' << 8) | 0x0a. Written as
 * a number for the same reason the HCI ioctls in tuner_linux.c are - there is
 * no guarantee this toolchain has the kernel's uapi headers, and one constant
 * is cheaper than a build that breaks on a different sysroot.
 */
#define RTC_SET_TIME_ 0x4024700au

/* struct rtc_time is struct tm's first nine fields and nothing else - same
   layout, same conventions: months from zero, years from 1900. */
struct rtc_time_ {
    int tm_sec, tm_min, tm_hour;
    int tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
};

static char s_desc[96];

int64_t en_clock_now(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, 0) != 0) return 0;
    return (int64_t)tv.tv_sec;
}

/*
 * The first RTC that opens for writing.
 *
 * Numbered rather than named because which index this device's PMIC clock
 * lands on depends on probe order, and a hard-coded /dev/rtc0 is a device that
 * silently stops keeping time the day another RTC driver loads first.
 */
static int rtc_open(char *which, size_t n)
{
    static const char *const PATHS[] = { "/dev/rtc", "/dev/rtc0", "/dev/rtc1" };
    unsigned i;

    for (i = 0; i < sizeof PATHS / sizeof PATHS[0]; i++) {
        int fd = open(PATHS[i], O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            if (which && n) snprintf(which, n, "%s", PATHS[i]);
            return fd;
        }
    }
    return -1;
}

en_clock_err_t en_clock_set_unix(int64_t utc)
{
    struct timeval tv;
    struct tm g;
    struct rtc_time_ rt;
    time_t t;
    char which[24] = "";
    int fd;

    /*
     * A time before this app existed is not a time; it is a decode that went
     * wrong. Cheap to check and it stops one corrupt group from setting the
     * hardware clock to 1970 - which is exactly the state this feature exists
     * to get out of.
     */
    if (utc < 1700000000LL) return EN_CLOCK_FAILED;

    t = (time_t)utc;
    if (!gmtime_r(&t, &g)) return EN_CLOCK_FAILED;

    /* The running system first. If this is refused there is no point trying
       the hardware, and the errno is the same one either way. */
    tv.tv_sec = t;
    tv.tv_usec = 0;
    if (settimeofday(&tv, 0) != 0)
        return (errno == EPERM) ? EN_CLOCK_DENIED : EN_CLOCK_FAILED;

    fd = rtc_open(which, sizeof which);
    if (fd < 0) {
        snprintf(s_desc, sizeof s_desc, "no RTC device (%s)", strerror(errno));
        return EN_CLOCK_NO_RTC;
    }

    memset(&rt, 0, sizeof rt);
    rt.tm_sec  = g.tm_sec;
    rt.tm_min  = g.tm_min;
    rt.tm_hour = g.tm_hour;
    rt.tm_mday = g.tm_mday;
    rt.tm_mon  = g.tm_mon;
    rt.tm_year = g.tm_year;
    rt.tm_wday = g.tm_wday;
    rt.tm_yday = g.tm_yday;
    rt.tm_isdst = 0;      /* the hardware clock is UTC and has no summer */

    if (ioctl(fd, RTC_SET_TIME_, &rt) != 0) {
        int e = errno;
        close(fd);
        snprintf(s_desc, sizeof s_desc, "%s refused the time: %s",
                 which, strerror(e));
        return (e == EPERM || e == EACCES) ? EN_CLOCK_DENIED : EN_CLOCK_NO_RTC;
    }
    close(fd);

    snprintf(s_desc, sizeof s_desc, "%s, UTC", which);
    return EN_CLOCK_OK;
}

const char *en_clock_backend(void)
{
    if (!s_desc[0]) {
        char which[24] = "";
        int fd = rtc_open(which, sizeof which);

        if (fd >= 0) {
            close(fd);
            snprintf(s_desc, sizeof s_desc, "%s, UTC", which);
        } else {
            snprintf(s_desc, sizeof s_desc, "no RTC device");
        }
    }
    return s_desc;
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
