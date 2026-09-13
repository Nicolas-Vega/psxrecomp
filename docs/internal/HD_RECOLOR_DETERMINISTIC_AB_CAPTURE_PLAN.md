# Deterministic capture-and-replay A/B for HD-texture rendering bugs

**Date:** 2026-09-12
**Status:** Steps 1 and 2 IMPLEMENTED and verified against the live game
(`gpu_capture_state` / `gpu_replay_capture` debug commands, `tools/
gpu_capture_state.py` / `tools/gpu_replay_capture.py`). Steps 3-6 not yet
built. Written to unblock the open `bcf02fd0` invisible-torso investigation
(`HD_RECOLOR_INVISIBLE_TORSO_INVESTIGATION.md`) and to become the standing
tool for any future native-vs-HD-backend rendering comparison.

## Problem this replaces

The previous A/B methodology (`psx-recomp-render-ab-testing-methodology`
memory) toggles `hd_backend` live between two capture bursts and compares
per-pixel medians to statistically cancel animation/timing jitter. That works,
but it only *cancels* jitter, never removes it, because the two bursts are
still captured at different real-world moments — and it compares whole
frames, which buries a single wrong texture inside thousands of unrelated
pixels.

This plan captures one frame's render input **once**, then replays that
identical, frozen input through both rendering configurations. Because both
renders consume byte-identical input, any pixel difference between them is
guaranteed to come from the rendering logic itself — not pose, animation, or
capture-timing drift. It also renders and diffs **one texture/polygon at a
time**, isolated on a flat chroma-key background, turning "is there a
difference somewhere in this frame" into a rankable, cycleable list of
per-element difference scores.

Both memories (`controlled-ab-methodology-for-rendering-bugs` and
`psx-deterministic-replay-ab-over-live-burst-median`) still apply: measure a
baseline before trusting a diff, and prefer this deterministic-replay,
per-element approach over live burst+median whenever the draw stream can be
captured and replayed — which, per the investigation below, it can.

## Why this is buildable now — existing building blocks

- **`gpu_write_gp0(uint32_t val)`** (`gpu_gl_renderer.c:6303`) is the real
  entry point that turns GP0 words into draw calls. Replaying a captured word
  stream through it means both renders go through the actual renderer code,
  not a reimplementation.
- **`hd_backend` is read live per-draw**, inside the primitive loop
  (`gpu_hd_texture_get_backend()`, `gpu_gl_renderer.c:4448`). Switching it
  does not reinit any GL resource, so a replay does not need to tear down and
  rebuild renderer state between backends.
- **Texture-upload tracking already has a wire format.** `hd_texture_pack_
  track_upload` / `hd_texture_pack_tracking_upload_count`
  (`hd_texture_pack.cpp`, `struct Upload` at line 254, serialization from
  line ~1264) serialize `pack->uploads` — the VRAM-region-hash tracker that
  the earlier A/B memory found is *not* carried by a savestate. This is the
  missing piece for a faithful snapshot: capture VRAM + this tracker's wire
  form + the frame's GP0 stream, and a replay has everything the HD matcher
  needs, with no silent fallback to native.
- **`gr_render_display_hires`** (used by `handle_screenshot_hires`,
  `debug_server.c:9138`) already resolves into an arbitrary caller-provided
  buffer, decoupled from the on-screen window — precedent that the renderer
  can target an offscreen destination, which per-texture isolation needs.
- **The GP0 ring / frame-capture pipeline** (`gpu_frame_capture.py`,
  `psx_gpu_frame.py`) already extracts a frame's decoded primitive stream
  (verts, UVs, texture page/CLUT/depth per primitive) and the raw `gpu_
  opcodes` dump. `gpu_frame_layers.py` already does per-function isolated
  rasterization, but deliberately does not sample textures — it is the
  closest existing analog to step 3 below, not a replacement for it (a real
  per-texture isolation must go through the actual GL draw calls, not a
  Python reimplementation, to be trustworthy).

## Design decision: sequential single-process replay, not concurrent threads

