# HD replacement shader parity audit (TEX_VS/TEX_FS vs HD_VS/HD_FS)

**Date:** 2026-09-09
**Why this exists:** the perspective-correction gap (see below, and ISSUES.md
Issue #12) was found by accident, from a user report of a wavy floor texture,
after an entire investigation session had already concluded the two render
paths were geometrically identical. That conclusion was correct as far as it
went (a ground-truth calibration test proved vertex POSITIONING math matches
exactly), but it never checked whether every OTHER per-vertex/per-pixel
FEATURE of the native path was also mirrored in the HD path. This document is
that missing systematic check: every attribute, uniform, varying, and
fragment-shader code path in native `TEX_VS`/`TEX_FS` (`gpu_gl_renderer.c`),
checked one by one against `HD_VS`/`HD_FS`, with a verdict for each.

Only textured, opaque prims are in scope for `HD_VS`/`HD_FS` at all — the HD
replacement pipeline is gated to `semi < 0` (see `gpu_textured_triangle`'s
comment above the HD-match block) and only ever draws through
`draw_hd_replacement_triangle`, never through the batched `GEO_*` (flat-color)
programs. Anything that only matters for flat-color or semi-transparent prims
is out of scope by construction, not a gap.

## Verdict legend

- ✅ **MATCHED** — same behavior, confirmed by reading both shaders.
- 🔷 **INTENTIONALLY DIFFERENT** — the HD path does something else on purpose,
  for a documented architectural reason (usually: it samples a dedicated
  replacement texture instead of the shared VRAM atlas, so VRAM-specific
  concepts like texpage/CLUT/depth don't apply to it at all).
- 🟡 **N/A BY SCOPE** — the native feature only applies to prim classes
  (semi-transparent, flat-color) the HD path never handles, so its absence
  isn't a gap.
- 🛠️ **FIXED THIS SESSION** — was a real gap, now fixed.
- ⚠️ **OPEN — NEEDS INVESTIGATION** — a real, unresolved difference found
  during this audit that hasn't been confirmed as a live bug yet, but looks
  suspicious enough to warrant a dedicated repro pass.

## Vertex attributes

| Native (`TEX_VS`) | HD (`HD_VS`) | Verdict | Notes |
|---|---|---|---|
| `a_pos` (vec2) | `a_pos` (vec2) | ✅ | Identical screen-space (VRAM px) coordinates — confirmed byte-identical this session via the ground-truth checkerboard calibration test (ISSUES.md #12). |
| `a_uv` (vec2) | `a_uv` (vec2) | ✅ | Native PS1 texel coords; both paths apply their own remap after (native: `u_twin`/`v_limits` in the fragment shader; HD: `u_scale`/`u_offset` baked into the vertex, see `gpu.c`'s match functions). |
| `a_col` (vec4) | `a_col` (vec3) | ✅ | Native carries an unused 4th (alpha) component the fragment shader never reads (`v_col.rgb` only) — dropping it in HD is not a functional difference. |
| `a_tpage` (vec2) | *(none)* | 🔷 | Native needs this to know which VRAM texture-page to sample; HD samples a dedicated bound replacement texture instead, so there's no "page" concept. |
| `a_clut` (vec2) | *(none)* | 🔷 | Same reasoning — CLUT lookup only applies to indexed VRAM sampling. |
| `a_depth` (float) | *(none)* | 🔷 | Same — 4/8/15-bit CLUT depth mode only applies to VRAM sampling. |
| `a_raw` (float) | *(none, uniform `u_raw` instead)* | ✅ | HD sends the same value as a per-draw-call uniform rather than a per-vertex attribute. Functionally identical because `draw_hd_replacement_triangle` draws exactly one (unbatched) triangle per call — a uniform and a flat attribute mean the same thing at that granularity. |
| `a_limits` (vec4) | `a_orig_uv` (vec2) + `u_piece_rects`/`u_piece_count` (see below) | 🛠️ | Was open, now fixed this session — see "Resolved items" below. |
| `a_semi` (float) | *(none)* | 🟡 | HD only ever draws `semi < 0` (opaque) prims; semi-transparent blend-mode state is meaningless here. |
| `a_q` (float, persp weight) | `a_q` (float) | 🛠️ | **Was completely absent from `HD_VS`/`HD_FS`** — always affine UV mapping regardless of what the native path (or an unmatched fallback of the exact same prim) would have used. Fixed this session: ported the same per-vertex `a_q` → `w = 1/q` → `smooth v_uv_p` / `flat v_persp` mechanism from `TEX_VS`/`TEX_FS`. This is what caused the reported "straight on `none`, wavy on `beetle`" floor/roof texture — affine vs. perspective-correct mapping on a receding surface viewed at an angle. |

## Vertex-shader uniforms

| Native (`TEX_VS`) | HD (`HD_VS`) | Verdict | Notes |
|---|---|---|---|
| `u_shift` | `u_shift` | ✅ | Same formula, same purpose (GL center-sample-grid alignment). |
| `u_xoff` | `u_xoff` | ✅ | Native-wide x translation. |
| `u_xhalf` | `u_xhalf` | ✅ | Native-wide x clip half-extent. |
| `u_xscale` (2D-backdrop x-stretch) | *(none)* | 🟡 | The backdrop-stretch special case only applies to flat 2D-backdrop prims (drawn through `GEO_*`), which never reach the HD-replacement path at all (already documented in the existing `draw_hd_replacement_triangle` comment: "HD shader has no backdrop-stretch uniforms... HD replacement only ever applies to textured triangles, never the flat-backdrop special case"). |
| `u_xcenter` | *(none)* | 🟡 | Same as above. |

## Fragment-shader inputs / uniforms

| Native (`TEX_FS`) | HD (`HD_FS`) | Verdict | Notes |
|---|---|---|---|
| `u_vram` (VRAM sampler) + `fetch_texel`/`vram_at` (CLUT decode) | `u_tex` (bound replacement texture) + `hd_sample_bilinear`/`hd_texelfetch_clamped` | 🔷 | Fundamentally different sampling source by design — this is *the* HD replacement mechanism, not a gap. |
| `u_semipass` | *(none)* | 🟡 | Opaque/semi STP-split pass selector — only meaningful for the semi-transparent-batch draw-order-correctness mechanism (see `flush_tex_batch`'s two-pass comment), which HD never participates in. |
| `u_semimode` + `v_semi` + `blend_factor` output (dual-source blend factors) | *(none)* | 🟡 | Same — PS1 blend-equation modes only apply to semi-transparent prims. |
| `u_twin` (texture window wrap/tile) | *(none)* | ✅ *(already handled upstream)* | **Not actually a gap** — `gpu_textured_triangle` explicitly skips HD matching entirely whenever a texture window is active (`(s_tw_mask_x \| s_tw_mask_y) == 0` gate before the HD-match block), with a detailed comment explaining exactly this: HD has no wrap/tile equivalent, so a windowed prim's `u_first..u_last` range is wider than the true tiled sub-region and would sample past it into whatever is adjacent in the replacement image (confirmed live previously: a battle-mode speech-bubble fill bleeding into unrelated stat-label text). Falls back to native, which already handles windowing correctly. Documented here for completeness since it's exactly the kind of gap this audit is looking for — this one was already caught and fixed before this session. |
| `u_maskset` (GP0 E6h set-mask, ORs bit15 into output alpha) | `u_maskset` + real per-texel STP lookup (see below) | 🛠️ | Was open, now fixed this session — see "Resolved items" below. |
| `u_filter` (native bilinear on/off) | *(HD always samples via `hd_sample_bilinear`, no nearest-only mode)* | 🔷 *(probably fine, not verified live)* | HD replacement textures are art assets meant to be viewed upscaled/smoothed; forcing bilinear regardless of the native `[video]` filter setting is very likely the intended behavior (matches how DuckStation/Beetle's own real HD-pack code always filters replacements), but this audit did not specifically verify it against either reference implementation's exact conditions. Low priority. |
| `u_shift` (bilinear texel-centre recentre) | *(not needed — `hd_sample_bilinear`'s own `fract(uv)-0.5` already centres correctly, see its 2026-09-09 comment)* | ✅ | Different mechanism, same effect; already covered by this session's Beetle-ported bilinear rewrite. |
| `u_silhouette_mode`, `u_viewport_x0`/`u_viewport_w` | same | ✅ | Debug-only visualization uniforms added this session, present in both. |
| `v_persp`/`v_uv_p` (perspective-correct UV) | same | 🛠️ | See vertex-attribute table above. |
| `v_raw` (unlit-mode Gouraud skip) | `u_raw` (uniform, same value) | ✅ | Same as `a_raw` above — uniform vs. attribute is a non-issue at this draw granularity. |
| `v_col` Gouraud shading (`rgb * v_col.rgb * 2.0`, skipped when raw) | `v_col` (same formula, `u_tint` multiplied in first) | ✅ | Confirmed already fixed in an earlier session (see the `HD_FS` comment: "Missing this entirely was why HD-replaced textures rendered at flat full brightness regardless of the room's actual lighting"). |
| discard on `raw==0` / `c00==0` (native cutout) | discard on `c.a<0.5` (HD pack's own alpha) | ✅ *(known, accepted asymmetry)* | This is the discard-mask content/authoring mismatch already root-caused and documented in ISSUES.md #12 — two independently-drawn cutout shapes at different resolutions, not a code bug, not fixable without either snapping HD to the native blocky boundary or reintroducing real alpha blending (previously tried and reverted for a different regression). |

## Resolved items (were open, fixed this session)

### 1. `u_maskset` / output alpha (mask/stencil bit) — 🛠️ fixed

`TEX_FS` writes `frag.a = (stp == 1 || u_maskset == 1) ? 1.0 : 0.0`, where
`stp` is the actual per-texel STP bit (`(raw >> 15) & 1`) read from the
sampled VRAM content — i.e. for an opaque prim, whether THIS texel's mask bit
should end up set is real, per-pixel, content-dependent data, not a constant.

`HD_FS` previously always wrote `frag = vec4(rgb, 1.0)` — unconditionally
alpha=1, regardless of `u_maskset` and regardless of what the replaced
content's own STP bit would have been.

**Fix:** `HD_FS` now looks up the ORIGINAL native VRAM texel at the
corresponding position (new `u_vram`/`u_orig_tpage`/`u_orig_clut`/
`u_orig_depth`/`u_orig_limits` uniforms, bound to texture unit 1, plus a new
`a_orig_uv` vertex attribute carrying the native UV alongside the
replacement UV) via `hd_orig_vram_at`/`hd_orig_fetch_texel`, extracts the
real `stp` bit from it, and writes
`frag = vec4(rgb, (stp == 1 || u_maskset == 1) ? 1.0 : 0.0)` — matching
native exactly. A sentinel (`u_orig_depth < 0`) forces `stp = 1` for call
sites that have no native texinfo to look up (calibration test, debug
markers), preserving old behavior there.

**Follow-on regression, found and fixed in the same session:** making
`frag.a` legitimately vary uncovered a latent, previously-invisible bug in
`draw_hd_replacement_triangle`: it left `GL_BLEND` permanently enabled with
`glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO)`,
using `frag.a` as the RGB blend weight. This was a harmless no-op while
`frag.a` was hardcoded to `1.0` (`dst*0 + src*1`), but once `frag.a` could be
`0.0` (any texel whose real STP bit is clear — the common case for opaque
PS1 content), the same blend equation reduced to `dst*1 + src*0`: the draw
became fully invisible wherever `stp == 0`. Symptom in live testing: garbled
dialogue text and a missing character model, while non-HD-replaced content
was unaffected (a strong signal it was specific to the HD draw path).
**Root cause and correct fix:** native's own `tex_batch_draw_passes` already
does `if (semi < 0) glDisable(GL_BLEND)` for opaque prims — the *only* prim
class HD replacement ever draws (`semi < 0` is the HD-match gate). HD's draw
call had simply never matched that. Changed `draw_hd_replacement_triangle`
to `glDisable(GL_BLEND)` unconditionally, matching native's treatment of the
same prim class exactly. This is a parity fix, not a workaround — blending
was never legitimately active for this prim class in the first place.
Verified live across all three `hd_backend` values (`none`, `duckstation`,
`beetle`) with no corruption.

### 2. `a_limits` equivalent for fused/composite HD textures — 🛠️ fixed

Native `fetch_texel` clamps (or wraps, via `u_twin`) `u,v` to `v_limits`
(`flat in ivec4 v_limits`) — the SPECIFIC sub-rectangle this exact prim is
allowed to sample from within the shared VRAM texture page, to avoid bleeding
into whatever unrelated content happens to be packed next to it in the same
page.

`HD_FS`'s `hd_texelfetch_clamped` previously clamped only to `ivec2(0),
size-1` — the bound texture's OWN full dimensions, with no per-prim
sub-rectangle at all. For the common case (one matched PNG = one dedicated
replacement texture, `hd_gl_get_texture`) this was already correct (the whole
bound texture IS the intended sample region), but for the **fused-page
compositing path** (`build_fused_composite`, opt-in via
`gpu_hd_texture_fusion_set`, off by default) it risked `hd_sample_bilinear`'s
4-tap footprint (up to 1 texel beyond the nominal sample point per axis)
bleeding across an adjacent piece's region of the same shared composite
texture.

**Fix:** added a per-piece rect lookup. `hd_piece_bounds` determines, once
per fragment, which packed piece the BASE (non-offset) sample texel falls
into, using a new uniform array `u_piece_rects[24]` (`ivec4` per piece:
x/y/w/h) plus `u_piece_count`; all 4 bilinear taps then clamp to that piece's
own sub-rect instead of the whole composite texture. `draw_hd_replacement_
triangle` populates these from `build_fused_composite`'s own piece list
(`fused_pieces[].x/y/width/height`) only when it actually built a fused
composite for this draw; every other call site passes `piece_count = 0`,
which makes `hd_piece_bounds` a no-op and falls back to the old whole-texture
clamp — so this is fix-only for the fused path and behaviorally identical to
before for the vast majority of ordinary single-PNG-match draws.

This path is still off by default (`hd_texture_page_fusion` opt-in in
`game.toml`) and has not had a dedicated live repro exercising actual piece
boundaries this session (fused compositing wasn't triggered by any of the
scenes tested) — the fix is code-reviewed and logically sound (mirrors
`v_limits`' own clamp-to-sub-rect approach) and is a strict no-op for
everything that WAS tested, but a dedicated fused-page test is still
worthwhile before treating it as fully proven.

### 3. Non-premultiplied-alpha bilinear blending — 🛠️ fixed

Found later the same day via a direct user report: a thin blue line visible
along a character's jaw/chin silhouette edge, HD-backend-only. Distinct from
item #77's "known, accepted asymmetry" row above (discard-boundary POSITION
mismatch between native's blocky cutout and HD's own alpha shape) — this is
a color-fringe HALO at the edge, not a shape mismatch, and had a different
root cause.

`hd_sample_bilinear` reads 4 raw RGBA texels via `texelFetch` and blends them
with a plain per-channel linear interpolation (`c00*w00 + c10*w10 + ...`).
Fully or near-transparent texels near a cutout edge commonly hold whatever
arbitrary RGB the pack author's tooling happened to leave there — alpha=0
normally makes that RGB invisible, so nobody bothers clearing it (observed:
a stray solid blue). Because the blend weights RGB and alpha independently
rather than in premultiplied space, a tap pair straddling the edge (one
fully-opaque skin/chin texel, one near-transparent stray-blue texel) can
blend to a result whose alpha still clears the `c.a < 0.5` discard cutoff
while its RGB has already been pulled toward that stray color — exactly the
thin colored fringe reported.

**Fix:** `hd_normalize_alpha` (`gpu_gl_renderer.c`, PNG-decode time) now
premultiplies RGB by alpha, immediately after its existing alpha-stretch
step (a near-transparent texel's RGB is scaled toward black, giving it
negligible weight in any future blend regardless of its original stray
color). `HD_FS`'s `main()` un-premultiplies (`c.rgb / c.a`, guarded against
near-zero alpha) immediately after `hd_sample_bilinear` returns, before the
`c.a < 0.5` discard test and before any tint/Gouraud color math touches the
result — so every downstream consumer of the sampled color still sees plain
(straight-alpha) RGB, only the blend step itself operates in premultiplied
space. `HD_CACHE_MAGIC` was bumped `0x33434448` → `0x34434448` ("HDC3" →
"HDC4") so any on-disk decode cache built before this fix (non-premultiplied
RGB) is treated as a cache miss and transparently re-decoded/re-cached in
the new format, rather than silently loaded in the wrong one.

Verified live on the PGXP build across three distinct HD-replaced
close-up scenes (a blond armored character in profile, three different
dialogue frames) with clean jaw/chin/silhouette edges and no fringe in any
of them, cache-busted via the magic bump so the fix was actually exercised
rather than serving a stale cached decode. The non-PGXP build shares this
exact object code (`gpu_gl_renderer.c` is not PGXP-specific) — a live
non-PGXP repro attempt hit an unrelated test-harness quirk (debug-savestate
load landing in an in-game menu instead of applying) and was not completed,
but there is no code-level reason to expect divergent behavior between the
two builds for this fix.

### 4. Missing per-primitive clamp (native's real `v_limits`) — 🛠️ fixed

Found via direct user report (a solid blue patch on a character's neck/
collar, HD-backend-only) and confirmed root-caused by extracting the raw
disc's ISO9660 filesystem and porting the relevant parts of the
[morris/vstools](https://github.com/morris/vstools) SHP/WEP texture-decode
logic (`WEPTextureMap.js`/`WEPPalette.js`/`VSTOOLS.js`'s `parseColor`) to
Python, then decoding the affected character's `.SHP` file directly from
the untouched disc data — completely independent of both our renderer and
the community HD pack. That decode showed the SAME blue lines already
baked into the ORIGINAL game texture: Vagrant Story packs multiple
independent body-part UV regions (face, collar, torso, boots) into one
shared 128x128 texture sheet per character, separated by the original
game's own padding pixels. The community HD pack's export faithfully
reproduced this real game content — **the 138 PNGs flagged earlier in this
investigation as "corrupted" are not corrupted; cropping them would have
been the wrong fix.**

The real gap: item #2's fix (above) only prevents bleeding ACROSS separate
pieces/uploads packed into a fused composite. It does nothing for bleeding
WITHIN a single upload/single-PNG match that itself packs multiple
independently-textured primitives sharing one texture (exactly this SHP
case) — `hd_sample_bilinear` still only clamped to the whole texture (or
whole fused piece), far coarser than native `fetch_texel`'s real clamp,
which is scoped to `v_limits` — the exact sub-window declared per
PRIMITIVE, not per upload. That's why `hd_backend none` never showed this:
native's tight per-primitive clamp excludes the padding entirely; ours
didn't.

**Fix:** a new `u_prim_limits`/`u_has_prim_limits` uniform pair in
`HD_FS`, intersected into `hd_sample_bilinear`'s existing bounds
(`bounds.xy = max(bounds.xy, u_prim_limits.xy)`, similarly for `.zw`).
Computed on the CPU side in `draw_hd_replacement_triangle` by mapping the
primitive's own native texel sub-window (`orig_texinfo[5..8]`, i.e.
`lim_u0/v0/u1/v1` — already threaded through for the mask-bit fix, item #1)
through the SAME `u_scale`/`u_offset`/`v_scale`/`v_offset` affine remap
already baked into that draw's vertex UVs, landing on the equivalent tight
rectangle in the replacement texture's own pixel space. Disabled
(`u_has_prim_limits=0`) for callers with no real underlying VRAM primitive
(calibration test, coverage-debug marker), keeping their old behavior
exactly. Intersects with (never widens beyond) the existing piece/whole-
texture bound, so it's additive/safe for the fused path too.

Verified live: the user confirmed the reported blue patch is gone after
this fix, rebuilt and tested on the PGXP build against the exact reported
scene.

## What's already confirmed solid (no further action needed)

- Vertex position math: bit-identical between paths, proven with a synthetic
  ground-truth test with a known, hand-specified offset (ISSUES.md #12) —
  this rules out any remaining "character position" concern.
- Perspective-correct vs. affine UV mapping: now matched (this session).
- Gouraud/vertex-color shading: already matched (earlier session).
- Texture-window tiling: correctly gated off (HD never attempts to replace a
  windowed prim at all, already fixed and documented before this session).
- Bilinear sampling texel-centre alignment: matched via the Beetle-ported
  manual 4-tap `texelFetch` blend (earlier this session).
- Native-wide mirror positioning: matched (uses the same `wide_dx()`/
  `u_xoff`/`u_xhalf` mechanism as the native path, modulo the separate
  `gl_wide_fast` present-time bug fixed this session, which was unrelated to
  HD texturing entirely — it reproduces with `hd_backend none`).

## Suggested next steps

1. Give the fused-page piece-clamp fix (resolved item #2) a dedicated live
   test that actually exercises a fused composite with multiple pieces —
   it hasn't been exercised by any scene tested this session since the
   feature is off by default.
1b. The disc-extraction + vstools-port tooling built for resolved item #4
   (`tools/`-adjacent scratch scripts, not yet committed anywhere permanent)
   is a reusable asset: a ground-truth, gameplay-independent way to decode
   any SHP/WEP character texture straight from the original disc data for
   comparison against either the community pack or this renderer's own
   output. Worth formalizing into a real tool if more texture-fidelity
   questions come up (it already achieves ~92% success across all on-disc
   .SHP/.WEP files; the ~8% failures are a contiguous ID range the
   reference vstools implementation itself doesn't fully handle either).
2. When touching `draw_hd_replacement_triangle` or `HD_FS` again, keep the
   `GL_BLEND`/`frag.a` interaction in mind (see resolved item #1's
   follow-on regression) — any future change that makes `frag.a` vary again
   needs to be checked against whatever the draw's blend state is at the
   time, since a silent no-op blend can turn into a silent full-invisibility
   bug with no compiler or type-system signal at all.
3. Treat this document as the reference to update whenever either shader
   pair changes, so a future feature added to one side doesn't silently
   create a new parity gap the way perspective correction did.
