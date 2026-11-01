/*
 * store.c — see store.h.
 */

#include "store.h"

/* ---- the writer ---------------------------------------------------------- */

static void put(en_json_t *j, const char *s)
{
    if (j->overflow) return;
    while (*s) {
        if (j->len + 1 >= j->cap) { j->overflow = true; return; }
        j->buf[j->len++] = *s++;
    }
    j->buf[j->len] = 0;
}

static void putc_(en_json_t *j, char c)
{
    if (j->overflow) return;
    if (j->len + 1 >= j->cap) { j->overflow = true; return; }
    j->buf[j->len++] = c;
    j->buf[j->len] = 0;
}

static void put_uint(en_json_t *j, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (!v) { putc_(j, '0'); return; }
    while (v && n < 11) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) putc_(j, tmp[--n]);
}

/* Escape what JSON requires and drop what it cannot carry. RDS text is not
   guaranteed to be clean - a mis-decoded group can put a control byte in a
   station name, and writing it raw would produce a file no parser accepts. */
static void put_escaped(en_json_t *j, const char *s)
{
    putc_(j, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  put(j, "\\\""); break;
        case '\\': put(j, "\\\\"); break;
        case '\n': put(j, "\\n");  break;
        case '\r': put(j, "\\r");  break;
        case '\t': put(j, "\\t");  break;
        default:
            if (c < 0x20 || c >= 0x7F) putc_(j, ' ');
            else putc_(j, (char)c);
        }
    }
    putc_(j, '"');
}

static void comma(en_json_t *j)
{
    if (j->need_comma) putc_(j, ',');
    j->need_comma = true;
}

static void key(en_json_t *j, const char *k)
{
    comma(j);
    if (k) { put_escaped(j, k); putc_(j, ':'); }
}

void en_json_init(en_json_t *j, char *buf, uint32_t cap)
{
    j->buf = buf; j->cap = cap; j->len = 0;
    j->depth = 0; j->need_comma = false; j->overflow = false;
    if (cap) buf[0] = 0;
}

void en_json_obj_open(en_json_t *j, const char *k)
{
    key(j, k); putc_(j, '{'); j->need_comma = false; j->depth++;
}

void en_json_obj_close(en_json_t *j)
{
    putc_(j, '}'); j->need_comma = true; if (j->depth) j->depth--;
}

void en_json_arr_open(en_json_t *j, const char *k)
{
    key(j, k); putc_(j, '['); j->need_comma = false; j->depth++;
}

void en_json_arr_close(en_json_t *j)
{
    putc_(j, ']'); j->need_comma = true; if (j->depth) j->depth--;
}

void en_json_str(en_json_t *j, const char *k, const char *v)
{
    key(j, k); put_escaped(j, v ? v : "");
}

void en_json_uint(en_json_t *j, const char *k, uint32_t v)
{
    key(j, k); put_uint(j, v);
}

void en_json_int(en_json_t *j, const char *k, int32_t v)
{
    key(j, k);
    if (v < 0) { putc_(j, '-'); put_uint(j, (uint32_t)(-(int64_t)v)); }
    else put_uint(j, (uint32_t)v);
}

void en_json_bool(en_json_t *j, const char *k, bool v)
{
    key(j, k); put(j, v ? "true" : "false");
}

void en_json_hex16(en_json_t *j, const char *k, uint16_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    key(j, k);
    putc_(j, '"'); putc_(j, '0'); putc_(j, 'x');
    for (int s = 12; s >= 0; s -= 4) putc_(j, hex[(v >> s) & 0xF]);
    putc_(j, '"');
}

uint32_t en_json_done(en_json_t *j)
{
    return j->overflow ? 0 : j->len;
}

