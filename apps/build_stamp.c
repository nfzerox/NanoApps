/*
 * build_stamp.c — see build_stamp.h.
 *
 * Deliberately tiny and deliberately its own translation unit: it is the one
 * file that has to be recompiled on every build, and making it small keeps
 * that free. Everything else in the app rebuilds only when it changes.
 *
 * The defaults below are what you get if the makefile forgets to pass the
 * flags. "unknown" rather than a plausible-looking date, because a wrong
 * version is worse than an absent one - an absent one makes you go and look.
 */

#include "build_stamp.h"

#ifndef EN_BUILD_STAMP
#define EN_BUILD_STAMP "unknown"
#endif

#ifndef EN_BUILD_GIT
#define EN_BUILD_GIT "nogit"
#endif

const char *en_build_stamp(void)  { return EN_BUILD_STAMP; }
const char *en_build_commit(void) { return EN_BUILD_GIT; }

const char *en_build_version(void)
{
    /* Assembled once, into a static, rather than by the caller each time it
       wants to draw it. No allocation and no stdio: this file is linked into
       the freestanding device builds as well. */
    static char buf[48];
    if (buf[0]) return buf;

    const char *s = EN_BUILD_STAMP;
    const char *g = EN_BUILD_GIT;
    int n = 0;

    while (*s && n < (int)sizeof buf - 12) buf[n++] = *s++;
    buf[n++] = ' ';
    buf[n++] = 'g';
    while (*g && n < (int)sizeof buf - 1) buf[n++] = *g++;
    buf[n] = 0;
    return buf;
}
