/*
 * hb_prefs — system-wide user preferences (theme, status bar, home
 * button) persisted under /Data/Settings/.
 *
 * Files (one value each, plain text):
 *   /Data/Settings/theme        — "light" | "dark" | "auto" (default)
 *   /Data/Settings/status_bar   — "1" (default) or "0"
 *   /Data/Settings/home_button  — "launcher" (default) or "os"
 *
 * Tint has no file: hb_tint_color() always follows the device
 * color (hb_devcolor_tint()), so every homebrew app matches the
 * hardware accent. The Screenshot app's device-color picker writes
 * the same live OS field, so its override is honoured automatically
 * on the next app launch.
 *
 * "auto" theme resolves at call time from the RTC hour: dark from
 * 18:00 to 06:00 local, light otherwise.
 *
 * Apps that opt into theming should call hb_lvgl_apply_theme()
 * after hb_lvgl_init() and use the semantic palette accessors for
 * any non-default-styled colors. Destructive UI (delete buttons,
 * confirm-delete dialogs) should use hb_color_danger() so it stays
 * red regardless of tint.
 */
#ifndef HB_PREFS_H_
#define HB_PREFS_H_

#include <stdbool.h>
#include <stdint.h>

#define HB_TINT_DEFAULT 0xfca311u

typedef enum { HB_THEME_LIGHT, HB_THEME_DARK, HB_THEME_AUTO } hb_theme_mode_t;
typedef enum { HB_HOME_QUIT_TO_OS = 0, HB_HOME_QUIT_TO_LAUNCHER = 1 } hb_home_policy_t;

/* Raw user prefs. */
uint32_t          hb_tint_color(void);
hb_theme_mode_t   hb_theme_mode(void);
bool              hb_status_bar_enabled(void);
hb_home_policy_t  hb_home_button_policy(void);

/* Resolves HB_THEME_AUTO based on the current hour. */
bool              hb_theme_is_dark(void);

/* Semantic palette derived from tint + theme. Apps should prefer
 * these over hardcoded hex. */
uint32_t hb_color_bg(void);
uint32_t hb_color_surface(void);
uint32_t hb_color_text(void);
uint32_t hb_color_text_dim(void);
uint32_t hb_color_primary(void);
uint32_t hb_color_on_primary(void);
uint32_t hb_color_danger(void);
uint32_t hb_color_success(void);
uint32_t hb_color_warning(void);

/* Drop the in-memory cache so the next getter re-reads the on-disk
 * pref files. Settings app calls this after every write so its own
 * preview reflects the new pref immediately. */
void hb_prefs_invalidate(void);

#endif