/* ---- a very small reader ------------------------------------------------- */

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Find "key": within [p,end). Returns the position just past the colon. */
static const char *find_key(const char *p, const char *end, const char *k)
{
    uint32_t klen = 0;
    while (k[klen]) klen++;

    for (; p + klen + 2 < end; p++) {
        if (*p != '"') continue;
        uint32_t i = 0;
        while (i < klen && p[1 + i] == k[i]) i++;
        if (i != klen || p[1 + klen] != '"') continue;
        const char *q = skip_ws(p + klen + 2, end);
        if (q < end && *q == ':') return skip_ws(q + 1, end);
    }
    return 0;
}

static uint32_t read_uint(const char *p, const char *end, uint32_t *out)
{
    uint32_t v = 0, n = 0;
    bool hex = false;
    if (p + 1 < end && p[0] == '"' ) { p++; n++; }
    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        hex = true; p += 2; n += 2;
    }
    while (p < end) {
        char c = *p;
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (hex && c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else if (hex && c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else break;
        v = v * (hex ? 16u : 10u) + d;
        p++; n++;
    }
    *out = v;
    return n;
}

static bool read_uint_hex_digit(char c, uint32_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint32_t)(c - '0'); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint32_t)(c - 'A' + 10); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint32_t)(c - 'a' + 10); return true; }
    return false;
}

static void read_str(const char *p, const char *end, char *out, uint32_t cap)
{
    uint32_t n = 0;
    out[0] = 0;
    if (p >= end || *p != '"') return;
    p++;
    while (p < end && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p + 1 < end) p++;
        out[n++] = *p++;
    }
    out[n] = 0;
}

/* ---- presets ------------------------------------------------------------- */

static void copy_str(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!cap) return;
    if (src) while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

void en_presets_init(en_presets_t *p, const char *region)
{
    if (!p) return;
    uint8_t *b = (uint8_t *)p;
    for (uint32_t i = 0; i < sizeof *p; i++) b[i] = 0;
    copy_str(p->region, sizeof p->region, region);
}

int en_preset_find(const en_presets_t *p, uint32_t khz)
{
    if (!p) return -1;
    for (uint8_t i = 0; i < p->count; i++)
        if (p->list[i].khz == khz) return (int)i;
    return -1;
}

bool en_preset_add(en_presets_t *p, const en_preset_t *entry)
{
    if (!p || !entry || !entry->khz) return false;

    /* Re-adding a frequency updates it rather than duplicating: a station whose
       name has since been decoded should replace the placeholder, not sit
       beside it. */
    int at = en_preset_find(p, entry->khz);
    if (at >= 0) { p->list[at] = *entry; return true; }

    if (p->count >= EN_PRESET_MAX) return false;
    p->list[p->count++] = *entry;
    return true;
}

bool en_preset_remove(en_presets_t *p, uint32_t khz)
{
    int at = en_preset_find(p, khz);
    if (at < 0) return false;
    for (uint8_t i = (uint8_t)at; i + 1 < p->count; i++)
        p->list[i] = p->list[i + 1];
    p->count--;
    return true;
}

uint8_t en_presets_simple(const en_presets_t *p, const en_preset_t **out,
                          uint8_t cap)
{
    if (!p) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < p->count && n < cap && n < EN_SIMPLE_MAX; i++) {
        if (!p->list[i].simple) continue;
        if (out) out[n] = &p->list[i];
        n++;
    }
    return n;
}

bool en_preset_set_simple(en_presets_t *p, uint32_t khz, bool on)
{
    int at = en_preset_find(p, khz);
    if (at < 0) return false;

    if (!on) { p->list[at].simple = false; return true; }
    if (p->list[at].simple) return true;      /* already there, not an error */

    /* Counted rather than assumed: a preset file can be hand-edited, and one
       that flags ten would otherwise quietly show the first six with no clue
       why the rest never appear. */
    uint8_t n = en_presets_simple(p, 0, EN_SIMPLE_MAX);
    if (n >= EN_SIMPLE_MAX) return false;

    p->list[at].simple = true;
    return true;
}

