#ifndef PSX_FONT_PICKER_MENU_H
#define PSX_FONT_PICKER_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HD-font-pack generator: a small full-screen overlay, opened from within
 * the texture-pack menu (psx_texpack_menu.h) when the Beetle PSX HW format
 * row is selected, listing every TrueType font installed on the machine
 * (C:\Windows\Fonts) so the player can pick one and regenerate the
 * dialogue/menu font (generate_font_pack.py) without leaving the game.
 * Same rasterize-to-ARGB8888 pattern as its siblings; unlike them, one
 * action (ENTER) can take up to a second or two (spawns the Python
 * generator as a child process and waits for it), hence the separate
 * `busy`/`status_line` state -- the overlay shows "GENERATING..." while a
 * generation is in flight instead of looking frozen with no feedback.
 *
 * `names`/`visible_count` is the already-scrolled SLICE to draw this frame
 * (at most 16 entries -- the caller keeps a cursor into the full font list
 * and windows it before calling this; this module has no idea how long the
 * full list is). `cursor_in_view` selects which of those visible rows is
 * highlighted (0..visible_count-1). `scroll_top`/`total_count` are only
 * used for the "N/TOTAL" position readout, not for indexing `names`.
 * `status_line` (nullable) replaces the footer's key-hint text with a
 * result message ("FONT UPDATED: Ink Free", "GENERATION FAILED (see
 * console)") after ENTER completes, until the cursor next moves.
 *
 * `bold_on`/`italic_on` reflect the B/I toggle state (shown as checkboxes
 * in the header) -- one font family collapses every weight variant
 * Windows ships as a separate file (regular/bold/italic/bold-italic) into
 * one row, and these two toggles pick which weight to actually use when
 * generating, instead of listing all four as separate rows. */
void psx_font_picker_menu_set_state(int open, int busy, int cursor_in_view,
                                     const char *const *names, int visible_count,
                                     int scroll_top, int total_count,
                                     int bold_on, int italic_on,
                                     const char *status_line);
int  psx_font_picker_menu_needs_present(void);
int  psx_font_picker_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FONT_PICKER_MENU_H */
