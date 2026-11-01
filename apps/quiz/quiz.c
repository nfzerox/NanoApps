/*
 * quiz — multiple-choice quiz game with built-in + user-loaded decks.
 *
 * Deck picker on launch, then play view: question text + 4 answer
 * buttons; tap the right one to score and advance. End-of-deck shows
 * a score summary.
 *
 * Deck file format (one question per record, records separated by
 * blank line):
 *   Q: How many continents are there?
 *   *: 7        ← lines starting "*: " are the correct answer
 *   A: 5
 *   A: 6
 *   A: 8
 *
 * Built-in deck "General Knowledge" ships with the binary; the user
 * can drop additional .txt deck files at /Data/Quiz/decks/.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define MAX_DECKS    16
#define MAX_Q        128
#define MAX_TEXT     128
#define DATA_DIR     "/Data/Quiz"
#define DECKS_DIR    "/Data/Quiz/decks"

typedef struct {
    const char *q;
    const char *a[4];
    int correct;        /* index into a[] */
} question_t;

typedef struct {
    const char *name;
    const question_t *questions;
    int n_questions;
    /* For loaded decks, the storage is heap-allocated and the pointers
       above point into it. The loader fills q/a/correct with offsets
       into a blob and we point them at the blob bytes. */
    char *blob;
} deck_t;

/* ---- built-in deck (compact general knowledge) ---- */

static const question_t s_general_qs[] = {
    { "How many continents are there?", { "5", "6", "7", "8" }, 2 },
    { "Which planet is closest to the sun?", { "Earth", "Mercury", "Venus", "Mars" }, 1 },
    { "What is the largest ocean?", { "Atlantic", "Indian", "Arctic", "Pacific" }, 3 },
    { "In what year did WW2 end?", { "1942", "1945", "1948", "1950" }, 1 },
    { "Square root of 144?", { "10", "11", "12", "13" }, 2 },
    { "Capital of Australia?", { "Sydney", "Melbourne", "Canberra", "Perth" }, 2 },
    { "Author of 1984?", { "Huxley", "Orwell", "Bradbury", "Asimov" }, 1 },
    { "How many sides does a hexagon have?", { "5", "6", "7", "8" }, 1 },
    { "Chemical symbol for gold?", { "Gd", "Go", "Au", "Ag" }, 2 },
    { "Largest mammal?", { "Elephant", "Blue whale", "Giraffe", "Rhino" }, 1 },
    { "What language has the most native speakers?", { "English", "Spanish", "Mandarin", "Hindi" }, 2 },
    { "How many bones does an adult human have?", { "186", "206", "226", "246" }, 1 },
    { "Element with symbol Fe?", { "Fluorine", "Iron", "Francium", "Iodine" }, 1 },
    { "Country of origin of pizza?", { "Greece", "France", "Italy", "Spain" }, 2 },
    { "How many strings does a guitar typically have?", { "4", "5", "6", "7" }, 2 },
};
#define N_GENERAL  (int)(sizeof s_general_qs / sizeof s_general_qs[0])

static deck_t s_decks[MAX_DECKS];
static int    s_n_decks = 0;

static int       s_active_deck = 0;
static int       s_q_idx       = 0;
static int       s_correct     = 0;
static int       s_wrong       = 0;

typedef enum { VIEW_PICKER, VIEW_PLAY, VIEW_RESULTS } view_t;
static view_t    s_view = VIEW_PICKER;

static lv_obj_t *s_scr;
static lv_obj_t *s_picker_view;
static lv_obj_t *s_play_view;
static lv_obj_t *s_results_view;
static lv_obj_t *s_q_lbl;
static lv_obj_t *s_a_btns[4];
static lv_obj_t *s_a_lbls[4];
static lv_obj_t *s_progress_lbl;
static lv_obj_t *s_results_lbl;

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

/* ---- file loader ----
   Parse the simple Q:/*:/A: format. Returns 1 on success. */
