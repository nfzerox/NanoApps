/*
 * heads_up — charades-style guessing game with built-in decks.
 *
 * Hold the device to your forehead. Friends shout clues. Tilt the
 * device down (top falls forward, away from you) to mark CORRECT,
 * tilt up to PASS. Each round is 60 seconds; words come from the
 * currently-selected deck.
 *
 * Deck picker on launch; per-round score; results summary after the
 * timer runs out (correct vs passed list).
 *
 * Tilt direction: we read the accelerometer's z axis (screen normal).
 * Held against the forehead with the screen facing your friends, z ≈ 0.
 * Pitch the top toward the floor and z swings strongly negative
 * ("correct"); toward the ceiling and z swings strongly positive ("pass").
 * We use ABSOLUTE z (no per-round baseline) so the two directions can't
 * collapse to the same sign. Threshold + cooldown prevents bouncing.
 */

#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

#define ROUND_MS         60000u
/* Tilt mechanics for the forehead pose. The screen faces your friends, so
   the screen normal (z) is roughly horizontal → z ≈ 0 at rest. Pitch the
   top edge toward the FLOOR and the screen tips to face down → z strongly
   NEGATIVE → CORRECT; pitch toward the CEILING → z strongly POSITIVE → PASS.
   We read ABSOLUTE z (no captured baseline), so a tilt always lands on the
   correct side regardless of the exact hold; and we require a return to
   near-upright (|z| small) before the next trigger so a head-bob can't
   re-fire. */
#define TILT_Z_DELTA_MG  650       /* z swing from rest (raised: was over-sensitive) */
#define TILT_RESET_MG    350       /* |z| below this = near-upright, re-arm */
#define TILT_COOLDOWN_MS 900u

#define DATA_DIR     "/Data/HeadsUp"
#define DECKS_DIR    "/Data/HeadsUp/decks"
#define MAX_DECKS    24
#define MAX_DECK_WORDS 256

typedef struct {
    const char  *name;
    const char **words;
    int          n_words;
} deck_t;

/* Unified deck list (built-ins + custom). Built at startup. */
static deck_t s_all_decks[MAX_DECKS];
static int    s_n_all_decks = 0;

/* ---- decks (built-in, family-safe) ---- */

static const char *deck_animals[] = {
    "Elephant","Tiger","Penguin","Dolphin","Giraffe","Kangaroo",
    "Octopus","Eagle","Butterfly","Hedgehog","Chameleon","Cheetah",
    "Flamingo","Sloth","Walrus","Panda","Owl","Crocodile","Fox",
    "Koala","Lobster","Hummingbird","Hippopotamus","Raccoon",
};
static const char *deck_movies[] = {
    "Star Wars","Titanic","Jurassic Park","Frozen","Inception",
    "The Lion King","Toy Story","Avatar","The Matrix","Forrest Gump",
    "Harry Potter","Finding Nemo","Up","Ratatouille","Cars","Shrek",
    "Wall-E","Coco","Moana","Encanto","Interstellar","Gladiator",
    "Back to the Future","Indiana Jones",
};
static const char *deck_food[] = {
    "Pizza","Sushi","Burger","Tacos","Pasta","Ice Cream","Pancakes",
    "Donut","Salad","Soup","Sandwich","Curry","Ramen","Fries",
    "Chocolate","Apple","Banana","Strawberry","Cheese","Bread",
    "Cookie","Cake","Pie","Burrito","Dumplings",
};
static const char *deck_actions[] = {
    "Swimming","Dancing","Running","Sleeping","Cooking","Reading",
    "Painting","Singing","Driving","Climbing","Fishing","Skating",
    "Skiing","Yoga","Hiking","Surfing","Knitting","Gardening",
    "Baking","Juggling","Boxing","Cycling","Rowing","Jumping",
};
static const char *deck_objects[] = {
    "Umbrella","Telescope","Backpack","Lamp","Mirror","Camera",
    "Bicycle","Skateboard","Guitar","Piano","Drum","Pencil","Globe",
    "Clock","Hammer","Scissors","Toothbrush","Compass","Helmet",
    "Headphones","Microphone","Suitcase","Telescope","Anchor",
};

