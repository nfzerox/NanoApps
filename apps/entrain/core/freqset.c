/*
 * freqset.c — see freqset.h.
 *
 * Freestanding: no stdio, no strtod, no locale. A scanner over a buffer, in
 * the same style as progfile.c, which it deliberately mirrors so that the two
 * file readers can be read side by side.
 */

#include "freqset.h"

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static const char *line_end(const char *p, const char *end)
{
    while (p < end && *p != '\n') p++;
    return p;
}

static const char *next_line(const char *p, const char *end)
{
    p = line_end(p, end);
    return p < end ? p + 1 : end;
}

/* Is this line one a set lives on? Blank lines and comments are neither, and
   skipping them here is what lets the file carry a header. */
static int is_entry(const char *p, const char *end)
{
    const char *le = line_end(p, end);
    while (p < le && is_blank(*p)) p++;
    return p < le && *p != '#';
}

static const char *find_entry(const char *text, uint32_t len, int index)
{
    if (!text || !len || index < 0) return 0;
    const char *end = text + len;
    int seen = 0;
    for (const char *p = text; p < end; p = next_line(p, end)) {
        if (!is_entry(p, end)) continue;
        if (seen == index) return p;
        seen++;
    }
    return 0;
}

int en_freqset_count(const char *text, uint32_t len)
{
    if (!text || !len) return 0;
    const char *end = text + len;
    int n = 0;
    for (const char *p = text; p < end; p = next_line(p, end))
        if (is_entry(p, end)) n++;
    return n;
}

/* An unsigned decimal, optionally fractional. Advances `p` past it. Returns 0
   if there was no number to read, leaving `p` alone. No sign: a negative
   frequency is not a thing, and accepting one here would only let a typo
   through to the oscillator. */
static int read_num(const char **p, const char *end, double *out)
{
    const char *s = *p;
    while (s < end && is_blank(*s)) s++;

    int any = 0;
    double v = 0.0;
    while (s < end && is_digit(*s)) { v = v * 10.0 + (*s - '0'); s++; any = 1; }
    if (s < end && *s == '.') {
        s++;
        double scale = 0.1;
        while (s < end && is_digit(*s)) {
            v += (*s - '0') * scale;
            scale *= 0.1;
            s++;
            any = 1;
        }
    }
    if (!any) return 0;
    *p = s;
    *out = v;
    return 1;
}

/* Split an entry line into its three fields. Returns 0 if it does not have
   three, which is the only structural check there is: everything after this
   is numbers, and a bad number reads as no number. */
static int split_entry(const char *p, const char *le,
                       const char **name_end,
                       const char **dwell, const char **freqs)
{
    const char *bar1 = p;
    while (bar1 < le && *bar1 != '|') bar1++;
    if (bar1 >= le) return 0;

    const char *bar2 = bar1 + 1;
    while (bar2 < le && *bar2 != '|') bar2++;
    if (bar2 >= le) return 0;

    *name_end = bar1;
    *dwell    = bar1 + 1;
    *freqs    = bar2 + 1;
    return 1;
}

/* How many comma-separated numbers are on the rest of this line. */
static int count_steps(const char *p, const char *le)
{
    int n = 0;
    double v;
    while (p < le) {
        if (!read_num(&p, le, &v)) break;
        n++;
        while (p < le && (*p == ',' || is_blank(*p))) p++;
    }
    return n;
}

bool en_freqset_head(const char *text, uint32_t len, int index,
                     char *name_out, int name_cap,
                     int *steps_out, uint32_t *dwell_out)
{
    const char *entry = find_entry(text, len, index);
    if (!entry) return false;

    const char *end = text + len;
    const char *le = line_end(entry, end);
    const char *name_end, *dwell, *freqs;
    if (!split_entry(entry, le, &name_end, &dwell, &freqs)) return false;

    if (name_out && name_cap > 0) {
        const char *s = entry;
        while (s < name_end && is_blank(*s)) s++;
        /* Trailing blanks trimmed too, or a stray tab before the bar becomes
           part of the name and shows up as a gap in the list. */
        const char *e = name_end;
        while (e > s && is_blank(e[-1])) e--;

        int n = 0;
        while (s < e && n < name_cap - 1) name_out[n++] = *s++;
        name_out[n] = 0;
    }

    if (dwell_out) {
        double d = 0.0;
        const char *q = dwell;
        *dwell_out = read_num(&q, le, &d) && d > 0.0 ? (uint32_t)(d + 0.5) : 0u;
    }

    if (steps_out) *steps_out = count_steps(freqs, le);
    return true;
}

int en_freqset_segs(const char *text, uint32_t len, int index,
                    uint32_t dwell_s, double nyquist_hz,
                    en_prog_seg_t *segs, int cap, int *dropped_out)
{
    if (dropped_out) *dropped_out = 0;
    if (!segs || cap <= 0) return 0;

    const char *entry = find_entry(text, len, index);
    if (!entry) return 0;

    const char *end = text + len;
    const char *le = line_end(entry, end);
    const char *name_end, *dwell, *freqs;
    if (!split_entry(entry, le, &name_end, &dwell, &freqs)) return 0;

    uint32_t secs = dwell_s;
    if (!secs) {
        double d = 0.0;
        const char *q = dwell;
        if (read_num(&q, le, &d) && d > 0.0) secs = (uint32_t)(d + 0.5);
    }
    if (!secs) secs = 3u;   /* the bundle's own default, if the field is junk */

    /* Refuse a set that will not fit rather than returning the front of it:
       these are sweeps, and the back half is not optional. */
    int want = count_steps(freqs, le);
    if (want <= 0 || want > cap) return 0;

    int n = 0, dropped = 0;
    const char *p = freqs;
    double hz;
    while (p < le && n < cap) {
        if (!read_num(&p, le, &hz)) break;
        while (p < le && (*p == ',' || is_blank(*p))) p++;

        if (hz <= 0.0 || (nyquist_hz > 0.0 && hz >= nyquist_hz)) {
            dropped++;
            continue;
        }

        en_prog_seg_t *s = &segs[n++];
        /* Zeroed by hand rather than memset: core/ has no string.h, and the
           struct is small enough that naming every field is also the
           documentation for what a flat tone is. */
        s->beat_start  = 0.0;
        s->beat_end    = 0.0;
        s->carrier_hz  = hz;
        s->seconds     = secs;
        s->noise       = EN_NOISE_NONE;
        s->noise_level = 0.0;
        s->interp      = EN_INTERP_LINEAR;
        s->layers      = 0;      /* the four fields above are the one layer */
        for (int i = 0; i < EN_PROG_MAX_LAYERS; i++) {
            s->layer[i].carrier_hz  = 0.0;
            s->layer[i].beat_start  = 0.0;
            s->layer[i].beat_end    = 0.0;
            s->layer[i].level_start = 0.0;
            s->layer[i].level_end   = 0.0;
        }
    }

    if (dropped_out) *dropped_out = dropped;
    return n;
}
