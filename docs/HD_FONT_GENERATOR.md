# HD font generator

`tools/generate_font_pack.py` regenerates the dialogue/menu font in the
Beetle-format HD texture pack (`f98b3634-*.png`, the four palette variants
of the shared font texture) using any TrueType font installed on the
machine, instead of vierock's stylized gothic font that ships with that
pack.

## In-game menu

The usual way to use this: open the texture-pack menu (F10 by default,
`HOST_KEYMAP_TEXPACK_MENU`), select **BEETLE PSX HW FORMAT**, and press
**F** — this opens a scrollable list of every font installed on the machine
(psx_font_picker_menu.c/.h). Arrow keys move the cursor (Page Up/Down jump
a full page), ENTER regenerates the font pack using the selected font and
hot-reloads it live (no relaunch — see `gpu_hd_texture_pack_reload_paths`
in `gpu.c` / `gl_renderer_hd_tex_reload` in `gpu_gl_renderer.c` for how the
GL texture cache eviction works), ESC backs out. Generation takes a
second or two; the menu shows "GENERATING TEXTURES, PLEASE WAIT..." while
the child `python` process runs, then either "FONT UPDATED: \<name\>" or a
failure message in the footer.

This spawns `python` as a child process (`generate_font_pack.py --bold 2
--pack-dir <kFontPackDir>`, both hardcoded to this dev machine's layout in
`main.cpp`, same as the script's own `DEFAULT_PACK_DIR`) and requires
python3 + Pillow to be installed and on `PATH` — see "Why a whole tool
instead of just editing the PNGs" below for why that dependency was a
deliberate trade rather than reimplementing TrueType rasterization in C++.

## CLI usage

```
python tools/generate_font_pack.py --font Inkfree.ttf --bold 2
```

This writes the 4 regenerated PNGs into the current directory, installs
them into the HD texture pack directory (`--pack-dir`, defaults to
`D:\RecompWork\Sources\Vagrant Story (USA)-texture-replacements`), and
deletes the corresponding stale entries under the pack's `processed/`
disk-cache folder so the game regenerates its compressed cache for just
those 4 files on next launch (see `gpu_hd_texture_preload_active()` in
`gpu_gl_renderer.c` for how that cache works). Relaunch the game to see
the result.

Pass `--no-install` to only write the 4 PNGs into `--out-dir` for review
before copying them into the pack yourself.

`--font` accepts a bare font name (resolved by filename against
`C:\Windows\Fonts`, e.g. `"Comic Sans MS"` or `Inkfree.ttf`) or a full
path to any `.ttf`/`.otf`/`.ttc` file. Other useful flags:

- `--bold N` — stroke width used to fake a bold weight by outlining each
  glyph (default 2). Most single-weight fonts (Ink Free, Comic Sans, etc.)
  look better bolded since the pack's other art is fairly heavy; set to 0
  for fonts that are already bold, or that look worse thickened.
- `--size N` — font size in px (default 40; each glyph cell is 48x48).
- `--shear F` — horizontal shear used to fake italics for the font's
  italic-block glyphs, since most fonts only ship one weight (default
  0.22; pass 0 to keep that block upright too).
- `--fallback-font` — used for any character the main font's glyph table
  is missing (default Arial Bold). In practice only three math symbols
  (≠ ≤ ≥) are ever missing from a normal text font.

## Why a whole tool instead of just editing the PNGs

The font page isn't laid out in a way you can eyeball once and trust —
see below. `generate_font_pack.py` bakes in the character-to-cell mapping
that was reverse-engineered empirically (by installing a guess, relaunching,
and reading the actual in-game text), so swapping fonts is just "run the
script again with a different `--font`" instead of redoing that mapping
work by hand.

## Layout of the font page

The original SYSTEM.DAT font resource (`tools/extract_original_font.py`
extracts it straight from a disc image) packs two full character sets on
a 12x12-native-px grid, 22 columns wide, at 4x scale (48x48 px/cell) in
the HD replacement textures:

| Rows  | Content |
|-------|---------|
| 0-2   | upright digits + uppercase + lowercase |
| 3-5   | upright accented Latin-1 supplement (French/German/etc.) |
| 6-7   | upright ASCII punctuation |
| 8     | UI icons (▲ □ ○ ✕ arrows ★ ■ etc.) — never touched, no font has these |
| 9-11  | italic mirror of rows 0-2 |
| 12-14 | italic mirror of rows 3-5 |
| 15-17 | italic mirror of rows 6-7 (sparser) |

The atlas's left-to-right visual order does **not** reliably match the
game's own character-code table — during development, a "!" placed by
its apparent shape (a plain vertical bar in row 6) rendered in-game as
"/" instead, and swapping the two cells based on that one data point
*also* didn't fix it on the first attempt (a transient file-lock issue
on the freshly-written PNGs silently made an install a no-op — always
verify an md5 of the installed file matches what was just generated
before concluding a mapping is wrong). The mapping that's actually baked
into `generate_font_pack.py` was confirmed by installing a change and
reading the real dialogue text in-game, not by visual inspection alone —
if you extend the character map, verify the same way rather than trusting
the atlas layout at face value.