static int load_deck_file(const char *path, const char *filename)
{
    if (s_n_decks >= MAX_DECKS) return 0;
    uint32_t sz = hb_fs_size(path);
    if (sz == 0 || sz > 64 * 1024) return 0;
    char *blob = lv_malloc(sz + 1);
    if (!blob) return 0;
    uint32_t rd = hb_fs_read(path, blob, sz);
    blob[rd < sz ? rd : sz] = 0;

    /* Question array + answer pointer arrays. Cap at MAX_Q. */
    question_t *qs = lv_malloc(MAX_Q * sizeof(question_t));
    if (!qs) { lv_free(blob); return 0; }
    int n_q = 0;

    char *p = blob;
    while (*p && n_q < MAX_Q) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *q_text = NULL;
        const char *a_text[4] = { NULL, NULL, NULL, NULL };
        int correct = -1;
        int a_count = 0;
        while (*p) {
            /* Read one line */
            char *line = p;
            while (*p && *p != '\n' && *p != '\r') p++;
            if (*p) { *p = 0; p++; }
            while (*p == '\r' || *p == '\n') {
                if (*p == '\n' && p != line) {
                    /* blank line separator — emit if we have content */
                    p++;
                    goto emit;
                }
                p++;
            }
            if (line[0] == 'Q' && line[1] == ':') {
                q_text = line + 2;
                while (*q_text == ' ') q_text++;
            } else if (line[0] == '*' && line[1] == ':') {
                if (a_count < 4) {
                    const char *t = line + 2;
                    while (*t == ' ') t++;
                    a_text[a_count] = t;
                    correct = a_count;
                    a_count++;
                }
            } else if (line[0] == 'A' && line[1] == ':') {
                if (a_count < 4) {
                    const char *t = line + 2;
                    while (*t == ' ') t++;
                    a_text[a_count++] = t;
                }
            }
        }
        emit:
        if (q_text && correct >= 0 && a_count >= 2) {
            /* Pad to 4 with empty strings if fewer than 4. */
            for (int i = a_count; i < 4; i++) a_text[i] = "";
            qs[n_q].q = q_text;
            for (int i = 0; i < 4; i++) qs[n_q].a[i] = a_text[i];
            qs[n_q].correct = correct;
            n_q++;
        }
    }
    if (n_q == 0) { lv_free(blob); lv_free(qs); return 0; }

    /* Deck name from filename minus .txt */
    int nl = 0; while (filename[nl]) nl++;
    int label_len = nl;
    if (label_len >= 4 && filename[label_len-4]=='.' &&
        filename[label_len-3]=='t' && filename[label_len-2]=='x' &&
        filename[label_len-1]=='t') label_len -= 4;
    char *name = lv_malloc((uint32_t)label_len + 1);
    if (!name) { lv_free(blob); lv_free(qs); return 0; }
    for (int i = 0; i < label_len; i++) name[i] = filename[i];
    name[label_len] = 0;

    s_decks[s_n_decks].name = name;
    s_decks[s_n_decks].questions = qs;
    s_decks[s_n_decks].n_questions = n_q;
    s_decks[s_n_decks].blob = blob;
    s_n_decks++;
    return 1;
}

static bool ends_with_txt(const char *s)
{
    int n = 0; while (s[n]) n++;
    return n >= 5 && s[n-4]=='.' && s[n-3]=='t' && s[n-2]=='x' && s[n-1]=='t';
}

static void load_all_decks(void)
{
    s_decks[s_n_decks].name = "General Knowledge";
    s_decks[s_n_decks].questions = s_general_qs;
    s_decks[s_n_decks].n_questions = N_GENERAL;
    s_decks[s_n_decks].blob = NULL;
    s_n_decks++;

    hb_dir_t d;
    if (!hb_fs_dir_open(&d, DECKS_DIR, false)) return;
    char fn[64]; bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (is_dir || !ends_with_txt(fn)) continue;
        char path[256]; int k = 0;
        const char *p = DECKS_DIR "/";
        while (*p) path[k++] = *p++;
        for (int i = 0; fn[i] && k < (int)sizeof path - 1; i++) path[k++] = fn[i];
        path[k] = 0;
        load_deck_file(path, fn);
    }
    hb_fs_dir_close(&d);
}