static const deck_t s_builtin_decks[] = {
    { "Animals",  deck_animals,  (int)(sizeof deck_animals  / sizeof *deck_animals)  },
    { "Movies",   deck_movies,   (int)(sizeof deck_movies   / sizeof *deck_movies)   },
    { "Food",     deck_food,     (int)(sizeof deck_food     / sizeof *deck_food)     },
    { "Actions",  deck_actions,  (int)(sizeof deck_actions  / sizeof *deck_actions)  },
    { "Objects",  deck_objects,  (int)(sizeof deck_objects  / sizeof *deck_objects)  },
};
#define N_BUILTIN  (int)(sizeof s_builtin_decks / sizeof s_builtin_decks[0])

/* Load one custom deck from `path` (a .txt file). The file content
   is read once into a heap blob; words[] points into that blob (after
   we replace every line terminator with NUL so each entry is a C
   string). Deck name is taken from the filename with the .txt suffix
   stripped. Returns 1 if a deck was added, 0 otherwise. */
static int load_custom_deck(const char *path, const char *filename)
{
    if (s_n_all_decks >= MAX_DECKS) return 0;
    uint32_t sz = hb_fs_size(path);
    if (sz == 0 || sz > 16 * 1024) return 0;     /* sanity cap */
    char *blob = lv_malloc(sz + 1);
    if (!blob) return 0;
    uint32_t rd = hb_fs_read(path, blob, sz);
    blob[rd < sz ? rd : sz] = 0;

    const char **words = lv_malloc(MAX_DECK_WORDS * sizeof(char *));
    if (!words) { lv_free(blob); return 0; }
    int n_words = 0;
    char *p = blob;
    while (*p && n_words < MAX_DECK_WORDS) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        words[n_words++] = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
    }
    if (n_words == 0) { lv_free(blob); lv_free(words); return 0; }

    /* Strip .txt from filename for the deck label. */
    int nl = 0; while (filename[nl]) nl++;
    int label_len = nl;
    if (label_len >= 4 && filename[label_len-4] == '.' &&
        filename[label_len-3] == 't' && filename[label_len-2] == 'x' &&
        filename[label_len-1] == 't') label_len -= 4;
    char *name = lv_malloc((uint32_t)label_len + 1);
    if (!name) { lv_free(blob); lv_free(words); return 0; }
    for (int i = 0; i < label_len; i++) name[i] = filename[i];
    name[label_len] = 0;

    s_all_decks[s_n_all_decks].name    = name;
    s_all_decks[s_n_all_decks].words   = words;
    s_all_decks[s_n_all_decks].n_words = n_words;
    s_n_all_decks++;
    return 1;
}

static bool ends_with_txt(const char *s)
{
    int n = 0; while (s[n]) n++;
    return n >= 5 && s[n-4]=='.' && s[n-3]=='t' && s[n-2]=='x' && s[n-1]=='t';
}

static void load_all_decks(void)
{
    /* Built-ins first so they stay at the top of the picker. */
    for (int i = 0; i < N_BUILTIN && s_n_all_decks < MAX_DECKS; i++) {
        s_all_decks[s_n_all_decks++] = s_builtin_decks[i];
    }
    /* Then custom decks from /Data/HeadsUp/decks/*.txt. */
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
        load_custom_deck(path, fn);
    }
    hb_fs_dir_close(&d);
}

/* ---- state ---- */

typedef enum { VIEW_PICKER, VIEW_PLAY, VIEW_RESULTS } view_t;
static view_t   s_view = VIEW_PICKER;
static int      s_deck_idx;
static int     *s_used_word_idx;     /* recently shown — avoid repeats */
static int      s_n_used;
static uint32_t s_round_start_ms;
static uint32_t s_last_tilt_ms;
static int      s_correct;
static int      s_passed;
static int      s_current_word;
static bool     s_armed;            /* true when ready for next tilt */

#define MAX_LOG 60
static const char *s_correct_log[MAX_LOG];
static int         s_n_correct_log = 0;
static const char *s_passed_log[MAX_LOG];
static int         s_n_passed_log = 0;

