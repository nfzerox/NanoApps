/*
 * hb_prefs — preferences, both ours and the OS's.
 *
 * Two small stores share this file: (1) our own file-backed user prefs
 * (theme / status-bar / home-button) under /Data/Settings, cached on
 * first read so the per-frame status-bar tick doesn't re-stat disk; and
 * (2) thin read/write accessors over the OS preference-store singleton
 * (the hb_settings_* calls).
 */
#include "hb_prefs.h"
#include "hb_sdk.h"
#include "hb_sysfx.h"

#define THEME_PATH    "/Data/Settings/theme"
#define BAR_PATH      "/Data/Settings/status_bar"
#define HOME_PATH     "/Data/Settings/home_button"

static int       s_cached       = 0;
static uint32_t  s_tint         = HB_TINT_DEFAULT;
static hb_theme_mode_t s_theme  = HB_THEME_DARK;   /* dark by default at runtime */
static bool      s_status_bar   = false;           /* demo status bar off by default */
static hb_home_policy_t s_home  = HB_HOME_QUIT_TO_OS;

static int strieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static void ensure_loaded(void)
{
    if (s_cached) return;
    s_cached = 1;

    char buf[32];

    /* Tint follows the DEVICE color (which the Screenshot app's override
       also sets), not a Settings file — homebrew apps match the hardware accent. */
    s_tint = hb_devcolor_tint();

    /* theme (file override; defaults to dark) */
    uint32_t n = hb_fs_read(THEME_PATH, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ' || buf[n-1] == '\r')) buf[--n] = 0;
        if (strieq(buf, "light")) s_theme = HB_THEME_LIGHT;
        else if (strieq(buf, "dark")) s_theme = HB_THEME_DARK;
        else s_theme = HB_THEME_AUTO;
    }

    /* status bar */
    n = hb_fs_read(BAR_PATH, buf, sizeof buf - 1);
    if (n > 0) s_status_bar = (buf[0] != '0');

    /* home button */
    n = hb_fs_read(HOME_PATH, buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        if (strieq(buf, "launcher") || buf[0] == '1') s_home = HB_HOME_QUIT_TO_LAUNCHER;
        else s_home = HB_HOME_QUIT_TO_OS;
    }
}

uint32_t hb_tint_color(void)
{
    ensure_loaded();
    return s_tint;
}

bool hb_status_bar_enabled(void)
{
    ensure_loaded();
    return s_status_bar;
}

hb_home_policy_t hb_home_button_policy(void)
{
    ensure_loaded();
    return s_home;
}

void hb_prefs_invalidate(void)
{
    s_cached     = 0;
    s_tint       = HB_TINT_DEFAULT;
    s_theme      = HB_THEME_DARK;
    s_status_bar = false;
    s_home       = HB_HOME_QUIT_TO_OS;
}

hb_theme_mode_t hb_theme_mode(void)
{
    ensure_loaded();
    return s_theme;
}

bool hb_theme_is_dark(void)
{
    ensure_loaded();
    if (s_theme == HB_THEME_LIGHT) return false;
    if (s_theme == HB_THEME_DARK)  return true;
    /* AUTO: dark from 18:00 to 06:00 */
    hb_rtc_time_t t; hb_rtc_read(&t);
    return (t.hours >= 18 || t.hours < 6);
}

/* ---- semantic palette ----
 * Background / surface / text pulled from the resolved theme; tint
 * is the user's chosen accent; danger is always red so destructive
 * UI never gets lost under a red tint, etc. */
uint32_t hb_color_bg(void)        { return hb_theme_is_dark() ? 0x0a0e1a : 0xf6f7fb; }
uint32_t hb_color_surface(void)   { return hb_theme_is_dark() ? 0x1a1f2e : 0xffffff; }
uint32_t hb_color_text(void)      { return hb_theme_is_dark() ? 0xffffff : 0x121826; }
uint32_t hb_color_text_dim(void)  { return hb_theme_is_dark() ? 0x9aa5b1 : 0x6b7280; }
uint32_t hb_color_primary(void)   { return hb_tint_color(); }
uint32_t hb_color_on_primary(void)
{
    /* Pick black or white text based on luminance of the tint. */
    uint32_t c = hb_tint_color();
    uint32_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    uint32_t lum = (r * 30 + g * 59 + b * 11) / 100;
    return lum > 140 ? 0x121826 : 0xffffff;
}
uint32_t hb_color_danger(void)    { return 0xe63946; }
uint32_t hb_color_success(void)   { return 0x2a9d8f; }
uint32_t hb_color_warning(void)   { return 0xf4a261; }


/* ======================================================================
 * OS preference store — thin wrappers over the singleton pref object.
 * ==================================================================== */

#define PREFS_SINGLETON  ((void *(*)(void))(0x0841cfacu | 1u))

/* Member slots on the store's vtable (byte offsets). The set path seems to
   write through without a separate flush slot. */
#define PREFS_SLOT_GET_INT  0x44
#define PREFS_SLOT_SET_INT  0x34
#define PREFS_SLOT_SET_STR  0x5c

#define PREFS_UNSET  (-3)   /* this is returned when the key isn't set */

/* Resolve the singleton and hand back its vtable. Returns NULL if the store
   isn't up yet (early boot, or a context where the getter returns null). */
static void *prefs_resolve(void ***vt_out)
{
    void *self = PREFS_SINGLETON();
    if (self) *vt_out = *(void ***)self;
    return self;
}

/* Read an integer setting; returns `default_v` if the key isn't present. */
int32_t hb_settings_get_int(const char *key, int32_t default_v)
{
    void **vt = 0;
    void *self = prefs_resolve(&vt);
    if (!self) return default_v;
    int32_t out = PREFS_UNSET;
    ((void (*)(void *, const char *, int32_t *))vt[PREFS_SLOT_GET_INT / 4])(self, key, &out);
    return out == PREFS_UNSET ? default_v : out;
}

/* Write an integer setting. */
void hb_settings_set_int(const char *key, int32_t value)
{
    void **vt = 0;
    void *self = prefs_resolve(&vt);
    if (!self) return;
    ((void (*)(void *, const char *, int32_t))vt[PREFS_SLOT_SET_INT / 4])(self, key, value);
}

/* Write a string setting (value must be a null-terminated UTF-8 string). The
   trailing argument is unused by our calls; we pass 0. */
void hb_settings_set_str(const char *key, const char *value)
{
    void **vt = 0;
    void *self = prefs_resolve(&vt);
    if (!self) return;
    ((void (*)(void *, const char *, const char *, int))vt[PREFS_SLOT_SET_STR / 4])(self, key, value, 0);
}

/* Convenience: current brightness. */
int32_t hb_settings_get_brightness(void)
{
    return hb_settings_get_int("General.Brightness", 2);
}

void hb_settings_set_brightness(int32_t level)
{
    hb_settings_set_int("General.Brightness", level);
}

/* Convenience: backlight timer in seconds (0 = always on). */
int32_t hb_settings_get_backlight_timer(void)
{
    return hb_settings_get_int("General.BacklightTimer", 15);
}

void hb_settings_set_backlight_timer(int32_t seconds)
{
    hb_settings_set_int("General.BacklightTimer", seconds);
}