/* ---- view: picker → play → results ---- */

static void show_picker(void);
static void show_play(void);
static void show_results(void);

static void render_question(void)
{
    const deck_t *d = &s_decks[s_active_deck];
    if (s_q_idx >= d->n_questions) {
        show_results();
        return;
    }
    const question_t *q = &d->questions[s_q_idx];
    lv_label_set_text(s_q_lbl, q->q);
    for (int i = 0; i < 4; i++) {
        lv_label_set_text(s_a_lbls[i], q->a[i] ? q->a[i] : "");
        lv_obj_set_style_bg_color(s_a_btns[i], lv_color_hex(hb_color_surface()), 0);
    }
    char prog[24]; int k = 0;
    char nb[12]; itoa_u((uint32_t)(s_q_idx + 1), nb);
    for (int i = 0; nb[i]; i++) prog[k++] = nb[i];
    prog[k++] = '/';
    itoa_u((uint32_t)d->n_questions, nb);
    for (int i = 0; nb[i]; i++) prog[k++] = nb[i];
    prog[k] = 0;
    lv_label_set_text(s_progress_lbl, prog);
}

static void on_answer(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    const question_t *q = &s_decks[s_active_deck].questions[s_q_idx];
    if (idx == q->correct) {
        s_correct++;
        lv_obj_set_style_bg_color(s_a_btns[idx], lv_color_hex(hb_color_success()), 0);
    } else {
        s_wrong++;
        lv_obj_set_style_bg_color(s_a_btns[idx], lv_color_hex(hb_color_danger()), 0);
        lv_obj_set_style_bg_color(s_a_btns[q->correct], lv_color_hex(hb_color_success()), 0);
    }
    /* Advance after a short delay so the user sees the result. We use
       a simple LVGL timer rather than blocking on a sleep. */
    lv_timer_t *t = lv_timer_create((lv_timer_cb_t)render_question, 700, NULL);
    lv_timer_set_repeat_count(t, 1);
    s_q_idx++;
    /* Disable buttons briefly by removing the event */
    for (int i = 0; i < 4; i++) lv_obj_remove_state(s_a_btns[i], LV_STATE_DISABLED);
    /* render_question is called by the timer; until then current view
       shows the colored answer. */
}

static void on_deck_pick(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_decks) return;
    s_active_deck = idx;
    s_q_idx = 0;
    s_correct = 0;
    s_wrong = 0;
    show_play();
}

static void on_play_again(lv_event_t *e) { (void)e; show_picker(); }

static void show_picker(void)
{
    s_view = VIEW_PICKER;
    lv_obj_clear_flag(s_picker_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_results_view, LV_OBJ_FLAG_HIDDEN);
}

static void show_play(void)
{
    s_view = VIEW_PLAY;
    lv_obj_add_flag(s_picker_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_play_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_results_view, LV_OBJ_FLAG_HIDDEN);
    render_question();
}

static void show_results(void)
{
    s_view = VIEW_RESULTS;
    lv_obj_add_flag(s_picker_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_results_view, LV_OBJ_FLAG_HIDDEN);

    char buf[80]; int k = 0;
    const char *p = "Score: "; while (*p) buf[k++] = *p++;
    char nb[12];
    itoa_u((uint32_t)s_correct, nb); for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k++] = '/';
    itoa_u((uint32_t)s_decks[s_active_deck].n_questions, nb);
    for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k++] = '\n';
    p = "Right "; while (*p) buf[k++] = *p++;
    itoa_u((uint32_t)s_correct, nb); for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    p = "  Wrong "; while (*p) buf[k++] = *p++;
    itoa_u((uint32_t)s_wrong, nb); for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k] = 0;
    lv_label_set_text(s_results_lbl, buf);
}

