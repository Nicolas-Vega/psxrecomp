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
| `a_limits` (vec4) | *(none — see `hd_texelfetch_clamped`)* | ⚠️ | See "Open items" below — this is the one attribute-level omission that isn't obviously safe. |
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
| `u_maskset` (GP0 E6h set-mask, ORs bit15 into output alpha) | *(none — HD_FS always outputs `frag = vec4(rgb, 1.0)`)* | ⚠️ | See "Open items" below. |
| `u_filter` (native bilinear on/off) | *(HD always samples via `hd_sample_bilinear`, no nearest-only mode)* | 🔷 *(probably fine, not verified live)* | HD replacement textures are art assets meant to be viewed upscaled/smoothed; forcing bilinear regardless of the native `[video]` filter setting is very likely the intended behavior (matches how DuckStation/Beetle's own real HD-pack code always filters replacements), but this audit did not specifically verify it against either reference implementation's exact conditions. Low priority. |
| `u_shift` (bilinear texel-centre recentre) | *(not needed — `hd_sample_bilinear`'s own `fract(uv)-0.5` already centres correctly, see its 2026-09-09 comment)* | ✅ | Different mechanism, same effect; already covered by this session's Beetle-ported bilinear rewrite. |
| `u_silhouette_mode`, `u_viewport_x0`/`u_viewport_w` | same | ✅ | Debug-only visualization uniforms added this session, present in both. |
| `v_persp`/`v_uv_p` (perspective-correct UV) | same | 🛠️ | See vertex-attribute table above. |
| `v_raw` (unlit-mode Gouraud skip) | `u_raw` (uniform, same value) | ✅ | Same as `a_raw` above — uniform vs. attribute is a non-issue at this draw granularity. |
| `v_col` Gouraud shading (`rgb * v_col.rgb * 2.0`, skipped when raw) | `v_col` (same formula, `u_tint` multiplied in first) | ✅ | Confirmed already fixed in an earlier session (see the `HD_FS` comment: "Missing this entirely was why HD-replaced textures rendered at flat full brightness regardless of the room's actual lighting"). |
| discard on `raw==0` / `c00==0` (native cutout) | discard on `c.a<0.5` (HD pack's own alpha) | ✅ *(known, accepted asymmetry)* | This is the discard-mask content/authoring mismatch already root-caused and documented in ISSUES.md #12 — two independently-drawn cutout shapes at different resolutions, not a code bug, not fixable without either snapping HD to the native blocky boundary or reintroducing real alpha blending (previously tried and reverted for a different regression). |

## Open items found by this audit (not yet confirmed as live bugs)

### 1. `u_maskset` / output alpha (mask/stencil bit) — ⚠️ needs investigation

`TEX_FS` writes `frag.a = (stp == 1 || u_maskset == 1) ? 1.0 : 0.0`, where
`stp` is the actual per-texel STP bit (`(raw >> 15) & 1`) read from the
sampled VRAM content — i.e. for an opaque prim, whether THIS texel's mask bit
should end up set is real, per-pixel, content-dependent data, not a constant.

`HD_FS` always writes `frag = vec4(rgb, 1.0)` — unconditionally alpha=1,
regardless of `u_maskset` and regardless of what the replaced content's own
STP bit would have been. `draw_hd_replacement_triangle` does call
`mask_stencil(s_mask_set)` before drawing (same as the native path), but that
only wires up the GL *stencil test/write* side (a per-draw-call fixed
reference value replacing on stencil-test pass) — it says nothing about the
per-fragment alpha this shader outputs into the color attachment, which is a
separate value read back elsewhere (see `STENCIL_FS`, "Rebuild stencil bit 0
from a copied RGBA target's alpha").

**Why this might matter:** if a later draw or copy pass relies on that
alpha/mask-bit distinction being correct per-pixel (e.g. a masked check
against previously-drawn opaque content, or a semi-transparent effect layered
on top later), an HD-replaced opaque prim would report EVERY pixel as
mask-set, even ones whose original PS1 content had the mask bit clear.

**Why it might NOT matter in practice:** this needs a concrete repro before
treating it as confirmed. It's plausible the overwhelming majority of opaque
prims have STP=1 for all their solid (non-discarded) texels anyway (STP=0 on
an opaque, non-discarded texel is a fairly unusual authoring pattern), which
would make this invisible in normal play. Flagging for a dedicated test
(e.g. find a scene where a masked check or semi-transparent overlay
interacts with HD-replaced geometry, and A/B it with `hd_backend none`).

### 2. `a_limits` equivalent for fused/composite HD textures — ⚠️ needs investigation

Native `fetch_texel` clamps (or wraps, via `u_twin`) `u,v` to `v_limits`
(`flat in ivec4 v_limits`) — the SPECIFIC sub-rectangle this exact prim is
allowed to sample from within the shared VRAM texture page, to avoid bleeding
into whatever unrelated content happens to be packed next to it in the same
page.

`HD_FS`'s `hd_texelfetch_clamped` instead clamps to `ivec2(0), size-1` — the
bound texture's OWN full dimensions, with no per-prim sub-rectangle at all.
For the common case (one matched PNG = one dedicated replacement texture,
`hd_gl_get_texture`), this is fine: the whole bound texture IS the intended
sample region, so clamping to its full bounds is correct.

For the **fused-page compositing path** (`build_fused_composite`,
opt-in via `gpu_hd_texture_fusion_set`, off by default) it's less obviously
safe: that path deliberately composites MULTIPLE distinct HD entries and/or
native-fallback pieces into ONE shared texture, specifically so pieces that
individually only partially cover a page can be assembled into a complete
replacement. If a piece's bilinear sampling footprint (`hd_sample_bilinear`
reads up to 1 texel beyond the nominal sample point in each direction) can
reach across into an ADJACENT piece's region of that same composite texture
— rather than being clamped to just its own sub-rect the way native `v_limits`
would — that's a bleeding risk analogous to the already-fixed texture-window
case, just for a different feature. This is OFF BY DEFAULT (`hd_texture_page_
fusion` opt-in in `game.toml`), so it doesn't affect normal play, but is
worth a dedicated check before ever turning that feature on by default.

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

1. Live-verify open item #1 (mask/stencil alpha) with a targeted repro if one
   can be found; if confirmed, thread the real per-fragment STP-equivalent
   value through (the replacement PNG's own alpha channel already encodes a
   cutout shape — the question is whether ITS value, or a fixed 1.0, is the
   right thing to write for opaque HD-replaced pixels).
2. Revisit open item #2 only if/when fused-page compositing is considered
   for default-on — not urgent while it stays opt-in.
3. Treat this document as the reference to update whenever either shader
   pair changes, so a future feature added to one side doesn't silently
   create a new parity gap the way perspective correction did.
