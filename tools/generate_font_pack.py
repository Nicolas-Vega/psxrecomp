#!/usr/bin/env python3
"""Regenerate the Vagrant Story dialogue/menu font using any installed
TrueType font, instead of vierock's stylized gothic HD texture pack font.

Replaces this exact glyph set (letters, digits, the full Latin-1 accented
range, standard ASCII punctuation, and row 8's UI icons -- controller
face buttons, arrows, star, music note, etc, mapped to Unicode
equivalents) across all four palette variants of the shared font texture
("f98b3634-*.png" in the Beetle-format HD texture pack), while leaving
every other cell -- a couple of unclear accent marks and blank padding --
exactly as it already is in the pack.

The SYSTEM.DAT font page (see extract_original_font.py) packs TWO full
character sets on a 12x12-native-px / 22-column grid, at 4x scale (48x48
px per cell) in the HD replacement textures:
  - rows 0-2:  upright digits + uppercase + lowercase
  - rows 3-5:  upright accented Latin-1 supplement (French/German/etc.)
  - rows 6-7:  upright ASCII punctuation
  - row 8:     UI icons (face buttons/arrows/star/etc, see PUNCT_ROWS[8])
  - rows 9-11:  italic mirror of rows 0-2
  - rows 12-14: italic mirror of rows 3-5
  - rows 15-17: italic mirror of rows 6-7 (sparser)
This layout was mapped out empirically (session notes in
docs/HD_FONT_GENERATOR.md) and is independent of which font renders it --
only the glyphs drawn into each cell change when you pick a different font.

Each variant is rendered directly in its target ink color (the font atlas
convention here is "flat color + alpha as coverage", not the PS1 CLUT's
"gray level encodes coverage" trick used for reverse-engineered VRAM data --
these are fresh vector glyphs, so plain alpha compositing is correct and
much simpler).

Usage:
    python generate_font_pack.py --font Inkfree.ttf --bold 2
    python generate_font_pack.py --font "Comic Sans MS" --bold 0 --no-install

Any font name/path Pillow's ImageFont.truetype can open works; bare names
are resolved against C:\\Windows\\Fonts. ANY character the chosen font
lacks a real glyph for (detected per-font, per-character -- see
font_has_glyph) automatically falls back to Arial Bold instead of being
left as native pixel art, so every font choice ends up fully covering the
character set regardless of its own coverage gaps.

By default this writes straight into --pack-dir and deletes the 4 stale
cache entries under its processed/ subfolder so the game picks up the
change on next launch. Pass --no-install to only write the 4 PNGs into
the current directory for review first.
"""
import argparse
import glob
import hashlib
import os
import time
from PIL import Image, ImageDraw, ImageFont

DEFAULT_PACK_DIR = r"D:\RecompWork\Sources\Vagrant Story (USA)-texture-replacements"
WINDOWS_FONTS_DIR = r"C:\Windows\Fonts"
CELL = 48
CANVAS_SIZE = (1024, 896)

NORMAL_ROWS = {
    0: "0123456789ABCDEFGHIJK",
    1: "LMNOPQRSTUVWXYZabcdef",
    2: "ghijklmnopqrstuvwxyz\u0152",  # ...Œ
    3: "\u00C0\u00C1\u00C2\u00C4\u00C7\u00C8\u00C9\u00CA\u00CB\u00CC\u00CD\u00CE\u00CF\u00D2\u00D3\u00D4\u00D6\u00D9\u00DA\u00DB\u00DC",
    4: "\u00DF\u0153\u00E0\u00E1\u00E2\u00E3\u00E7\u00E8\u00E9\u00EA\u00EB\u00EC\u00ED\u00EE\u00EF\u00F2\u00F3\u00F4\u00F6\u00F9\u00FA",
    5: "\u00FB\u00FC",
}
PUNCT_ROWS = {
    # col8 = low double quote „, col19 = the real apostrophe actually used
    # in-game for contractions ("Got 'er open?", "No' even").
    # "!" sits directly above "?" (col18), "/" directly right of "." (col14).
    # col9 = "!!" (two exclamation marks joined).
    6: {8: "„", 9: "!!", 13: "÷", 14: "·", 15: "—", 16: "…", 18: "!",
        19: "’", 20: "#"},
    7: {0: "$", 1: "%", 2: "&", 3: "'", 4: "(", 5: ")", 6: "=", 7: "@",
        8: "[", 9: "]", 10: ";", 11: ":", 12: ",", 13: ".", 14: "/",
        15: "\\", 16: "<", 17: ">", 18: "?", 19: "_", 20: "-"},
    # Row 8 is UI icons, not letters -- every cell here routes through the
    # 3-tier font_has_glyph fallback chain (chosen font -> Arial Bold ->
    # Segoe UI Symbol) since ordinary text fonts (Ink Free included) have
    # none of these glyphs; col2 is a small "+"-shaped cursor/reticle icon,
    # distinct from col0's plain "+" only in the native art's size, mapped
    # the same since there's no dedicated Unicode glyph for it either.
    8: {0: "+", 1: "*", 2: "+", 3: "{", 4: "}", 5: "♪", 6: "△", 7: "▢",
        8: "◯", 9: "✕", 10: "←", 11: "→", 12: "↑", 13: "↓", 14: "Lv.",
        15: "★", 16: "■", 17: "~", 18: "▼", 19: "▼", 20: "▼"},
}

