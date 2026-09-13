#ifndef PSX_CHEATS_MENU_H
#define PSX_CHEATS_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GameShark cheat browser: a full-screen overlay (sibling of
 * psx_savestate_menu.h / psx_texpack_menu.h, same rasterize-to-ARGB8888
 * pattern) that groups psx_cheats_db()'s entries by category and lets the
 * player check/uncheck individual codes. Toggling calls
 * psx_cheats_set_enabled() immediately (effective next VBlank, see
 * psx_cheats_apply_all()) and persists to disk right away, matching how the
 * texpack menu applies a selection instantly rather than needing a separate
 * confirm step. */

/* Number of categories psx_cheats_db() groups into, and how many cheats sit
 * in one category -- callers (main.cpp's navigation state machine) use
 * these to clamp category/item indices without duplicating the grouping
 * logic this module already computes from the database. */
int psx_cheats_menu_category_count(void);
int psx_cheats_menu_category_item_count(int category);
const char *psx_cheats_menu_category_name(int category);
/* Absolute index into psx_cheats_db() for (category, item), or -1. */
int psx_cheats_menu_absolute_index(int category, int item);

void psx_cheats_menu_set_state(int open, int category, int item);
int  psx_cheats_menu_needs_present(void);
int  psx_cheats_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_CHEATS_MENU_H */
