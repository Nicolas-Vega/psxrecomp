#!/usr/bin/env python3
"""gpu_replay_capture.py -- deterministic-replay A/B, step 2.

See docs/internal/HD_RECOLOR_DETERMINISTIC_AB_CAPTURE_PLAN.md. Step 1
(gpu_capture_state.py) captures one frame's VRAM, HD-pack upload-tracker
state, and GP0 stream into a bundle. This is step 2: replay that SAME
frozen bundle through gpu_write_gp0 -- the real runtime entry point every
live GP0 write goes through, not a reimplementation -- once per requested
backend, from an identical restored starting state each time. Because both
passes consume byte-identical input, any pixel difference between
replay_a.png and replay_b.png is guaranteed to come from the rendering
logic itself, never from pose, animation, or capture-timing drift (see the
psx-deterministic-replay-ab-over-live-burst-median memory for why that
matters more than it sounds).

    python3 gpu_replay_capture.py analysis/captures/bad
    python3 gpu_replay_capture.py analysis/captures/bad --backend-a 0 --backend-b 2

Before calling the runtime, this filters the bundle's gp0_stream.json down
to entries safe to replay verbatim and writes replay_words.bin (a flat
(count, word...)* stream) next to it:

  * drops any entry the ring itself marked truncated (its captured n_words
    is a lower bound, not the true command -- replaying it would desync
    gpu_write_gp0's own word-count state machine)
  * drops the whole VRAM-transfer opcode family (0x80-0xDF): 0x80 VRAM->VRAM
    and 0xA0 CPU->VRAM's effects are already fully baked into vram.bin, and
    0xA0's pixel payload streams as separate words the ring never records
    in the first place -- replaying its 3-word header alone would put the
    state machine into GP0_VRAM_WRITE expecting pixels that never come.
    0xC0-0xDF (VRAM->CPU readback) doesn't affect VRAM at all, so it's
    simply not needed.
  * drops polyline commands (0x48-0x4F mono, 0x58-0x5F shaded): variable
    length, so the ring only ever records the 1-word header (see gpu.c's
    gp0_write_gp0_body, the "Record polyline header (variable-length body
    not captured...)" comment) -- the vertex/color words and terminator
    that make it a real command never make it into gp0_stream.json.
    Replaying just that header word flips gpu_write_gp0's parser into
    GP0_POLYLINE_MONO/SHADED with no terminator ever coming, which then
    swallows every subsequent replayed word (state-setters AND drawing
    primitives alike) as fake polyline vertex/color data for the rest of
    the pass -- confirmed live: a captured "Ashley!!" scene's mono-
    polyline header at seq 9263206 corrupted its replay from that point
    on, matching a coverage-loss finding (477/597 primitives rendering in
    one backend but not the other) that a long investigation had otherwise
    pinned on the HD-recolor renderer itself. The dropped polyline's own
    pixels are simply not replayable from this stream (same as a VRAM
    transfer) -- there's no way to recover its body after the fact.

Everything else (drawing primitives, texpage/texwindow/draw-area/draw-
offset/mask-bit setters) is small, fixed-size, and fully captured whenever
not truncated, so it replays verbatim.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from psx_gpu_frame import DEFAULT_NATIVE_PORT, DebugConn, DebugError  # noqa: E402

# VRAM-transfer family: 0x80 VRAM->VRAM, 0xA0-0xBF CPU->VRAM, 0xC0-0xDF
# VRAM->CPU. See the module docstring for why each is excluded from replay.
_NO_REPLAY_OPCODE_LO = 0x80
_NO_REPLAY_OPCODE_HI = 0xDF

# Polyline headers: 0x48-0x4F mono, 0x58-0x5F shaded. See the module
# docstring for why replaying just the header corrupts the rest of the pass.
_POLYLINE_RANGES = ((0x48, 0x4F), (0x58, 0x5F))


def build_replay_words(gp0_stream_path: str, words_bin_path: str) -> dict:
    """Filters gp0_stream.json -> replay_words.bin. Returns a small report.

    Each kept entry is framed as (seq: u32, n_words: u32, words[n_words]: u32
    each, all little-endian). `seq` is carried through unused by step 2's
    replay but read by step 3's per-primitive isolation to cross-reference
    each isolated snapshot back to its original gp0_stream.json entry (func/
    pc/ra/op) for the manifest.
    """
    with open(gp0_stream_path, encoding="utf-8") as f:
        stream = json.load(f)

    kept = 0
    dropped_truncated = 0
    dropped_transfer = 0
    dropped_polyline = 0
    with open(words_bin_path, "wb") as out:
        for entry in stream.get("entries", []):
            if entry.get("truncated"):
                dropped_truncated += 1
                continue
            opcode = int(entry["op"], 16)
            if _NO_REPLAY_OPCODE_LO <= opcode <= _NO_REPLAY_OPCODE_HI:
                dropped_transfer += 1
                continue
            if any(lo <= opcode <= hi for lo, hi in _POLYLINE_RANGES):
                dropped_polyline += 1
                continue
            words = [int(w, 16) for w in entry.get("w", [])]
            if not words:
                continue
            out.write(struct.pack(f"<II{len(words)}I", entry["seq"], len(words), *words))
            kept += 1

    return {
        "total": len(stream.get("entries", [])),
        "kept": kept,
        "dropped_truncated": dropped_truncated,
        "dropped_transfer": dropped_transfer,
        "dropped_polyline": dropped_polyline,
    }


def diff_summary(path_a: str, path_b: str) -> str:
    """Cheap pixel-diff headline between the two replay PNGs, if PIL is
    available. This is NOT the per-texture isolation step (step 3) -- just
    an immediate "did anything actually change" signal."""
    try:
        from PIL import Image
        import numpy as np
    except ImportError:
        return "(install pillow + numpy for an automatic pixel diff)"

    if not (os.path.isfile(path_a) and os.path.isfile(path_b)):
        return "(one or both replay PNGs missing -- see wrote_a/wrote_b above)"

    a = np.asarray(Image.open(path_a).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(path_b).convert("RGB"), dtype=np.int16)
    if a.shape != b.shape:
        return f"(size mismatch: {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]})"

    diff = np.abs(a - b).sum(axis=2)
    changed = diff > 0
    n_changed = int(changed.sum())
    total = diff.size
    if n_changed == 0:
        return "identical -- 0 pixels differ"
    ys, xs = np.nonzero(changed)
    bbox = [int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())]
    return (f"{n_changed}/{total} pixels differ ({100.0 * n_changed / total:.2f}%), "
            f"bbox={bbox}, mean abs diff over changed px={diff[changed].mean():.1f}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", help="a bundle directory written by gpu_capture_state.py")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_NATIVE_PORT)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--backend-a", type=int, default=0,
                    help="0=none/native, 1=HD replacement pack, 2=live GPU recolor (default 0)")
    ap.add_argument("--backend-b", type=int, default=2,
                    help="see --backend-a (default 2)")
    args = ap.parse_args(argv)

    bundle_dir = os.path.abspath(args.dir)
    gp0_stream_path = os.path.join(bundle_dir, "gp0_stream.json")
    words_bin_path = os.path.join(bundle_dir, "replay_words.bin")
    if not os.path.isfile(gp0_stream_path):
        print(f"error: {gp0_stream_path} not found -- run gpu_capture_state.py first",
              file=sys.stderr)
        return 2

    report = build_replay_words(gp0_stream_path, words_bin_path)
    print(f"replay_words.bin: {report['kept']}/{report['total']} entries kept "
          f"({report['dropped_truncated']} truncated, "
          f"{report['dropped_transfer']} VRAM-transfer opcodes, "
          f"{report['dropped_polyline']} polyline headers dropped)")
    if report["kept"] == 0:
        print("warning: nothing to replay -- this capture had no replayable "
              "drawing/state commands", file=sys.stderr)

    try:
        with DebugConn(args.host, args.port, args.timeout) as conn:
            r = conn.cmd("gpu_replay_capture", dir=bundle_dir,
                         backend_a=args.backend_a, backend_b=args.backend_b)
    except DebugError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    print(f"backend_a={r['backend_a']} replayed_words={r['replayed_words_a']} "
          f"wrote={r['wrote_a']}  ->  replay_a.png")
    print(f"backend_b={r['backend_b']} replayed_words={r['replayed_words_b']} "
          f"wrote={r['wrote_b']}  ->  replay_b.png")

    path_a = os.path.join(bundle_dir, "replay_a.png")
    path_b = os.path.join(bundle_dir, "replay_b.png")
    print(f"diff: {diff_summary(path_a, path_b)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
