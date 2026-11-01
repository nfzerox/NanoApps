/*
 * periodic_table — interactive 118-element periodic table.
 *
 * Originally rendered with one lv_button per element + one lv_label
 * each — ~250 widgets total — which blew the 64 KB LVGL TLSF heap and
 * panicked the device. Rewritten to draw the entire table into a
 * single canvas with direct XRGB8888 buffer writes; tap is hit-tested
 * by grid math from the touch's local (x, y). One canvas widget for
 * the whole table, no per-element children.
 *
 * Element symbols are drawn via lv_canvas_draw_text into the canvas
 * buffer in black on the category-colored cell. The detail view is
 * a separate screen (still uses widgets — only one element shown).
 */
#include "hb_sdk.h"
#include "hb_prefs.h"
#include "lvgl/lvgl.h"

typedef enum {
    CAT_ALKALI = 0, CAT_ALKEARTH, CAT_TRANSITION, CAT_POST,
    CAT_METALLOID, CAT_NONMETAL, CAT_HALOGEN, CAT_NOBLE,
    CAT_LANTH, CAT_ACTIN, CAT_UNKNOWN, CAT_COUNT
} category_t;

static const uint32_t s_cat_color[CAT_COUNT] = {
    0xff6b6b, 0xffa94d, 0xffd43b, 0x868e96,
    0x4ecdc4, 0x2bb673, 0x4ea8de, 0xa663cc,
    0xeb6383, 0xf08080, 0x6b7280,
};
static const char *s_cat_name[CAT_COUNT] = {
    "Alkali metal","Alkaline earth","Transition metal","Post-transition",
    "Metalloid","Nonmetal","Halogen","Noble gas","Lanthanide","Actinide","Unknown",
};

typedef struct {
    const char *sym;
    const char *name;
    short z;
    short weight_x10;
    char  period;       /* 1..7, 8=lanth row, 9=actin row */
    char  group;        /* 1..18 */
    char  cat;
} elem_t;

