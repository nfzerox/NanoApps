/*
 * dictionary — English word definitions, T9-driven lookup.
 *
 * v1 ships a tiny built-in dictionary (~60 common entries) so the
 * app is useful out of the box. The user can drop additional
 * dictionaries at /Data/Dictionary/*.txt — same format as the
 * embedded blob (one entry per line: "word\tdefinition") and the
 * lookups merge across all loaded sources.
 *
 * Search view: T9 keyboard for typing; results list shows matching
 * entries as soon as the prefix is unambiguous. Tap a result to
 * see its full definition.
 *
 * No serious dictionary (full Webster's etc.) without much larger
 * datasets — those should be dropped in as additional .txt files
 * since the binary itself stays small.
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "hb_t9.h"
#include "lvgl/lvgl.h"

#define MAX_ENTRIES   2000
#define MAX_WORD_LEN  48
#define MAX_DEF_LEN   256
#define DATA_DIR      "/Data/Dictionary"

typedef struct {
    const char *word;
    const char *def;
} entry_t;

/* Built-in starter dictionary. */
static const char *s_builtin =
    "abandon\tto give up completely; to leave behind\n"
    "abide\tto accept or endure; to remain or stay\n"
    "agile\tquick, light, and graceful in movement\n"
    "amend\tto change for the better; to revise\n"
    "anchor\ta heavy device that secures a vessel to the bottom of a body of water\n"
    "apex\tthe highest point; the summit\n"
    "ardent\tenthusiastic or passionate\n"
    "arid\textremely dry, especially of land or climate\n"
    "axiom\ta statement accepted as true without proof\n"
    "barren\tunable to produce vegetation, crops, or offspring\n"
    "beguile\tto charm or enchant; to mislead by trickery\n"
    "blithe\tcheerful and carefree\n"
    "candid\ttruthful and straightforward; frank\n"
    "celestial\trelating to the sky or outer space\n"
    "concise\tgiving information clearly and in few words\n"
    "decipher\tto figure out the meaning of something\n"
    "diligent\thaving or showing care and effort in work or duties\n"
    "dormant\tnot active or growing but able to become active\n"
    "eclipse\tan obscuring of the light from one celestial body by another\n"
    "elated\tvery happy and excited\n"
    "elucidate\tto make something clear; explain\n"
    "ephemeral\tlasting for a very short time\n"
    "epitome\ta perfect example of a particular quality or type\n"
    "fathom\tto understand after much thought; six feet\n"
    "fervent\thaving or displaying passionate intensity\n"
    "frugal\tsparing or economical with money or food\n"
    "gallant\tbrave and noble; chivalrous\n"
    "gleam\tto shine brightly with a soft light\n"
    "harbor\ta place on the coast where vessels may find shelter\n"
    "humble\thaving or showing a modest estimate of one's own importance\n"
    "ignite\tto catch fire or cause to catch fire\n"
    "indelible\tnot able to be removed or forgotten\n"
    "intrepid\tfearless and adventurous\n"
    "jovial\tcheerful and friendly\n"
    "kindle\tto start a fire or to arouse a feeling\n"
    "labyrinth\ta complicated network of paths; a maze\n"
    "lucid\texpressed clearly; easy to understand\n"
    "luminous\tgiving off light; shining\n"
    "myriad\ta countless or extremely great number\n"
    "nascent\tjust coming into existence and beginning to develop\n"
    "novice\ta person new to a field or activity\n"
    "obscure\tnot discovered or known about; not clear\n"
    "opaque\tnot able to be seen through; not transparent\n"
    "ornate\telaborately or highly decorated\n"
    "palpable\table to be touched or felt; very obvious\n"
    "paramount\tmore important than anything else; supreme\n"
    "pristine\tin its original condition; unspoiled\n"
    "quaint\tattractively unusual or old-fashioned\n"
    "quench\tto satisfy thirst by drinking; to extinguish\n"
    "raucous\tmaking or constituting a disturbingly harsh and loud noise\n"
    "rectify\tto put right; correct\n"
    "resilient\table to recover quickly from difficulties\n"
    "salient\tmost noticeable or important; prominent\n"
    "savor\tto taste good food slowly to enjoy it fully\n"
    "scrutiny\tcritical observation or examination\n"
    "serene\tcalm, peaceful, and untroubled\n"
    "stoic\tunaffected by joy, grief, pleasure, or pain\n"
    "tangible\tperceptible by touch; clear and definite\n"
    "trivial\tof little value or importance\n"
    "ubiquitous\tpresent, appearing, or found everywhere\n"
    "undulate\tto move with a smooth wavelike motion\n"
    "vivid\tproducing powerful feelings or strong, clear images\n"
    "wane\tto decrease in vigor, power, or extent\n"
    "whimsical\tplayfully quaint or fanciful\n"
    "yearn\tto have an intense feeling of longing for something\n"
    "zealous\thaving or showing great energy or enthusiasm\n";

