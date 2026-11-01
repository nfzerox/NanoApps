/*
 * passwords — credential vault with optional TOTP code display.
 *
 * Each entry has site / username / password / optional base32-encoded
 * TOTP secret. When a secret is present, the detail view shows the
 * current 6-digit RFC 6238 code with a 30-second countdown ring.
 *
 * Storage (plaintext): /Data/Passwords/items.txt — one entry
 * per line, tab-separated: site\tusername\tpassword\tsecret_b32
 *
 * No encryption in v1 — the device's own filesystem isn't network-
 * accessible, so we treat physical possession of the iPod as
 * authorization. Encryption-at-rest with a PIN is a follow-up.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "hb_swipe_row.h"
#include "lvgl/lvgl.h"

#define MAX_ENTRIES   64
#define MAX_FIELD     64
#define DATA_DIR      "/Data/Passwords"
#define ITEMS_PATH    "/Data/Passwords/items.txt"
#define FILE_BUF_SZ   (MAX_ENTRIES * (MAX_FIELD * 4 + 8))

typedef struct {
    char site[MAX_FIELD];
    char user[MAX_FIELD];
    char pass[MAX_FIELD];
    char secret[MAX_FIELD];     /* base32 TOTP secret, "" = no OTP */
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static int     s_n_entries = 0;
static int     s_edit_idx  = -1;

typedef enum { F_SITE, F_USER, F_PASS, F_SECRET } field_id_t;
static field_id_t s_editing_field;

typedef enum {
    VIEW_LIST, VIEW_DETAIL, VIEW_EDITOR, VIEW_FIELD_INPUT,
} view_t;
static view_t s_view = VIEW_LIST;

static entry_t s_edit_buf;

static lv_obj_t *s_scr;
static lv_obj_t *s_list_view;
static lv_obj_t *s_detail_view;
static lv_obj_t *s_editor_view;
static lv_obj_t *s_field_view;
static lv_obj_t *s_list_box;

static lv_obj_t *s_d_site, *s_d_user, *s_d_pass, *s_d_otp, *s_d_countdown;
static lv_obj_t *s_e_site_lbl, *s_e_user_lbl, *s_e_pass_lbl, *s_e_secret_lbl;
static lv_obj_t *s_f_title_lbl, *s_f_ta;

/* ---- string helpers ---- */