# Italic block mirrors the upright one 9 rows down.
ITALIC_OFFSET = 9
ITALIC_NORMAL_ROWS = {row + ITALIC_OFFSET: chars for row, chars in NORMAL_ROWS.items()}
ITALIC_PUNCT_ROWS = {
    15: {8: "„", 15: "—", 18: "!", 19: "’"},
    16: {3: "’", 4: "(", 5: ")", 8: "[", 9: "]", 10: ";", 11: ":", 12: ",",
         13: ".", 14: "/", 16: "<", 17: ">", 18: "?", 20: "-"},
    17: {0: "+"},
}

VARIANTS = {
    # This one draws over the light gray speech-bubble background, so it
    # needs dark ink; the other three draw over dark menu/HUD backgrounds
    # and need light ink.
    "f98b3634-876b1c17.png": (5, 5, 5),
    "f98b3634-4e0a4ea7.png": (255, 255, 255),
    "f98b3634-1443be53.png": (255, 255, 255),
    "f98b3634-d4354028.png": (255, 220, 40),
}


def build_char_map():
    """Returns {(row, col): (char, italic)}. Whether a cell ends up using
    --font or falling back to --fallback-font is decided per-font at
    render time (see font_has_glyph), not baked in here -- a glyph one
    font lacks another might have."""
    char_map = {}
    for row, chars in NORMAL_ROWS.items():
        for col, ch in enumerate(chars):
            char_map[(row, col)] = (ch, False)
    for row, mapping in PUNCT_ROWS.items():
        for col, ch in mapping.items():
            char_map[(row, col)] = (ch, False)
    for row, chars in ITALIC_NORMAL_ROWS.items():
        for col, ch in enumerate(chars):
            char_map[(row, col)] = (ch, True)
    for row, mapping in ITALIC_PUNCT_ROWS.items():
        for col, ch in mapping.items():
            char_map[(row, col)] = (ch, True)
    return char_map


def font_has_glyph(font, ch):
    """Heuristic: True if `font` has a real glyph for `ch`, False if it
    would just render FreeType's .notdef fallback box. Pillow doesn't
    expose a font's cmap directly, so this compares ch's rendered bitmap
    against a Private Use Area codepoint that's essentially guaranteed to
    be unmapped in any real font -- if the two render identically, ch is
    ALSO just showing .notdef (i.e. unsupported), regardless of what
    .notdef happens to look like in this particular font file."""
    probe = ""  # Private Use Area, essentially never mapped

    def render(c):
        # font.getmask() returns a low-level ImagingCore with no public
        # pixel-access API -- draw into a real Image instead, whose
        # tobytes() is stable public API.
        bbox = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), c, font=font)
        w, h = max(1, bbox[2] - bbox[0]), max(1, bbox[3] - bbox[1])
        im = Image.new("L", (w, h), 0)
        ImageDraw.Draw(im).text((-bbox[0], -bbox[1]), c, font=font, fill=255)
        return im.size, im.tobytes()

    if ch == probe:
        return True
    size_ch, bytes_ch = render(ch)
    size_probe, bytes_probe = render(probe)
    if size_ch != size_probe:
        return True
    return bytes_ch != bytes_probe