static const elem_t s_elems[] = {
    {"H",  "Hydrogen",     1,   10, 1, 1,  CAT_NONMETAL},
    {"He", "Helium",       2,   40, 1, 18, CAT_NOBLE},
    {"Li", "Lithium",      3,   69, 2, 1,  CAT_ALKALI},
    {"Be", "Beryllium",    4,   90, 2, 2,  CAT_ALKEARTH},
    {"B",  "Boron",        5,  108, 2, 13, CAT_METALLOID},
    {"C",  "Carbon",       6,  120, 2, 14, CAT_NONMETAL},
    {"N",  "Nitrogen",     7,  140, 2, 15, CAT_NONMETAL},
    {"O",  "Oxygen",       8,  159, 2, 16, CAT_NONMETAL},
    {"F",  "Fluorine",     9,  189, 2, 17, CAT_HALOGEN},
    {"Ne", "Neon",        10,  201, 2, 18, CAT_NOBLE},
    {"Na", "Sodium",      11,  229, 3, 1,  CAT_ALKALI},
    {"Mg", "Magnesium",   12,  243, 3, 2,  CAT_ALKEARTH},
    {"Al", "Aluminum",    13,  269, 3, 13, CAT_POST},
    {"Si", "Silicon",     14,  280, 3, 14, CAT_METALLOID},
    {"P",  "Phosphorus",  15,  309, 3, 15, CAT_NONMETAL},
    {"S",  "Sulfur",      16,  320, 3, 16, CAT_NONMETAL},
    {"Cl", "Chlorine",    17,  354, 3, 17, CAT_HALOGEN},
    {"Ar", "Argon",       18,  399, 3, 18, CAT_NOBLE},
    {"K",  "Potassium",   19,  391, 4, 1,  CAT_ALKALI},
    {"Ca", "Calcium",     20,  400, 4, 2,  CAT_ALKEARTH},
    {"Sc", "Scandium",    21,  449, 4, 3,  CAT_TRANSITION},
    {"Ti", "Titanium",    22,  478, 4, 4,  CAT_TRANSITION},
    {"V",  "Vanadium",    23,  509, 4, 5,  CAT_TRANSITION},
    {"Cr", "Chromium",    24,  519, 4, 6,  CAT_TRANSITION},
    {"Mn", "Manganese",   25,  549, 4, 7,  CAT_TRANSITION},
    {"Fe", "Iron",        26,  558, 4, 8,  CAT_TRANSITION},
    {"Co", "Cobalt",      27,  589, 4, 9,  CAT_TRANSITION},
    {"Ni", "Nickel",      28,  586, 4, 10, CAT_TRANSITION},
    {"Cu", "Copper",      29,  635, 4, 11, CAT_TRANSITION},
    {"Zn", "Zinc",        30,  653, 4, 12, CAT_TRANSITION},
    {"Ga", "Gallium",     31,  697, 4, 13, CAT_POST},
    {"Ge", "Germanium",   32,  726, 4, 14, CAT_METALLOID},
    {"As", "Arsenic",     33,  749, 4, 15, CAT_METALLOID},
    {"Se", "Selenium",    34,  789, 4, 16, CAT_NONMETAL},
    {"Br", "Bromine",     35,  799, 4, 17, CAT_HALOGEN},
    {"Kr", "Krypton",     36,  838, 4, 18, CAT_NOBLE},
    {"Rb", "Rubidium",    37,  854, 5, 1,  CAT_ALKALI},
    {"Sr", "Strontium",   38,  876, 5, 2,  CAT_ALKEARTH},
    {"Y",  "Yttrium",     39,  889, 5, 3,  CAT_TRANSITION},
    {"Zr", "Zirconium",   40,  912, 5, 4,  CAT_TRANSITION},
    {"Nb", "Niobium",     41,  929, 5, 5,  CAT_TRANSITION},
    {"Mo", "Molybdenum",  42,  959, 5, 6,  CAT_TRANSITION},
    {"Tc", "Technetium",  43,  980, 5, 7,  CAT_TRANSITION},
    {"Ru", "Ruthenium",   44, 1010, 5, 8,  CAT_TRANSITION},
    {"Rh", "Rhodium",     45, 1029, 5, 9,  CAT_TRANSITION},
    {"Pd", "Palladium",   46, 1064, 5, 10, CAT_TRANSITION},
    {"Ag", "Silver",      47, 1078, 5, 11, CAT_TRANSITION},
    {"Cd", "Cadmium",     48, 1124, 5, 12, CAT_TRANSITION},
    {"In", "Indium",      49, 1148, 5, 13, CAT_POST},
    {"Sn", "Tin",         50, 1187, 5, 14, CAT_POST},
    {"Sb", "Antimony",    51, 1217, 5, 15, CAT_METALLOID},
    {"Te", "Tellurium",   52, 1276, 5, 16, CAT_METALLOID},
    {"I",  "Iodine",      53, 1268, 5, 17, CAT_HALOGEN},
    {"Xe", "Xenon",       54, 1313, 5, 18, CAT_NOBLE},
    {"Cs", "Cesium",      55, 1329, 6, 1,  CAT_ALKALI},
    {"Ba", "Barium",      56, 1373, 6, 2,  CAT_ALKEARTH},
    {"La", "Lanthanum",   57, 1389, 8, 3,  CAT_LANTH},
    {"Ce", "Cerium",      58, 1401, 8, 4,  CAT_LANTH},
    {"Pr", "Praseodymium",59, 1409, 8, 5,  CAT_LANTH},
    {"Nd", "Neodymium",   60, 1442, 8, 6,  CAT_LANTH},
    {"Pm", "Promethium",  61, 1450, 8, 7,  CAT_LANTH},
    {"Sm", "Samarium",    62, 1504, 8, 8,  CAT_LANTH},
    {"Eu", "Europium",    63, 1520, 8, 9,  CAT_LANTH},
    {"Gd", "Gadolinium",  64, 1573, 8, 10, CAT_LANTH},
    {"Tb", "Terbium",     65, 1589, 8, 11, CAT_LANTH},
    {"Dy", "Dysprosium",  66, 1625, 8, 12, CAT_LANTH},
    {"Ho", "Holmium",     67, 1649, 8, 13, CAT_LANTH},
    {"Er", "Erbium",      68, 1673, 8, 14, CAT_LANTH},
    {"Tm", "Thulium",     69, 1689, 8, 15, CAT_LANTH},
    {"Yb", "Ytterbium",   70, 1730, 8, 16, CAT_LANTH},
    {"Lu", "Lutetium",    71, 1750, 8, 17, CAT_LANTH},
    {"Hf", "Hafnium",     72, 1785, 6, 4,  CAT_TRANSITION},
    {"Ta", "Tantalum",    73, 1809, 6, 5,  CAT_TRANSITION},
    {"W",  "Tungsten",    74, 1838, 6, 6,  CAT_TRANSITION},
    {"Re", "Rhenium",     75, 1862, 6, 7,  CAT_TRANSITION},
    {"Os", "Osmium",      76, 1902, 6, 8,  CAT_TRANSITION},
    {"Ir", "Iridium",     77, 1922, 6, 9,  CAT_TRANSITION},
    {"Pt", "Platinum",    78, 1950, 6, 10, CAT_TRANSITION},
    {"Au", "Gold",        79, 1969, 6, 11, CAT_TRANSITION},
    {"Hg", "Mercury",     80, 2006, 6, 12, CAT_TRANSITION},
    {"Tl", "Thallium",    81, 2043, 6, 13, CAT_POST},
    {"Pb", "Lead",        82, 2072, 6, 14, CAT_POST},
    {"Bi", "Bismuth",     83, 2089, 6, 15, CAT_POST},
    {"Po", "Polonium",    84, 2090, 6, 16, CAT_POST},
    {"At", "Astatine",    85, 2100, 6, 17, CAT_HALOGEN},
    {"Rn", "Radon",       86, 2220, 6, 18, CAT_NOBLE},
    {"Fr", "Francium",    87, 2230, 7, 1,  CAT_ALKALI},
    {"Ra", "Radium",      88, 2260, 7, 2,  CAT_ALKEARTH},
    {"Ac", "Actinium",    89, 2270, 9, 3,  CAT_ACTIN},
    {"Th", "Thorium",     90, 2320, 9, 4,  CAT_ACTIN},
    {"Pa", "Protactinium",91, 2310, 9, 5,  CAT_ACTIN},
    {"U",  "Uranium",     92, 2380, 9, 6,  CAT_ACTIN},
    {"Np", "Neptunium",   93, 2370, 9, 7,  CAT_ACTIN},
    {"Pu", "Plutonium",   94, 2440, 9, 8,  CAT_ACTIN},
    {"Am", "Americium",   95, 2430, 9, 9,  CAT_ACTIN},
    {"Cm", "Curium",      96, 2470, 9, 10, CAT_ACTIN},
    {"Bk", "Berkelium",   97, 2470, 9, 11, CAT_ACTIN},
    {"Cf", "Californium", 98, 2510, 9, 12, CAT_ACTIN},
    {"Es", "Einsteinium", 99, 2520, 9, 13, CAT_ACTIN},
    {"Fm", "Fermium",    100, 2570, 9, 14, CAT_ACTIN},
    {"Md", "Mendelevium",101, 2580, 9, 15, CAT_ACTIN},
    {"No", "Nobelium",   102, 2590, 9, 16, CAT_ACTIN},
    {"Lr", "Lawrencium", 103, 2660, 9, 17, CAT_ACTIN},
    {"Rf", "Rutherfordium",104,2670,7, 4,  CAT_TRANSITION},
    {"Db", "Dubnium",    105, 2680, 7, 5,  CAT_TRANSITION},
    {"Sg", "Seaborgium", 106, 2690, 7, 6,  CAT_TRANSITION},
    {"Bh", "Bohrium",    107, 2700, 7, 7,  CAT_TRANSITION},
    {"Hs", "Hassium",    108, 2770, 7, 8,  CAT_TRANSITION},
    {"Mt", "Meitnerium", 109, 2780, 7, 9,  CAT_UNKNOWN},
    {"Ds", "Darmstadtium",110,2810, 7, 10, CAT_UNKNOWN},
    {"Rg", "Roentgenium",111, 2820, 7, 11, CAT_UNKNOWN},
    {"Cn", "Copernicium",112, 2850, 7, 12, CAT_TRANSITION},
    {"Nh", "Nihonium",   113, 2860, 7, 13, CAT_UNKNOWN},
    {"Fl", "Flerovium",  114, 2890, 7, 14, CAT_POST},
    {"Mc", "Moscovium",  115, 2900, 7, 15, CAT_UNKNOWN},
    {"Lv", "Livermorium",116, 2930, 7, 16, CAT_UNKNOWN},
    {"Ts", "Tennessine", 117, 2940, 7, 17, CAT_UNKNOWN},
    {"Og", "Oganesson",  118, 2940, 7, 18, CAT_NOBLE},
};
#define N_ELEMS (int)(sizeof s_elems / sizeof s_elems[0])

