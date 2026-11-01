/*
 * wordle — guess the 5-letter word in 6 tries.
 *
 * Built-in answer wordlist (~150 common 5-letter words); users can
 * drop additional wordlists at /Data/Wordle/<name>.txt (one
 * word per line) and they're appended to the pool.
 *
 * Grid is rendered with one lv_obj per cell — 6 rows x 5 cols = 30
 * cells, easily within the LVGL widget budget. The QWERTY keyboard
 * below is also lv_obj per key.
 *
 * Feedback: tiles flip color after each guess —
 *   green   = letter in correct position
 *   yellow  = letter is somewhere in the answer but wrong position
 *   gray    = letter not in the answer
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define MAX_GUESSES   6
#define WORD_LEN      5
#define MAX_WORDS     800
#define DATA_DIR      "/Data/Wordle"

/* Built-in answer list — common safe 5-letter English words. */
static const char *s_builtin_words[] = {
    "apple","beach","brave","bread","bring","build","chair","chase","clean","clear",
    "climb","cloud","color","could","cream","crown","dance","dream","drink","earth",
    "eight","empty","every","faith","field","fight","first","flame","flesh","flock",
    "floor","flour","flush","focus","force","forge","forty","found","frame","fresh",
    "front","fruit","funny","ghost","giant","given","glass","glove","going","grace",
    "grand","grant","grass","great","green","group","grown","guess","guide","hands",
    "happy","heart","heavy","hello","honey","horse","hotel","house","hover","human",
    "image","kings","knife","known","large","laugh","learn","level","light","lemon",
    "lower","lunch","magic","maker","march","metal","model","money","month","mouse",
    "music","never","night","nurse","ocean","offer","often","other","ought","paint",
    "panel","paper","party","peace","piece","place","plant","plate","point","pound",
    "power","press","price","pride","prove","quick","quiet","queen","ratio","reach",
    "ready","right","river","round","royal","scale","score","sense","seven","shake",
    "shape","share","sharp","sheet","shine","shirt","short","shown","sight","since",
    "skill","sleep","slice","small","smart","smile","smoke","solid","solve","sorry",
    "sound","south","space","speak","speed","spend","spent","spoke","sport","stage",
    "stand","start","state","stick","still","stock","stone","store","storm","story",
    "study","style","sugar","sweet","table","taken","teach","teeth","thank","their",
    "there","these","thing","think","third","those","three","threw","throw","tiger",
    "today","total","touch","tough","tower","track","trade","train","treat","trend",
    "trial","tried","tries","trust","truth","twice","under","unity","until","upper",
    "value","video","virus","vital","voice","wagon","watch","water","wheel","while",
    "white","whole","whose","woman","women","world","worry","worse","worst","worth",
    "would","write","wrong","young","youth", NULL
};

static char  *s_words[MAX_WORDS];   /* all candidate answers (built-in + custom) */
static int    s_n_words = 0;
/* For custom decks we keep the blob alive so the string pointers
   into it stay valid. */
static char  *s_extra_blob = NULL;

static char  s_answer[6];
static char  s_grid[MAX_GUESSES][WORD_LEN + 1];
static int   s_g_row = 0;
static int   s_g_col = 0;
static bool  s_done = false;

static lv_obj_t *s_scr;
static lv_obj_t *s_tile_objs[MAX_GUESSES][WORD_LEN];
static lv_obj_t *s_tile_labs[MAX_GUESSES][WORD_LEN];
static lv_obj_t *s_status_lbl;
/* Keyboard state per letter — used to grey out tried-and-missing
   letters on the on-screen keyboard. */
static uint8_t s_key_state[26];  /* 0=unused, 1=miss, 2=present, 3=hit */
static lv_obj_t *s_key_objs[26];

/* PRNG */
static uint32_t s_rng;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

/* ---- load wordlists ---- */