static lv_obj_t *s_picker_view;
static lv_obj_t *s_play_view;
static lv_obj_t *s_results_view;
static lv_obj_t *s_play_word_lbl;
static lv_obj_t *s_play_time_lbl;
static lv_obj_t *s_play_score_lbl;
static lv_obj_t *s_results_lbl;

/* PRNG */
static uint32_t s_rng;
static uint32_t rnd(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

/* Pick a word index not in the recent-used list. */
static int pick_word(void)
{
    const deck_t *d = &s_all_decks[s_deck_idx];
    if (d->n_words <= 0) return 0;
    for (int tries = 0; tries < 50; tries++) {
        int i = (int)(rnd() % (uint32_t)d->n_words);
        bool used = false;
        for (int j = 0; j < s_n_used; j++)
            if (s_used_word_idx[j] == i) { used = true; break; }
        if (!used) {
            if (s_n_used < d->n_words / 2) {
                s_used_word_idx[s_n_used++] = i;
            } else {
                /* shift up and append */
                for (int j = 0; j < s_n_used - 1; j++)
                    s_used_word_idx[j] = s_used_word_idx[j+1];
                s_used_word_idx[s_n_used - 1] = i;
            }
            return i;
        }
    }
    return (int)(rnd() % (uint32_t)d->n_words);
}

static void show_picker(void);
static void show_play(void);
static void show_results(void);

static void on_deck_pick(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n_all_decks) return;
    s_deck_idx = idx;
    s_n_used = 0;
    if (s_used_word_idx) lv_free(s_used_word_idx);
    s_used_word_idx = lv_malloc((uint32_t)(s_all_decks[idx].n_words * sizeof(int)));
    if (!s_used_word_idx) return;
    s_correct = 0; s_passed = 0;
    s_n_correct_log = 0; s_n_passed_log = 0;
    s_round_start_ms = hb_time_uptime_ms();
    s_last_tilt_ms = s_round_start_ms;
    s_current_word = pick_word();
    s_armed = true;
    show_play();
}

static void on_play_again(lv_event_t *e)
{
    (void)e;
    show_picker();
}

static void show_picker(void)
{
    s_view = VIEW_PICKER;
    lv_obj_clear_flag(s_picker_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_view,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_results_view, LV_OBJ_FLAG_HIDDEN);
}

static void show_play(void)
{
    s_view = VIEW_PLAY;
    lv_obj_add_flag(s_picker_view,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_play_view,   LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_results_view,  LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_play_word_lbl,
        s_all_decks[s_deck_idx].words[s_current_word]);
}

static void show_results(void)
{
    s_view = VIEW_RESULTS;
    lv_obj_add_flag(s_picker_view,    LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_play_view,      LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_results_view, LV_OBJ_FLAG_HIDDEN);

    /* Build a multi-line summary. */
    static char buf[1024]; int k = 0;
    char nb[12];
    const char *p = "Time's up!\nCorrect: "; while (*p) buf[k++] = *p++;
    itoa_u((uint32_t)s_correct, nb); for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    p = "  Passed: "; while (*p) buf[k++] = *p++;
    itoa_u((uint32_t)s_passed, nb); for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k++] = '\n'; buf[k++] = '\n';
    p = "Got: "; while (*p) buf[k++] = *p++;
    for (int i = 0; i < s_n_correct_log && k < (int)sizeof buf - 32; i++) {
        if (i) { buf[k++] = ','; buf[k++] = ' '; }
        const char *w = s_correct_log[i];
        while (*w && k < (int)sizeof buf - 16) buf[k++] = *w++;
    }
    if (s_n_passed_log > 0) {
        buf[k++] = '\n'; buf[k++] = '\n';
        p = "Passed: "; while (*p) buf[k++] = *p++;
        for (int i = 0; i < s_n_passed_log && k < (int)sizeof buf - 32; i++) {
            if (i) { buf[k++] = ','; buf[k++] = ' '; }
            const char *w = s_passed_log[i];
            while (*w && k < (int)sizeof buf - 16) buf[k++] = *w++;
        }
    }
    buf[k] = 0;
    lv_label_set_text(s_results_lbl, buf);
}

