/*
 * build_stamp.h — which build is this, actually.
 *
 * Shared by every app in this tree. It exists because of a specific and very
 * expensive kind of confusion: an app on the device's disk that is months old
 * looks exactly like the one you just compiled, right up until you spend an
 * afternoon debugging a fault that was fixed in April.
 *
 * So every app can say, on screen and on the command line, when it was built
 * and from which commit. If a version on the device does not match what the
 * tree says, that is the first thing to find out and it should take one look.
 *
 * The stamp is baked in with -D at compile time by a rule that ALWAYS runs -
 * see any of the app makefiles. A stamp compiled once and then cached would
 * be worse than none: it would say "current" while being stale, which is
 * exactly the failure it exists to catch.
 */

#ifndef NANOAPPS_BUILD_STAMP_H
#define NANOAPPS_BUILD_STAMP_H

/* "20260830.1422" - UTC, sortable, and short enough for a status bar. */
const char *en_build_stamp(void);

/* Short commit hash, or "nogit" when built outside a checkout. */
const char *en_build_commit(void);

/* "20260830.1422 g1a2b3c4", for one line on an about screen. */
const char *en_build_version(void);

#endif /* NANOAPPS_BUILD_STAMP_H */
