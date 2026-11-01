/*
 * ctscan.c — the state machine behind ctscan.h.
 *
 * Sit on a station until it says what time it is, or until waiting any longer
 * stops being worth it. Then the next one.
 */

#include "ctscan.h"

#include <string.h>

static void copy_name(char *dst, int cap, const char *src)
{
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    for (; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;

    /* The programme service name is space-padded to eight characters, and a
       name with three spaces after it sorts and compares badly. */
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = 0;
}

/* Point at the next candidate and ask for it. Ends the scan when there is
   none, which is the only place the phase becomes DONE. */
static en_ctscan_req_t advance(en_ctscan_t *s, uint32_t *tune_khz)
{
    s->waited_ms = 0;
    s->marked = false;      /* re-taken against whatever is tuned next */

    if (s->at + 1 >= s->n_cand) {
        s->phase = EN_CTSCAN_DONE;
        *tune_khz = s->resume_khz;
        return EN_CTSCAN_TUNE;
    }

    s->at++;
    *tune_khz = s->cand[s->at];
    return EN_CTSCAN_TUNE;
}

bool en_ctscan_start(en_ctscan_t *s, const uint32_t *khz, uint8_t n,
                     uint32_t resume_khz)
{
    uint8_t i;

    if (!s || !khz || !n) return false;

    memset(s, 0, sizeof *s);

    if (n > EN_CTSCAN_MAX) n = EN_CTSCAN_MAX;
    for (i = 0; i < n; i++) {
        s->cand[i] = khz[i];
        s->hits[i].khz = khz[i];
    }
    s->n_cand = n;
    s->n_hits = n;          /* one row per candidate, filled in as they answer */

    s->dwell_ms = EN_CTSCAN_DWELL_MS;
    s->resume_khz = resume_khz;
    s->phase = EN_CTSCAN_RUNNING;
    return true;
}

void en_ctscan_stop(en_ctscan_t *s)
{
    if (!s || s->phase == EN_CTSCAN_IDLE) return;
    s->phase = EN_CTSCAN_IDLE;
}

en_ctscan_req_t en_ctscan_tick(en_ctscan_t *s, uint32_t dt_ms,
                               const en_rds_t *rds, uint32_t *tune_khz)
{
    uint32_t dummy;
    en_ctscan_hit_t *h;

    if (!tune_khz) tune_khz = &dummy;
    if (!s) return EN_CTSCAN_WAIT;

    /*
     * Time keeps accumulating after the scan has finished, and that is
     * deliberate: the results are only as good as their age, and the age has
     * to keep growing while somebody reads the list and decides. A scan that
     * stopped its clock at the end would hand out a time that was right when
     * it was collected and is quietly wrong by the time it is used.
     */
    s->elapsed_ms += dt_ms;

    if (s->phase != EN_CTSCAN_RUNNING) return EN_CTSCAN_WAIT;

    s->waited_ms += dt_ms;
    h = &s->hits[s->at];

    /*
     * Take the counter's starting value on the first tick after tuning.
     *
     * It cannot be taken in advance(), which has no decoder to read, and it
     * must not be assumed to be zero: whether the caller resets the decoder on
     * retune is the caller's business, and if it does not, the count arriving
     * here is the PREVIOUS station's. Without this the second station in the
     * list is credited with the first one's clock within a millisecond of
     * being tuned - a wrong answer that looks like a very fast right one.
     */
    if (!s->marked) {
        s->ct_mark = rds ? rds->ct_groups : 0;
        s->marked = true;
    }

    /*
     * A clock group landing is the signal, not the clock fields having values.
     *
     * ct_valid stays true from the first group onwards, and the decoder state
     * belongs to whatever is tuned - so on the second station a check for
     * "does it have a time" would be answered by the first station's time
     * within a millisecond of tuning. The counter moving is the only thing
     * that means THIS station just spoke.
     */
    if (rds && !h->got && rds->ct_groups != s->ct_mark) {
        s->ct_mark = rds->ct_groups;

        if (rds->ct_valid) {
            h->unix_utc = en_rds_ct_unix(rds);
            h->offset = rds->ct_offset;
            h->at_ms = s->elapsed_ms;
            h->pi = rds->pi;
            if (rds->ps_valid) copy_name(h->name, (int)sizeof h->name, rds->ps);
            h->got = h->unix_utc != 0;
        }

        /* Answered, so there is nothing else to wait for here. */
        if (h->got) return advance(s, tune_khz);
    }

    /* Take the name whenever it turns up, whether or not the clock ever does -
       a row that says which station failed to answer is worth more than a
       bare frequency. */
    if (rds && rds->ps_valid && !h->name[0])
        copy_name(h->name, (int)sizeof h->name, rds->ps);

    if (s->waited_ms >= s->dwell_ms) return advance(s, tune_khz);
    return EN_CTSCAN_WAIT;
}

uint32_t en_ctscan_age_ms(const en_ctscan_t *s, uint8_t i)
{
    if (!s || i >= s->n_hits || !s->hits[i].got) return 0;
    if (s->elapsed_ms < s->hits[i].at_ms) return 0;
    return s->elapsed_ms - s->hits[i].at_ms;
}

int64_t en_ctscan_now(const en_ctscan_t *s, uint8_t i)
{
    if (!s || i >= s->n_hits || !s->hits[i].got) return 0;

    /*
     * Rounded to the nearest second rather than truncated. Half a second of
     * bias on every reading is avoidable, and this is the one number in the
     * app where a systematic error would accumulate into the hardware clock.
     */
    return s->hits[i].unix_utc + (int64_t)((en_ctscan_age_ms(s, i) + 500u) / 1000u);
}

uint8_t en_ctscan_percent(const en_ctscan_t *s)
{
    uint32_t done, total;

    if (!s) return 0;
    if (s->phase == EN_CTSCAN_DONE) return 100;
    if (s->phase == EN_CTSCAN_IDLE || !s->n_cand) return 0;

    /*
     * Counted in milliseconds of dwell rather than in stations, because the
     * stations are not the same size: one that answers in five seconds and one
     * that runs the full seventy are a fourteenth of the work apart, and a bar
     * that gave them equal weight would leap and then stall.
     */
    total = (uint32_t)s->n_cand * s->dwell_ms;
    done = (uint32_t)s->at * s->dwell_ms + s->waited_ms;
    if (done > total) done = total;
    return (uint8_t)((done * 100u) / total);
}