/* ---- build ---- */

static void build_picker(void)
{
    s_picker_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_picker_view, 240, 432);
    lv_obj_set_style_radius(s_picker_view, 0, 0);
    lv_obj_set_pos(s_picker_view, 0, 0);
    lv_obj_set_style_bg_color(s_picker_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_picker_view, 0, 0);
    lv_obj_set_style_pad_all(s_picker_view, 0, 0);
    lv_obj_clear_flag(s_picker_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_picker_view);
    lv_label_set_text(title, "Quiz");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *sub = lv_label_create(s_picker_view);
    lv_label_set_text(sub, "Pick a deck");
    lv_obj_set_style_text_color(sub, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_16, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 56);

    lv_obj_t *box = lv_obj_create(s_picker_view);
    lv_obj_set_size(box, 240, 432 - 90);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < s_n_decks; i++) {
        lv_obj_t *b = lv_button_create(box);
        lv_obj_set_size(b, 220, 44);
        lv_obj_set_style_bg_color(b, lv_color_hex(hb_color_surface()), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_margin_bottom(b, 6, 0);
        lv_obj_add_event_cb(b, on_deck_pick, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, s_decks[i].name);
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }
}

static void build_play(void)
{
    s_play_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_play_view, 240, 432);
    lv_obj_set_style_radius(s_play_view, 0, 0);
    lv_obj_set_pos(s_play_view, 0, 0);
    lv_obj_set_style_bg_color(s_play_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_play_view, 0, 0);
    lv_obj_set_style_pad_all(s_play_view, 0, 0);
    lv_obj_clear_flag(s_play_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_play_view, LV_OBJ_FLAG_HIDDEN);

    s_progress_lbl = lv_label_create(s_play_view);
    lv_label_set_text(s_progress_lbl, "0/0");
    lv_obj_set_style_text_color(s_progress_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_progress_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_progress_lbl, LV_ALIGN_TOP_RIGHT, -10, 10);

    s_q_lbl = lv_label_create(s_play_view);
    lv_label_set_text(s_q_lbl, "");
    lv_obj_set_style_text_color(s_q_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_q_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_q_lbl, 220);
    lv_label_set_long_mode(s_q_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_q_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_q_lbl, LV_ALIGN_TOP_MID, 0, 50);

    int y = 200;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(s_play_view);
        lv_obj_set_size(b, 220, 44);
        lv_obj_set_pos(b, 10, y);
        lv_obj_set_style_bg_color(b, lv_color_hex(hb_color_surface()), 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_event_cb(b, on_answer, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, "");
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_center(l);
        s_a_btns[i] = b;
        s_a_lbls[i] = l;
        y += 50;
    }
}

static void build_results(void)
{
    s_results_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_results_view, 240, 432);
    lv_obj_set_style_radius(s_results_view, 0, 0);
    lv_obj_set_pos(s_results_view, 0, 0);
    lv_obj_set_style_bg_color(s_results_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_results_view, 0, 0);
    lv_obj_set_style_pad_all(s_results_view, 0, 0);
    lv_obj_add_flag(s_results_view, LV_OBJ_FLAG_HIDDEN);

    s_results_lbl = lv_label_create(s_results_view);
    lv_label_set_text(s_results_lbl, "Results");
    lv_obj_set_style_text_color(s_results_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_results_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_results_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_results_lbl, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *btn = lv_button_create(s_results_view);
    lv_obj_set_size(btn, 200, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(btn, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, on_play_again, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, "Another deck");
    lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    hb_fs_mkdir(DECKS_DIR);
    load_all_decks();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_picker();
    build_play();
    build_results();
    show_picker();

}