static entry_t s_entries[MAX_ENTRIES];
static int     s_n_entries = 0;
static char   *s_blob_builtin = NULL;
static char   *s_blob_custom  = NULL;

static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int s_starts_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s++, b = *prefix++;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (!a || a != b) return 0;
    }
    return 1;
}

/* Parse a "word\tdef\n" blob into entries. The blob is modified in
   place: tabs and newlines become NULs so each (word, def) pair is
   a pointer pair into the same buffer. */
static int parse_blob(char *blob)
{
    int added = 0;
    char *p = blob;
    while (*p && s_n_entries < MAX_ENTRIES) {
        while (*p == '\r' || *p == '\n' || *p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *word = p;
        while (*p && *p != '\t' && *p != '\n' && *p != '\r') p++;
        if (*p != '\t') {
            while (*p && *p != '\n' && *p != '\r') p++;
            if (*p) p++;
            continue;
        }
        *p = 0; p++;
        char *def = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        if (*p) { *p = 0; p++; }
        s_entries[s_n_entries].word = word;
        s_entries[s_n_entries].def = def;
        s_n_entries++;
        added++;
    }
    return added;
}

static bool ends_with_txt(const char *s)
{
    int n = s_len(s);
    return n >= 5 && s[n-4]=='.' && s[n-3]=='t' && s[n-2]=='x' && s[n-1]=='t';
}

static void load_custom(void)
{
    hb_dir_t d;
    if (!hb_fs_dir_open(&d, DATA_DIR, false)) return;
    char fn[64]; bool is_dir = false;
    while (hb_fs_dir_next(&d, fn, sizeof fn, &is_dir)) {
        if (is_dir || !ends_with_txt(fn)) continue;
        char path[256]; int pk = 0;
        const char *p = DATA_DIR "/"; while (*p) path[pk++] = *p++;
        for (int i = 0; fn[i] && pk < 254; i++) path[pk++] = fn[i];
        path[pk] = 0;
        uint32_t sz = hb_fs_size(path);
        if (sz == 0 || sz > 256 * 1024) continue;
        char *blob = lv_malloc(sz + 1);
        if (!blob) continue;
        uint32_t rd = hb_fs_read(path, blob, sz);
        blob[rd < sz ? rd : sz] = 0;
        int prev = s_n_entries;
        int added = parse_blob(blob);
        if (added == 0) lv_free(blob);
        /* If anything was added we keep the first blob alive in
           s_blob_custom — subsequent blob leaks here are intentional
           (process exits clean). */
        if (added > 0 && !s_blob_custom) s_blob_custom = blob;
        (void)prev;
    }
    hb_fs_dir_close(&d);
}

static void load_all(void)
{
    s_n_entries = 0;
    /* Duplicate the builtin blob (it's `const char *`, but parse_blob
       mutates the buffer). */
    int n = 0; while (s_builtin[n]) n++;
    s_blob_builtin = lv_malloc((uint32_t)n + 1);
    if (s_blob_builtin) {
        for (int i = 0; i <= n; i++) s_blob_builtin[i] = s_builtin[i];
        parse_blob(s_blob_builtin);
    }
    load_custom();
}

/* ---- views ---- */

static lv_obj_t *s_scr;
static lv_obj_t *s_search_view;
static lv_obj_t *s_def_view;
static lv_obj_t *s_results_box;
static lv_obj_t *s_search_ta;
static lv_obj_t *s_d_word;
static lv_obj_t *s_d_def;

static int s_results[20];
static int s_n_results = 0;

static void render_results(void)
{
    lv_obj_clean(s_results_box);
    const char *q = lv_textarea_get_text(s_search_ta);
    int qlen = 0; while (q[qlen]) qlen++;
    s_n_results = 0;
    if (qlen == 0) {
        lv_obj_t *l = lv_label_create(s_results_box);
        lv_label_set_text(l, "Type to search");
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    for (int i = 0; i < s_n_entries && s_n_results < (int)(sizeof s_results / sizeof s_results[0]); i++) {
        if (s_starts_ci(s_entries[i].word, q)) {
            s_results[s_n_results++] = i;
        }
    }
    if (s_n_results == 0) {
        lv_obj_t *l = lv_label_create(s_results_box);
        lv_label_set_text(l, "No match");
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text_dim()), 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    for (int i = 0; i < s_n_results; i++) {
        int idx = s_results[i];
        lv_obj_t *r = lv_button_create(s_results_box);
        lv_obj_set_size(r, 220, 28);
        lv_obj_set_style_bg_color(r, lv_color_hex(hb_color_surface()), 0);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_margin_bottom(r, 2, 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_set_user_data(r, (void *)(uintptr_t)idx);
        lv_obj_add_event_cb(r, (lv_event_cb_t)NULL, LV_EVENT_CLICKED, NULL);
        /* Wire a real handler via plain event — done below in build. */
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, s_entries[idx].word);
        lv_obj_set_style_text_color(l, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
    }
}

static void open_def(int idx);

static void on_result_clicked(lv_event_t *e)
{
    lv_obj_t *r = (lv_obj_t *)lv_event_get_target(e);
    int idx = (int)(uintptr_t)lv_obj_get_user_data(r);
    open_def(idx);
}

/* Re-wire result rows' click handlers after render. */
static void rewire_results(void)
{
    /* Find each child of s_results_box that is a button, attach the
       click handler. Doing this once after render keeps render_results
       above simple. */
    uint32_t n = lv_obj_get_child_count(s_results_box);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(s_results_box, i);
        /* Replace any prior handler with our real one. */
        lv_obj_remove_event_cb(c, NULL);
        lv_obj_add_event_cb(c, on_result_clicked, LV_EVENT_CLICKED, NULL);
    }
}

static void on_search_changed(lv_event_t *e)
{
    (void)e;
    render_results();
    rewire_results();
}

static void open_def(int idx)
{
    if (idx < 0 || idx >= s_n_entries) return;
    lv_label_set_text(s_d_word, s_entries[idx].word);
    lv_label_set_text(s_d_def, s_entries[idx].def);
    lv_obj_add_flag(s_search_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_def_view, LV_OBJ_FLAG_HIDDEN);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_search_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_def_view, LV_OBJ_FLAG_HIDDEN);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();
    hb_fs_mkdir("/Data");
    hb_fs_mkdir(DATA_DIR);
    load_all();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    /* Search view */
    s_search_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_search_view, 240, 432);
    lv_obj_set_style_radius(s_search_view, 0, 0);
    lv_obj_set_style_bg_color(s_search_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_search_view, 0, 0);
    lv_obj_set_style_pad_all(s_search_view, 0, 0);
    lv_obj_clear_flag(s_search_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_label_create(s_search_view);
    lv_label_set_text(hdr, "Dictionary");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_LEFT, 10, 6);

    s_search_ta = lv_textarea_create(s_search_view);
    lv_obj_set_size(s_search_ta, 240, 36);
    lv_obj_align(s_search_ta, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_search_ta, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_text_color(s_search_ta, lv_color_hex(hb_color_text()), 0);
    lv_textarea_set_one_line(s_search_ta, true);
    lv_obj_add_event_cb(s_search_ta, on_search_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_results_box = lv_obj_create(s_search_view);
    /* Start below the search field (ends ~y=72) with a clear gap so the list
       background doesn't cover the bottom of the field. */
    lv_obj_set_size(s_results_box, 240, 112);
    lv_obj_align(s_results_box, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_color(s_results_box, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_results_box, 0, 0);
    lv_obj_set_style_pad_all(s_results_box, 6, 0);
    lv_obj_set_flex_flow(s_results_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *kb_box = lv_obj_create(s_search_view);
    lv_obj_set_size(kb_box, 240, 232);
    lv_obj_align(kb_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(kb_box, lv_color_hex(hb_color_surface()), 0);
    lv_obj_set_style_border_width(kb_box, 0, 0);
    lv_obj_set_style_pad_all(kb_box, 0, 0);
    lv_obj_clear_flag(kb_box, LV_OBJ_FLAG_SCROLLABLE);
    hb_t9_create(kb_box, s_search_ta);

    /* Definition view */
    s_def_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_def_view, 240, 432);
    lv_obj_set_style_radius(s_def_view, 0, 0);
    lv_obj_set_style_bg_color(s_def_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_def_view, 0, 0);
    lv_obj_set_style_pad_all(s_def_view, 0, 0);
    lv_obj_clear_flag(s_def_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_def_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_def_view);
    lv_obj_set_size(back, 56, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(bl, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(bl);

    s_d_word = lv_label_create(s_def_view);
    lv_label_set_text(s_d_word, "");
    lv_obj_set_style_text_color(s_d_word, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_d_word, &lv_font_montserrat_24, 0);
    lv_obj_align(s_d_word, LV_ALIGN_TOP_MID, 0, 50);

    s_d_def = lv_label_create(s_def_view);
    lv_label_set_text(s_d_def, "");
    lv_obj_set_style_text_color(s_d_def, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_d_def, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_d_def, 220);
    lv_label_set_long_mode(s_d_def, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_d_def, LV_ALIGN_TOP_MID, 0, 100);

    render_results();
    rewire_results();

}