void en_preset_sort(en_presets_t *p)
{
    if (!p) return;
    for (uint8_t i = 1; i < p->count; i++) {
        en_preset_t k = p->list[i];
        int8_t j = (int8_t)(i - 1);
        while (j >= 0 && p->list[j].khz > k.khz) {
            p->list[j + 1] = p->list[j];
            j--;
        }
        p->list[j + 1] = k;
    }
}

uint32_t en_presets_save(const en_presets_t *p, char *buf, uint32_t cap)
{
    if (!p || !buf) return 0;

    en_json_t j;
    en_json_init(&j, buf, cap);
    en_json_obj_open(&j, 0);
    en_json_uint(&j, "version", 1);
    en_json_str(&j, "region", p->region);
    en_json_arr_open(&j, "presets");
    for (uint8_t i = 0; i < p->count; i++) {
        const en_preset_t *e = &p->list[i];
        en_json_obj_open(&j, 0);
        en_json_uint(&j, "khz", e->khz);
        en_json_str(&j, "name", e->name);
        en_json_hex16(&j, "pi", e->pi);
        en_json_uint(&j, "pty", e->pty);
        en_json_bool(&j, "rbds", e->rbds);
        en_json_bool(&j, "simple", e->simple);
        en_json_obj_close(&j);
    }
    en_json_arr_close(&j);
    en_json_obj_close(&j);
    return en_json_done(&j);
}

bool en_presets_load(en_presets_t *p, const char *json, uint32_t len)
{
    if (!p || !json || !len) return false;

    const char *end = json + len;
    en_presets_init(p, "");

    const char *v = find_key(json, end, "region");
    if (v) read_str(v, end, p->region, sizeof p->region);

    const char *arr = find_key(json, end, "presets");
    if (!arr || *arr != '[') return false;

    const char *q = arr + 1;
    while (q < end && p->count < EN_PRESET_MAX) {
        q = skip_ws(q, end);
        if (q >= end || *q == ']') break;
        if (*q != '{') { q++; continue; }

        /* Each object is scanned within its own bounds, so a missing field in
           one entry cannot pick up the next entry's value. */
        const char *obj = q;
        int depth = 0;
        while (q < end) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (!depth) { q++; break; } }
            q++;
        }
        const char *oend = q;

        en_preset_t e;
        uint8_t *b = (uint8_t *)&e;
        for (uint32_t i = 0; i < sizeof e; i++) b[i] = 0;

        uint32_t n = 0;
        const char *f = find_key(obj, oend, "khz");
        if (f && read_uint(f, oend, &n)) e.khz = n;

        f = find_key(obj, oend, "name");
        if (f) read_str(f, oend, e.name, sizeof e.name);

        f = find_key(obj, oend, "pi");
        if (f && read_uint(f, oend, &n)) e.pi = (uint16_t)n;

        f = find_key(obj, oend, "pty");
        if (f && read_uint(f, oend, &n)) e.pty = (uint8_t)(n & 0x1Fu);

        f = find_key(obj, oend, "rbds");
        e.rbds = f && *f == 't';

        /* Absent means false, which is what a file written before the simple
           screen existed should mean. The struct was zeroed above, so this is
           only ever setting it true. */
        f = find_key(obj, oend, "simple");
        e.simple = f && *f == 't';

        if (e.khz) p->list[p->count++] = e;
    }
    return true;
}

/* ---- register overrides -------------------------------------------------- */

const en_override_t *en_override_find(const en_overrides_t *o, uint8_t addr)
{
    if (!o) return 0;
    for (uint8_t i = 0; i < o->count; i++)
        if (o->list[i].addr == addr) return &o->list[i];
    return 0;
}

