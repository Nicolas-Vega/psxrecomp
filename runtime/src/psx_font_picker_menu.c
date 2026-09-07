/* psx_font_picker_menu.c - HD font-pack generator overlay, opened from the
 * texture-pack menu when Beetle format is selected. Sibling of
 * psx_texpack_menu.c / psx_savestate_menu.c: same rasterize-to-ARGB8888
 * pattern, own tiny font/primitives kept self-contained rather than shared
 * (matching those modules' own existing convention). */

#include "psx_font_picker_menu.h"

#include <stdio.h>
#include <string.h>

#define FPM_W 640
#define FPM_H 480
#define FPM_ROW_H 22
#define FPM_ROWS_Y 54
#define FPM_ROWS_X 24
#define FPM_ROWS_W 592
#define FPM_VISIBLE_ROWS 16

/* Public-domain 8x8 ASCII 32..90 subset from font8x8_basic -- same glyph
 * data as psx_texpack_menu.c's FONT8, duplicated rather than shared (see
 * the file comment above). */
static const uint8_t FONT8[59][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x06,0x06,0x3E,0x06,0x06,0x7F,0x00},
    {0x7F,0x06,0x06,0x3E,0x06,0x06,0x06,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x06,0x06,0x06,0x06,0x06,0x06,0x7F,0x00}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x06,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},
};

static int s_open;
static int s_busy;
static int s_cursor_in_view;
static char s_names[FPM_VISIBLE_ROWS][128];
static int s_visible_count;
static int s_scroll_top;
static int s_total_count;
static int s_bold_on;
static int s_italic_on;
static char s_status_line[128];
static uint32_t s_panel[FPM_W * FPM_H];

static void fill_rect(uint32_t *dst, int x0, int y0, int w, int h, uint32_t col)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > FPM_W) w = FPM_W - x0;
    if (y0 + h > FPM_H) h = FPM_H - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            dst[y * FPM_W + x] = col;
}

static void stroke_rect(uint32_t *dst, int x, int y, int w, int h, uint32_t col)
{
    fill_rect(dst, x, y, w, 1, col);
    fill_rect(dst, x, y + h - 1, w, 1, col);
    fill_rect(dst, x, y, 1, h, col);
    fill_rect(dst, x + w - 1, y, 1, h, col);
}

static void draw_char(uint32_t *dst, int x0, int y0, char c, uint32_t col, int scale)
{
    int x, y, sx, sy;
    const uint8_t *g;
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c < 32 || c > 90) c = '?';
    g = FONT8[(int)c - 32];
    for (y = 0; y < 8; y++) {
        uint8_t row = g[y];
        for (x = 0; x < 8; x++) {
            if ((row & (1u << x)) == 0) continue;
            for (sy = 0; sy < scale; sy++)
                for (sx = 0; sx < scale; sx++) {
                    int dx = x0 + x * scale + sx;
                    int dy = y0 + y * scale + sy;
                    if ((unsigned)dx < FPM_W && (unsigned)dy < FPM_H)
                        dst[dy * FPM_W + dx] = col;
                }
        }
    }
}

static void draw_text(uint32_t *dst, int x, int y, const char *s, uint32_t col, int scale)
{
    if (!s) return;
    while (*s) {
        draw_char(dst, x, y, *s++, col, scale);
        x += 8 * scale;
    }
}

/* Truncates to fit FPM_ROWS_W at scale 1 (74 chars) with an ellipsis. */
static void draw_text_clipped(uint32_t *dst, int x, int y, const char *s, uint32_t col)
{
    char buf[80];
    size_t len = strlen(s);
    const size_t maxlen = 74;
    if (len <= maxlen) {
        draw_text(dst, x, y, s, col, 1);
        return;
    }
    memcpy(buf, s, maxlen - 3);
    buf[maxlen - 3] = buf[maxlen - 2] = buf[maxlen - 1] = '.';
    buf[maxlen] = 0;
    draw_text(dst, x, y, buf, col, 1);
}