def resolve_font_path(name_or_path):
    if os.path.isfile(name_or_path):
        return name_or_path
    stem = os.path.splitext(name_or_path)[0].lower()
    candidates = glob.glob(os.path.join(WINDOWS_FONTS_DIR, "*"))
    for ext_pref in (".ttf", ".otf", ".ttc"):
        for path in candidates:
            base, ext = os.path.splitext(os.path.basename(path))
            if base.lower() == stem and ext.lower() == ext_pref:
                return path
    for path in candidates:
        base, _ext = os.path.splitext(os.path.basename(path))
        if stem in base.lower():
            return path
    available = sorted(os.path.basename(p) for p in candidates)[:15]
    raise SystemExit(
        f"Could not find a font matching {name_or_path!r} in {WINDOWS_FONTS_DIR}.\n"
        f"Some available fonts: {', '.join(available)} ..."
    )


def baseline_for(font):
    ascent, descent = font.getmetrics()
    return (CELL - (ascent + descent)) // 2 + ascent


def render_glyph_cell(ch, color, italic, f, by, stroke_width, shear):
    tile = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    draw = ImageDraw.Draw(tile)
    fill = (color[0], color[1], color[2], 255)
    bbox = draw.textbbox((0, by), ch, font=f, anchor="ls", stroke_width=stroke_width)
    tw = bbox[2] - bbox[0]
    x = (CELL - tw) / 2 - bbox[0]
    draw.text((x, by), ch, font=f, fill=fill, anchor="ls",
               stroke_width=stroke_width, stroke_fill=fill)
    if not italic:
        return tile
    # Fake italic via a horizontal shear, for fonts with no italic weight
    # (or when the caller doesn't want to bother loading one).
    if shear == 0:
        return tile
    return tile.transform(
        (CELL, CELL), Image.AFFINE, (1, shear, -shear * CELL / 2, 0, 1, 0),
        resample=Image.BICUBIC,
    )


