/*
 * rds.c — see rds.h.
 */

#include "rds.h"

/* ---- programme type tables ----------------------------------------------- */

/* IEC 62106, as used in Europe and most of the world. */
static const char *const pty_rds[32] = {
    "None", "News", "Current Affairs", "Information",
    "Sport", "Education", "Drama", "Culture",
    "Science", "Varied", "Pop Music", "Rock Music",
    "Easy Listening", "Light Classical", "Serious Classical", "Other Music",
    "Weather", "Finance", "Children", "Social Affairs",
    "Religion", "Phone In", "Travel", "Leisure",
    "Jazz", "Country", "National Music", "Oldies",
    "Folk Music", "Documentary", "Alarm Test", "Alarm"
};

/* NRSC RBDS, as used in North America. Same numbers, different meanings - which
   is why the selection is not cosmetic. */
static const char *const pty_rbds[32] = {
    "None", "News", "Information", "Sports",
    "Talk", "Rock", "Classic Rock", "Adult Hits",
    "Soft Rock", "Top 40", "Country", "Oldies",
    "Soft", "Nostalgia", "Jazz", "Classical",
    "Rhythm and Blues", "Soft R&B", "Foreign Language", "Religious Music",
    "Religious Talk", "Personality", "Public", "College",
    "Spanish Talk", "Spanish Music", "Hip Hop", "Unassigned",
    "Unassigned", "Weather", "Emergency Test", "Emergency"
};

const char *en_rds_pty_name(uint8_t pty, bool rbds)
{
    if (pty > 31) return "Unknown";
    return rbds ? pty_rbds[pty] : pty_rds[pty];
}

/* ---- helpers ------------------------------------------------------------- */

/*
 * RDS carries its own character set. The printable ASCII range maps straight
 * through, which covers essentially every station name and radio text in
 * practice; the accented and symbol positions above it do not, and are shown as
 * a space rather than as whatever byte happened to arrive. A control code that
 * reached a display would corrupt the line it landed on.
 */
static char rds_char(uint8_t c)
{
    if (c >= 0x20 && c < 0x7F) return (char)c;
    return ' ';
}

static void clear_text(char *s, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) s[i] = ' ';
    s[n] = 0;
}

/* True once every segment bit in `mask` has been seen. */
static bool complete(uint16_t seen, uint16_t mask)
{
    return (seen & mask) == mask;
}

uint32_t en_rds_af_khz(uint8_t code)
{
    /* 1 to 204 are 87.5 MHz plus 100 kHz per step. Everything else is a count,
       a filler, or a marker for a band this tuner does not receive. */
    if (code >= 1 && code <= 204) return 87500u + (uint32_t)code * 100u;
    return 0;
}

static bool af_add(en_rds_t *r, uint8_t code)
{
    uint32_t khz = en_rds_af_khz(code);
    if (!khz) return false;
    for (uint8_t i = 0; i < r->af_count; i++)
        if (r->af[i] == khz) return false;          /* already listed */
    if (r->af_count >= EN_RDS_AF_MAX) return false;
    r->af[r->af_count++] = khz;
    return true;
}

void en_rds_init(en_rds_t *r, bool rbds)
{
    uint8_t *p = (uint8_t *)r;
    for (uint32_t i = 0; i < sizeof *r; i++) p[i] = 0;

    clear_text(r->ps, EN_RDS_PS_LEN);
    clear_text(r->rt, EN_RDS_RT_LEN);
    clear_text(r->ptyn, EN_RDS_PTYN_LEN);
    r->rbds = rbds;
}

/* ---- group handlers ------------------------------------------------------ */

/* Groups 0A and 0B: programme service name, traffic announcement, music/speech,
   decoder identification, and in 0A the alternate frequency list. */
