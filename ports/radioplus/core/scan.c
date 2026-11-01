/*
 * scan.c — see scan.h.
 *
 * No allocation, no libc beyond copying bytes by hand, no knowledge of any
 * tuner.
 */

#include "scan.h"

static void copy_name(char *dst, int cap, const char *src)
{
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Trailing spaces are normal in an RDS station name - the field is eight
   characters and short names are padded - and a preset called "BBC R4  " sorts
   and compares differently from one called "BBC R4". */
static void trim(char *s)
{
    int n = 0;
    while (s[n]) n++;
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static int band_channels(const en_region_t *rg)
{
    if (!rg || !rg->step_khz || rg->high_khz <= rg->low_khz) return 0;
    return (int)((rg->high_khz - rg->low_khz) / rg->step_khz) + 1;
}

bool en_scan_start(en_scan_t *s, const en_region_t *rg, uint32_t from_khz,
                   uint8_t threshold, bool use_seek)
{
    int n = band_channels(rg);
    if (!s || n <= 0) return false;

    /* Zeroed by hand: core/ has no string.h, and the fields are few enough
       that naming them is also the list of what a scan consists of. */
    s->phase = EN_SCAN_SWEEP;
    s->region = rg;
    s->use_seek = use_seek;
    s->threshold = threshold;
    s->sweep_ms = 100;
    /* A hardware seek crosses however much empty band lies before the next
       station, so it is given far longer than a step - and the timeout is a
       backstop for a seek that never lands, not the normal path. */
    s->seek_ms = 2000;
    s->name_ms = 2500;
    s->khz = rg->low_khz;
    s->from_khz = rg->low_khz;
    s->last_khz = 0;
    s->waited_ms = 0;
    s->peak = 0;
    s->seek_pending = false;
    /*
     * The bottom of the band counts as a landed station.
     *
     * The caller tunes there before the first tick, and a seeking sweep used
     * to immediately seek away from it - so whatever was on the first channel
     * was never measured and never recorded. Starting in the settling state
     * means the first thing the sweep does is judge where it already is,
     * which is also what the software sweep does.
     */
    s->settling = use_seek;
    s->done = 0;
    s->total = n;
    s->n_hits = 0;
    s->overflowed = false;
    s->resume_khz = from_khz;

    s->profile_n = (uint16_t)(n > EN_SCAN_PROFILE_MAX ? EN_SCAN_PROFILE_MAX : n);
    for (uint16_t i = 0; i < s->profile_n; i++) s->profile[i] = 0;
    /* A seeking sweep never looks at the empty channels, so what it produces
       is spikes rather than a landscape, and the dial has to know which. */
    s->profile_sparse = use_seek;
    return true;
}

void en_scan_stop(en_scan_t *s)
{
    if (!s) return;
    s->phase = EN_SCAN_IDLE;
}

void en_scan_seek_failed(en_scan_t *s)
{
    if (!s || !s->use_seek) return;
    s->use_seek = false;
    s->seek_pending = false;
    s->settling = false;
    s->waited_ms = 0;
    s->peak = 0;
}

/* Take the strongest reading seen while sitting here, not the last one. RSSI
   on a just-tuned channel climbs for a moment; a station that fades during the
   dwell should be judged on its best moment rather than on whichever instant
   we happened to look. */
static void observe(en_scan_t *s, uint8_t rssi)
{
    if (rssi > s->peak) s->peak = rssi;
}

/* Note this channel's reading in the band profile, wherever it falls. */
static void profile_put(en_scan_t *s, uint32_t khz, uint8_t rssi)
{
    if (!s->region || !s->region->step_khz || khz < s->region->low_khz) return;
    uint32_t i = (khz - s->region->low_khz) / s->region->step_khz;
    if (i >= s->profile_n) return;
    if (rssi > s->profile[i]) s->profile[i] = rssi;
}

static void record(en_scan_t *s, uint32_t khz, uint8_t rssi)
{
    if (s->n_hits >= EN_SCAN_MAX_HITS) { s->overflowed = true; return; }

    en_scan_hit_t *h = &s->hits[s->n_hits++];
    h->khz = khz;
    h->rssi = rssi;
    h->pi = 0;
    h->pty = 0;
    h->rbds = s->region ? s->region->rbds : false;
    h->named = false;
    h->name[0] = 0;
}

/* Sweep over, on to the names - or straight to the end if nothing answered. */
static en_scan_req_t start_naming(en_scan_t *s, uint32_t *tune_khz)
{
    s->waited_ms = 0;
    s->peak = 0;

    if (s->n_hits == 0) {
        s->phase = EN_SCAN_DONE;
        *tune_khz = s->resume_khz;
        return EN_SCAN_TUNE;
    }
    s->phase = EN_SCAN_NAMING;
    s->done = 0;
    s->total = s->n_hits;
    s->khz = s->hits[0].khz;
    *tune_khz = s->khz;
    return EN_SCAN_TUNE;
}

/* One channel at a time, in software. */
static en_scan_req_t sweep_step(en_scan_t *s, uint32_t *tune_khz)
{
    s->last_khz = s->khz;
    profile_put(s, s->khz, s->peak);
    if (s->peak >= s->threshold) record(s, s->khz, s->peak);

    s->done++;
    s->peak = 0;
    s->waited_ms = 0;

    if (s->done >= s->total) return start_naming(s, tune_khz);

    s->khz = en_region_step(s->region, s->khz, true);
    *tune_khz = s->khz;
    return EN_SCAN_TUNE;
}

/*
 * Let the chip find the next station.
 *
 * Three states, because a seek is not instantaneous and neither is the RSSI
 * reading that follows it. Issue the seek; wait for the frequency to move;
 * then sit still briefly so the reading has settled before it is judged.
 * Collapsing the last of those into the landing tick was the first version,
 * and it read RSSI on the instant the chip retuned, which is when it is
 * lowest.
 *
 * It is over when the seek wraps. Seeking upward off the top of the band
 * comes back round to the bottom, so a frequency at or below the low edge is
 * the end of the band and not a new station.
 */
static en_scan_req_t sweep_seek(en_scan_t *s, uint32_t khz, uint32_t *tune_khz)
{
    if (!s->seek_pending && !s->settling) {
        s->from_khz = khz;
        s->waited_ms = 0;
        s->peak = 0;
        s->seek_pending = true;
        return EN_SCAN_SEEK;
    }

    if (s->seek_pending) {
        if (khz != s->from_khz) {
            s->seek_pending = false;
            s->settling = true;
            s->waited_ms = 0;
            s->peak = 0;
            s->khz = khz;
            return EN_SCAN_WAIT;
        }
        if (s->waited_ms < s->seek_ms) return EN_SCAN_WAIT;

        /* The seek never landed. Rather than sit through the timeout again on
           every remaining station, walk the rest in software. */
        s->seek_pending = false;
        s->use_seek = false;
        s->waited_ms = 0;
        s->peak = 0;
        return EN_SCAN_WAIT;
    }

    /* Settling. */
    if (s->waited_ms < s->sweep_ms) return EN_SCAN_WAIT;
    s->settling = false;

    /*
     * The end of the band is a seek that did not go up.
     *
     * This was "at or below the bottom of the band", which is also true of
     * the first station a sweep finds - because the sweep starts at the
     * bottom of the band. So the first landing ended the sweep, every time,
     * with no hits, and the app tuned straight back to where it started. It
     * looked like a scan that did nothing and it was a scan that ran one
     * step and stopped.
     *
     * Comparing against the highest channel reached instead is the real
     * test: an upward seek climbs until it wraps, and a chip that returns
     * the same station twice also fails to climb, which ends it just as
     * correctly rather than looping forever.
     */
    if (s->last_khz && s->khz <= s->last_khz)
        return start_naming(s, tune_khz);
    s->last_khz = s->khz;

    profile_put(s, s->khz, s->peak);
    if (s->peak >= s->threshold) record(s, s->khz, s->peak);

    /* Progress is where in the band we are, which is the only honest measure
       when the number of stations is not known in advance. */
    if (s->region && s->region->step_khz && s->khz >= s->region->low_khz)
        s->done = (int)((s->khz - s->region->low_khz) / s->region->step_khz);

    if (s->n_hits >= EN_SCAN_MAX_HITS) return start_naming(s, tune_khz);
    return EN_SCAN_WAIT;
}

en_scan_req_t en_scan_tick(en_scan_t *s, uint32_t dt_ms, uint32_t khz,
                           uint8_t rssi, const en_rds_t *rds,
                           uint32_t *tune_khz)
{
    uint32_t dummy;
    if (!tune_khz) tune_khz = &dummy;
    if (!s || s->phase == EN_SCAN_IDLE || s->phase == EN_SCAN_DONE)
        return EN_SCAN_WAIT;

    s->waited_ms += dt_ms;
    observe(s, rssi);

    if (s->phase == EN_SCAN_SWEEP) {
        if (s->use_seek) return sweep_seek(s, khz, tune_khz);
        if (s->waited_ms < s->sweep_ms) return EN_SCAN_WAIT;
        return sweep_step(s, tune_khz);
    }

    /* Naming. Take the name the moment it is complete rather than sitting out
       the full dwell: on a strong station it arrives in well under a second,
       and waiting anyway would triple the length of the pass for nothing. */
    en_scan_hit_t *h = &s->hits[s->done];
    if (rds && rds->ps_valid) {
        copy_name(h->name, (int)sizeof h->name, rds->ps);
        trim(h->name);
        h->named = h->name[0] != 0;
        h->pi = rds->pi;
        h->pty = rds->pty;
        h->rbds = rds->rbds;
    }
    if (h->rssi < s->peak) h->rssi = s->peak;

    if (h->named || s->waited_ms >= s->name_ms) {
        s->done++;
        s->waited_ms = 0;
        s->peak = 0;

        if (s->done >= s->total) {
            s->phase = EN_SCAN_DONE;
            *tune_khz = s->resume_khz;
            return EN_SCAN_TUNE;
        }
        s->khz = s->hits[s->done].khz;
        *tune_khz = s->khz;
        return EN_SCAN_TUNE;
    }
    return EN_SCAN_WAIT;
}

uint8_t en_scan_percent(const en_scan_t *s)
{
    if (!s) return 0;
    if (s->phase == EN_SCAN_DONE) return 100;
    if (s->phase == EN_SCAN_IDLE || s->total <= 0) return 0;

    /* The sweep is most of the time and most of the bar. Splitting it 70/30
       rather than by channel count keeps the bar moving at roughly one speed:
       a naming step is twenty-five times longer than a software sweep step, so
       equal weighting would crawl through the first three quarters and stall. */
    int within = (s->done * 100) / s->total;
    if (within > 100) within = 100;
    if (s->phase == EN_SCAN_SWEEP) return (uint8_t)((within * 70) / 100);
    return (uint8_t)(70 + (within * 30) / 100);
}

uint8_t en_scan_commit(const en_scan_t *s, en_presets_t *p)
{
    if (!s || !p) return 0;

    uint8_t n = 0;
    for (uint8_t i = 0; i < s->n_hits; i++) {
        const en_scan_hit_t *h = &s->hits[i];

        int at = en_preset_find(p, h->khz);
        if (at >= 0) {
            /* Already known. Fill in what the scan learned and leave the rest
               alone - in particular leave `simple` alone, because a rescan
               must not rearrange the screen the user chose. */
            en_preset_t *e = &p->list[at];
            if (h->named) copy_name(e->name, (int)sizeof e->name, h->name);
            if (h->pi) e->pi = h->pi;
            e->pty = h->pty;
            e->rbds = h->rbds;
            n++;
            continue;
        }

        en_preset_t e;
        e.khz = h->khz;
        e.pi = h->pi;
        e.pty = h->pty;
        e.rbds = h->rbds;
        e.simple = false;
        copy_name(e.name, (int)sizeof e.name, h->name);
        if (en_preset_add(p, &e)) n++;
    }

    en_preset_sort(p);
    return n;
}