bool en_override_set(en_overrides_t *o, uint8_t addr, const uint8_t *data,
                     uint8_t len)
{
    if (!o || !data || !len || len > 8) return false;

    en_override_t *slot = 0;
    for (uint8_t i = 0; i < o->count; i++)
        if (o->list[i].addr == addr) { slot = &o->list[i]; break; }

    if (!slot) {
        if (o->count >= EN_OVERRIDE_MAX) return false;
        slot = &o->list[o->count++];
    }

    slot->addr = addr;
    slot->len = len;
    for (uint8_t i = 0; i < len; i++) slot->data[i] = data[i];
    return true;
}

bool en_override_clear(en_overrides_t *o, uint8_t addr)
{
    if (!o) return false;
    for (uint8_t i = 0; i < o->count; i++) {
        if (o->list[i].addr != addr) continue;
        for (uint8_t k = i; k + 1 < o->count; k++) o->list[k] = o->list[k + 1];
        o->count--;
        return true;
    }
    return false;
}

/* ---- settings ------------------------------------------------------------ */

void en_settings_default(en_settings_t *s)
{
    if (!s) return;
    copy_str(s->region, sizeof s->region, "Americas");
    s->khz = 0;                 /* 0 means "the bottom of whatever band" */
    s->rds_on = true;
    s->live_seconds = 30;
    en_rectimer_init(&s->rectimer);
    s->ta_record = false;

    /* Empty means the platform's first output, which is the headphones -
       the one destination that is always there, since the jack is also the
       antenna. */
    s->output[0] = 0;

    /* Full on the software volume, and the codec below where it clips. This
       pair was measured: at 100 and 75 the output reads about -20 dBFS RMS
       and -9 dBFS peak on FM, against RetailOS at roughly -12. At 88 the peak
       pins at 0.0 with a flat crest factor, which is audible as fuzz - so the
       top of the codec's range is not a default. */
    s->volume = 100;
    s->hp_level = 75;
}

uint32_t en_settings_save(const en_settings_t *s, char *buf, uint32_t cap)
{
    if (!s || !buf) return 0;

    en_json_t j;
    en_json_init(&j, buf, cap);
    en_json_obj_open(&j, 0);
    en_json_uint(&j, "version", 1);
    en_json_str(&j, "region", s->region);
    en_json_uint(&j, "khz", s->khz);
    en_json_bool(&j, "rds", s->rds_on);
    en_json_uint(&j, "live_seconds", s->live_seconds);
    en_json_bool(&j, "ta_record", s->ta_record);
    en_json_uint(&j, "stereo_mode", s->stereo_mode);
    en_json_bool(&j, "af_follow", s->af_follow);
    en_json_bool(&j, "simple_screen", s->simple_screen);
    en_json_bool(&j, "wide_screen", s->wide_screen);
    en_json_str(&j, "output", s->output);
    en_json_uint(&j, "volume", s->volume);
    en_json_uint(&j, "hp_level", s->hp_level);
    en_json_uint(&j, "rec_limit_min", s->rectimer.limit_min);
    /* Written only when there is one. A start time of "none" is the absence
       of the key rather than a sentinel, so nothing has to agree on what the
       sentinel is between the writer, the reader and whoever reads the file
       with their eyes. */
    if (s->rectimer.at_min >= 0)
        en_json_uint(&j, "rec_at_min", (uint32_t)s->rectimer.at_min);

    /* Written as hex bytes rather than a number, because a register payload is
       a byte string of its own length and turning it into an integer would
       lose how long it was. */
    en_json_arr_open(&j, "registers");
    for (uint8_t i = 0; i < s->overrides.count; i++) {
        const en_override_t *o = &s->overrides.list[i];
        en_json_obj_open(&j, 0);
        en_json_uint(&j, "reg", o->addr);
        char hex[20];
        static const char d[] = "0123456789ABCDEF";
        uint8_t k = 0;
        for (uint8_t b = 0; b < o->len && k < 16; b++) {
            hex[k++] = d[(o->data[b] >> 4) & 0xF];
            hex[k++] = d[o->data[b] & 0xF];
        }
        hex[k] = 0;
        en_json_str(&j, "bytes", hex);
        en_json_obj_close(&j);
    }
    en_json_arr_close(&j);

    en_json_obj_close(&j);
    return en_json_done(&j);
}