static void load_custom(void)
{
    /* Walk /Data/Wordle/*.txt, append lines that are exactly 5 lowercase
       letters. Heap-dup the blob so the pointers stay valid. */
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    char fn[64]; bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (is_dir) continue;
        int nl = 0; while (fn[nl]) nl++;
        if (nl < 5 || fn[nl-4]!='.' || fn[nl-3]!='t' || fn[nl-2]!='x' || fn[nl-1]!='t') continue;
        char path[256]; int pk = 0;
        const char *pp = DATA_DIR "/"; while (*pp) path[pk++] = *pp++;
        for (int i = 0; fn[i] && pk < 254; i++) path[pk++] = fn[i];
        path[pk] = 0;
        uint32_t sz = hb_fs_size(path);
        if (sz == 0 || sz > 64 * 1024) continue;
        char *blob = lv_malloc(sz + 1);
        if (!blob) continue;
        uint32_t rdn = hb_fs_read(path, blob, sz);
        blob[rdn < sz ? rdn : sz] = 0;
        /* Walk lines */
        char *p = blob;
        while (*p && s_n_words < MAX_WORDS) {
            while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
            if (!*p) break;
            char *start = p;
            while (*p && *p != '\r' && *p != '\n') p++;
            int len = (int)(p - start);
            if (*p) { *p = 0; p++; }
            if (len == 5) {
                bool ok = true;
                for (int i = 0; i < 5; i++) {
                    char c = start[i];
                    if (c >= 'A' && c <= 'Z') { c = (char)(c + 32); start[i] = c; }
                    if (c < 'a' || c > 'z') { ok = false; break; }
                }
                if (ok) s_words[s_n_words++] = start;
            }
        }
        /* If at least one word came from this blob, keep it; otherwise free. */
        bool any = false;
        for (int i = 0; i < s_n_words; i++) {
            if (s_words[i] >= blob && s_words[i] < blob + sz) { any = true; break; }
        }
        if (any && s_extra_blob == NULL) s_extra_blob = blob;
        else if (!any) lv_free(blob);
        else { /* future improvement: chain multiple blobs */ }
    }
    hb_fs_dir_close(&d);
}

static void load_words(void)
{
    s_n_words = 0;
    for (int i = 0; s_builtin_words[i] && s_n_words < MAX_WORDS; i++) {
        s_words[s_n_words++] = (char *)s_builtin_words[i];
    }
    load_custom();
}

/* ---- game ---- */

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

static void pick_answer(void)
{
    int idx = (int)(rnd() % (uint32_t)s_n_words);
    for (int i = 0; i < 5; i++) s_answer[i] = s_words[idx][i];
    s_answer[5] = 0;
}

static void refresh_keyboard(void);

static void render_grid(void)
{
    for (int r = 0; r < MAX_GUESSES; r++) for (int c = 0; c < WORD_LEN; c++) {
        char letter = s_grid[r][c];
        char buf[2] = { letter ? letter : ' ', 0 };
        if (letter >= 'a' && letter <= 'z') buf[0] = (char)(letter - 32);
        lv_label_set_text(s_tile_labs[r][c], buf);
        uint32_t bg;
        if (!letter) {
            bg = (r == s_g_row && c == s_g_col) ? 0x4a5060 : 0x222531;
        } else if (r < s_g_row) {
            /* Color from feedback. We compute per render so we don't
               have to store separately. */
            char a = letter, ans = s_answer[c];
            if (a == ans) bg = 0x2a9d8f;
            else {
                bool found = false;
                for (int k = 0; k < WORD_LEN; k++) if (s_answer[k] == a) { found = true; break; }
                bg = found ? 0xfcbf49 : 0x6b7280;
            }
        } else {
            bg = (r == s_g_row && c == s_g_col) ? 0x4a5060 : 0x333540;
        }
        lv_obj_set_style_bg_color(s_tile_objs[r][c], lv_color_hex(bg), 0);
    }
}

