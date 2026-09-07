#ifndef PSX_TEXPACK_MENU_H
#define PSX_TEXPACK_MENU_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* HD texture-replacement pack switcher: a small full-screen overlay (sibling
 * of psx_savestate_menu.h, same rasterize-to-ARGB8888 pattern) listing the
 * three modes gpu_hd_texture_set_backend supports -- Original (no HD pack,
 * listed first), DuckStation-format, and Beetle PSX HW-format -- with each
 * pack's loaded entry counts, so a player can flip between two differently-
 * sourced community texture packs (or back to the untouched original PS1
 * textures, a safety net if a pack has a bad entry) without editing
 * settings.toml or a TCP command. Switching takes effect on the very next
 * drawn frame -- PS1 rendering redraws the whole scene every frame, so there
 * is no stale-frame texture to explicitly invalidate; each backend's own
 * upload tracker stays fed regardless of which is selected (gpu.c's
 * gp0_commit_cpu_to_vram).
 * selected_backend is a menu SLOT (0=Original, 1=DuckStation, 2=Beetle) --
 * the caller's display/cursor order -- not gpu_hd_texture_set_backend's own
 * value (0=duckstation, 1=beetle, 2=none); the two are deliberately
 * different so Original can be listed first without renumbering the
 * on-disk/wire backend value. See main.cpp's texpack_backend_to_slot /
 * kTexpackSlotToBackend for the mapping. */
void psx_texpack_menu_set_state(int open, int selected_backend);
int  psx_texpack_menu_needs_present(void);
int  psx_texpack_menu_overlay_image(const uint32_t **pixels, int *w, int *h);

#ifdef __cplusplus
}
#endif

#endif /* PSX_TEXPACK_MENU_H */
