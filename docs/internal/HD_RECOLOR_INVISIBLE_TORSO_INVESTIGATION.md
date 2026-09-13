# Live-GPU-recolor: invisible torso investigation (texture_hash `bcf02fd0`)

**Date:** 2026-09-12, resolved 2026-09-13
**Status:** FIXED — root cause confirmed live via `hd_recolor_diag` and
corrected in `gpu_gl_renderer.c`. See "Resolution" at the bottom.

## Background

This session added the live-GPU-recolor fallback path (`HD_RECOLOR_FS` /
`draw_hd_recolor_triangle` in `gpu_gl_renderer.c`, `compute_recolor_table` in
`hd_texture_pack.cpp`): when a texture's exact `(texture_hash, palette_hash)`
match misses, but the texture_hash has a recolorable "master" bundle
(`masters/<texture_hash>/{hd.png,index.png,palette.bin}`), the shader recolors
the master live against the CURRENT palette instead of falling back to native.

Three real bugs were found and fixed earlier in this session:

1. **Masters-data mismatch** — `write_master_bundle()` picked the wrong
   `hd.png` candidate (didn't match the recorded palette_hash). Fixed in
   `export_hd_pack.py`; whole pack re-validated/repaired
   (`validate_master_consistency.py` / `fix_master_hd_mismatch.py`), 995/995
   bundles now consistent.
2. **Recolor-table truncation** — `compute_recolor_table()` only wrote table
   entries up to `ref_palette_count` instead of the full legal range (16 or
   256), leaving GPU lookup-texture columns with stale data from a previous
   draw. Fixed by always sizing the table to 16/256 with an identity/live-alpha
   fallback for out-of-range indices. This fixed `83059c83` (a character that
   was rendering red instead of gray, with a transparent shield).
3. **Redundant/wrong native mask-bit (STP) gate** — `HD_RECOLOR_FS` used to
   ALSO require the original native texel's mask bit (`stp`) to agree with
   `scale.a` before showing a pixel, ported verbatim from `HD_FS`'s own parity
   fix. But `HD_FS` needs that check because a replacement PNG's alpha has no
   relation to the real PS1 mask bit — this recolor path's `scale.a` is
   *already* derived from the live CLUT's own "raw word == 0" check, the exact
   same rule native `TEX_FS`'s `fetch_texel` applies. Requiring `stp` on top
   was redundant at best, and for a character whose real content legitimately
   sets the mask bit on most of its own texels (**this is `bcf02fd0`**), it
   discarded almost everything. Fixed by removing the `u_vram`/`u_orig_*`
   machinery from `HD_RECOLOR_FS` entirely.

## The remaining bug

After fix #3, `bcf02fd0` (confirmed to be Ashley's face/head — visible and
correct) is still fully invisible for **the torso/dress** in the "Ashley!!"
cutscene (young woman hiding behind a pillar with a male character). Confirmed
via a same-frame, same-pose comparison using the F2 (native)/F4 (Beetle HD)
debug hotkeys (`main.cpp` ~line 7397: F2=none, F3=DuckStation, F4=Beetle) — F2
shows her torso/dress fully rendered; F4 shows the exact same pixels as fully
transparent (the wall behind her, drawn earlier in painter's order, shows
through where her dress should be).

### What's been ruled out