static void rasterize_panel(void)
{
    int i;
    char buf[96];

    for (i = 0; i < FPM_W * FPM_H; i++)
        s_panel[i] = 0xFF0F1118u;

    fill_rect(s_panel, 0, 0, FPM_W, 40, 0xFF171B25u);
    draw_text(s_panel, 24, 12, "CHOOSE FONT", 0xFFFFD24Du, 2);

    /* Checkbox indicators, right of the title. Toggling these picks which
     * weight variant of the highlighted font family generate_font_pack
     * uses (a real bold/italic file if that family ships one, else
     * synthesis -- see font_picker_generate_selected in main.cpp), instead
     * of listing every weight as its own row.
     * FONT8 only covers ASCII 32-90 (space through 'Z') -- no brackets, no
     * lowercase -- so the on/off state is spelled out rather than using a
     * "[X]" checkbox glyph, which would draw as "?X?" (91/93 both fall
     * outside that range and render as the tofu fallback). */
    snprintf(buf, sizeof(buf), "BOLD %s (B)", s_bold_on ? "ON" : "OFF");
    draw_text(s_panel, 280, 14, buf, s_bold_on ? 0xFFFFD24Du : 0xFFB8BDC8u, 1);
    snprintf(buf, sizeof(buf), "ITALIC %s (I)", s_italic_on ? "ON" : "OFF");
    draw_text(s_panel, 420, 14, buf, s_italic_on ? 0xFFFFD24Du : 0xFFB8BDC8u, 1);

    if (s_visible_count == 0) {
        draw_text(s_panel, FPM_ROWS_X, FPM_ROWS_Y + 8,
                  "NO TTF/OTF FONTS FOUND IN C:\\WINDOWS\\FONTS", 0xFFB2727Cu, 1);
    }

    for (i = 0; i < s_visible_count && i < FPM_VISIBLE_ROWS; i++) {
        int y = FPM_ROWS_Y + i * FPM_ROW_H;
        int sel = (i == s_cursor_in_view);
        uint32_t bg = sel ? 0xFF2B2830u : 0xFF171A22u;
        uint32_t fg = sel ? 0xFFFFD24Du : 0xFFE2E5EBu;
        fill_rect(s_panel, FPM_ROWS_X, y, FPM_ROWS_W, FPM_ROW_H - 2, bg);
        if (sel)
            stroke_rect(s_panel, FPM_ROWS_X, y, FPM_ROWS_W, FPM_ROW_H - 2, 0xFFFFD24Du);
        draw_text_clipped(s_panel, FPM_ROWS_X + 10, y + 7, s_names[i], fg);
    }

    /* Scroll position indicator, right edge of the list. */
    if (s_total_count > FPM_VISIBLE_ROWS) {
        snprintf(buf, sizeof(buf), "%d/%d", s_scroll_top + s_cursor_in_view + 1, s_total_count);
        draw_text(s_panel, FPM_ROWS_X + FPM_ROWS_W - 56,
                  FPM_ROWS_Y + FPM_VISIBLE_ROWS * FPM_ROW_H + 6, buf, 0xFFB8BDC8u, 1);
    }

    fill_rect(s_panel, 0, 440, FPM_W, 40, 0xFF171B25u);
    if (s_busy) {
        draw_text(s_panel, 32, 456, "GENERATING TEXTURES, PLEASE WAIT...", 0xFFFFD24Du, 1);
    } else if (s_status_line[0]) {
        draw_text(s_panel, 32, 456, s_status_line, 0xFF8BE28Bu, 1);
    } else {
        draw_text(s_panel, 32, 456,
                  "ARROWS/PGUP/PGDN SELECT  B/I TOGGLE  ENTER GENERATE  ESC BACK",
                  0xFFB8BDC8u, 1);
    }
}

void psx_font_picker_menu_set_state(int open, int busy, int cursor_in_view,
                                     const char *const *names, int visible_count,
                                     int scroll_top, int total_count,
                                     int bold_on, int italic_on,
                                     const char *status_line)
{
    int i;
    s_open = open ? 1 : 0;
    s_busy = busy ? 1 : 0;
    s_bold_on = bold_on ? 1 : 0;
    s_italic_on = italic_on ? 1 : 0;
    s_cursor_in_view = cursor_in_view;
    s_scroll_top = scroll_top;
    s_total_count = total_count;
    s_visible_count = visible_count;
    for (i = 0; i < visible_count && i < FPM_VISIBLE_ROWS; i++)
        snprintf(s_names[i], sizeof(s_names[i]), "%s", names[i]);
    if (status_line)
        snprintf(s_status_line, sizeof(s_status_line), "%s", status_line);
    else
        s_status_line[0] = 0;
}

int psx_font_picker_menu_needs_present(void)
{
    return s_open;
}

int psx_font_picker_menu_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_open) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    rasterize_panel();
    if (pixels) *pixels = s_panel;
    if (w) *w = FPM_W;
    if (h) *h = FPM_H;
    return 1;
}
