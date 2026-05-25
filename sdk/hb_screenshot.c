/*
 * hb_screenshot.c — OS screenshots plus homebrew direct-MIPI captures.
 *
 * The OS screenshot handler is also in the SDK because it can screenshot
 * system UI under custom apps. Homebrew apps draw directly to the LCD MIPI
 * controller, bypassing the OS compositor, so Home+Power uses our own
 * shadow framebuffer and writes /appshotNNNN.bmp instead.
 */

#include "hb_sdk.h"

#define HB_SCREENSHOT_HANDLER_ADDR  (0x08240860u | 1u)
#define HB_GLOBAL_CONTROLLER_ADDR   (0x083fb524u | 1u)

#ifndef NANOAPPS_ENABLE_APP_SCREENSHOTS
#define NANOAPPS_ENABLE_APP_SCREENSHOTS 0
#endif

typedef void *(*global_controller_get_t)(void);
typedef void  (*screenshot_handler_t)(void *controller);

#if NANOAPPS_ENABLE_APP_SCREENSHOTS

#define FS_MAIN_VOLUME 0

#define HB_SHADOW_FB_ADDR   0x09140000u
#define HB_SHADOW_ROW_ADDR  0x09173000u
#define HB_SHADOW_W         HB_SCREEN_W
#define HB_SHADOW_H         HB_SCREEN_H
#define HB_BMP_HEADER_SIZE  54u
#define HB_BMP_ROW_BYTES    (HB_SCREEN_W * 3u)
#define HB_BMP_SIZE         (HB_BMP_HEADER_SIZE + HB_BMP_ROW_BYTES * HB_SCREEN_H)

typedef void (*fs_file_ctor_c_t)(void *this, const char *path,
                                 int readOnly, int volume,
                                 uint32_t cacheSize,
                                 uint32_t numCaches, void *cachePtr);
typedef void (*fs_file_dtor_t)(void *this);
typedef int  (*fs_file_isopen_t)(void *this);
typedef int  (*fs_file_write_t)(void *this, uint32_t numBytes,
                                const void *buf, uint32_t *bytesOut);
typedef int  (*fs_file_seteof_t)(void *this, uint32_t eof);
typedef int  (*fs_file_flush_t)(void *this);

#define FILE_CTOR_C ((fs_file_ctor_c_t)(0x084137a8u | 1u))
#define FILE_DTOR   ((fs_file_dtor_t)  (0x08423be0u | 1u))
#define FILE_ISOPEN ((fs_file_isopen_t)(0x08417e18u | 1u))
#define FILE_WRITE  ((fs_file_write_t) (0x0841ba42u | 1u))
#define FILE_SETEOF ((fs_file_seteof_t)(0x0841b09eu | 1u))
#define FILE_FLUSH  ((fs_file_flush_t) (0x0840718cu | 1u))

#define FILEOBJ_SIZE     0x60
#define FILE_CACHE_SIZE  0x1010

static uint8_t g_fs_cache[FILE_CACHE_SIZE];

static bool s_shadow_ready = false;
static int16_t s_win_x = 0;
static int16_t s_win_y = 0;
static int16_t s_win_w = 0;
static int16_t s_win_h = 0;
static int16_t s_cur_x = 0;
static int16_t s_cur_y = 0;

static uint16_t color_to_565(hb_color_t color)
{
    uint16_t r = (uint16_t)((color >> 19) & 0x1Fu);
    uint16_t g = (uint16_t)((color >> 10) & 0x3Fu);
    uint16_t b = (uint16_t)((color >> 3)  & 0x1Fu);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t *shadow_fb(void)
{
    return (uint16_t *)HB_SHADOW_FB_ADDR;
}

static uint8_t *shadow_row(void)
{
    return (uint8_t *)HB_SHADOW_ROW_ADDR;
}

static void shadow_ensure(void)
{
    if (s_shadow_ready) return;
    uint16_t *fb = shadow_fb();
    for (uint32_t i = 0; i < HB_SHADOW_W * HB_SHADOW_H; i++) {
        fb[i] = 0;
    }
    s_shadow_ready = true;
}

static void shadow_plot(int16_t x, int16_t y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= HB_SHADOW_W || y >= HB_SHADOW_H) return;
    shadow_fb()[(uint32_t)y * HB_SHADOW_W + (uint32_t)x] = c;
}