Once the input is a frozen snapshot, **determinism, not literal
simultaneity, is what removes jitter** — a single thread replaying the
snapshot twice (once per backend) already gets the full benefit. The
renderer carries substantial shared mutable global state not designed for
concurrent use (HD texture caches, `s_hd_draws_issued`, the recolor-table
upload, GL context/program objects) — running two backends concurrently
against it would mean duplicating all of that per context, a large lift for
a speed-only win.

**Chosen approach:** replay sequentially, one process, same renderer code
path, once per backend. If real parallelism is wanted later, the project
already has a precedent for it that avoids the shared-state problem
entirely: two independent OS processes on separate debug ports (as
`psx-runtime`/`psx-beetle` already do), not two threads sharing one
process's state.

## Plan

### 1. Capture — new debug command `gpu_capture_state`
Snapshot, as one bundle:
- Full VRAM image.
- The upload-tracker wire blob (`hd_texture_pack_track_upload`'s existing
  serialization).
- The current frame's GP0 word stream (same source `gpu_opcodes` / the ring
  already reads for `gpu_frame_capture.py`).

Triggered live, mid-gameplay, no savestate involved — sidesteps the
savestate/upload-tracking trap the earlier A/B memory found.

### 2. Replay — `gpu_replay_capture` debug command + `tools/gpu_replay_capture.py` (DONE)
Loads a capture, restores VRAM + tracker state, runs `gpu_write_gp0` over the
captured word stream twice, sequentially, in one synchronous handler call:
- once with `backend_a` (default 0, native)
- once with `backend_b` (default 2, live GPU recolor)

Both runs consume byte-identical input by construction. Implementation notes
from building it:

- **Restoring VRAM is two steps, not one.** `vram[]` (the CPU-side 1024x512
  mirror) is only half the picture — under the GL backend, native texture
  *sampling* reads from a separate GPU-resident mirror that `vram[]` writes
  get pushed to via `gr_vram_transfer_in()` (see `gp0_commit_cpu_to_vram`,
  `gpu.c`). `gpu_vram_restore()` does both: `memcpy` into `vram[]`, then one
  full-frame `gr_vram_transfer_in(0, 0, 1024, 512, vram)` so native rendering
  in the replay doesn't sample stale GPU-side texture data left over from
  whatever the live game was doing before the capture. It deliberately does
  **not** go through the HD-pack upload tracker (`hd_texture_pack_track_
  upload`) the way a real CPU→VRAM commit would — that would record this as
  one monolithic whole-VRAM upload and destroy the fine-grained upload
  history the tracker snapshot restores separately.
- **The replay filters the GP0 stream, keeping it in `gp0_stream.json`'s
  format on disk and only writing a flat binary `replay_words.bin` for the
  entries actually safe to replay** — the C handler never parses JSON.
  Excluded: anything the ring marked `truncated`, and the entire
  VRAM-transfer opcode family (`0x80`-`0xDF`): 0x80 VRAM→VRAM and 0xA0
  CPU→VRAM's effects are already fully captured in `vram.bin`, and 0xA0's
  pixel payload is never in the ring at all (it streams as separate
  `gpu_write_gp0` calls in a different state) — replaying its bare header
  would desync the state machine expecting pixels that never arrive.
  Everything else (drawing primitives, `0xE1`-`0xE6` state setters) is small
  and fully captured whenever not truncated.
- **The live game's own state is snapshotted first and unconditionally
  restored after both passes** (VRAM, tracker, `hd_backend`) — this command
  runs inside the actual live process, so it must never leave the running
  game rendering from replayed state once it returns control.
- **Per-frame GP0 state resets in practice.** The open question below about
  hidden dependencies on state *not* captured (texpage/draw-area/draw-offset
  latched from a previous frame) turned out not to matter empirically: real
  captures show the game reissuing `0xE3/0xE4/0xE5/0xE1/0xE2` (draw area,
  offset, draw mode, texture window) at the very start of every frame's GP0
  stream, so a frame's own captured words are self-sufficient. Worth
  re-checking if a future capture ever shows drawing before any state-setter
  in the same frame.
