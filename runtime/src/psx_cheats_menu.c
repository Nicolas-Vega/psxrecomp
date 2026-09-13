/* psx_cheats_menu.c - GameShark cheat browser overlay.
 * Sibling of psx_savestate_menu.c / psx_texpack_menu.c: same rasterize-to-
 * ARGB8888 pattern (own tiny font + primitives, kept self-contained rather
 * than sharing code with those modules, matching their own convention). */

#include "psx_cheats_menu.h"

#include "psx_cheats.h"
#include "host_keymap.h"

#include <stdio.h>
#include <string.h>

#define CM_W 640
#define CM_H 480
#define CM_LIST_Y 84
#define CM_LIST_X 24
#define CM_LIST_W 592
#define CM_ROW_H 28
#define CM_ROW_GAP 2
#define CM_VISIBLE_ROWS 11
#define CM_MAX_CATEGORIES 32

/* Public-domain 8x8 ASCII 32..90 subset from font8x8_basic -- same glyph
 * data as psx_savestate_menu.c's FONT8, duplicated rather than shared (see
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
static int s_category;
static int s_item;
static int s_dirty = 1;
static uint32_t s_panel[CM_W * CM_H];

typedef struct { const char *name; int start; int count; } CmCategory;
static CmCategory s_categories[CM_MAX_CATEGORIES];
static int s_category_count = -1; /* -1 => not built yet */

static void build_categories(void) {
    int i, n;
    const PsxCheatDef *db;
    if (s_category_count >= 0) return;
    db = psx_cheats_db(&n);
    s_category_count = 0;
    for (i = 0; i < n; i++) {
        if (s_category_count > 0 &&
            strcmp(s_categories[s_category_count - 1].name, db[i].category) == 0) {
            s_categories[s_category_count - 1].count++;
            continue;
        }
        if (s_category_count >= CM_MAX_CATEGORIES) break;
        s_categories[s_category_count].name = db[i].category;
        s_categories[s_category_count].start = i;
        s_categories[s_category_count].count = 1;
        s_category_count++;
    }
}

int psx_cheats_menu_category_count(void) {
    build_categories();
    return s_category_count;
}

int psx_cheats_menu_category_item_count(int category) {
    build_categories();
    if (category < 0 || category >= s_category_count) return 0;
    return s_categories[category].count;
}

const char *psx_cheats_menu_category_name(int category) {
    build_categories();
    if (category < 0 || category >= s_category_count) return "";
    return s_categories[category].name;
}

int psx_cheats_menu_absolute_index(int category, int item) {
    build_categories();
    if (category < 0 || category >= s_category_count) return -1;
    if (item < 0 || item >= s_categories[category].count) return -1;
    return s_categories[category].start + item;
}

static void fill_rect(uint32_t *dst, int x0, int y0, int w, int h, uint32_t col)
{
    int x, y;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > CM_W) w = CM_W - x0;
    if (y0 + h > CM_H) h = CM_H - y0;
    if (w <= 0 || h <= 0) return;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            dst[y * CM_W + x] = col;
}

static void stroke_rect(uint32_t *dst, int x, int y, int w, int h, uint32_t col)
{
    fill_rect(dst, x, y, w, 1, col);
    fill_rect(dst, x, y + h - 1, w, 1, col);
    fill_rect(dst, x, y, 1, h, col);
    fill_rect(dst, x + w - 1, y, 1, h, col);
}

static void draw_char(uint32_t *dst, int x0, int y0, char c,
                      uint32_t col, int scale)
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
                    if ((unsigned)dx < CM_W && (unsigned)dy < CM_H)
                        dst[dy * CM_W + dx] = col;
                }
        }
    }
}

static void draw_text(uint32_t *dst, int x, int y, const char *s,
                      uint32_t col, int scale)
{
    if (!s) return;
    while (*s) {
        draw_char(dst, x, y, *s++, col, scale);
        x += 8 * scale;
    }
}

static void draw_checkbox(uint32_t *dst, int x, int y, int checked)
{
    stroke_rect(dst, x, y, 16, 16, 0xFF8B93A3u);
    if (checked) {
        fill_rect(dst, x + 3, y + 3, 10, 10, 0xFF8BE28Bu);
    }
}