static uint16_t group0(en_rds_t *r, const uint16_t blk[4], uint8_t valid,
                       bool version_b)
{
    uint16_t ch = 0;
    uint8_t seg = (uint8_t)(blk[1] & 0x03u);

    bool ta = (blk[1] & 0x0010u) != 0;
    if (ta != r->ta) { r->ta = ta; ch |= EN_RDS_CH_TA; }

    bool ms = (blk[1] & 0x0008u) != 0;
    if (ms != r->ms) { r->ms = ms; ch |= EN_RDS_CH_MS; }

    /* One decoder-identification bit per group, addressed by the segment. */
    uint8_t di_bit = (uint8_t)((blk[1] >> 2) & 1u);
    uint8_t di = (uint8_t)((r->di & ~(1u << seg)) | (di_bit << seg));
    if (di != r->di) { r->di = di; ch |= EN_RDS_CH_DI; }

    if (!version_b && (valid & EN_RDS_C)) {
        /* Block C is two alternate-frequency codes. The first list entry is a
           count rather than a frequency, which is how a receiver knows when it
           has the whole list. */
        uint8_t a = (uint8_t)(blk[2] >> 8), b = (uint8_t)(blk[2] & 0xFFu);
        if (a >= 224 && a <= 249) {
            r->af_expected = (uint8_t)(a - 224);
            if (af_add(r, b)) ch |= EN_RDS_CH_AF;
        } else {
            if (af_add(r, a)) ch |= EN_RDS_CH_AF;
            if (af_add(r, b)) ch |= EN_RDS_CH_AF;
        }
    }

    if (valid & EN_RDS_D) {
        char c0 = rds_char((uint8_t)(blk[3] >> 8));
        char c1 = rds_char((uint8_t)(blk[3] & 0xFFu));
        uint8_t at = (uint8_t)(seg * 2u);
        if (r->ps[at] != c0 || r->ps[at + 1] != c1) ch |= EN_RDS_CH_PS;
        r->ps[at] = c0;
        r->ps[at + 1] = c1;
        r->ps_seen |= (uint8_t)(1u << seg);
        if (complete(r->ps_seen, 0x0Fu)) {
            if (!r->ps_valid) ch |= EN_RDS_CH_PS;
            r->ps_valid = true;
        }
    }
    return ch;
}

/* Group 1A: extended country code and programme item number. */
static uint16_t group1(en_rds_t *r, const uint16_t blk[4], uint8_t valid,
                       bool version_b)
{
    uint16_t ch = 0;

    if (!version_b && (valid & EN_RDS_C)) {
        /* Block C is a variant code and its payload; variant 0 carries the
           extended country code, which is what turns a PI into a country. */
        uint8_t variant = (uint8_t)((blk[2] >> 12) & 0x07u);
        if (variant == 0) {
            uint8_t ecc = (uint8_t)(blk[2] & 0xFFu);
            if (!r->ecc_valid || r->ecc != ecc) ch |= EN_RDS_CH_ECC;
            r->ecc = ecc;
            r->ecc_valid = true;
        }
    }

    if (valid & EN_RDS_D) {
        if (!r->pin_valid || r->pin != blk[3]) ch |= EN_RDS_CH_PIN;
        r->pin = blk[3];
        r->pin_valid = true;
    }
    return ch;
}

/* Groups 2A and 2B: radio text. 2A carries four characters in blocks C and D,
   2B carries two in block D and repeats the PI in block C. */