/* Canvas layout. 18 cols of 13 px = 234 px wide, centered. Main 7
   periods + 2 f-block rows + legend. Canvas size 234 × 200 to fit
   everything. */
#define CELL_W       13
#define CELL_H       17
#define MAIN_Y0      0
#define F_GAP        10
#define F_Y0         (MAIN_Y0 + 7 * CELL_H + F_GAP)
#define CANVAS_W     234
#define CANVAS_H     (F_Y0 + 2 * CELL_H + 4)
#define CANVAS_BYTES (CANVAS_W * CANVAS_H * 4)

/* Canvas buffer parked at high RAM. */
/* In the app's own arena: the old fixed 0x092E0000 lies inside the OS heap
 * and was overwriting live objects (and sometimes this app's own code). */
static uint8_t s_canvas_buf[CANVAS_W * CANVAS_H * 4] __attribute__((aligned(4)));

static lv_obj_t *s_scr;
static lv_obj_t *s_table_view;
static lv_obj_t *s_canvas;
static lv_obj_t *s_detail_view;
static lv_obj_t *s_legend_view;
static lv_obj_t *s_d_symbol;
static lv_obj_t *s_d_name;
static lv_obj_t *s_d_z;
static lv_obj_t *s_d_weight;
static lv_obj_t *s_d_cat;
static lv_obj_t *s_d_color_chip;