static void rasterize_panel(void)
{
    int i, first, cat_count, item_count;
    char buf[96];
    char key[32];

    build_categories();
    if (s_category < 0) s_category = 0;
    if (s_category >= s_category_count) s_category = s_category_count > 0 ? s_category_count - 1 : 0;
    cat_count = s_category_count;
    item_count = cat_count > 0 ? s_categories[s_category].count : 0;
    if (s_item < 0) s_item = 0;
    if (s_item >= item_count) s_item = item_count > 0 ? item_count - 1 : 0;

    for (i = 0; i < CM_W * CM_H; i++)
        s_panel[i] = 0xFF0F1118u;

    fill_rect(s_panel, 0, 0, CM_W, 46, 0xFF171B25u);
    draw_text(s_panel, 24, 14, "CHEATS", 0xFFFFD24Du, 2);
    host_keymap_label(HOST_KEYMAP_CHEATS_MENU, key, sizeof(key));
    snprintf(buf, sizeof(buf), "%s MENU", key[0] ? key : "F11");
    draw_text(s_panel, 456, 18, buf, 0xFFB8BDC8u, 1);

    /* Category bar. */
    fill_rect(s_panel, 0, 46, CM_W, 30, 0xFF13161Fu);
    if (cat_count > 0) {
        snprintf(buf, sizeof(buf), "< %s (%d/%d) >",
                 s_categories[s_category].name, s_category + 1, cat_count);
    } else {
        snprintf(buf, sizeof(buf), "NO CHEATS AVAILABLE");
    }
    draw_text(s_panel, 24, 56, buf, 0xFFE2E5EBu, 1);

    /* Scroll window: keep one row above the selection visible, like the
     * save-state menu's slot windowing. */
    first = s_item - 1;
    if (first < 0) first = 0;
    if (first > item_count - CM_VISIBLE_ROWS)
        first = item_count - CM_VISIBLE_ROWS;
    if (first < 0) first = 0;

    for (i = first; i < first + CM_VISIBLE_ROWS && i < item_count; i++) {
        int visible = i - first;
        int y = CM_LIST_Y + visible * (CM_ROW_H + CM_ROW_GAP);
        int sel = (i == s_item);
        int abs_index = s_categories[s_category].start + i;
        int enabled = psx_cheats_is_enabled(abs_index);
        const PsxCheatDef *def = psx_cheats_get(abs_index);
        uint32_t bg = sel ? 0xFF2B2830u : 0xFF191D27u;
        uint32_t fg = sel ? 0xFFFFD24Du : (enabled ? 0xFF8BE28Bu : 0xFFE2E5EBu);

        fill_rect(s_panel, CM_LIST_X, y, CM_LIST_W, CM_ROW_H, bg);
        stroke_rect(s_panel, CM_LIST_X, y, CM_LIST_W, CM_ROW_H,
                    sel ? 0xFFFFD24Du : 0xFF303746u);
        draw_checkbox(s_panel, CM_LIST_X + 8, y + 6, enabled);
        if (def)
            draw_text(s_panel, CM_LIST_X + 34, y + 10, def->name, fg, 1);
    }

    if (item_count > CM_VISIBLE_ROWS) {
        snprintf(buf, sizeof(buf), "%02d-%02d / %02d",
                 first + 1,
                 (first + CM_VISIBLE_ROWS < item_count) ? first + CM_VISIBLE_ROWS : item_count,
                 item_count);
        draw_text(s_panel, CM_LIST_X, CM_LIST_Y - 10, buf, 0xFF7F8796u, 1);
    }

    fill_rect(s_panel, 0, 442, CM_W, 38, 0xFF171B25u);
    draw_text(s_panel, 24, 454,
              "UP/DOWN: SELECT  LEFT/RIGHT: CATEGORY  ENTER: TOGGLE  ESC: BACK",
              0xFFB8BDC8u, 1);
    s_dirty = 0;
}

void psx_cheats_menu_set_state(int open, int category, int item)
{
    build_categories();
    if (category < 0) category = 0;
    if (category >= s_category_count) category = s_category_count > 0 ? s_category_count - 1 : 0;
    if (item < 0) item = 0;
    if (s_open != (open ? 1 : 0) || s_category != category || s_item != item)
        s_dirty = 1;
    s_open = open ? 1 : 0;
    s_category = category;
    s_item = item;
}

int psx_cheats_menu_needs_present(void)
{
    return s_open;
}

int psx_cheats_menu_overlay_image(const uint32_t **pixels, int *w, int *h)
{
    if (!s_open) {
        if (pixels) *pixels = NULL;
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
    /* Toggling a checkbox changes the ENABLED column without necessarily
     * moving the cursor, so repaint every frame while open rather than only
     * on cursor movement -- same reasoning as the texpack menu's ACTIVE
     * row. Cheap: CM_W*CM_H fills, no decode/IO. */
    (void)s_dirty;
    rasterize_panel();
    if (pixels) *pixels = s_panel;
    if (w) *w = CM_W;
    if (h) *h = CM_H;
    return 1;
}