static void s_copy(char *dst, const char *src, int cap)
{
    int i = 0; while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void s_clear(char *dst, int cap) { for (int i = 0; i < cap; i++) dst[i] = 0; }

/* ---- persistence ---- */

static void load_items(void)
{
    s_n_entries = 0;
    static char buf[FILE_BUF_SZ];
    uint32_t n = hb_fs_read(ITEMS_PATH, buf, sizeof buf - 1);
    if (n == 0) return;
    buf[n] = 0;

    uint32_t i = 0;
    while (i < n && s_n_entries < MAX_ENTRIES) {
        entry_t *e = &s_entries[s_n_entries];
        s_clear(e->site, MAX_FIELD); s_clear(e->user, MAX_FIELD);
        s_clear(e->pass, MAX_FIELD); s_clear(e->secret, MAX_FIELD);
        char *fields[4] = { e->site, e->user, e->pass, e->secret };
        for (int f = 0; f < 4; f++) {
            int k = 0;
            while (i < n && buf[i] != '\t' && buf[i] != '\n' && k < MAX_FIELD - 1) {
                fields[f][k++] = buf[i++];
            }
            fields[f][k] = 0;
            if (i < n && buf[i] == '\t') i++;
            else break;
        }
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;
        if (e->site[0]) s_n_entries++;
    }
}

static void save_items(void)
{
    static char buf[FILE_BUF_SZ];
    uint32_t pos = 0;
    for (int i = 0; i < s_n_entries; i++) {
        entry_t *e = &s_entries[i];
        const char *flds[4] = { e->site, e->user, e->pass, e->secret };
        for (int f = 0; f < 4; f++) {
            for (int j = 0; flds[f][j] && pos < sizeof buf - 4; j++) buf[pos++] = flds[f][j];
            buf[pos++] = (f == 3) ? '\n' : '\t';
        }
    }
    hb_fs_write(ITEMS_PATH, buf, pos);
}

/* ============================================================
 * SHA-1, HMAC-SHA-1, TOTP (RFC 4226 / 6238)
 * ============================================================ */

typedef struct {
    uint32_t h[5];
    uint8_t  buf[64];
    uint32_t buflen;
    uint64_t total_bits;
} sha1_t;

static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_init(sha1_t *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->buflen = 0; c->total_bits = 0;
}

static void sha1_block(sha1_t *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    }
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    uint32_t a=c->h[0], b=c->h[1], cc=c->h[2], d=c->h[3], e=c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;          k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;          k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void sha1_update(sha1_t *c, const uint8_t *data, uint32_t len)
{
    c->total_bits += (uint64_t)len * 8;
    while (len > 0) {
        uint32_t take = 64 - c->buflen;
        if (take > len) take = len;
        for (uint32_t i = 0; i < take; i++) c->buf[c->buflen + i] = data[i];
        c->buflen += take;
        data += take; len -= take;
        if (c->buflen == 64) { sha1_block(c, c->buf); c->buflen = 0; }
    }
}

static void sha1_final(sha1_t *c, uint8_t out[20])
{
    c->buf[c->buflen++] = 0x80;
    if (c->buflen > 56) {
        while (c->buflen < 64) c->buf[c->buflen++] = 0;
        sha1_block(c, c->buf);
        c->buflen = 0;
    }
    while (c->buflen < 56) c->buf[c->buflen++] = 0;
    for (int i = 7; i >= 0; i--) c->buf[c->buflen++] = (uint8_t)(c->total_bits >> (i * 8));
    sha1_block(c, c->buf);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)(c->h[i] >> 24);
        out[i*4+1] = (uint8_t)(c->h[i] >> 16);
        out[i*4+2] = (uint8_t)(c->h[i] >> 8);
        out[i*4+3] = (uint8_t)c->h[i];
    }
}

static void hmac_sha1(const uint8_t *key, uint32_t klen,
                      const uint8_t *msg, uint32_t mlen,
                      uint8_t out[20])
{
    uint8_t k[64] = {0};
    if (klen > 64) {
        sha1_t c; sha1_init(&c); sha1_update(&c, key, klen); sha1_final(&c, k);
    } else {
        for (uint32_t i = 0; i < klen; i++) k[i] = key[i];
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    sha1_t c; uint8_t inner[20];
    sha1_init(&c); sha1_update(&c, ipad, 64); sha1_update(&c, msg, mlen); sha1_final(&c, inner);
    sha1_init(&c); sha1_update(&c, opad, 64); sha1_update(&c, inner, 20); sha1_final(&c, out);
}

/* Base32 decode (RFC 4648). Returns # of bytes written to out, or 0
   on invalid input. Whitespace and = padding are skipped. */
static int b32_decode(const char *src, uint8_t *out, int out_max)
{
    int bits = 0, val = 0, n = 0;
    for (; *src; src++) {
        char c = *src;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '=') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= '2' && c <= '7') d = 26 + (c - '2');
        else return 0;
        val = (val << 5) | d;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_max) return n;
            out[n++] = (uint8_t)((val >> bits) & 0xFF);
        }
    }
    return n;
}

/* Compute the current TOTP 6-digit code for a base32 secret. Returns
   -1 if the secret is invalid; otherwise the 6-digit code (0..999999)
   and writes the seconds remaining in the current 30-second window. */