static void itoa_u(uint32_t v, char *out)
{
    char b[12]; int i = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { b[i++] = '0' + (v % 10); v /= 10; }
    int k = 0; while (i) out[k++] = b[--i];
    out[k] = 0;
}

static void cell_origin(const elem_t *e, int *out_x, int *out_y)
{
    int col = e->group - 1;
    int row = e->period - 1;
    if (e->period >= 8) {
        *out_x = col * CELL_W;
        *out_y = F_Y0 + (e->period - 8) * CELL_H;
    } else {
        *out_x = col * CELL_W;
        *out_y = MAIN_Y0 + row * CELL_H;
    }
}

/* Returns element index for a (cx, cy) touch in canvas coords, or -1. */
static int hit_test(int cx, int cy)
{
    for (int i = 0; i < N_ELEMS; i++) {
        int x, y;
        cell_origin(&s_elems[i], &x, &y);
        if (cx >= x && cx < x + CELL_W && cy >= y && cy < y + CELL_H) return i;
    }
    return -1;
}

/* Fill a rectangle in the canvas backing buffer with a packed color. */
static void canvas_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    uint32_t *buf = (uint32_t *)s_canvas_buf;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= CANVAS_H) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= CANVAS_W) continue;
            buf[yy * CANVAS_W + xx] = color;
        }
    }
}

static uint32_t pack_color(uint32_t hex)
{
    /* hex is 0xRRGGBB; canvas expects BGR_ in little-endian XRGB8888. */
    uint8_t r = (hex >> 16) & 0xFF, g = (hex >> 8) & 0xFF, b = hex & 0xFF;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void draw_table(void)
{
    /* Clear */
    canvas_fill_rect(0, 0, CANVAS_W, CANVAS_H, pack_color(0x0a0e1a));
    /* Cells */
    for (int i = 0; i < N_ELEMS; i++) {
        int x, y;
        cell_origin(&s_elems[i], &x, &y);
        canvas_fill_rect(x + 1, y + 1, CELL_W - 2, CELL_H - 2,
                         pack_color(s_cat_color[(int)s_elems[i].cat]));
    }
    /* Element symbols — drawn via lv_canvas's text API which honors
       the small-font glyphs. Pre-built so the labels render with
       sharp anti-alias against the canvas pixels. */
    for (int i = 0; i < N_ELEMS; i++) {
        int x, y;
        cell_origin(&s_elems[i], &x, &y);
        /* The 13px cells can't fit a 2-letter symbol, so the grid shows just the
           first letter; the full symbol is on the element's detail screen. */
        char one[2] = { s_elems[i].sym[0], 0 };
        lv_layer_t layer; lv_canvas_init_layer(s_canvas, &layer);
        lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x000000);
        dsc.font = &lv_font_montserrat_14;
        dsc.text = one;
        dsc.align = LV_TEXT_ALIGN_CENTER;
        lv_area_t a;
        a.x1 = x + 1; a.y1 = y + 1;
        a.x2 = x + CELL_W - 1; a.y2 = y + CELL_H - 1;
        lv_draw_label(&layer, &dsc, &a);
        lv_canvas_finish_layer(s_canvas, &layer);
    }
}

