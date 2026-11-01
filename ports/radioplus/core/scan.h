/*
 * scan.h — sweep the band and turn what is on it into presets.
 *
 * Filling the preset list by hand means tuning to a station, deciding it is
 * worth keeping, saving it, and doing that twenty times. The tuner can do it.
 *
 * Two passes, deliberately.
 *
 *   The SWEEP asks only "is anything here". Where the chip can seek for
 *   itself it is asked to, and it jumps straight from one station to the next
 *   without this code ever naming the empty channels between them. Where it
 *   cannot, every channel is stepped and measured in software - a hundred
 *   milliseconds each, so a European band at 100 kHz spacing is 205 channels
 *   and twenty seconds.
 *
 *   The NAMING pass goes back to what the sweep found and sits on each one
 *   long enough for RDS to arrive. That is seconds, not milliseconds - the
 *   station name comes in group 0A, two characters at a time, and a weak
 *   signal can take several seconds to spell eight letters. Doing it on every
 *   channel would take eight minutes; doing it on the twenty that have
 *   something on them takes one.
 *
 * The chip can do more than seek: it has a preset-scan mode that would return
 * the whole station list in one go (I2C_FM_SEARCH_METHOD = Preset, then
 * I2C_FM_PRESET_MAX_CHANNEL and I2C_FM_PRESET_CHANNEL). That is not used here
 * and the reason is in fmreg.c: those three registers are marked EN_FM_TBD,
 * and the value of I2C_FM_SEARCH_TUNE_MODE that starts a preset search is not
 * one of the ones the bring-up sequence pinned down. Guessing at a mode
 * register on a chip nobody has been able to test against yet is how you get
 * a scan that appears to work and returns furniture. When the driver is up and
 * the mode value is known from something other than inference, this is the
 * file that changes, and only the sweep half of it.
 *
 * The whole thing is a state machine over an externally supplied clock with no
 * knowledge of any tuner. It says what it wants done and is told what it got.
 * That is what lets it run identically on the device and in the host preview,
 * and be tested with neither.
 */

#ifndef RADIOPLUS_SCAN_H
#define RADIOPLUS_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "rds.h"
#include "region.h"
#include "store.h"

/* Room for a full band of strong stations. Past this the sweep stops
   recording and en_scan_overflowed() says so - a scan that quietly kept the
   first forty and dropped the rest would look like a complete scan. */
#define EN_SCAN_MAX_HITS 40

/* One byte per channel of the widest band we support. Japan's 76-95 MHz at
   100 kHz is 191; Europe's 87.5-108 is 206; the Americas at 200 kHz is 101.
   Sized above all of them, and a band longer than this simply records the
   part that fits rather than refusing to scan. */
#define EN_SCAN_PROFILE_MAX 256

typedef struct {
    uint32_t khz;
    uint8_t  rssi;                        /* the best seen while dwelling */
    uint16_t pi;
    uint8_t  pty;
    bool     rbds;
    bool     named;                       /* RDS gave us a station name */
    char     name[EN_PRESET_NAME_LEN + 1];
} en_scan_hit_t;

typedef enum {
    EN_SCAN_IDLE = 0,
    EN_SCAN_SWEEP,      /* finding stations */
    EN_SCAN_NAMING,     /* sitting on each one, waiting for RDS */
    EN_SCAN_DONE
} en_scan_phase_t;

/* What the scan wants the caller to do next. */
typedef enum {
    EN_SCAN_WAIT = 0,   /* nothing; keep ticking */
    EN_SCAN_TUNE,       /* tune to *tune_khz */
    EN_SCAN_SEEK        /* let the chip find the next station upward */
} en_scan_req_t;