static void register_correct(void)
{
    if (s_n_correct_log < MAX_LOG)
        s_correct_log[s_n_correct_log++] = s_all_decks[s_deck_idx].words[s_current_word];
    s_correct++;
    s_current_word = pick_word();
    lv_label_set_text(s_play_word_lbl,
        s_all_decks[s_deck_idx].words[s_current_word]);
    lv_obj_set_style_bg_color(s_play_view, lv_color_hex(hb_color_success()), 0);
}

static void register_pass(void)
{
    if (s_n_passed_log < MAX_LOG)
        s_passed_log[s_n_passed_log++] = s_all_decks[s_deck_idx].words[s_current_word];
    s_passed++;
    s_current_word = pick_word();
    lv_label_set_text(s_play_word_lbl,
        s_all_decks[s_deck_idx].words[s_current_word]);
    lv_obj_set_style_bg_color(s_play_view, lv_color_hex(hb_color_danger()), 0);
}

static void on_frame(void)
{
    if (s_view != VIEW_PLAY) return;

    /* Time */
    uint32_t now = hb_time_uptime_ms();
    uint32_t elapsed = (uint32_t)(now - s_round_start_ms);
    if (elapsed >= ROUND_MS) {
        show_results();
        return;
    }
    uint32_t left = (ROUND_MS - elapsed) / 1000;
    char buf[12]; itoa_u(left, buf);
    int k = 0; while (buf[k]) k++;
    /* zero-pad to 2 digits */
    char out[12]; int oi = 0;
    if (k < 2) out[oi++] = '0';
    for (int i = 0; i < k; i++) out[oi++] = buf[i];
    out[oi++] = 's'; out[oi] = 0;
    lv_label_set_text(s_play_time_lbl, out);

    int k2 = 0;
    char sc[24]; const char *p = "Score: "; while (*p) sc[k2++] = *p++;
    itoa_u((uint32_t)s_correct, buf); for (int i = 0; buf[i]; i++) sc[k2++] = buf[i];
    sc[k2] = 0;
    lv_label_set_text(s_play_score_lbl, sc);

    /* Tilt detection (vertical-hold pose).
       Compare current z to the baseline captured at round-start. A
       large positive swing (top tipping AWAY from the player, screen
       toward floor) = CORRECT; a large negative swing (top tipping
       TOWARD the player, screen toward ceiling) = PASS. We arm/disarm
       so a single tilt fires exactly once: each trigger sets armed
       = false; we re-arm only after the device has returned to within
       TILT_RESET_MG of the baseline. */
    if ((uint32_t)(now - s_last_tilt_ms) > 350) {
        /* Brief background flash already faded; restore neutral. */
        lv_obj_set_style_bg_color(s_play_view, lv_color_hex(0x111522), 0);
    }
    int32_t a[3]; hb_accel_read_milli_g(a);
    int z = a[2];                       /* screen-normal pitch (≈0 held screen-out) */
    int az = z < 0 ? -z : z;

    if (s_armed && (uint32_t)(now - s_last_tilt_ms) >= TILT_COOLDOWN_MS) {
        /* Top toward the floor: screen tips face-down → z strongly negative → CORRECT.
           Top toward the ceiling: screen face-up → z strongly positive → PASS. */
        if (z < -TILT_Z_DELTA_MG) {
            register_correct();
            s_armed = false;
            s_last_tilt_ms = now;
        } else if (z > TILT_Z_DELTA_MG) {
            register_pass();
            s_armed = false;
            s_last_tilt_ms = now;
        }
    } else if (!s_armed && az < TILT_RESET_MG) {
        /* Back near upright (screen vertical) — ready for the next tilt. */
        s_armed = true;
    }
}

/* ---- view construction ---- */

