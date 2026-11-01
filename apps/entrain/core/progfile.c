/*
 * progfile.c — see progfile.h.
 *
 * Freestanding: no stdio, no strtod, no locale. A scanner over a buffer, in
 * the same style as the user-program parser in program.c.
 */

#include "progfile.h"

static int is_digit(char c) { return c >= '0' && c <= '9'; }

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

/* A decimal number, optionally signed and optionally fractional. Advances `p`
   past it. Returns 0 if there was no number to read, leaving `p` alone. */
static int read_num(const char **p, const char *end, double *out)
{
    const char *s = *p;
    while (s < end && (*s == ' ' || *s == '\t')) s++;

    int neg = 0;
    if (s < end && (*s == '-' || *s == '+')) { neg = (*s == '-'); s++; }

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
    *out = neg ? -v : v;
    return 1;
}

/* Start of the `index`th program's '@' line, or NULL. */
static const char *find_entry(const char *text, uint32_t len, int index)
{
    if (!text || !len || index < 0) return 0;
    const char *end = text + len;
    int seen = 0;
    for (const char *p = text; p < end; p = next_line(p, end)) {
        if (*p != '@') continue;
        if (seen == index) return p;
        seen++;
    }
    return 0;
}

int en_progfile_count(const char *text, uint32_t len)
{
    if (!text || !len) return 0;
    const char *end = text + len;
    int n = 0;
    for (const char *p = text; p < end; p = next_line(p, end))
        if (*p == '@') n++;
    return n;
}

/* Copy up to the next '|' or end of line into `dst`. */
static void read_name(const char *p, const char *le, char *dst, int cap)
{
    int n = 0;
    while (p < le && *p != '|' && *p != '\r') {
        if (n < cap - 1) dst[n] = *p;
        n++;
        p++;
    }
    dst[n < cap ? n : cap - 1] = 0;
}

bool en_progfile_head(const char *text, uint32_t len, int index,
                      char *name_out, int name_cap, uint32_t *seconds_out)
{
    const char *entry = find_entry(text, len, index);
    if (!entry) return false;

    const char *end = text + len;
    const char *le = line_end(entry, end);

    if (name_out && name_cap > 0) read_name(entry + 1, le, name_out, name_cap);

    if (seconds_out) {
        *seconds_out = 0;
        const char *bar = entry;
        while (bar < le && *bar != '|') bar++;
        if (bar < le) {
            const char *q = bar + 1;
            double v;
            if (read_num(&q, le, &v) && v >= 0.0) *seconds_out = (uint32_t)v;
        }
    }
    return true;
}

bool en_progfile_load(const char *text, uint32_t len, int index,
                      en_progfile_t *out)
{
    if (!out) return false;
    const char *entry = find_entry(text, len, index);
    if (!entry) return false;

    const char *end = text + len;

    out->n_segs = 0;
    out->layers = 0;
    out->seconds = 0;
    out->name[0] = 0;

    en_progfile_head(text, len, index, out->name, sizeof out->name,
                     &out->seconds);

    double carrier[EN_PROG_MAX_LAYERS];
    int n_car = 0;

    for (const char *p = next_line(entry, end); p < end;
         p = next_line(p, end)) {

        if (*p == '@') break;                    /* the next program */
        if (*p == '#' || *p == '\n' || *p == '\r') continue;

        const char *le = line_end(p, end);

        if (*p == 'C') {
            /* Carriers, one per layer, in the order every S line uses. */
            const char *q = p + 1;
            n_car = 0;
            double v;
            while (q < le && read_num(&q, le, &v)) {
                if (n_car >= EN_PROG_MAX_LAYERS) return false;
                carrier[n_car++] = v;
                if (q < le && *q == ',') q++;
            }
            if (n_car == 0) return false;
            out->layers = (uint8_t)n_car;
            continue;
        }

        if (*p != 'S') continue;
        if (n_car == 0) return false;            /* an S before its C line */
        if (out->n_segs >= EN_PROGFILE_MAX_SEGS) return false;

        const char *q = p + 1;
        double secs;
        if (!read_num(&q, le, &secs) || secs <= 0.0) return false;

        en_prog_seg_t *seg = &out->segs[out->n_segs];
        /* Zeroed rather than assumed: en_prog_seg_t has fields this format
           does not carry, and they have to be their documented defaults
           rather than whatever the previous program left here. */
        for (unsigned b = 0; b < sizeof *seg; b++) ((uint8_t *)seg)[b] = 0;

        seg->seconds = (uint32_t)secs;
        seg->noise = EN_NOISE_NONE;
        seg->noise_level = 0.0;
        seg->interp = EN_INTERP_SMOOTH;
        seg->layers = (uint8_t)n_car;
        seg->carrier_hz = carrier[0];

        for (int j = 0; j < n_car; j++) {
            while (q < le && *q != '|') q++;
            if (q >= le) return false;
            q++;                                  /* past the '|' */

            double b0, b1, g0, g1;
            if (!read_num(&q, le, &b0)) return false;
            if (q < le && *q == ',') q++;
            if (!read_num(&q, le, &b1)) return false;
            if (q < le && *q == ',') q++;
            if (!read_num(&q, le, &g0)) return false;
            if (q < le && *q == ',') q++;
            if (!read_num(&q, le, &g1)) return false;

            seg->layer[j].carrier_hz  = carrier[j];
            seg->layer[j].beat_start  = b0;
            seg->layer[j].beat_end    = b1;
            seg->layer[j].level_start = g0;
            seg->layer[j].level_end   = g1;
        }

        /* Layer zero is the primary by convention, and the scalar fields
           mirror it - the Now Playing readout reads those. */
        seg->beat_start = seg->layer[0].beat_start;
        seg->beat_end   = seg->layer[0].beat_end;

        out->n_segs++;
    }

    return out->n_segs > 0;
}