typedef struct {
    en_scan_phase_t phase;
    const en_region_t *region;

    bool     use_seek;       /* let the chip find stations for itself */
    uint8_t  threshold;      /* RSSI at or above which a channel counts */
    uint32_t sweep_ms;       /* dwell while stepping in software */
    uint32_t seek_ms;        /* how long to give one hardware seek */
    uint32_t name_ms;        /* dwell while waiting for a name */

    uint32_t khz;            /* the channel it believes it is on */
    uint32_t from_khz;       /* where the current seek started */
    /*
     * The highest channel this sweep has reached.
     *
     * A seek upward moves up until it runs off the top and comes back round,
     * so "not higher than last time" is what a wrap actually looks like. The
     * end of the sweep used to be tested as "at or below the bottom of the
     * band", which is also true of the very first station a sweep finds -
     * because a sweep starts at the bottom of the band.
     */
    uint32_t last_khz;
    uint32_t waited_ms;
    uint8_t  peak;           /* best RSSI seen on this channel so far */

    /* A seek has been asked for and the frequency has not moved yet; and the
       frequency has moved and the reading is being given a moment to settle.
       Never both. */
    bool     seek_pending;
    bool     settling;

    int      done;           /* channels swept, or hits named */
    int      total;          /* channels in the band, or hits to name */
    uint8_t  n_hits;
    bool     overflowed;
    en_scan_hit_t hits[EN_SCAN_MAX_HITS];

    /* Where the tuner was before the scan, so cancelling puts it back. A scan
       that leaves you on a different station than you started on has taken
       something away as well as given something. */
    uint32_t resume_khz;

    /*
     * What the band looked like: one signal reading per channel, kept from
     * the sweep so the dial can draw the landscape instead of a bare line.
     *
     * Nearly free - the sweep measured every one of these anyway and then
     * threw all but the peaks away.
     *
     * Only the SOFTWARE sweep visits every channel; a seeking sweep jumps
     * between stations and never looks at what is in between. So the profile
     * is marked as sparse in that case and the dial draws spikes at the
     * stations rather than a continuous floor - which is honest, and better
     * than a landscape with invented valleys in it.
     */
    uint8_t  profile[EN_SCAN_PROFILE_MAX];
    uint16_t profile_n;
    bool     profile_sparse;
} en_scan_t;

/*
 * Begin.
 *
 * `threshold` is an RSSI as the chip reports it, 0..255. `use_seek` asks the
 * chip to find stations rather than stepping every channel - pass what the
 * platform can actually do, since a seek that never lands would otherwise
 * stall the sweep until its timeout on every station.
 *
 * Returns false if the region is missing or has no channels, in which case the
 * scan stays idle rather than running over an empty band.
 */
bool en_scan_start(en_scan_t *s, const en_region_t *rg, uint32_t from_khz,
                   uint8_t threshold, bool use_seek);

/* Give up. Leaves `resume_khz` for the caller to tune back to. */
void en_scan_stop(en_scan_t *s);

/*
 * Advance by `dt_ms`, having observed `khz`, `rssi` and `rds`.
 *
 * `khz` is where the tuner actually is, which in seek mode is the only way to
 * know where the chip landed. Returns what it wants done next.
 */
en_scan_req_t en_scan_tick(en_scan_t *s, uint32_t dt_ms, uint32_t khz,
                           uint8_t rssi, const en_rds_t *rds,
                           uint32_t *tune_khz);

/* The caller could not seek. The sweep drops to stepping in software from
   here rather than waiting out the timeout on every station - a scan that is
   slower than it could be still finishes, and one that stalls does not. */
void en_scan_seek_failed(en_scan_t *s);

/* 0..100, across both passes, so the bar does not jump back to zero when the
   naming pass starts. */
uint8_t en_scan_percent(const en_scan_t *s);

/* Write the results into a preset list. Existing presets at the same
   frequency are updated rather than duplicated - a rescan should improve the
   list, not double it. Returns how many were added or updated. */
uint8_t en_scan_commit(const en_scan_t *s, en_presets_t *p);

#endif /* RADIOPLUS_SCAN_H */