static void build_picker(lv_obj_t *scr)
{
    s_picker_view = lv_obj_create(scr);
    lv_obj_set_size(s_picker_view, 240, 432);
    lv_obj_set_style_radius(s_picker_view, 0, 0);
    lv_obj_set_pos(s_picker_view, 0, 0);
    lv_obj_set_style_bg_color(s_picker_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_picker_view, 0, 0);
    lv_obj_set_style_pad_all(s_picker_view, 0, 0);
    lv_obj_clear_flag(s_picker_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_picker_view);
    lv_label_set_text(title, "Heads Up!");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    lv_obj_t *sub = lv_label_create(s_picker_view);
    lv_label_set_text(sub,
        "Pick a deck.\nHold to forehead.\nTilt DOWN = correct\nTilt UP = pass");
    lv_obj_set_style_text_color(sub, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 64);

    /* Deck buttons inside a scrollable container so the picker still
       fits even when the user has dropped a bunch of custom decks
       in /Data/HeadsUp/decks/. */
    lv_obj_t *box = lv_obj_create(s_picker_view);
    lv_obj_set_size(box, 240, 432 - 160);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 4, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);   /* centre the deck buttons */
    for (int i = 0; i < s_n_all_decks; i++) {
        lv_obj_t *b = lv_button_create(box);
        lv_obj_set_size(b, 200, 40);
        lv_obj_set_style_bg_color(b, lv_color_hex(hb_color_surface()), 0);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_margin_bottom(b, 6, 0);
        lv_obj_add_event_cb(b, on_deck_pick, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, s_all_decks[i].name);
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_center(l);
    }
}

static void build_play(lv_obj_t *scr)
{
    s_play_view = lv_obj_create(scr);
    lv_obj_set_size(s_play_view, 240, 432);
    lv_obj_set_style_radius(s_play_view, 0, 0);
    lv_obj_set_pos(s_play_view, 0, 0);
    lv_obj_set_style_bg_color(s_play_view, lv_color_hex(0x111522), 0);
    lv_obj_set_style_border_width(s_play_view, 0, 0);
    lv_obj_set_style_pad_all(s_play_view, 0, 0);
    lv_obj_clear_flag(s_play_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_play_view, LV_OBJ_FLAG_HIDDEN);

    s_play_time_lbl = lv_label_create(s_play_view);
    lv_label_set_text(s_play_time_lbl, "60s");
    lv_obj_set_style_text_color(s_play_time_lbl, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_play_time_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(s_play_time_lbl, LV_ALIGN_TOP_LEFT, 14, 14);

    s_play_score_lbl = lv_label_create(s_play_view);
    lv_label_set_text(s_play_score_lbl, "Score: 0");
    lv_obj_set_style_text_color(s_play_score_lbl, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_play_score_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_play_score_lbl, LV_ALIGN_TOP_RIGHT, -14, 18);

    s_play_word_lbl = lv_label_create(s_play_view);
    lv_label_set_text(s_play_word_lbl, "—");
    lv_obj_set_style_text_color(s_play_word_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_play_word_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_width(s_play_word_lbl, 224);
    lv_label_set_long_mode(s_play_word_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_play_word_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_play_word_lbl);
}

static void build_results(lv_obj_t *scr)
{
    s_results_view = lv_obj_create(scr);
    lv_obj_set_size(s_results_view, 240, 432);
    lv_obj_set_style_radius(s_results_view, 0, 0);
    lv_obj_set_pos(s_results_view, 0, 0);
    lv_obj_set_style_bg_color(s_results_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_results_view, 0, 0);
    lv_obj_set_style_pad_all(s_results_view, 8, 0);
    lv_obj_add_flag(s_results_view, LV_OBJ_FLAG_HIDDEN);

    s_results_lbl = lv_label_create(s_results_view);
    lv_label_set_text(s_results_lbl, "Results");
    lv_obj_set_style_text_color(s_results_lbl, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_results_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_results_lbl, 220);
    lv_label_set_long_mode(s_results_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_results_lbl, LV_ALIGN_TOP_LEFT, 4, 8);

    lv_obj_t *btn = lv_button_create(s_results_view);
    lv_obj_set_size(btn, 200, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(hb_color_success()), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, on_play_again, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, "Play again");
    lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    s_rng = hb_time_uptime_us() | 1u;

    /* Built-in decks first, then any custom decks from disk. */
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    hb_fs_mkdir(DECKS_DIR);
    load_all_decks();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_picker(scr);
    build_play(scr);
    build_results(scr);

    show_picker();
    hb_lv_set_frame_cb(on_frame);
}