bool en_settings_load(en_settings_t *s, const char *json, uint32_t len)
{
    if (!s || !json || !len) return false;

    /* Defaults first, then whatever the file happens to carry. A file from an
       older build is missing fields rather than wrong, and missing should mean
       default rather than zero - a zero live buffer would be a bug that looked
       like a setting. */
    en_settings_default(s);

    const char *end = json + len;
    uint32_t n = 0;

    const char *v = find_key(json, end, "region");
    if (v) read_str(v, end, s->region, sizeof s->region);

    v = find_key(json, end, "khz");
    if (v && read_uint(v, end, &n)) s->khz = n;

    v = find_key(json, end, "rds");
    if (v) s->rds_on = (*v == 't');

    v = find_key(json, end, "live_seconds");
    if (v && read_uint(v, end, &n) && n) s->live_seconds = (uint8_t)n;

    v = find_key(json, end, "ta_record");
    if (v) s->ta_record = (*v == 't');

    v = find_key(json, end, "stereo_mode");
    if (v && read_uint(v, end, &n) && n <= 2u) s->stereo_mode = (uint8_t)n;

    v = find_key(json, end, "af_follow");
    if (v) s->af_follow = (*v == 't');

    v = find_key(json, end, "simple_screen");
    if (v) s->simple_screen = (*v == 't');

    v = find_key(json, end, "wide_screen");
    if (v) s->wide_screen = (*v == 't');

    v = find_key(json, end, "output");
    if (v) read_str(v, end, s->output, sizeof s->output);

    /* Clamped rather than trusted. A hand-edited file with 500 in it should
       come back as full volume, not as whatever 500 does to the control. */
    v = find_key(json, end, "volume");
    if (v && read_uint(v, end, &n)) s->volume = (uint8_t)(n > 100u ? 100u : n);

    /* The codec's range, and no further: above this it clips, and a settings
       file is not a good enough reason to let it. */
    v = find_key(json, end, "hp_level");
    if (v && read_uint(v, end, &n)) s->hp_level = (uint8_t)(n > 88u ? 88u : n);

    v = find_key(json, end, "rec_limit_min");
    if (v && read_uint(v, end, &n) && n <= 24u * 60u)
        s->rectimer.limit_min = (uint16_t)n;

    v = find_key(json, end, "rec_at_min");
    if (v && read_uint(v, end, &n) && n < 24u * 60u)
        s->rectimer.at_min = (int16_t)n;

    const char *arr = find_key(json, end, "registers");
    if (arr && *arr == '[') {
        const char *q = arr + 1;
        while (q < end && s->overrides.count < EN_OVERRIDE_MAX) {
            q = skip_ws(q, end);
            if (q >= end || *q == ']') break;
            if (*q != '{') { q++; continue; }

            const char *obj = q;
            int depth = 0;
            while (q < end) {
                if (*q == '{') depth++;
                else if (*q == '}') { depth--; if (!depth) { q++; break; } }
                q++;
            }
            const char *oend = q;

            uint32_t addr = 0;
            const char *f = find_key(obj, oend, "reg");
            if (!f || !read_uint(f, oend, &addr)) continue;

            char hex[20];
            f = find_key(obj, oend, "bytes");
            if (!f) continue;
            read_str(f, oend, hex, sizeof hex);

            uint8_t data[8], nbytes = 0;
            for (uint8_t k = 0; hex[k] && hex[k + 1] && nbytes < 8; k += 2) {
                uint32_t hi = 0, lo = 0;
                if (!read_uint_hex_digit(hex[k], &hi)) break;
                if (!read_uint_hex_digit(hex[k + 1], &lo)) break;
                data[nbytes++] = (uint8_t)((hi << 4) | lo);
            }
            if (nbytes)
                en_override_set(&s->overrides, (uint8_t)addr, data, nbytes);
        }
    }

    return true;
}