static int totp_now(const char *secret_b32, uint32_t *secs_remaining)
{
    uint8_t key[64];
    int klen = b32_decode(secret_b32, key, sizeof key);
    if (klen == 0) return -1;
    /* Unix epoch from RTC. */
    hb_rtc_time_t t; hb_rtc_read(&t);
    /* days_from_epoch (Howard Hinnant) tied to a 0000-03-01 epoch;
       1970-01-01 in that calendar = 719468. */
    int32_t y = t.year, m = t.month, d = t.day_of_month;
    if (m <= 2) { y -= 1; m += 12; }
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (uint32_t)(m - 3) + 2u) / 5u + (uint32_t)d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    int32_t days = era * 146097 + (int32_t)doe - 719468;
    uint64_t unix_secs = (uint64_t)days * 86400ull
                       + (uint64_t)t.hours * 3600ull
                       + (uint64_t)t.minutes * 60ull
                       + (uint64_t)t.seconds;
    uint64_t T = unix_secs / 30ull;
    if (secs_remaining) *secs_remaining = 30u - (uint32_t)(unix_secs % 30ull);
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) { msg[i] = (uint8_t)(T & 0xFF); T >>= 8; }
    uint8_t mac[20];
    hmac_sha1(key, (uint32_t)klen, msg, 8, mac);
    int off = mac[19] & 0x0F;
    uint32_t code = ((uint32_t)(mac[off] & 0x7F) << 24)
                  | ((uint32_t)mac[off+1] << 16)
                  | ((uint32_t)mac[off+2] << 8)
                  | (uint32_t)mac[off+3];
    return (int)(code % 1000000u);
}

/* ---- forward decls ---- */
static void open_list(void);
static void open_detail(int idx);
static void open_editor(int idx);
static void open_field(field_id_t fid);

/* ---- list callbacks ---- */

static void on_row_tap(void *cookie)   { open_detail((int)(uintptr_t)cookie); }
static void on_row_delete(void *cookie)
{
    int idx = (int)(uintptr_t)cookie;
    if (idx < 0 || idx >= s_n_entries) return;
    for (int i = idx; i < s_n_entries - 1; i++) s_entries[i] = s_entries[i+1];
    s_n_entries--;
    save_items();
    open_list();
}

static void on_new(lv_event_t *e) { (void)e; open_editor(-1); }
static void on_back(lv_event_t *e) { (void)e; open_list(); }
static void on_detail_edit(lv_event_t *e) { (void)e; open_editor(s_edit_idx); }

/* ---- editor callbacks ---- */

static void on_edit_site(lv_event_t *e)   { (void)e; open_field(F_SITE); }
static void on_edit_user(lv_event_t *e)   { (void)e; open_field(F_USER); }
static void on_edit_pass(lv_event_t *e)   { (void)e; open_field(F_PASS); }
static void on_edit_secret(lv_event_t *e) { (void)e; open_field(F_SECRET); }

static void on_save_editor(lv_event_t *e)
{
    (void)e;
    if (s_edit_buf.site[0] == 0) { open_list(); return; }
    if (s_edit_idx >= 0 && s_edit_idx < s_n_entries) {
        s_entries[s_edit_idx] = s_edit_buf;
    } else if (s_n_entries < MAX_ENTRIES) {
        s_entries[s_n_entries++] = s_edit_buf;
    }
    save_items();
    open_list();
}

static void on_delete_editor(lv_event_t *e)
{
    (void)e;
    if (s_edit_idx < 0 || s_edit_idx >= s_n_entries) { open_list(); return; }
    for (int i = s_edit_idx; i < s_n_entries - 1; i++) s_entries[i] = s_entries[i+1];
    s_n_entries--;
    save_items();
    open_list();
}

/* ---- field input ---- */

static void on_field_save(lv_event_t *e)
{
    (void)e;
    const char *txt = lv_textarea_get_text(s_f_ta);
    int n = 0; while (txt[n] && n < MAX_FIELD - 1) n++;
    while (n > 0 && (txt[n-1]==' '||txt[n-1]=='\n'||txt[n-1]=='\r')) n--;
    char *dst = (s_editing_field == F_SITE)  ? s_edit_buf.site :
                (s_editing_field == F_USER)  ? s_edit_buf.user :
                (s_editing_field == F_PASS)  ? s_edit_buf.pass :
                                               s_edit_buf.secret;
    for (int i = 0; i < n; i++) dst[i] = txt[i];
    dst[n] = 0;
    lv_label_set_text(s_e_site_lbl,   s_edit_buf.site[0]   ? s_edit_buf.site   : "(tap)");
    lv_label_set_text(s_e_user_lbl,   s_edit_buf.user[0]   ? s_edit_buf.user   : "(tap)");
    lv_label_set_text(s_e_pass_lbl,   s_edit_buf.pass[0]   ? s_edit_buf.pass   : "(tap)");
    lv_label_set_text(s_e_secret_lbl, s_edit_buf.secret[0] ? s_edit_buf.secret : "(no OTP)");
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view,    LV_OBJ_FLAG_HIDDEN);
    s_view = VIEW_EDITOR;
}