static bool is_valid_word(const char *w)
{
    /* Accept any 5-letter sequence in the word pool. */
    for (int i = 0; i < s_n_words; i++) {
        bool eq = true;
        for (int j = 0; j < 5; j++) if (w[j] != s_words[i][j]) { eq = false; break; }
        if (eq) return true;
    }
    return false;
}

static void update_key_states(int row)
{
    for (int c = 0; c < WORD_LEN; c++) {
        char letter = s_grid[row][c];
        int li = letter - 'a';
        if (li < 0 || li >= 26) continue;
        uint8_t newst = 1;     /* miss */
        if (s_answer[c] == letter) newst = 3;
        else {
            for (int k = 0; k < WORD_LEN; k++) if (s_answer[k] == letter) { newst = 2; break; }
        }
        if (newst > s_key_state[li]) s_key_state[li] = newst;
    }
}

static void refresh_keyboard(void)
{
    for (int i = 0; i < 26; i++) {
        uint32_t bg;
        switch (s_key_state[i]) {
            case 3:  bg = 0x2a9d8f; break;
            case 2:  bg = 0xfcbf49; break;
            case 1:  bg = 0x4a5060; break;
            default: bg = 0x14213d; break;
        }
        lv_obj_set_style_bg_color(s_key_objs[i], lv_color_hex(bg), 0);
    }
}

static void submit_guess(void)
{
    if (s_g_col != WORD_LEN) {
        lv_label_set_text(s_status_lbl, "Not enough letters");
        return;
    }
    if (!is_valid_word(s_grid[s_g_row])) {
        lv_label_set_text(s_status_lbl, "Not in word list");
        return;
    }
    bool win = true;
    for (int c = 0; c < WORD_LEN; c++) {
        if (s_grid[s_g_row][c] != s_answer[c]) { win = false; break; }
    }
    update_key_states(s_g_row);
    s_g_row++;
    s_g_col = 0;
    if (win) {
        char buf[40]; int k = 0;
        const char *p = "Solved in "; while (*p) buf[k++] = *p++;
        char nb[8]; itoa_u((uint32_t)s_g_row, nb);
        for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
        const char *p2 = "/6  Tap any letter to restart"; while (*p2) buf[k++] = *p2++;
        buf[k] = 0;
        lv_label_set_text(s_status_lbl, buf);
        s_done = true;
    } else if (s_g_row >= MAX_GUESSES) {
        char buf[40] = "Out! Answer: ";
        int k = 13;
        for (int i = 0; i < 5; i++) buf[k++] = (char)(s_answer[i] - 32);
        buf[k] = 0;
        lv_label_set_text(s_status_lbl, buf);
        s_done = true;
    } else {
        lv_label_set_text(s_status_lbl, "");
    }
    render_grid();
    refresh_keyboard();
}

static void reset_game(void)
{
    for (int r = 0; r < MAX_GUESSES; r++) for (int c = 0; c <= WORD_LEN; c++) s_grid[r][c] = 0;
    s_g_row = 0; s_g_col = 0; s_done = false;
    for (int i = 0; i < 26; i++) s_key_state[i] = 0;
    pick_answer();
    lv_label_set_text(s_status_lbl, "");
    render_grid();
    refresh_keyboard();
}

static void on_letter(lv_event_t *e)
{
    int li = (int)(uintptr_t)lv_event_get_user_data(e);
    if (s_done) { reset_game(); return; }
    if (s_g_col >= WORD_LEN) return;
    s_grid[s_g_row][s_g_col++] = (char)('a' + li);
    render_grid();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (s_done) { reset_game(); return; }
    if (s_g_col > 0) {
        s_g_col--;
        s_grid[s_g_row][s_g_col] = 0;
        render_grid();
    }
}