- **Alpha-table content is correct.** Added a live debug bypass
  (`u_silhouette_mode` uniform wired into `HD_RECOLOR_FS`, reusing the
  existing F1/`hd_silhouette_mode` debug toggle already used by `HD_FS`): mode
  1 skips the `scale.a < 0.5` discard while keeping real sampled color. With
  this on, her torso appeared with plausible, roughly-correct color/shape —
  proving the master (`hd.png`/`index.png`), UV mapping, and RGB scale/offset
  correction are all fundamentally right. Only the pixels are being wrongly
  discarded (or something downstream of the shader's output is hiding them).
- **Not a bad-index-value bug.** Extended `hd_recolor_diag` (see below) to
  list which specific palette indices come out `scale.a < 0.5` (transparent).
  For `bcf02fd0` at clut (768,233) depth 1: only indices `0, 160, 208-229` are
  transparent. The master's own `index.png` (128x128, mode L) only ever
  contains values `0-130` (confirmed by direct inspection with PIL) — so
  every index her dress mesh could possibly sample is marked OPAQUE in the
  live table. The alpha-table logic is not the cause.
- **Table upload/GPU-texture-format bug also looks clean.** The scale/offset
  table is a plain `glTexSubImage2D` into a `GL_RGBA32F` texture, freshly
  re-uploaded every draw call (no caching/staleness possible), sampled with
  `texelFetch` (so no filtering/interpolation could corrupt the binary alpha
  signal even if the sampler weren't already integer-exact).
- **Not a semi-transparency fallthrough.** The whole HD/recolor code block is
  gated on `semi < 0` (opaque prims only; semi-transparent prims fall straight
  to the native path, untouched by any of this — see the `draw_hd_replacement_
  triangle` header comment). `bcf02fd0` IS showing up in `hd_recolor_diag`
  (which only records entries that actually went through
  `draw_hd_recolor_triangle`), so this specific draw call for the torso is
  taking the opaque recolor path, not silently falling through to some
  separate semi-transparent handler.

### Current working theory (not yet confirmed)

`draw_hd_recolor_triangle` computes two independent clamp windows per
triangle from `orig_texinfo`'s `u_first/v_first/u_last/v_last` (which
ultimately come from `hd_texture_pack_match`'s miss-branch anchor search,
itself NEW code added this session):

- `u_index_limits` (`ix0,iy0,ix1,iy1`) — clamps the per-pixel index-texture
  lookup (`itex = clamp(ivec2(floor(uv * u_index_size)), u_index_limits.xy,
  u_index_limits.zw)` in `HD_RECOLOR_FS`).
- `u_prim_limits` (`px0,py0,px1,py1`) — same idea for the `hd.png` color
  sample.

**If either window comes out inverted for one specific triangle** (e.g.
`ix0 > ix1` because the miss-branch anchor search picked a wrong/degenerate
`u_first`/`u_last` for that triangle specifically), GLSL's `clamp(x, minVal,
maxVal)` with `minVal > maxVal` is defined as `min(max(x,minVal),maxVal)`,
which collapses to a single FIXED value (`maxVal`) regardless of the real
`uv` — meaning the WHOLE triangle would sample one wrong, constant palette
index no matter what it should actually show. If that fixed index lands in
the transparent band (`0`, `160`, `208+`), the whole triangle vanishes. This
would explain why the face (presumably a separate, correctly-computed
triangle/anchor) renders fine while the torso (a different triangle, same
texture_hash, same master) doesn't.

This has NOT been confirmed yet — it's a plausible mechanism consistent with
every ruled-out alternative above, not a proven cause.

## Diagnostics added this session (still in the tree, useful for next pass)

- **`hd_silhouette_mode on=1`** (debug_client.py passthrough) — bypasses the
  `scale.a < 0.5` discard in `HD_RECOLOR_FS` (and the equivalent in `HD_FS`)
  while keeping real sampled color, for visually checking "is something
  actually being computed here, just wrongly hidden?" without a rebuild.
  `on=0` restores normal discard behavior. Also bound to the F1 hotkey in
  `main.cpp` for modes 0/2 only (not mode 1 — mode 1 is debug_client-only for
  now).
- **`hd_recolor_diag`** (debug_client.py passthrough) — dumps, per distinct
  `texture_hash` currently drawn through the recolor path: `clut_x/clut_y/
  depth/palette_count/opaque_count/transparent_indices` (first 24 indices
  whose `scale.a < 0.5`) AND (as of this investigation) `index_limits`
  (`ix0,iy0,ix1,iy1`) / `prim_limits` (`px0,py0,px1,py1`) from the MOST
  RECENT draw of that texture_hash. **Caveat:** entries are deduplicated by
  `texture_hash` alone — if the same texture_hash is drawn more than once per
  frame (e.g. face and torso both using `bcf02fd0`) with DIFFERENT limits,
  only the last draw's limits survive in the dump. If the two mesh parts are
  drawn back-to-back, querying right after reaching the scene should still
  catch whichever was drawn last; if that turns out to be the face (not the
  torso), the dedup will need to become keyed by something finer (e.g. also
  include a running draw-order counter or the actual `orig_texinfo` rect) to
  see both entries distinctly.
- Handler buffer for `hd_recolor_diag` was bumped from `char buf[2048]` to
  `char buf[16384]` in `debug_server.c` (`handle_hd_recolor_diag`) — the
  per-index arrays made the old size truncate mid-JSON.

## Next step

1. Rebuild, relaunch, reach the "Ashley!!" cutscene (same scene as
   `bcf02fd0`'s repro — she's hiding behind a pillar with a male character,
   dialogue reads "ASHLEY!!").
2. Run `python psxrecomp/tools/debug_client.py hd_recolor_diag` and inspect
   `bcf02fd0`'s `index_limits`/`prim_limits`. If `ix0 > ix1` or `iy0 > iy1`
   (or same for `px0/px1`, `py0/py1`), that confirms the inverted-clamp
   theory — next fix the miss-branch anchor search in
   `hd_texture_pack_match()` (or wherever `orig_texinfo`'s `u_first/u_last/
   v_first/v_last` are actually sourced for this triangle) to never emit an
   inverted range, and/or make `draw_hd_recolor_triangle` defensively swap the
   bounds if inverted before uploading them as uniforms.
3. If the limits look fine (not inverted), the dedup caveat above becomes the
   next blocker — extend the diagnostic to distinguish multiple draws of the
   same texture_hash per frame before continuing.

## Resolution (2026-09-13)

Confirmed live via `hd_recolor_diag` on the exact repro (slot04 savestate,
"Ashley!!" scene, Beetle backend): `bcf02fd0`'s `index_limits` came back
`[40, 213, 49, 127]` — `iy0=213 > iy1=127`, the inverted-clamp signature the
"Current working theory" section above predicted almost exactly.

Two distinct bugs were found and fixed in `draw_hd_recolor_triangle`
(`gpu_gl_renderer.c`), both in the `u_index_limits`/`u_prim_limits`
computation:

1. **Asymmetric clamping produced the inversion.** The clamp only ever
   restricted `ix0`/`iy0`'s LOWER bound (`< 0`) and `ix1`/`iy1`'s UPPER bound
   (`> native_w/h - 1`) — never `ix0`/`iy0`'s upper bound or `ix1`/`iy1`'s
   lower bound. When the whole raw range sat entirely above `native_h - 1`
   (as it did here — the master's `index.png` is 128x128, and this
   triangle's real `v_first`/`v_last` were far above 127), `iy0` kept its
   raw, unclamped, too-large value while `iy1` got pulled down to 127,
   inverting the pair. Fixed by clamping BOTH bounds of BOTH `iy0` and `iy1`
   (and `ix0`/`ix1`, and the matching `px0`/`px1`/`py0`/`py1` in the sibling
   `u_prim_limits` block, and in `draw_hd_replacement_triangle`'s own
   `u_prim_limits` block, which had the identical pattern) into
   `[0, native_w/h - 1]` — clamp is monotonic, so clamping both bounds of
   both values to the same range guarantees `lo <= hi` holds no matter how
   far out of range the raw values are.
2. **The real root cause: `index_limits` was missing the anchor-upload
   remap entirely.** Fixing (1) alone made the range valid again but
   degenerate (`[40, 127, 49, 127]` — both `iy0` and `iy1` pinned to the
   same edge row), and the torso was STILL invisible after rebuilding,
   because clamping an out-of-range window can only ever produce a
   single-row/column result, not the real content. The actual bug: this
   block treated `orig_texinfo`'s raw `u_first/v_first/u_last/v_last` as
   already being coordinates within the master's own `index.png` space,
   which is only true when this triangle's own upload IS the master's
   anchor upload (the exact-match case). In the miss/live-recolor-fallback
   case (why `draw_hd_recolor_triangle` runs at all), the anchor is a
   DIFFERENT upload sharing the same `texture_hash`, and
   `gpu_hd_texture_pack_match`'s miss branch already computes
   `u_scale`/`u_offset`/`v_scale`/`v_offset` to remap into that anchor's own
   space — the sibling `u_prim_limits` block a few lines below already
   applies this remap (scaled by `hd_w`/`hd_h`) and was reporting a valid,
   non-degenerate window (`[160,340]-[196,420]`) the whole time, proving the
   remap itself was correct. Fixed by applying the identical affine remap to
   `index_limits`, just scaled by `native_w`/`native_h` (the index map's own
   resolution) instead of `hd_w`/`hd_h` (the upscaled replacement image's
   resolution).

After both fixes: `index_limits` came back `[40, 85, 49, 105]` — valid,
non-degenerate, and proportionally consistent with `prim_limits`
(340/4=85, 420/4=105, confirming the master's 4x upscale factor). Verified
live: Ashley's torso/dress renders fully and correctly under the Beetle
backend, matching native — screenshotted side-by-side on the identical
paused dialogue frame (slot04 savestate), byte-diff confined to expected
HD-vs-native texture detail differences (0.9% of pixels, no coverage
holes), not the previous full-torso blackout.