- **Smoke-tested against the live game**: captured a real in-progress frame
  (198 GP0 entries, real textured-rect content), replayed it native vs.
  recolor and native vs. replacement-pack — both came back byte-identical
  (expected: the captured frame was boot/title UI content with no HD-pack
  entries for either backend, not a coverage bug), confirmed live game state
  was unaffected by the two destructive replay-and-restore passes.
  **Not yet done:** replaying a frame with actual textured 3D character
  content and confirming the tool reports a real, non-zero, expected
  difference — worth doing once a capture from an actual gameplay scene
  (not boot/title) is available; old savestates from before this session's
  rebuild are ABI-incompatible with the current binary, so reaching such a
  scene needs live play, not a savestate shortcut.

### 3. Isolate — per-primitive / per-texture chroma-key render
Instead of one composited framebuffer, replay each primitive (or each group
of primitives sharing a `texture_hash`) alone, against a freshly-cleared
chroma-key (flat green) offscreen target, for both backends — reusing the
real `draw_hd_recolor_triangle` / `draw_hd_replacement_triangle` / native
draw calls, redirected to a throwaway FBO and invoked one element at a time.
Grouping key (per-texture vs. per-polygon) should be a parameter, not a
fixed choice — both are useful depending on the bug shape.

### 4. Diff per element
Background is a known constant, so the comparison is cheap and unambiguous:
- chroma-key in one render, opaque in the other → **coverage bug** (this is
  the `bcf02fd0` torso signature)
- both opaque, different color → **tint/recolor-table bug**

Score each texture/polygon (e.g. differing-pixel count and/or mean color
delta over the covered region) and rank worst-first.

### 5. Cycle-through review tool
A local HTML page, same pattern as `generate_upscale_review.py`: native-
isolated vs. HD-isolated pairs, sorted by difference score, paged through
worst-first, so a whole capture's worth of textures can be triaged in one
sitting instead of hunting for the one broken texture in a full-frame diff.

### 6. Freeze confirmed bugs as regression fixtures
Once step 4 finds a real divergence, capture its exact shader inputs
(`recolor_table`, `orig_texinfo`, clut/palette, UV range) as a fixture and
exercise it through a headless `hd_recolor_shader_eval` debug command that
runs the actual compiled `s_hd_recolor_prog` against saved inputs and
returns the output pixel — a permanent, oracle-faithful regression test
(no second/reimplemented shader to drift from the real one), re-run on every
future shader change.

## Open questions / risks — resolved during steps 1-2

- ~~Does the GP0 ring already retain raw words...~~ **Resolved**: yes, each
  ring entry (`GpuGp0RingEntry.cmd[]`) holds up to `GPU_GP0_RING_MAX_WORDS`
  (12) raw words per command; fixed-size drawing/state commands always fit,
  variable-length ones (polylines, VRAM transfers) get marked `truncated`
  and are excluded from replay by `gpu_replay_capture.py`'s filter.
- ~~Does gpu_write_gp0 have hidden dependencies on state not captured...~~
  **Resolved empirically**: not in practice — see step 2's per-frame-reset
  note above. Restoring `vram[]` alone was NOT enough, though — see step 2's
  note on `gr_vram_transfer_in`, the one real surprise this raised.
- Upload-tracker wire round-trip: exercised (a real 2076-byte tracker blob
  round-tripped through a full capture→restore→replay cycle without error),
  but not yet checked against a case where the replay's HD MATCH RESULT is
  verified to be identical to the live frame's match result index-for-index
  (the smoke test's frame had no HD-pack matches on either backend to
  compare) — worth confirming once step 3 or a richer capture exercises real
  matches.

## Next step

Steps 1-2 are done. Next: step 3, the isolated per-primitive/per-texture
chroma-key render — reusing the real `draw_hd_recolor_triangle` / `draw_hd_
replacement_triangle` / native draw calls against a throwaway offscreen
target, once per captured primitive (or per `texture_hash` group), for both
backends. Before that, it would be worth capturing and replaying a frame
with actual textured 3D character content (not boot/title UI) to confirm
the step-2 tool reports a real, expected, non-zero difference — the two
smoke tests so far only exercised the "correctly reports zero difference"
path.