static void shadow_put_windowed(uint16_t c)
{
    if (s_cur_y >= s_win_h || s_win_w <= 0 || s_win_h <= 0) return;
    shadow_plot((int16_t)(s_win_x + s_cur_x), (int16_t)(s_win_y + s_cur_y), c);
    s_cur_x++;
    if (s_cur_x >= s_win_w) {
        s_cur_x = 0;
        s_cur_y++;
    }
}

void hb_screenshot_shadow_set_window(int16_t x, int16_t y, int16_t w, int16_t h)
{
    shadow_ensure();
    s_win_x = x;
    s_win_y = y;
    s_win_w = w;
    s_win_h = h;
    s_cur_x = 0;
    s_cur_y = 0;
}

void hb_screenshot_shadow_fill(int16_t x, int16_t y, int16_t w, int16_t h,
                               hb_color_t color)
{
    shadow_ensure();
    if (w <= 0 || h <= 0) return;

    int16_t x0 = x;
    int16_t y0 = y;
    int16_t x1 = (int16_t)(x + w);
    int16_t y1 = (int16_t)(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > HB_SHADOW_W) x1 = HB_SHADOW_W;
    if (y1 > HB_SHADOW_H) y1 = HB_SHADOW_H;
    if (x0 >= x1 || y0 >= y1) return;

    uint16_t c = color_to_565(color);
    uint16_t *fb = shadow_fb();
    for (int16_t yy = y0; yy < y1; yy++) {
        uint32_t off = (uint32_t)yy * HB_SHADOW_W + (uint32_t)x0;
        for (int16_t xx = x0; xx < x1; xx++) {
            fb[off++] = c;
        }
    }
}

void hb_screenshot_shadow_text_strip(uint8_t row_byte, uint8_t scale,
                                     hb_color_t fg, hb_color_t bg)
{
    shadow_ensure();
    uint16_t f = color_to_565(fg);
    uint16_t b = color_to_565(bg);

    for (uint8_t sub_v = 0; sub_v < scale; sub_v++) {
        for (int bit = 7; bit >= 0; bit--) {
            uint16_t c = ((row_byte >> bit) & 1) ? f : b;
            for (uint8_t sc = 0; sc < scale; sc++) {
                shadow_put_windowed(c);
            }
        }
    }
}