static void show_detail(int idx);

static void on_canvas_press(lv_event_t *e)
{
    (void)e;
    lv_indev_t *ind = lv_indev_active(); if (!ind) return;
    lv_point_t p; lv_indev_get_point(ind, &p);
    lv_area_t area; lv_obj_get_coords(s_canvas, &area);
    int cx = p.x - area.x1;
    int cy = p.y - area.y1;
    int idx = hit_test(cx, cy);
    if (idx >= 0) show_detail(idx);
}

static void on_back(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(s_table_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
}

static void show_detail(int idx)
{
    const elem_t *e = &s_elems[idx];
    lv_label_set_text(s_d_symbol, e->sym);
    lv_label_set_text(s_d_name, e->name);

    char buf[40]; int k = 0;
    char nb[12];
    buf[k++] = '#';
    itoa_u((uint32_t)e->z, nb); for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k] = 0;
    lv_label_set_text(s_d_z, buf);

    k = 0;
    int whole = e->weight_x10 / 10;
    int frac  = e->weight_x10 % 10;
    itoa_u((uint32_t)whole, nb); for (int i = 0; nb[i]; i++) buf[k++] = nb[i];
    buf[k++] = '.';
    buf[k++] = '0' + frac;
    buf[k++] = ' '; buf[k++] = 'u';
    buf[k] = 0;
    lv_label_set_text(s_d_weight, buf);

    lv_label_set_text(s_d_cat, s_cat_name[(int)e->cat]);
    lv_obj_set_style_bg_color(s_d_color_chip,
        lv_color_hex(s_cat_color[(int)e->cat]), 0);

    lv_obj_add_flag(s_table_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_detail_view, LV_OBJ_FLAG_HIDDEN);
}

static void build_table(void)
{
    s_table_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_table_view, 240, 432);
    lv_obj_set_style_radius(s_table_view, 0, 0);
    lv_obj_set_pos(s_table_view, 0, 0);
    lv_obj_set_style_bg_color(s_table_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_table_view, 0, 0);
    lv_obj_set_style_pad_all(s_table_view, 0, 0);
    lv_obj_clear_flag(s_table_view, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_table_view);
    lv_label_set_text(title, "Periodic Table");
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    s_canvas = lv_canvas_create(s_table_view);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, CANVAS_W, CANVAS_H,
                         LV_COLOR_FORMAT_XRGB8888);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_canvas, on_canvas_press, LV_EVENT_PRESSED, NULL);

    draw_table();

    /* Legend moved to its own screen (it was a cramped grid under the table). */
    extern void pt_show_legend(lv_event_t *e);
    lv_obj_t *leg_btn = lv_button_create(s_table_view);
    lv_obj_set_size(leg_btn, 120, 32);
    lv_obj_align(leg_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(leg_btn, lv_color_hex(hb_color_surface()), 0);
    lv_obj_add_event_cb(leg_btn, pt_show_legend, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ll = lv_label_create(leg_btn);
    lv_label_set_text(ll, "Legend " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(ll, lv_color_hex(hb_color_text()), 0);
    lv_obj_center(ll);
}

static void on_legend_back(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_legend_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_table_view, LV_OBJ_FLAG_HIDDEN);
}

void pt_show_legend(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_table_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_legend_view, LV_OBJ_FLAG_HIDDEN);
}