static void on_enter(lv_event_t *e)
{
    (void)e;
    if (s_done) { reset_game(); return; }
    submit_guess();
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    s_rng = hb_time_uptime_us() | 1u;
    load_words();
    if (s_n_words == 0) {
        /* Defensive — built-in always has some words. */
        s_n_words = 1; s_words[0] = (char *)"apple";
    }

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* Status line at top. */
    s_status_lbl = lv_label_create(s_scr);
    lv_label_set_text(s_status_lbl, "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_status_lbl, 224);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 6);

    /* 6x5 tile grid */
    int tile_size = 38, gap = 4;
    int grid_w = WORD_LEN * tile_size + (WORD_LEN - 1) * gap;
    int start_x = (240 - grid_w) / 2;
    int start_y = 28;
    for (int r = 0; r < MAX_GUESSES; r++) {
        for (int c = 0; c < WORD_LEN; c++) {
            lv_obj_t *t = lv_obj_create(s_scr);
            lv_obj_set_size(t, tile_size, tile_size);
            lv_obj_set_pos(t, start_x + c * (tile_size + gap),
                              start_y + r * (tile_size + gap));
            lv_obj_set_style_bg_color(t, lv_color_hex(0x222531), 0);
            lv_obj_set_style_border_color(t, lv_color_hex(hb_color_text_dim()), 0);
            lv_obj_set_style_border_width(t, 1, 0);
            lv_obj_set_style_radius(t, 4, 0);
            lv_obj_set_style_pad_all(t, 0, 0);
            lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
            s_tile_objs[r][c] = t;
            lv_obj_t *l = lv_label_create(t);
            lv_label_set_text(l, "");
            lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
            lv_obj_center(l);
            s_tile_labs[r][c] = l;
        }
    }

    /* QWERTY keyboard. */
    int kb_y = start_y + MAX_GUESSES * (tile_size + gap) + 8;
    int key_w = 21, key_h = 30, key_gap = 2;
    const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    int row_offs[3] = { 0, 11, 22 };
    for (int r = 0; r < 3; r++) {
        const char *rstr = rows[r];
        int n_keys = 0; while (rstr[n_keys]) n_keys++;
        int row_w = n_keys * key_w + (n_keys - 1) * key_gap;
        int row_x = (240 - row_w) / 2;
        int ry = kb_y + r * (key_h + 4);
        for (int k = 0; k < n_keys; k++) {
            lv_obj_t *b = lv_button_create(s_scr);
            lv_obj_set_size(b, key_w, key_h);
            lv_obj_set_pos(b, row_x + k * (key_w + key_gap), ry);
            lv_obj_set_style_bg_color(b, lv_color_hex(hb_color_surface()), 0);
            lv_obj_set_style_radius(b, 4, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            int li = rstr[k] - 'a';
            lv_obj_add_event_cb(b, on_letter, LV_EVENT_CLICKED,
                                (void *)(uintptr_t)li);
            lv_obj_t *l = lv_label_create(b);
            char buf[2] = { (char)(rstr[k] - 32), 0 };
            lv_label_set_text(l, buf);
            lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
            lv_obj_center(l);
            s_key_objs[li] = b;
        }
        (void)row_offs;
    }
    /* Backspace (X) sits on the bottom-right of the last letter row. */
    int br_y = kb_y + 2 * (key_h + 4);
    lv_obj_t *bb = lv_button_create(s_scr);
    lv_obj_set_size(bb, 34, key_h);
    lv_obj_set_pos(bb, 200, br_y);
    lv_obj_set_style_bg_color(bb, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_radius(bb, 4, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bb);
    lv_label_set_text(bl, LV_SYMBOL_BACKSPACE);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    /* Enter is its own full-width row below the keyboard. */
    int er_y = kb_y + 3 * (key_h + 4) + 6;
    lv_obj_t *eb = lv_button_create(s_scr);
    lv_obj_set_size(eb, 224, 34);
    lv_obj_set_pos(eb, 8, er_y);
    lv_obj_set_style_bg_color(eb, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(eb, 6, 0);
    lv_obj_add_event_cb(eb, on_enter, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = lv_label_create(eb);
    lv_label_set_text(el, "Enter");
    lv_obj_set_style_text_color(el, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(el);

    reset_game();

}