void hb_screenshot_shadow_blit_glyph(int16_t x, int16_t y,
                                     int16_t w, int16_t h,
                                     const uint8_t *gray, uint8_t threshold,
                                     hb_color_t fg, hb_color_t bg)
{
    shadow_ensure();
    if (w <= 0 || h <= 0 || !gray) return;

    uint16_t f = color_to_565(fg);
    uint16_t b = color_to_565(bg);
    uint32_t src = 0;
    for (int16_t yy = 0; yy < h; yy++) {
        for (int16_t xx = 0; xx < w; xx++) {
            shadow_plot((int16_t)(x + xx), (int16_t)(y + yy),
                        gray[src++] >= threshold ? f : b);
        }
    }
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void appshot_path(char path[17], uint16_t n)
{
    path[0] = '/'; path[1] = 'a'; path[2] = 'p'; path[3] = 'p';
    path[4] = 's'; path[5] = 'h'; path[6] = 'o'; path[7] = 't';

    uint8_t d0 = 0, d1 = 0, d2 = 0;
    while (n >= 1000) { n -= 1000; d0++; }
    while (n >= 100)  { n -= 100;  d1++; }
    while (n >= 10)   { n -= 10;   d2++; }
    path[8]  = (char)('0' + d0);
    path[9]  = (char)('0' + d1);
    path[10] = (char)('0' + d2);
    path[11] = (char)('0' + n);
    path[12] = '.';
    path[13] = 'b';
    path[14] = 'm';
    path[15] = 'p';
    path[16] = 0;
}

static void pick_appshot_path(char path[17])
{
    for (uint16_t n = 0; n < 10000; n++) {
        appshot_path(path, n);
        if (!hb_fs_exists(path)) return;
    }
    appshot_path(path, 0);
}

static bool file_write_all(void *file_obj, const void *data, uint32_t size)
{
    uint32_t bytes_out = 0;
    int rc = FILE_WRITE(file_obj, size, data, &bytes_out);
    return rc == 0 && bytes_out == size;
}

bool hb_screenshot_take(void)
{
    void *controller = ((global_controller_get_t)HB_GLOBAL_CONTROLLER_ADDR)();
    if (!controller) return false;
    ((screenshot_handler_t)HB_SCREENSHOT_HANDLER_ADDR)(controller);
    return true;
}

bool hb_app_screenshot_take(void)
{
    shadow_ensure();

    char path[17];
    pick_appshot_path(path);

    uint8_t file_obj[FILEOBJ_SIZE];
    for (uint32_t i = 0; i < FILE_CACHE_SIZE; i++) g_fs_cache[i] = 0;

    FILE_CTOR_C(file_obj, path, /*readOnly=*/0, FS_MAIN_VOLUME,
                /*cacheSize=*/0x1000, /*numCaches=*/1, g_fs_cache);

    bool ok = false;
    if (FILE_ISOPEN(file_obj)) {
        uint8_t header[HB_BMP_HEADER_SIZE];
        for (uint32_t i = 0; i < HB_BMP_HEADER_SIZE; i++) header[i] = 0;

        header[0] = 'B';
        header[1] = 'M';
        put_le32(&header[2], HB_BMP_SIZE);
        put_le32(&header[10], HB_BMP_HEADER_SIZE);
        put_le32(&header[14], 40);
        put_le32(&header[18], HB_SCREEN_W);
        put_le32(&header[22], HB_SCREEN_H);
        put_le16(&header[26], 1);
        put_le16(&header[28], 24);
        put_le32(&header[34], HB_BMP_ROW_BYTES * HB_SCREEN_H);

        ok = file_write_all(file_obj, header, HB_BMP_HEADER_SIZE);

        uint16_t *fb = shadow_fb();
        uint8_t *row = shadow_row();
        for (int16_t y = (int16_t)(HB_SCREEN_H - 1); ok && y >= 0; y--) {
            uint32_t dst = 0;
            uint32_t src = (uint32_t)y * HB_SCREEN_W;
            for (uint16_t x = 0; x < HB_SCREEN_W; x++) {
                uint16_t c = fb[src + x];
                uint8_t r5 = (uint8_t)((c >> 11) & 0x1Fu);
                uint8_t g6 = (uint8_t)((c >> 5) & 0x3Fu);
                uint8_t b5 = (uint8_t)(c & 0x1Fu);
                row[dst++] = (uint8_t)((b5 << 3) | (b5 >> 2));
                row[dst++] = (uint8_t)((g6 << 2) | (g6 >> 4));
                row[dst++] = (uint8_t)((r5 << 3) | (r5 >> 2));
            }
            ok = file_write_all(file_obj, row, HB_BMP_ROW_BYTES);
        }

        if (ok) {
            FILE_SETEOF(file_obj, HB_BMP_SIZE);
            FILE_FLUSH(file_obj);
        }
    }

    FILE_DTOR(file_obj);
    return ok;
}

#else

bool hb_screenshot_take(void)
{
    void *controller = ((global_controller_get_t)HB_GLOBAL_CONTROLLER_ADDR)();
    if (!controller) return false;
    ((screenshot_handler_t)HB_SCREENSHOT_HANDLER_ADDR)(controller);
    return true;
}

void hb_screenshot_shadow_set_window(int16_t x, int16_t y, int16_t w, int16_t h)
{
    (void)x; (void)y; (void)w; (void)h;
}

void hb_screenshot_shadow_fill(int16_t x, int16_t y, int16_t w, int16_t h,
                               hb_color_t color)
{
    (void)x; (void)y; (void)w; (void)h; (void)color;
}

void hb_screenshot_shadow_text_strip(uint8_t row_byte, uint8_t scale,
                                     hb_color_t fg, hb_color_t bg)
{
    (void)row_byte; (void)scale; (void)fg; (void)bg;
}

void hb_screenshot_shadow_blit_glyph(int16_t x, int16_t y,
                                     int16_t w, int16_t h,
                                     const uint8_t *gray, uint8_t threshold,
                                     hb_color_t fg, hb_color_t bg)
{
    (void)x; (void)y; (void)w; (void)h;
    (void)gray; (void)threshold; (void)fg; (void)bg;
}

bool hb_app_screenshot_take(void)
{
    return false;
}

#endif

bool hb_screenshot_poll_chord(void)
{
    static bool was_down = false;
    static uint32_t cooldown = 0;

    if (cooldown > 0) cooldown--;

    bool down = hb_button_pressed(HB_BTN_HOME) &&
                hb_button_pressed(HB_BTN_POWER);
    bool fire = down && !was_down && cooldown == 0;
    was_down = down;

    if (!fire) return false;

#if NANOAPPS_ENABLE_APP_SCREENSHOTS
    cooldown = 1500;
    (void)hb_app_screenshot_take();
    return true;
#else
    return false;
#endif
}