static uint16_t group2(en_rds_t *r, const uint16_t blk[4], uint8_t valid,
                       bool version_b)
{
    uint16_t ch = 0;
    uint8_t seg = (uint8_t)(blk[1] & 0x0Fu);
    bool ab = (blk[1] & 0x0010u) != 0;

    /* The A/B flag toggling means a new message. Without clearing, the tail of
       a longer previous message shows through wherever the new one has not
       reached yet. */
    if (!r->rt_ab_known || ab != r->rt_ab) {
        clear_text(r->rt, EN_RDS_RT_LEN);
        r->rt_seen = 0;
        r->rt_valid = false;
        r->rt_ab = ab;
        r->rt_ab_known = true;
        ch |= EN_RDS_CH_RT;
    }

    uint8_t at, n;
    char c[4];

    if (version_b) {
        if (!(valid & EN_RDS_D)) return ch;
        at = (uint8_t)(seg * 2u);
        n = 2;
        c[0] = rds_char((uint8_t)(blk[3] >> 8));
        c[1] = rds_char((uint8_t)(blk[3] & 0xFFu));
    } else {
        if (!(valid & EN_RDS_C) || !(valid & EN_RDS_D)) return ch;
        at = (uint8_t)(seg * 4u);
        n = 4;
        c[0] = rds_char((uint8_t)(blk[2] >> 8));
        c[1] = rds_char((uint8_t)(blk[2] & 0xFFu));
        c[2] = rds_char((uint8_t)(blk[3] >> 8));
        c[3] = rds_char((uint8_t)(blk[3] & 0xFFu));
    }

    if ((uint16_t)(at + n) > EN_RDS_RT_LEN) return ch;

    for (uint8_t i = 0; i < n; i++) {
        if (r->rt[at + i] != c[i]) ch |= EN_RDS_CH_RT;
        r->rt[at + i] = c[i];
    }
    r->rt_seen |= (uint16_t)(1u << seg);

    /* A carriage return ends the message early, so everything after it is
       padding and the text is complete without the remaining segments. */
    for (uint8_t i = 0; i < n; i++) {
        uint8_t raw = version_b
            ? (uint8_t)(i == 0 ? (blk[3] >> 8) : (blk[3] & 0xFFu))
            : (uint8_t)(i < 2 ? (i == 0 ? (blk[2] >> 8) : (blk[2] & 0xFFu))
                              : (i == 2 ? (blk[3] >> 8) : (blk[3] & 0xFFu)));
        if (raw == 0x0D) {
            r->rt[at + i] = 0;
            r->rt_valid = true;
            return ch | EN_RDS_CH_RT;
        }
    }

    uint16_t want = version_b ? 0x00FFu : 0xFFFFu;
    if (complete(r->rt_seen, want)) {
        if (!r->rt_valid) ch |= EN_RDS_CH_RT;
        r->rt_valid = true;
    }
    return ch;
}