static void on_field_cancel(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
    s_view = VIEW_EDITOR;
}

static void open_field(field_id_t fid)
{
    s_editing_field = fid;
    s_view = VIEW_FIELD_INPUT;
    const char *labels[4] = { "Site", "Username", "Password", "TOTP Secret (base32)" };
    lv_label_set_text(s_f_title_lbl, labels[fid]);
    const char *cur = (fid == F_SITE) ? s_edit_buf.site :
                      (fid == F_USER) ? s_edit_buf.user :
                      (fid == F_PASS) ? s_edit_buf.pass :
                                        s_edit_buf.secret;
    lv_textarea_set_text(s_f_ta, cur);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- per-frame OTP refresh ---- */

static void on_frame(void)
{
    if (s_view != VIEW_DETAIL) return;
    if (s_edit_idx < 0 || s_edit_idx >= s_n_entries) return;
    const entry_t *e = &s_entries[s_edit_idx];
    if (!e->secret[0]) return;
    uint32_t secs_left = 0;
    int code = totp_now(e->secret, &secs_left);
    if (code < 0) {
        lv_label_set_text(s_d_otp, "(invalid secret)");
        return;
    }
    char buf[16]; int k = 0;
    for (int div = 100000; div >= 1; div /= 10) {
        buf[k++] = (char)('0' + (code / div) % 10);
        if (div == 1000) buf[k++] = ' ';
    }
    buf[k] = 0;
    lv_label_set_text(s_d_otp, buf);
    char cd[8]; int ck = 0;
    if (secs_left >= 10) cd[ck++] = '0' + (secs_left / 10);
    cd[ck++] = '0' + (secs_left % 10);
    cd[ck++] = 's';
    cd[ck] = 0;
    lv_label_set_text(s_d_countdown, cd);
}

/* ---- views ---- */

static void open_list(void)
{
    s_view = VIEW_LIST;
    s_edit_idx = -1;
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clean(s_list_box);
    if (s_n_entries == 0) {
        lv_obj_t *empty = lv_label_create(s_list_box);
        lv_label_set_text(empty, "No saved logins.\nTap + to add one.");
        lv_obj_set_style_text_color(empty, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    for (int i = 0; i < s_n_entries; i++) {
        entry_t *e = &s_entries[i];
        lv_obj_t *body = hb_swipe_row_create(s_list_box, 224, 48, 64,
            LV_SYMBOL_TRASH, 0xe63946, on_row_tap, on_row_delete,
            (void *)(uintptr_t)i);
        if (!body) continue;
        lv_obj_t *t = lv_label_create(body);
        lv_label_set_text(t, e->site);
        lv_obj_set_style_text_color(t, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, 210);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 10, 4);
        lv_obj_t *u = lv_label_create(body);
        lv_label_set_text(u, e->user[0] ? e->user : "");
        lv_obj_set_style_text_color(u, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_set_style_text_font(u, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(u, LV_LABEL_LONG_DOT);
        lv_obj_set_width(u, 210);
        lv_obj_align(u, LV_ALIGN_BOTTOM_LEFT, 10, -4);
    }
}

static void open_detail(int idx)
{
    if (idx < 0 || idx >= s_n_entries) return;
    s_edit_idx = idx;
    s_view = VIEW_DETAIL;
    entry_t *e = &s_entries[idx];
    lv_label_set_text(s_d_site, e->site);
    lv_label_set_text(s_d_user, e->user[0] ? e->user : "(no username)");
    lv_label_set_text(s_d_pass, e->pass[0] ? e->pass : "(no password)");
    if (e->secret[0]) {
        lv_label_set_text(s_d_otp, "------");
        lv_label_set_text(s_d_countdown, "");
        lv_obj_clear_flag(s_d_otp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_d_countdown, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_d_otp, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_d_countdown, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
}

static void open_editor(int idx)
{
    s_view = VIEW_EDITOR;
    s_edit_idx = idx;
    if (idx >= 0 && idx < s_n_entries) {
        s_edit_buf = s_entries[idx];
    } else {
        s_clear(s_edit_buf.site, MAX_FIELD);
        s_clear(s_edit_buf.user, MAX_FIELD);
        s_clear(s_edit_buf.pass, MAX_FIELD);
        s_clear(s_edit_buf.secret, MAX_FIELD);
    }
    lv_label_set_text(s_e_site_lbl,   s_edit_buf.site[0]   ? s_edit_buf.site   : "(tap)");
    lv_label_set_text(s_e_user_lbl,   s_edit_buf.user[0]   ? s_edit_buf.user   : "(tap)");
    lv_label_set_text(s_e_pass_lbl,   s_edit_buf.pass[0]   ? s_edit_buf.pass   : "(tap)");
    lv_label_set_text(s_e_secret_lbl, s_edit_buf.secret[0] ? s_edit_buf.secret : "(no OTP)");
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);
}

/* ---- view construction ---- */

static lv_obj_t *make_field_row(lv_obj_t *parent, int y, const char *hdr,
                                lv_event_cb_t cb, lv_obj_t **out_val)
{
    lv_obj_t *h = lv_label_create(parent);
    lv_label_set_text(h, hdr);
    lv_obj_set_style_text_color(h, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(h, 12, y);

    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, 220, 34);
    lv_obj_set_pos(row, 10, y + 18);
    lv_obj_set_style_bg_color(row, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "");
    lv_obj_set_style_text_color(v, lv_color_hex(hb_color_text()), 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v, 200);
    lv_obj_align(v, LV_ALIGN_LEFT_MID, 8, 0);
    *out_val = v;
    return row;
}

static void build_list_view(void)
{
    s_list_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_list_view, 240, 432);
    lv_obj_set_style_radius(s_list_view, 0, 0);
    lv_obj_set_pos(s_list_view, 0, 0);
    lv_obj_set_style_bg_color(s_list_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_view, 0, 0);
    lv_obj_set_style_pad_all(s_list_view, 0, 0);
    lv_obj_clear_flag(s_list_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s_list_view);
    lv_label_set_text(hdr, "Passwords");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_24, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 12, 10);

    lv_obj_t *new_btn = lv_button_create(s_list_view);
    lv_obj_set_size(new_btn, 40, 36);
    lv_obj_align(new_btn, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_color(new_btn, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(new_btn, on_new, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = lv_label_create(new_btn);
    lv_label_set_text(nl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(nl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(nl, &lv_font_montserrat_20, 0);
    lv_obj_center(nl);

    s_list_box = lv_obj_create(s_list_view);
    lv_obj_set_size(s_list_box, 240, 432 - 56);
    lv_obj_align(s_list_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_list_box, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_list_box, 0, 0);
    lv_obj_set_style_pad_all(s_list_box, 6, 0);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
}

static void build_detail_view(void)
{
    s_detail_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_detail_view, 240, 432);
    lv_obj_set_style_radius(s_detail_view, 0, 0);
    lv_obj_set_pos(s_detail_view, 0, 0);
    lv_obj_set_style_bg_color(s_detail_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_detail_view, 0, 0);
    lv_obj_set_style_pad_all(s_detail_view, 0, 0);
    lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_detail_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *edit = lv_button_create(s_detail_view);
    lv_obj_set_size(edit, 56, 32);
    lv_obj_align(edit, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(edit, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(edit, on_detail_edit, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(edit);
    lv_label_set_text(el, "Edit");
    lv_obj_set_style_text_color(el, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(el);

    s_d_site = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_site, "");
    lv_obj_set_style_text_color(s_d_site, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_d_site, &lv_font_montserrat_24, 0);
    lv_label_set_long_mode(s_d_site, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_d_site, 220);
    lv_obj_align(s_d_site, LV_ALIGN_TOP_MID, 0, 50);

    s_d_user = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_user, "");
    lv_obj_set_style_text_color(s_d_user, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_d_user, &lv_font_montserrat_16, 0);
    lv_label_set_long_mode(s_d_user, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_d_user, 220);
    lv_obj_align(s_d_user, LV_ALIGN_TOP_MID, 0, 92);

    s_d_pass = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_pass, "");
    lv_obj_set_style_text_color(s_d_pass, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_d_pass, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_d_pass, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_d_pass, 220);
    lv_obj_set_style_text_align(s_d_pass, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_d_pass, LV_ALIGN_TOP_MID, 0, 130);

    s_d_otp = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_otp, "");
    lv_obj_set_style_text_color(s_d_otp, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_text_font(s_d_otp, &lv_font_montserrat_48, 0);
    lv_obj_align(s_d_otp, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_add_flag(s_d_otp, LV_OBJ_FLAG_HIDDEN);

    s_d_countdown = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_countdown, "");
    lv_obj_set_style_text_color(s_d_countdown, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_d_countdown, &lv_font_montserrat_16, 0);
    lv_obj_align(s_d_countdown, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_flag(s_d_countdown, LV_OBJ_FLAG_HIDDEN);
}

static void build_editor_view(void)
{
    s_editor_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_editor_view, 240, 432);
    lv_obj_set_style_radius(s_editor_view, 0, 0);
    lv_obj_set_pos(s_editor_view, 0, 0);
    lv_obj_set_style_bg_color(s_editor_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_editor_view, 0, 0);
    lv_obj_set_style_pad_all(s_editor_view, 0, 0);
    lv_obj_clear_flag(s_editor_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_editor_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_editor_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    lv_obj_t *save = lv_button_create(s_editor_view);
    lv_obj_set_size(save, 56, 32);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(save, on_save_editor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    make_field_row(s_editor_view, 44,  "Site",          on_edit_site,   &s_e_site_lbl);
    make_field_row(s_editor_view, 102, "Username",      on_edit_user,   &s_e_user_lbl);
    make_field_row(s_editor_view, 160, "Password",      on_edit_pass,   &s_e_pass_lbl);
    make_field_row(s_editor_view, 218, "OTP secret",    on_edit_secret, &s_e_secret_lbl);

    lv_obj_t *del = lv_button_create(s_editor_view);
    lv_obj_set_size(del, 220, 36);
    lv_obj_align(del, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(del, lv_color_hex(hb_color_danger()), 0);
    lv_obj_add_event_cb(del, on_delete_editor, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(del);
    lv_label_set_text(dl, LV_SYMBOL_TRASH "  Delete");
    lv_obj_set_style_text_color(dl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(dl);
}

static void build_field_view(void)
{
    s_field_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_field_view, 240, 432);
    lv_obj_set_style_radius(s_field_view, 0, 0);
    lv_obj_set_pos(s_field_view, 0, 0);
    lv_obj_set_style_bg_color(s_field_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_field_view, 0, 0);
    lv_obj_set_style_pad_all(s_field_view, 0, 0);
    lv_obj_clear_flag(s_field_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_field_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_field_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_field_cancel, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    s_f_title_lbl = lv_label_create(s_field_view);
    lv_label_set_text(s_f_title_lbl, "Field");
    lv_obj_set_style_text_color(s_f_title_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_f_title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_f_title_lbl, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t *save = lv_button_create(s_field_view);
    lv_obj_set_size(save, 56, 32);
    lv_obj_align(save, LV_ALIGN_TOP_RIGHT, -6, 6);
    lv_obj_set_style_bg_color(save, lv_color_hex(hb_color_success()), 0);
    lv_obj_add_event_cb(save, on_field_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(save);
    lv_label_set_text(sl, LV_SYMBOL_OK);
    lv_obj_set_style_text_color(sl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(sl);

    s_f_ta = lv_textarea_create(s_field_view);
    lv_obj_set_size(s_f_ta, 240, 152);
    lv_obj_align(s_f_ta, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_bg_color(s_f_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_f_ta, lv_color_hex(hb_color_text()), 0);
    lv_textarea_set_one_line(s_f_ta, true);

    lv_obj_t *kb_box = lv_obj_create(s_field_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_f_ta);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    load_items();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_list_view();
    build_detail_view();
    build_editor_view();
    build_field_view();

    open_list();
    hb_lv_set_frame_cb(on_frame);
}