def md5_of(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def install_with_retry(src_path, dst_path, attempts=3, delay=1.0):
    """Copy src over dst, retrying on the transient share-violation errors
    Windows occasionally throws on freshly-written files (AV/indexer scan),
    and verifying the destination's hash matches afterward."""
    last_err = None
    for _ in range(attempts):
        try:
            with open(src_path, "rb") as f:
                data = f.read()
            with open(dst_path, "wb") as f:
                f.write(data)
            if md5_of(dst_path) == hashlib.md5(data).hexdigest():
                return True
        except OSError as e:
            last_err = e
        time.sleep(delay)
    print(f"  WARNING: failed to verify install of {dst_path} ({last_err})")
    return False


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--font", required=True,
                         help="TrueType font name (resolved against C:\\Windows\\Fonts) or full path")
    parser.add_argument("--fallback-font", default="arialbd.ttf",
                         help="Font used for glyphs --font lacks (default: Arial Bold)")
    parser.add_argument("--symbol-fallback-font", default="seguisym.ttf",
                         help="Third-tier font used for glyphs BOTH --font and --fallback-font "
                              "lack -- Arial Bold has no geometric-shape glyphs (the PS1 "
                              "controller-button icons among them), Segoe UI Symbol does "
                              "(default: seguisym.ttf)")
    parser.add_argument("--bold", type=int, default=2,
                         help="Stroke width in px used to synthesize bold for --font "
                              "(0 disables; use 0 if the font already looks bold/is a real bold weight)")
    parser.add_argument("--size", type=int, default=40, help="Font size in px (cell is 48x48)")
    parser.add_argument("--shear", type=float, default=0.22,
                         help="Horizontal shear used to fake italics for the italic block (0 disables)")
    parser.add_argument("--force-italic", action="store_true",
                         help="Apply --shear to EVERY cell, not just the italic-block rows -- "
                              "for when --font has no real italic weight but italic was still "
                              "requested (mirrors how --bold already fakes bold either way)")
    parser.add_argument("--pack-dir", default=DEFAULT_PACK_DIR,
                         help="HD texture pack directory containing the f98b3634-*.png files")
    parser.add_argument("--out-dir", default=".",
                         help="Where to write the 4 generated PNGs before installing")
    parser.add_argument("--no-install", action="store_true",
                         help="Only write to --out-dir; don't copy into --pack-dir or touch its cache")
    args = parser.parse_args()

    font_path = resolve_font_path(args.font)
    fallback_path = resolve_font_path(args.fallback_font)
    symbol_path = resolve_font_path(args.symbol_fallback_font)
    print(f"Using font: {font_path}")
    print(f"Fallback font: {fallback_path}")
    print(f"Symbol fallback font: {symbol_path}")

    # Three-tier chain, in priority order: the chosen font, then Arial Bold
    # (covers ordinary text-font gaps like math symbols), then Segoe UI
    # Symbol (covers geometric shapes/dingbats -- e.g. Arial Bold has no
    # glyph at all for U+25A2 WHITE SQUARE WITH ROUNDED CORNERS, which one
    # of the controller-button icons needs, but Segoe UI Symbol does).
    tiers = [
        (ImageFont.truetype(font_path, args.size), args.bold),
        (ImageFont.truetype(fallback_path, args.size), 0),
        (ImageFont.truetype(symbol_path, args.size), 0),
    ]
    tier_baselines = [baseline_for(f) for f, _stroke in tiers]
    char_map = build_char_map()

    # Decide once per cell (not per variant/color -- coverage doesn't
    # depend on ink color) which tier renders it: the first one that has a
    # REAL glyph for every character in the cell (a multi-char cell like
    # "!!" falls back as a unit if ANY of its characters are missing, so
    # the glued pair never mixes two different fonts' metrics), or the
    # last tier regardless if none fully cover it (best effort beats blank).
    tier_choice = {}
    tier_counts = [0, 0, 0]
    for key, (ch, _italic) in char_map.items():
        chosen = len(tiers) - 1
        for i, (f, _stroke) in enumerate(tiers):
            if all(font_has_glyph(f, c) for c in ch):
                chosen = i
                break
        tier_choice[key] = chosen
        tier_counts[chosen] += 1
    if tier_counts[1] or tier_counts[2]:
        print(f"{tier_counts[1]} cell(s) fell back to {fallback_path}, "
              f"{tier_counts[2]} to {symbol_path} (missing from higher tiers)")

    os.makedirs(args.out_dir, exist_ok=True)
    generated = []
    for fname, color in VARIANTS.items():
        src = os.path.join(args.pack_dir, fname)
        canvas = Image.open(src).convert("RGBA")
        assert canvas.size == CANVAS_SIZE, (fname, canvas.size)
        replaced = 0
        for (row, col), (ch, italic) in char_map.items():
            tier = tier_choice[(row, col)]
            f, stroke_width = tiers[tier]
            by = tier_baselines[tier]
            effective_italic = italic or args.force_italic
            tile = render_glyph_cell(ch, color, effective_italic, f, by, stroke_width, args.shear)
            px, py = col * CELL, row * CELL
            if px + CELL > canvas.width or py + CELL > canvas.height:
                continue
            canvas.paste(Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0)), (px, py))
            canvas.paste(tile, (px, py), tile)
            replaced += 1
        out_path = os.path.join(args.out_dir, fname)
        canvas.save(out_path)
        generated.append((fname, out_path))
        print(f"wrote {out_path} ({replaced} cells replaced)")

    if args.no_install:
        print("--no-install passed: not touching --pack-dir. Review the files above, then rerun without it.")
        return

    print(f"Installing into {args.pack_dir} ...")
    ok = True
    for fname, out_path in generated:
        dst = os.path.join(args.pack_dir, fname)
        if install_with_retry(out_path, dst):
            print(f"  OK {fname}")
        else:
            ok = False

    cache_dir = os.path.join(args.pack_dir, "processed")
    removed = 0
    for fname, _ in generated:
        cache_file = os.path.join(cache_dir, os.path.splitext(fname)[0] + ".bin")
        if os.path.exists(cache_file):
            os.remove(cache_file)
            removed += 1
    print(f"Invalidated {removed} stale cache file(s) under {cache_dir}")
    if not ok:
        raise SystemExit("One or more files failed to install -- see warnings above.")
    print("Done. Relaunch the game to see the new font.")


if __name__ == "__main__":
    main()