/* ---- the sidecar --------------------------------------------------------- */

static void write_decoded(en_json_t *j, const en_rds_t *r)
{
    if (!r) return;
    en_json_hex16(j, "pi", r->pi);
    en_json_uint(j, "pty", r->pty);
    en_json_str(j, "pty_name", en_rds_pty_name(r->pty, r->rbds));
    en_json_str(j, "ps", r->ps_valid ? r->ps : "");
    en_json_str(j, "rt", r->rt_valid ? r->rt : "");
    en_json_str(j, "ptyn", r->ptyn_valid ? r->ptyn : "");
    en_json_bool(j, "tp", r->tp);
    en_json_bool(j, "ta", r->ta);
    en_json_uint(j, "groups", r->groups);
    en_json_uint(j, "blocks_bad", r->blocks_bad);

    en_json_arr_open(j, "af_khz");
    for (uint8_t i = 0; i < r->af_count; i++) en_json_uint(j, 0, r->af[i]);
    en_json_arr_close(j);
}

uint32_t en_sidecar_begin(en_sidecar_t *s, char *buf, uint32_t cap,
                          uint32_t khz, const char *region, bool rbds,
                          const en_rds_t *r)
{
    if (!s || !buf) return 0;

    s->groups = 0;
    s->open = true;

    en_json_init(&s->j, buf, cap);
    en_json_obj_open(&s->j, 0);
    en_json_uint(&s->j, "version", 1);
    en_json_str(&s->j, "khz_unit", "kHz");
    en_json_uint(&s->j, "khz", khz);
    en_json_str(&s->j, "region", region ? region : "");
    en_json_str(&s->j, "standard", rbds ? "RBDS" : "RDS");

    en_json_obj_open(&s->j, "start");
    write_decoded(&s->j, r);
    en_json_obj_close(&s->j);

    /* Left open. Groups are appended one flush at a time, and the array is
       closed by en_sidecar_end. */
    en_json_arr_open(&s->j, "groups");
    return en_json_done(&s->j);
}

uint32_t en_sidecar_group(en_sidecar_t *s, char *buf, uint32_t cap,
                          uint32_t ms, const uint16_t blk[4], uint8_t valid)
{
    if (!s || !s->open || !buf || !blk) return 0;

    /* A fresh writer over the caller's buffer each time, carrying the comma
       state forward, so an hour of groups never has to fit in memory at once. */
    en_json_init(&s->j, buf, cap);
    s->j.need_comma = s->groups > 0;

    en_json_obj_open(&s->j, 0);
    en_json_uint(&s->j, "ms", ms);
    en_json_arr_open(&s->j, "blocks");
    for (uint8_t i = 0; i < 4; i++) en_json_hex16(&s->j, 0, blk[i]);
    en_json_arr_close(&s->j);
    /* Which blocks were good, so a later re-decode knows what to trust. */
    en_json_uint(&s->j, "valid", valid);
    en_json_obj_close(&s->j);

    s->groups++;
    return en_json_done(&s->j);
}

uint32_t en_sidecar_end(en_sidecar_t *s, char *buf, uint32_t cap,
                        uint32_t duration_ms, const en_rds_t *r)
{
    if (!s || !buf) return 0;

    en_json_init(&s->j, buf, cap);
    s->j.need_comma = false;
    en_json_arr_close(&s->j);          /* close "groups" */

    en_json_uint(&s->j, "duration_ms", duration_ms);
    en_json_uint(&s->j, "group_count", s->groups);

    en_json_obj_open(&s->j, "end");
    write_decoded(&s->j, r);
    en_json_obj_close(&s->j);

    en_json_obj_close(&s->j);          /* close the document */
    s->open = false;
    return en_json_done(&s->j);
}