/* Group 4A: clock time and date. */
static uint16_t group4(en_rds_t *r, const uint16_t blk[4], uint8_t valid)
{
    if (!(valid & EN_RDS_C) || !(valid & EN_RDS_D)) return 0;

    /* Modified Julian day is split across blocks B and C, the hour across
       blocks C and D. */
    uint32_t mjd = ((uint32_t)(blk[1] & 0x0003u) << 15)
                 | ((uint32_t)blk[2] >> 1);
    uint8_t hour = (uint8_t)(((blk[2] & 0x0001u) << 4) | (blk[3] >> 12));
    uint8_t minute = (uint8_t)((blk[3] >> 6) & 0x3Fu);
    bool west = (blk[3] & 0x0020u) != 0;
    int8_t offset = (int8_t)(blk[3] & 0x1Fu);
    if (west) offset = (int8_t)(-offset);

    if (hour > 23 || minute > 59) return 0;    /* transmitted rubbish */

    /* Modified Julian day to calendar date, the standard conversion from the
       RDS specification itself. */
    uint32_t yp = (uint32_t)(((double)mjd - 15078.2) / 365.25);
    uint32_t mp = (uint32_t)(((double)mjd - 14956.1 - (double)(uint32_t)(yp * 365.25))
                             / 30.6001);
    uint32_t day = mjd - 14956u - (uint32_t)(yp * 365.25)
                 - (uint32_t)(mp * 30.6001);
    uint32_t k = (mp == 14u || mp == 15u) ? 1u : 0u;
    uint32_t year = yp + k + 1900u;
    uint32_t month = mp - 1u - k * 12u;

    if (month < 1u || month > 12u || day < 1u || day > 31u) return 0;

    /* The offset is transmitted in half hours and applies to the transmitted
       UTC, so local time is what a listener should actually see. */
    int32_t total = (int32_t)hour * 60 + minute + (int32_t)offset * 30;
    int32_t days = 0;
    while (total < 0)      { total += 1440; days--; }
    while (total >= 1440)  { total -= 1440; days++; }

    /* Crossing midnight moves the date. Kept simple deliberately: a day either
       side is all the offset can ever reach. */
    if (days) {
        int32_t d = (int32_t)day + days;
        static const uint8_t mdays[13] =
            { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        uint8_t len = mdays[month];
        if (month == 2u && ((year % 4u == 0u && year % 100u != 0u)
                            || year % 400u == 0u)) len = 29;
        if (d < 1) {
            month = (month == 1u) ? 12u : month - 1u;
            if (month == 12u) year--;
            len = mdays[month];
            if (month == 2u && ((year % 4u == 0u && year % 100u != 0u)
                                || year % 400u == 0u)) len = 29;
            d = len;
        } else if (d > (int32_t)len) {
            d = 1;
            month = (month == 12u) ? 1u : month + 1u;
            if (month == 1u) year++;
        }
        day = (uint32_t)d;
    }

    /* What was actually sent, kept before the offset is applied. */
    r->ct_mjd = mjd;
    r->ct_utc_hour = hour;
    r->ct_utc_minute = minute;
    r->ct_groups++;

    uint8_t nh = (uint8_t)(total / 60), nm = (uint8_t)(total % 60);

    bool changed = !r->ct_valid || r->ct_hour != nh || r->ct_minute != nm
                || r->ct_day != (uint8_t)day || r->ct_month != (uint8_t)month
                || r->ct_year != (uint16_t)year;

    r->ct_year = (uint16_t)year;
    r->ct_month = (uint8_t)month;
    r->ct_day = (uint8_t)day;
    r->ct_hour = nh;
    r->ct_minute = nm;
    r->ct_offset = offset;
    r->ct_valid = true;

    return changed ? EN_RDS_CH_CT : 0;
}

/* Group 10A: programme type name, four characters at a time. */
static uint16_t group10(en_rds_t *r, const uint16_t blk[4], uint8_t valid,
                        bool version_b)
{
    if (version_b) return 0;
    if (!(valid & EN_RDS_C) || !(valid & EN_RDS_D)) return 0;

    uint16_t ch = 0;
    uint8_t seg = (uint8_t)(blk[1] & 0x01u);
    uint8_t at = (uint8_t)(seg * 4u);
    char c[4] = {
        rds_char((uint8_t)(blk[2] >> 8)), rds_char((uint8_t)(blk[2] & 0xFFu)),
        rds_char((uint8_t)(blk[3] >> 8)), rds_char((uint8_t)(blk[3] & 0xFFu))
    };
    for (uint8_t i = 0; i < 4; i++) {
        if (r->ptyn[at + i] != c[i]) ch |= EN_RDS_CH_PTYN;
        r->ptyn[at + i] = c[i];
    }
    r->ptyn_seen |= (uint8_t)(1u << seg);
    if (complete(r->ptyn_seen, 0x03u)) {
        if (!r->ptyn_valid) ch |= EN_RDS_CH_PTYN;
        r->ptyn_valid = true;
    }
    return ch;
}

/* ---- RadioText+ ----------------------------------------------------------
 *
 * Group 3A announces which group carries an open-data application:
 *
 *   block B, bits 4..0   the group it will use - type in 4..1, version in 0
 *   block D              the application identifier
 *
 * 0x4BD7 is RadioText+. Nothing else is acted on; other applications are
 * announced by plenty of stations and none of them mean anything here.
 */
#define EN_RTP_AID 0x4BD7u

/*
 * Every application a station announces, not only the one acted on.
 *
 * Group 3A says "the application with this identifier is in that group slot".
 * Only RadioText+ is decoded, but knowing that a station is also running
 * Alert-C in 8A is exactly the sort of thing the inspector exists to show, and
 * it costs four bytes each to remember.
 *
 * Re-announcements are constant - 3A repeats for as long as the application
 * does - so an identifier already held is updated in place rather than added
 * again, which is what stops the list filling up with one application.
 */
static void oda_note(en_rds_t *r, uint16_t aid, uint8_t group)
{
    uint8_t i;

    if (!aid) return;      /* 0 means "no application", not an application */

    for (i = 0; i < r->oda_count; i++) {
        if (r->oda[i].aid == aid) {
            r->oda[i].group = group;
            return;
        }
    }
    if (r->oda_count >= EN_RDS_ODA_MAX) return;
    r->oda[r->oda_count].aid = aid;
    r->oda[r->oda_count].group = group;
    r->oda_count++;
}

static uint16_t group3(en_rds_t *r, const uint16_t blk[4], uint8_t valid,
                       bool version_b)
{
    if (version_b) return 0;
    if (!(valid & EN_RDS_B) || !(valid & EN_RDS_D)) return 0;

    uint8_t g = (uint8_t)(blk[1] & 0x1Fu);

    /* Recorded whatever it is. Only RadioText+ is decoded, but which
       applications a station is running is worth knowing on its own, and this
       group is the only place it is ever said. */
    oda_note(r, blk[3], g);

    if (blk[3] != EN_RTP_AID) return 0;

    /* Group 0 would mean "carried in group 0A", which is the basic tuning
       group and cannot also carry markers. A station announcing that is
       announcing nothing. */
    if (g == 0) return 0;

    if (!r->rtplus_known || r->rtplus_group != g) {
        r->rtplus_group = g;
        r->rtplus_known = true;
        return EN_RDS_CH_RTPLUS;
    }
    return 0;
}

/*
 * One RT+ tag: a content type, and where in the radio text it starts and how
 * long it is.
 *
 * The markers point into the RT buffer, so a tag is only usable once the text
 * it describes has actually arrived. A marker running past what we have is
 * dropped rather than clamped - half a title is worse than no title, because
 * nothing on screen would say it was half.
 */
static bool rtp_take(en_rds_t *r, uint8_t content, uint8_t start, uint8_t len,
                     char *out, bool *valid_out)
{
    if (!len) return false;
    (void)content;

    uint16_t end = (uint16_t)start + len;
    if (end > EN_RDS_RT_LEN) return false;

    /* Every character it covers has to have been received. rt_seen is a bit
       per four-character segment, which is the granularity RT arrives in. */
    for (uint16_t i = start; i < end; i++)
        if (!(r->rt_seen & (uint16_t)(1u << (i / 4u)))) return false;

    char buf[EN_RDS_RT_LEN + 1];
    uint8_t n = 0;
    for (uint16_t i = start; i < end; i++) buf[n++] = r->rt[i];
    buf[n] = 0;

    /* Trailing spaces are how stations pad a marker to a tidy length. */
    while (n && (buf[n - 1] == ' ' || buf[n - 1] == '\r')) buf[--n] = 0;
    if (!n) return false;

    bool changed = !*valid_out;
    for (uint8_t i = 0; i <= n; i++) {
        if (out[i] != buf[i]) changed = true;
        out[i] = buf[i];
    }
    *valid_out = true;
    return changed;
}

/*
 * The markers themselves, in whichever group 3A named.
 *
 *   block B  b4     item toggle
 *            b3     item running
 *            b2..b0 content type 1, high three bits
 *   block C  b15..b13 content type 1, low three bits
 *            b12..b7  start marker 1
 *            b6..b1   length marker 1
 *            b0       content type 2, high bit
 *   block D  b15..b11 content type 2, low five bits
 *            b10..b5  start marker 2
 *            b4..b0   length marker 2
 *
 * The two length fields are different widths - six bits and five - which is
 * not a typo in this comment and is the kind of thing that produces a title
 * that is right and an artist that is one character short.
 */
static uint16_t group_rtplus(en_rds_t *r, const uint16_t blk[4], uint8_t valid)
{
    if (!(valid & EN_RDS_B) || !(valid & EN_RDS_C) || !(valid & EN_RDS_D))
        return 0;

    uint16_t ch = 0;

    bool toggle  = (blk[1] & 0x0010u) != 0;
    bool running = (blk[1] & 0x0008u) != 0;

    /* The toggle flipping means a different item is playing, so what we had
       describes the previous one and has to go. Without this the artist of
       the last track sits under the title of the next. */
    if (r->rtplus_toggle_known && toggle != r->rtplus_toggle) {
        r->rt_title[0] = 0;
        r->rt_artist[0] = 0;
        r->rt_title_valid = false;
        r->rt_artist_valid = false;
        ch |= EN_RDS_CH_RTPLUS;
    }
    r->rtplus_toggle = toggle;
    r->rtplus_toggle_known = true;

    if (running != r->rtplus_running) {
        r->rtplus_running = running;
        ch |= EN_RDS_CH_RTPLUS;
    }

    uint8_t c1 = (uint8_t)(((blk[1] & 0x0007u) << 3) | (blk[2] >> 13));
    uint8_t s1 = (uint8_t)((blk[2] >> 7) & 0x3Fu);
    uint8_t l1 = (uint8_t)(((blk[2] >> 1) & 0x3Fu) + 1u);

    uint8_t c2 = (uint8_t)(((blk[2] & 0x0001u) << 5) | (blk[3] >> 11));
    uint8_t s2 = (uint8_t)((blk[3] >> 5) & 0x3Fu);
    uint8_t l2 = (uint8_t)((blk[3] & 0x1Fu) + 1u);

    struct { uint8_t c, s, l; } tag[2] = { { c1, s1, l1 }, { c2, s2, l2 } };

    for (int i = 0; i < 2; i++) {
        switch (tag[i].c) {
        case EN_RTP_TITLE:
        case EN_RTP_PROG_NOW:
            if (rtp_take(r, tag[i].c, tag[i].s, tag[i].l, r->rt_title,
                         &r->rt_title_valid))
                ch |= EN_RDS_CH_RTPLUS;
            break;
        case EN_RTP_ARTIST:
        case EN_RTP_BAND:
            if (rtp_take(r, tag[i].c, tag[i].s, tag[i].l, r->rt_artist,
                         &r->rt_artist_valid))
                ch |= EN_RDS_CH_RTPLUS;
            break;
        default:
            break;
        }
    }
    return ch;
}

/* ---- the entry point ----------------------------------------------------- */

/*
 * Modified Julian day 40587 is 1970-01-01, so the whole conversion is one
 * subtraction. No calendar arithmetic, and therefore nothing here that can be
 * wrong about a leap year.
 */
int64_t en_rds_ct_unix(const en_rds_t *r)
{
    if (!r || !r->ct_valid || r->ct_mjd < 40587u) return 0;

    return ((int64_t)r->ct_mjd - 40587) * 86400
         + (int64_t)r->ct_utc_hour * 3600
         + (int64_t)r->ct_utc_minute * 60;
}

uint16_t en_rds_group(en_rds_t *r, const uint16_t blk[4], uint8_t valid)
{
    if (!r || !blk) return 0;

    for (uint8_t i = 0; i < 4; i++)
        if (!(valid & (1u << i))) r->blocks_bad++;

    /* Without block B there is no group type, so nothing can be interpreted -
       not even the parts carried in blocks that did arrive. */
    if (!(valid & EN_RDS_B)) return 0;

    r->groups++;

    uint16_t ch = 0;
    uint8_t type = (uint8_t)(blk[1] >> 12);
    bool version_b = (blk[1] & 0x0800u) != 0;
    r->group_count[(type << 1) | (version_b ? 1u : 0u)]++;

    /* Block A is the PI, and in version B groups block C repeats it. A PI
       change means a different station, at which point everything accumulated
       belongs to the previous one. */
    uint16_t pi = 0;
    bool have_pi = false;
    if (valid & EN_RDS_A) { pi = blk[0]; have_pi = true; }
    else if (version_b && (valid & EN_RDS_C)) { pi = blk[2]; have_pi = true; }

    if (have_pi) {
        if (r->pi_valid && r->pi != pi) {
            bool rbds = r->rbds;
            uint32_t groups = r->groups, bad = r->blocks_bad;
            en_rds_init(r, rbds);
            r->groups = groups;
            r->blocks_bad = bad;
        }
        if (!r->pi_valid || r->pi != pi) ch |= EN_RDS_CH_PI;
        r->pi = pi;
        r->pi_valid = true;
    }

    bool tp = (blk[1] & 0x0400u) != 0;
    if (tp != r->tp) { r->tp = tp; ch |= EN_RDS_CH_TP; }

    uint8_t pty = (uint8_t)((blk[1] >> 5) & 0x1Fu);
    if (pty != r->pty) { r->pty = pty; ch |= EN_RDS_CH_PTY; }

    switch (type) {
    case 0:  ch |= group0(r, blk, valid, version_b);  break;
    case 1:  ch |= group1(r, blk, valid, version_b);  break;
    case 2:  ch |= group2(r, blk, valid, version_b);  break;
    case 3:  ch |= group3(r, blk, valid, version_b); break;
    case 4:  if (!version_b) ch |= group4(r, blk, valid); break;
    case 10: ch |= group10(r, blk, valid, version_b); break;
    default: break;   /* counted above; TMC and EON are not decoded yet */
    }

    /* And whichever group 3A named for RadioText+, which is a different group
       on different stations and so cannot be a case label. */
    if (r->rtplus_known &&
        r->rtplus_group == (uint8_t)((type << 1) | (version_b ? 1u : 0u)))
        ch |= group_rtplus(r, blk, valid);
    return ch;
}

/* ---- the unverified part ------------------------------------------------- */

uint8_t en_rds_unpack_tuples_unverified(const uint8_t *fifo, uint16_t len,
                                        uint16_t (*out)[4], uint8_t *valid,
                                        uint8_t max_groups)
{
    if (!fifo || !out || !valid || !max_groups) return 0;

    uint8_t n = 0;
    uint16_t blk[4] = { 0, 0, 0, 0 };
    uint8_t got = 0;

    for (uint16_t i = 0; i + 2 < len; i += 3) {
        /* Two data bytes then a status byte whose top nibble is understood to
           be the block type and whose low bits report errors. */
        uint16_t data = (uint16_t)(((uint16_t)fifo[i] << 8) | fifo[i + 1]);
        uint8_t status = fifo[i + 2];
        uint8_t which = (uint8_t)((status >> 4) & 0x07u);
        bool ok = (status & 0x03u) == 0;

        if (which > 3) continue;          /* C-prime and E are not handled */

        /* A block arriving out of order means the group boundary was missed,
           so what has accumulated is not a group and starting over is the only
           safe thing to do. */
        if (which == 0) { got = 0; blk[0] = blk[1] = blk[2] = blk[3] = 0; }

        blk[which] = data;
        if (ok) got |= (uint8_t)(1u << which);

        if (which == 3) {
            if (n < max_groups) {
                for (uint8_t k = 0; k < 4; k++) out[n][k] = blk[k];
                valid[n] = got;
                n++;
            }
            got = 0;
        }
    }
    return n;
}