static void build_legend(void)
{
    s_legend_view = lv_obj_create(s_scr);
    lv_obj_set_size(s_legend_view, 240, 432);
    lv_obj_set_style_radius(s_legend_view, 0, 0);
    lv_obj_set_pos(s_legend_view, 0, 0);
    lv_obj_set_style_bg_color(s_legend_view, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_border_width(s_legend_view, 0, 0);
    lv_obj_set_style_pad_all(s_legend_view, 0, 0);
    lv_obj_clear_flag(s_legend_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_legend_view, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_legend_view);
    lv_obj_set_size(back, 50, 32);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 6, 6);
    lv_obj_set_style_bg_color(back, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_add_event_cb(back, on_legend_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back); lv_label_set_text(bl, LV_SYMBOL_LEFT); lv_obj_center(bl);

    lv_obj_t *hdr = lv_label_create(s_legend_view);
    lv_label_set_text(hdr, "Legend");
    lv_obj_set_style_text_color(hdr, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_20, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 10);

    for (int i = 0; i < CAT_COUNT; i++) {
        int ly = 52 + i * 34;
        lv_obj_t *chip = lv_obj_create(s_legend_view);
        lv_obj_set_size(chip, 24, 24);
        lv_obj_set_pos(chip, 16, ly);
        lv_obj_set_style_bg_color(chip, lv_color_hex(s_cat_color[i]), 0);
        lv_obj_set_style_radius(chip, 4, 0);
        lv_obj_set_style_border_width(chip, 0, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *lab = lv_label_create(s_legend_view);
        lv_label_set_text(lab, s_cat_name[i]);
        lv_obj_set_style_text_color(lab, lv_color_hex(hb_color_text()), 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_16, 0);
        lv_obj_set_pos(lab, 52, ly + 3);
    }
}

static void build_detail(void)
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

    s_d_color_chip = lv_obj_create(s_detail_view);
    lv_obj_set_size(s_d_color_chip, 200, 160);
    lv_obj_align(s_d_color_chip, LV_ALIGN_TOP_MID, 0, 56);
    lv_obj_set_style_bg_color(s_d_color_chip, lv_color_hex(0x4ea8de), 0);
    lv_obj_set_style_radius(s_d_color_chip, 14, 0);
    lv_obj_set_style_border_width(s_d_color_chip, 0, 0);
    lv_obj_clear_flag(s_d_color_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_d_color_chip, LV_OBJ_FLAG_CLICKABLE);

    s_d_symbol = lv_label_create(s_d_color_chip);
    lv_label_set_text(s_d_symbol, "");
    lv_obj_set_style_text_color(s_d_symbol, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_d_symbol, &lv_font_montserrat_48, 0);
    lv_obj_center(s_d_symbol);

    s_d_z = lv_label_create(s_d_color_chip);
    lv_label_set_text(s_d_z, "");
    lv_obj_set_style_text_color(s_d_z, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(s_d_z, &lv_font_montserrat_16, 0);
    lv_obj_align(s_d_z, LV_ALIGN_TOP_LEFT, 8, 6);

    s_d_name = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_name, "");
    lv_obj_set_style_text_color(s_d_name, lv_color_hex(hb_color_text()), 0);
    lv_obj_set_style_text_font(s_d_name, &lv_font_montserrat_24, 0);
    lv_obj_align(s_d_name, LV_ALIGN_TOP_MID, 0, 224);

    s_d_weight = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_weight, "");
    lv_obj_set_style_text_color(s_d_weight, lv_color_hex(hb_color_text_dim()), 0);
    lv_obj_set_style_text_font(s_d_weight, &lv_font_montserrat_20, 0);
    lv_obj_align(s_d_weight, LV_ALIGN_TOP_MID, 0, 262);

    s_d_cat = lv_label_create(s_detail_view);
    lv_label_set_text(s_d_cat, "");
    lv_obj_set_style_text_color(s_d_cat, lv_color_hex(hb_color_primary()), 0);
    lv_obj_set_style_text_font(s_d_cat, &lv_font_montserrat_16, 0);
    lv_obj_align(s_d_cat, LV_ALIGN_TOP_MID, 0, 296);
}

HB_APP_ENTRY(payload_entry)
{
    hb_trace_init();

    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, lv_color_hex(hb_color_bg()), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);

    build_table();
    build_detail();
    build_legend();
}
